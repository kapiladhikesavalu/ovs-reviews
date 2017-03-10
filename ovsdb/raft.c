/*
 * Copyright (c) 2014, 2016, 2017 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include "raft.h"

#include <errno.h>
#include <unistd.h>

#include "jsonrpc.h"
#include "lockfile.h"
#include "openvswitch/dynamic-string.h"
#include "openvswitch/hmap.h"
#include "openvswitch/json.h"
#include "openvswitch/list.h"
#include "openvswitch/vlog.h"
#include "ovs-rcu.h"
#include "ovs-thread.h"
#include "ovsdb-error.h"
#include "ovsdb-parser.h"
#include "ovsdb/log.h"
#include "poll-loop.h"
#include "random.h"
#include "seq.h"
#include "socket-util.h"
#include "stream.h"
#include "timeval.h"
#include "unicode.h"
#include "util.h"
#include "uuid.h"

VLOG_DEFINE_THIS_MODULE(raft);

static void raft_run_reconfigure(struct raft *);

struct raft;
union raft_rpc;

enum raft_role {
    RAFT_FOLLOWER,
    RAFT_CANDIDATE,
    RAFT_LEADER
};

enum raft_timer {
    RAFT_FAST,
    RAFT_SLOW
};

enum raft_server_phase {
    RAFT_PHASE_STABLE,          /* Not being changed. */

    /* Phases for servers being added. */
    RAFT_PHASE_CATCHUP,         /* Populating new server's log. */
    RAFT_PHASE_CAUGHT_UP,       /* Waiting for prev configuration to commit. */
    RAFT_PHASE_COMMITTING,      /* Waiting for new configuration to commit. */

    /* Phases for servers to be removed. */
    RAFT_PHASE_REMOVE,          /* To be removed. */
};

struct raft_server {
    struct hmap_node hmap_node; /* Hashed based on 'sid'. */

    struct uuid sid;            /* Server ID. */
    char *address;              /* "(tcp|ssl):1.2.3.4:5678" */
    struct jsonrpc_session *js; /* Connection to this server. */
    unsigned int js_seqno;

    /* Volatile state on candidates.  Reinitialized at start of election. */
    bool voted;              /* Has this server already voted? */

    /* Volatile state on leaders.  Reinitialized after election. */
    uint64_t next_index;     /* Index of next log entry to send this server. */
    uint64_t match_index;    /* Index of max log entry server known to have. */
    enum raft_server_phase phase;
    struct uuid reply_sid;      /* For use in AddServer/RemoveServer reply. */
};

static void raft_set_servers(struct raft *, const struct hmap *new_servers,
                             enum vlog_level);
static void raft_server_init_leader(struct raft *, struct raft_server *);

enum raft_entry_type {
    RAFT_DATA,
    RAFT_SERVERS
};

struct raft_entry {
    uint64_t term;
    enum raft_entry_type type;
    struct json *data;
};

struct raft_conn {
    struct ovs_list list_node;
    struct jsonrpc_session *js;
    struct uuid sid;

    /* Join. */
    unsigned int js_seqno;
};

struct raft_command {
    struct hmap_node hmap_node; /* In struct raft's 'commands' hmap. */
    uint64_t index;             /* Index in log. */

    unsigned int n_refs;
    enum raft_command_status status;
};

static void raft_command_complete(struct raft *, struct raft_command *,
                                  enum raft_command_status);

static void raft_complete_all_commands(struct raft *,
                                       enum raft_command_status);
static struct raft_command *raft_find_command(struct raft *, uint64_t index);

enum raft_waiter_type {
    RAFT_W_COMMAND,
    RAFT_W_APPEND,
    RAFT_W_VOTE
};

struct raft_waiter {
    struct ovs_list list_node;
    uint64_t fsync_seqno;
    enum raft_waiter_type type;
    union {
        /* RAFT_W_COMMAND. */
        struct {
            struct raft_command *cmd;
            uint64_t index;
        } command;

        /* RAFT_W_APPEND. */
        struct {
            struct raft_append_request *rq; /* Does not include 'entries'. */
        } append;
    };
};

static struct raft_waiter *raft_waiter_create(struct raft *,
                                              enum raft_waiter_type);

/* The Raft state machine. */
struct raft {
    struct ovsdb_log *storage;
    char *file_name;

/* Persistent derived state.
 *
 * This must be updated on stable storage before responding to RPCs, but it can
 * be derived from the header, snapshot, and log in 'storage'. */

    struct uuid cid;            /* Cluster ID (immutable for the cluster). */
    struct uuid sid;            /* Server ID (immutable for the server). */
    char *local_address;        /* Local address (immutable for the server). */
    char *name;                 /* Cluster name (immutable for the cluster). */

    struct hmap servers;        /* Contains "struct raft_server"s. */
    struct raft_server *me;     /* This server (points into 'servers'). */

/* Persistent state on all servers.
 *
 * Must be updated on stable storage before responding to RPCs. */

    uint64_t current_term;      /* Initialized to 0 and only increases. */
    struct uuid voted_for;      /* In current term, or all-zeros if none. */

    /* The log.
     *
     * A log entry with index 1 never really exists; the initial snapshot for a
     * Raft is considered to include this index.  The first real log entry has
     * index 2.
     *
     * A new Raft instance contains an empty log:  log_start=2, log_end=2.
     * Over time, the log grows:                   log_start=2, log_end=N.
     * At some point, the server takes a snapshot: log_start=N, log_end=N.
     * The log continues to grow:                  log_start=N, log_end=N+1...
     *
     * Must be updated on stable storage before responding to RPCs. */
    struct raft_entry *log;     /* Log entry i is in log[i - log_start]. */
    uint64_t log_start;         /* Index of first entry in log. */
    uint64_t log_end;           /* Index of last entry in log, plus 1. */
    size_t allocated_log;       /* Allocated entries in 'log'. */

    /* Snapshot state (see Figure 5.1)
     *
     * This is the state of the cluster as of the last discarded log entry,
     * that is, at log index 'log_start - 1' (called prevIndex in Figure 5.1).
     * Only committed log entries can be included in a snapshot. */
    uint64_t prev_term;               /* Term for index 'log_start - 1'. */
    struct hmap prev_servers;         /* Contains "struct raft_server"s. */
    struct json *snapshot;            /* Snapshot. */

/* Volatile state.
 *
 * The snapshot is always committed, but the rest of the log might not be yet.
 * 'last_applied' tracks what entries have been passed to the client.  If the
 * client hasn't yet read the latest snapshot, then even the snapshot isn't
 * applied yet.  Thus, the invariants are different for these members:
 *
 *     log_start - 1 <= commit_index < log_end.
 *     log_start - 2 <= last_applied < log_end.
 * */

    enum raft_role role;        /* Current role. */
    uint64_t commit_index;      /* Max log index known to be committed. */
    uint64_t last_applied;      /* Max log index applied to state machine. */
    struct uuid leader_sid;     /* Session ID of leader (zero, if unknown). */
    struct raft_waiter *vote_waiter;

#define ELECTION_TIME_BASE_MSEC 1024
#define ELECTION_TIME_RANGE_MSEC 1024
    long long int election_base;
    long long int election_timeout;

#define PING_TIME_MSEC (ELECTION_TIME_BASE_MSEC / 3)
    long long int ping_timeout;

    /* Used for joining a cluster. */
    bool joining;                 /* Attempting to join the cluster? */
    struct sset remote_addresses;
    long long int join_timeout;

    /* Used for leaving a cluster. */
    bool leaving;
    bool left;

    /* File synchronization. */
    bool fsync_thread_running;
    pthread_t fsync_thread;
    struct ovs_mutex fsync_mutex;
    uint64_t fsync_next OVS_GUARDED;
    uint64_t fsync_cur OVS_GUARDED;
    struct seq *fsync_request;
    struct seq *fsync_complete;
    struct ovs_list waiters;

    /* Network connections. */
    struct pstream *listener;
    long long int listen_backoff;
    struct ovs_list conns;

    /* Leaders only.  Reinitialized after becoming leader. */
    struct hmap add_servers;    /* Contains "struct raft_server"s to add. */
    struct raft_server *remove_server; /* Server being removed. */
    struct hmap commands;              /* Contains "struct raft_command"s. */

    /* Candidates only.  Reinitialized at start of election. */
    int n_votes;                /* Number of votes for me. */

    /* Parameters for deciding when to take next snapshot. */
#define SNAPSHOT_TIME_BASE_MSEC (10 * 60 * 1000)  /* 10 minutes. */
#define SNAPSHOT_TIME_RANGE_MSEC (10 * 60 * 1000) /* 10 minutes. */
    long long int next_snapshot; /* Minimum time for next snapshot, in ms. */
    off_t snapshot_size;         /* Size of previous snapshot, in bytes. */
};

static struct ovsdb_error *raft_read_header(struct raft *)
    OVS_WARN_UNUSED_RESULT;

static void *
raft_fsync_thread(void *raft_)
{
    struct raft *raft = raft_;
    for (;;) {
        ovsrcu_quiesce_start();

        uint64_t request_seq = seq_read(raft->fsync_request);

        ovs_mutex_lock(&raft->fsync_mutex);
        uint64_t next = raft->fsync_next;
        uint64_t cur = raft->fsync_cur;
        ovs_mutex_unlock(&raft->fsync_mutex);

        if (next == UINT64_MAX) {
            break;
        }

        if (cur != next) {
            /* XXX following has really questionable thread-safety. */
            struct ovsdb_error *error = ovsdb_log_commit(raft->storage);
            if (!error) {
                ovs_mutex_lock(&raft->fsync_mutex);
                raft->fsync_cur = next;
                ovs_mutex_unlock(&raft->fsync_mutex);

                seq_change(raft->fsync_complete);
            } else {
                char *error_string = ovsdb_error_to_string(error);
                VLOG_WARN("%s", error_string);
                free(error_string);
                ovsdb_error_destroy(error);
            }
        }

        seq_wait(raft->fsync_request, request_seq);
        poll_block();
    }
    return NULL;
}

#define RAFT_RPC_TYPES                                                  \
    /* Hello RPC. */                                                    \
    RAFT_RPC(RAFT_RPC_HELLO_REQUEST, "hello_request")                   \
                                                                        \
    /* AppendEntries RPC. */                                            \
    RAFT_RPC(RAFT_RPC_APPEND_REQUEST, "append_request")                 \
    RAFT_RPC(RAFT_RPC_APPEND_REPLY, "append_reply")                     \
                                                                        \
    /* RequestVote RPC. */                                              \
    RAFT_RPC(RAFT_RPC_VOTE_REQUEST, "vote_request")                     \
    RAFT_RPC(RAFT_RPC_VOTE_REPLY, "vote_reply")                         \
                                                                        \
    /* AddServer RPC. */                                                \
    RAFT_RPC(RAFT_RPC_ADD_SERVER_REQUEST, "add_server_request")         \
    RAFT_RPC(RAFT_RPC_ADD_SERVER_REPLY, "add_server_reply")             \
                                                                        \
    /* RemoveServer RPC. */                                             \
    RAFT_RPC(RAFT_RPC_REMOVE_SERVER_REQUEST, "remove_server_request")   \
    RAFT_RPC(RAFT_RPC_REMOVE_SERVER_REPLY, "remove_server_reply")       \
                                                                        \
    /* InstallSnapshot RPC. */                                          \
    RAFT_RPC(RAFT_RPC_INSTALL_SNAPSHOT_REQUEST, "install_snapshot_request") \
    RAFT_RPC(RAFT_RPC_INSTALL_SNAPSHOT_REPLY, "install_snapshot_reply") \
                                                                        \
    /* BecomeLeader RPC. */                                             \
    RAFT_RPC(RAFT_RPC_BECOME_LEADER, "become_leader")

enum raft_rpc_type {
#define RAFT_RPC(ENUM, NAME) ENUM,
    RAFT_RPC_TYPES
#undef RAFT_RPC
};

static const char *
raft_rpc_type_to_string(enum raft_rpc_type status)
{
    switch (status) {
#define RAFT_RPC(ENUM, NAME) case ENUM: return NAME;
        RAFT_RPC_TYPES
#undef RAFT_RPC
            }
    return "<unknown>";
}

static bool
raft_rpc_type_from_string(const char *s, enum raft_rpc_type *status)
{
#define RAFT_RPC(ENUM, NAME)                    \
    if (!strcmp(s, NAME)) {                     \
        *status = ENUM;                         \
        return true;                            \
    }
    RAFT_RPC_TYPES
#undef RAFT_RPC
        return false;
}

struct raft_rpc_common {
    enum raft_rpc_type type;    /* One of RAFT_RPC_*. */
    struct uuid sid;            /* SID of peer server. */
    char *comment;
};

struct raft_append_request {
    struct raft_rpc_common common;
    uint64_t term;              /* Leader's term. */
    uint64_t prev_log_index;    /* Log entry just before new ones. */
    uint64_t prev_log_term;     /* Term of prev_log_index entry. */
    uint64_t leader_commit;     /* Leader's commit_index. */

    /* The append request includes 0 or more log entries.  entries[0] is for
     * log entry 'prev_log_index + 1', and so on.
     *
     * A heartbeat append_request has no terms. */
    struct raft_entry *entries;
    unsigned int n_entries;
};

struct raft_append_reply {
    struct raft_rpc_common common;

    /* Copied from the state machine of the reply's sender. */
    uint64_t term;             /* Current term, for leader to update itself. */
    uint64_t log_end;          /* To allow capping next_index, see 4.2.1. */

    /* Copied from request. */
    uint64_t prev_log_index;   /* Log entry just before new ones. */
    uint64_t prev_log_term;    /* Term of prev_log_index entry. */
    unsigned int n_entries;

    /* Result. */
    bool success;
};

static void raft_send_append_reply(struct raft *,
                                   const struct raft_append_request *,
                                   bool success, const char *comment);
static void raft_update_match_index(struct raft *, struct raft_server *,
                                    uint64_t min_index);

struct raft_vote_request {
    struct raft_rpc_common common;
    uint64_t term;           /* Candidate's term. */
    uint64_t last_log_index; /* Index of candidate's last log entry. */
    uint64_t last_log_term;  /* Term of candidate's last log entry. */
};

struct raft_vote_reply {
    struct raft_rpc_common common;
    uint64_t term;          /* Current term, for candidate to update itself. */

    /* XXX is there any value in sending a reply with vote_granted==false? */
    bool vote_granted;      /* True means candidate received vote. */
};

static void raft_send_vote_reply(struct raft *, const struct uuid *dst,
                                 bool vote_granted);

struct raft_server_request {
    struct raft_rpc_common common;
    struct uuid sid;            /* Server to add or remove. */
    char *address;              /* For adding server only. */
};

#define RAFT_SERVER_STATUS_LIST                                         \
    /* The operation could not be initiated because this server is not  \
     * the current leader.  Only the leader can add or remove           \
     * servers. */                                                      \
    RSS(RAFT_SERVER_NOT_LEADER, "not-leader")                           \
                                                                        \
    /* The operation could not be initiated because there was nothing   \
     * to do.  For adding a new server, this means that the server is   \
     * already part of the cluster, and for removing a server, the      \
     * server to be removed was not part of the cluster. */             \
    RSS(RAFT_SERVER_NO_OP, "no-op")                                     \
                                                                        \
    /* The operation could not be initiated because an identical        \
     * operation was already in progress. */                            \
    RSS(RAFT_SERVER_IN_PROGRESS, "in-progress")                         \
                                                                        \
    /* Adding a server failed because of a timeout.  This could mean    \
     * that the server was entirely unreachable, or that it became      \
     * unreachable partway through populating it with an initial copy   \
     * of the log.  In the latter case, retrying the operation should   \
     * resume where it left off. */                                     \
    RSS(RAFT_SERVER_TIMEOUT, "timeout")                                 \
                                                                        \
    /* The operation was initiated but it later failed because this     \
     * server lost cluster leadership.  The operation may be retried    \
     * against the new cluster leader.  For adding a server, if the log \
     * was already partially copied to the new server, retrying the     \
     * operation should resume where it left off. */                    \
    RSS(RAFT_SERVER_LOST_LEADERSHIP, "lost-leadership")                 \
                                                                        \
    /* Adding a server was canceled by submission of an operation to    \
     * remove the same server, or removing a server was canceled by     \
     * submission of an operation to add the same server. */            \
    RSS(RAFT_SERVER_CANCELED, "canceled")                               \
                                                                        \
    /* Adding or removing a server could not be initiated because the   \
     * operation to remove or add the server, respectively, has been    \
     * logged but not committed.  The new operation may be retried once \
     * the former operation commits. */                                 \
    RSS(RAFT_SERVER_COMMITTING, "committing")                           \
                                                                        \
    /* Removing a server could not be initiated because, taken together \
     * with any other scheduled server removals, the cluster would be   \
     * empty.  (This calculation ignores scheduled or uncommitted add   \
     * server operations because of the possibility that they could     \
     * fail.)  */                                                       \
    RSS(RAFT_SERVER_EMPTY, "empty")                                     \
                                                                        \
    /* Success. */                                                      \
    RSS(RAFT_SERVER_OK, "success")

enum raft_server_status {
#define RSS(ENUM, NAME) ENUM,
    RAFT_SERVER_STATUS_LIST
#undef RSS
};

static const char *
raft_server_status_to_string(enum raft_server_status status)
{
    switch (status) {
#define RSS(ENUM, NAME) case ENUM: return NAME;
        RAFT_SERVER_STATUS_LIST
#undef RSS
            }
    return "<unknown>";
}

static bool
raft_server_status_from_string(const char *s, enum raft_server_status *status)
{
#define RSS(ENUM, NAME)                         \
    if (!strcmp(s, NAME)) {                     \
        *status = ENUM;                         \
        return true;                            \
    }
    RAFT_SERVER_STATUS_LIST
#undef RSS
    return false;
}

struct raft_server_reply {
    struct raft_rpc_common common;
    enum raft_server_status status;
    char *leader_address;
    struct uuid leader_sid;
};

struct raft_install_snapshot_request {
    struct raft_rpc_common common;

    uint64_t term;              /* Leader's term. */

    uint64_t last_index;        /* Replaces everything up to this index. */
    uint64_t last_term;         /* Term of last_index. */
    struct hmap last_servers;   /* Contains "struct raft_server"s. */

    /* Data. */
    struct json *data;
};

struct raft_install_snapshot_reply {
    struct raft_rpc_common common;

    uint64_t term;              /* For leader to update itself. */

    /* Repeated from the install_snapshot request. */
    uint64_t last_index;
    uint64_t last_term;
};

struct raft_become_leader {
    struct raft_rpc_common common;

    uint64_t term;              /* Leader's term. */
};

union raft_rpc {
    struct raft_rpc_common common;
    struct raft_append_request append_request;
    struct raft_append_reply append_reply;
    struct raft_vote_request vote_request;
    struct raft_vote_reply vote_reply;
    struct raft_server_request server_request;
    struct raft_server_reply server_reply;
    struct raft_install_snapshot_request install_snapshot_request;
    struct raft_install_snapshot_reply install_snapshot_reply;
    struct raft_become_leader become_leader;
};

static void raft_rpc_format(const union raft_rpc *, struct ds *);
static bool raft_rpc_is_heartbeat(const union raft_rpc *);

static void raft_handle_rpc(struct raft *, const union raft_rpc *);
static void raft_send(struct raft *, const union raft_rpc *);
static void raft_send__(struct raft *, const union raft_rpc *,
                        struct jsonrpc_session *);
static void raft_send_append_request(struct raft *,
                                     struct raft_server *, unsigned int n);
static void raft_rpc_destroy(union raft_rpc *);
static struct jsonrpc_msg *raft_rpc_to_jsonrpc(const struct raft *,
                                               const union raft_rpc *);
static struct ovsdb_error * OVS_WARN_UNUSED_RESULT
raft_rpc_from_jsonrpc(struct raft *, const struct jsonrpc_msg *,
                      union raft_rpc *);
static bool raft_receive_rpc(struct raft *, struct jsonrpc_session *,
                             struct uuid *sid, union raft_rpc *);
static void raft_run_session(struct raft *, struct jsonrpc_session *,
                             unsigned int *seqno, struct uuid *sid);
static void raft_wait_session(struct jsonrpc_session *);

static void raft_become_leader(struct raft *);
static void raft_become_follower(struct raft *);
static void raft_reset_timer(struct raft *);
static struct raft_server *raft_server_add(struct hmap *servers,
                                           const struct uuid *sid,
                                           const char *address);
static struct ovsdb_error * OVS_WARN_UNUSED_RESULT
raft_write_snapshot(struct raft *, struct ovsdb_log *,
                    uint64_t new_log_start, const struct json *snapshot);
static void raft_send_heartbeats(struct raft *);
static void raft_start_election(struct raft *);
static bool raft_truncate(struct raft *, uint64_t new_end);
static void raft_get_servers_from_log(struct raft *);

static struct raft_server *
raft_find_server__(const struct hmap *servers, const struct uuid *sid)
{
    struct raft_server *s;
    HMAP_FOR_EACH_IN_BUCKET (s, hmap_node, uuid_hash(sid), servers) {
        if (uuid_equals(sid, &s->sid)) {
            return s;
        }
    }
    return NULL;
}

static struct raft_server *
raft_find_server(const struct raft *raft, const struct uuid *sid)
{
    return raft_find_server__(&raft->servers, sid);
}

static struct ovsdb_error * OVS_WARN_UNUSED_RESULT
raft_address_parse(const char *address,
                   const char **classp, struct sockaddr_storage *ssp)
{
    const char *class;
    if (!strncmp(address, "ssl:", 4)) {
        class = "ssl";
    } else if (!strncmp(address, "tcp:", 4)) {
        class = "tcp";
    } else {
        return ovsdb_error(NULL, "%s: expected \"tcp\" or \"ssl\" address",
                           address);
    }

    struct sockaddr_storage ss;
    if (!inet_parse_active(address + 4, 0, &ss)) {
        return ovsdb_error(NULL, "%s: syntax error in address", address);
    }

    if (classp) {
        *classp = class;
    }
    if (ssp) {
        *ssp = ss;
    }
    return NULL;
}

static struct ovsdb_error * OVS_WARN_UNUSED_RESULT
raft_address_validate_json(const struct json *address)
{
    if (address->type != JSON_STRING) {
        return ovsdb_syntax_error(address, NULL,
                                  "server address is not string");
    }
    return raft_address_parse(json_string(address), NULL, NULL);
}

static char *
raft_make_address_passive(const char *address_)
{
    char *address = xstrdup(address_);
    char *p = strchr(address, ':') + 1;
    char *host = inet_parse_token(&p);
    char *port = inet_parse_token(&p);

    struct ds paddr = DS_EMPTY_INITIALIZER;
    ds_put_format(&paddr, "p%.3s:%s:", address, port);
    if (strchr(host, ':')) {
        ds_put_format(&paddr, "[%s]", host);
    } else {
        ds_put_cstr(&paddr, host);
    }
    free(address);
    return ds_steal_cstr(&paddr);
}

static void
raft_schedule_snapshot(struct raft *raft, bool quick)
{
    unsigned int base = SNAPSHOT_TIME_BASE_MSEC;
    unsigned int range = SNAPSHOT_TIME_RANGE_MSEC;
    if (quick) {
        base /= 10;
        range /= 10;
    }

    raft->next_snapshot = time_msec() + base + random_range(range);
}

static struct raft *
raft_alloc(const char *file_name)
{
    struct raft *raft = xzalloc(sizeof *raft);
    raft->file_name = xstrdup(file_name);
    hmap_init(&raft->servers);
    raft->log_start = raft->log_end = 1;
    hmap_init(&raft->prev_servers);
    raft->role = RAFT_FOLLOWER;
    sset_init(&raft->remote_addresses);
    raft->join_timeout = LLONG_MAX;
    ovs_mutex_init(&raft->fsync_mutex);
    raft->fsync_request = seq_create();
    raft->fsync_complete = seq_create();
    ovs_list_init(&raft->waiters);
    raft->listen_backoff = LLONG_MIN;
    ovs_list_init(&raft->conns);
    hmap_init(&raft->add_servers);
    hmap_init(&raft->commands);

    raft_reset_timer(raft);
    raft_schedule_snapshot(raft, false);

    return raft;
}

static struct ovsdb_error * OVS_WARN_UNUSED_RESULT
raft_write_header(struct ovsdb_log *storage,
                  const struct uuid *cid, const struct uuid *sid)
{
    struct json *header = json_object_create();
    json_object_put_format(header, "cluster_id", UUID_FMT, UUID_ARGS(cid));
    json_object_put_format(header, "server_id", UUID_FMT, UUID_ARGS(sid));
    struct ovsdb_error *error = ovsdb_log_write(storage, header);
    json_destroy(header);
    return error;
}

/* Creates an on-disk file thar represents a new Raft cluster and initializes
 * it to consist of a single server, the one on which this function is called.
 *
 * Creates the local copy of the cluster's log in 'file_name', which must not
 * already exist.  Gives it the name 'name', which should be the database
 * schema name and which is used only to match up this database with server
 * added to the cluster later if the cluster ID is unavailable.
 *
 * The new server is located at 'local_address', which must take one of the
 * forms "tcp:IP[:PORT]" or "ssl:IP[:PORT]", where IP is an IPv4 address or a
 * square bracket enclosed IPv6 address.  PORT, if present, is a port number
 * that defaults to RAFT_PORT.
 *
 * This only creates the on-disk file.  Use raft_open() to start operating the
 * new server.
 *
 * Returns null if successful, otherwise an ovsdb_error describing the
 * problem. */
struct ovsdb_error * OVS_WARN_UNUSED_RESULT
raft_create_cluster(const char *file_name, const char *name,
                    const char *local_address, const struct json *data)
{
    /* Parse and verify validity of the local address. */
    struct ovsdb_error *error = raft_address_parse(local_address, NULL, NULL);
    if (error) {
        return error;
    }

    /* Create log file. */
    struct ovsdb_log *storage;
    error = ovsdb_log_open(file_name, RAFT_MAGIC, OVSDB_LOG_CREATE_EXCL,
                           -1, &storage);
    if (error) {
        return error;
    }

    /* Write log file. */
    struct uuid sid, cid;
    uuid_generate(&sid);
    uuid_generate(&cid);

    char sid_s[UUID_LEN + 1];
    sprintf(sid_s, UUID_FMT, UUID_ARGS(&sid));

    struct json *snapshot = json_object_create();
    json_object_put_string(snapshot, "server_id", sid_s);
    json_object_put_string(snapshot, "address", local_address);
    json_object_put_string(snapshot, "name", name);

    struct json *prev_servers = json_object_create();
    json_object_put_string(prev_servers, sid_s, local_address);
    json_object_put(snapshot, "prev_servers", prev_servers);

    json_object_put_format(snapshot, "cluster_id", UUID_FMT, UUID_ARGS(&cid));
    json_object_put(snapshot, "prev_term", json_integer_create(0));
    json_object_put(snapshot, "prev_index", json_integer_create(1));
    json_object_put(snapshot, "prev_data", json_clone(data));

    error = ovsdb_log_write(storage, snapshot);
    json_destroy(snapshot);
    if (!error) {
        error = ovsdb_log_commit(storage);
    }
    ovsdb_log_close(storage);

    return error;
}

/* Creates a database file that represents a new server in an existing Raft
 * cluster.
 *
 * Creates the local copy of the cluster's log in 'file_name', which must not
 * already exist.  Gives it the name 'name', which must be the same name
 * passed in to raft_create_cluster() earlier.
 *
 * 'cid' is optional.  If specified, the new server will join only the cluster
 * with the given cluster ID.
 *
 * The new server is located at 'local_address', which must take one of the
 * forms "tcp:IP[:PORT]" or "ssl:IP[:PORT]", where IP is an IPv4 address or a
 * square bracket enclosed IPv6 address.  PORT, if present, is a port number
 * that defaults to RAFT_PORT.
 *
 * Joining the cluster requiring contacting it.  Thus, the 'n_remotes'
 * addresses in 'remote_addresses' specify the addresses of existing servers in
 * the cluster.  One server out of the existing cluster is sufficient, as long
 * as that server is reachable and not partitioned from the current cluster
 * leader.  If multiple servers from the cluster are specified, then it is
 * sufficient for any of them to meet this criterion.
 *
 * This only creates the on-disk file and does no network access.  Use
 * raft_open() to start operating the new server.  (Until this happens, the
 * new server has not joined the cluster.)
 *
 * Returns null if successful, otherwise an ovsdb_error describing the
 * problem. */
struct ovsdb_error * OVS_WARN_UNUSED_RESULT
raft_join_cluster(const char *file_name,
                  const char *name, const char *local_address,
                  char *remotes[], size_t n_remotes,
                  const struct uuid *cid)
{
    ovs_assert(n_remotes > 0);

    /* Parse and verify validity of the addresses. */
    struct ovsdb_error *error = raft_address_parse(local_address, NULL, NULL);
    if (error) {
        return error;
    }
    for (size_t i = 0; i < n_remotes; i++) {
        error = raft_address_parse(remotes[i], NULL, NULL);
        if (error) {
            return error;
        }
    }

    /* Verify validity of the cluster ID (if provided). */
    if (cid && uuid_is_zero(cid)) {
        return ovsdb_error(NULL, "all-zero UUID is not valid cluster ID");
    }

    /* Create log file. */
    struct ovsdb_log *storage;
    error = ovsdb_log_open(file_name, RAFT_MAGIC, OVSDB_LOG_CREATE_EXCL,
                           -1, &storage);
    if (error) {
        return error;
    }

    /* Write log file. */
    struct uuid sid;
    uuid_generate(&sid);

    char sid_s[UUID_LEN + 1];
    sprintf(sid_s, UUID_FMT, UUID_ARGS(&sid));

    struct json *remotes_json = json_array_create_empty();
    for (size_t i = 0; i < n_remotes; i++) {
        json_array_add(remotes_json, json_string_create(remotes[i]));
    }

    struct json *snapshot = json_object_create();
    json_object_put_string(snapshot, "server_id", sid_s);
    json_object_put_string(snapshot, "address", local_address);
    json_object_put_string(snapshot, "name", name);
    json_object_put(snapshot, "remotes", remotes_json);
    if (cid) {
        json_object_put_format(snapshot, "cluster_id",
                               UUID_FMT, UUID_ARGS(cid));
    }

    error = ovsdb_log_write(storage, snapshot);
    json_destroy(snapshot);
    if (!error) {
        error = ovsdb_log_commit(storage);
    }
    ovsdb_log_close(storage);

    return error;
}

struct ovsdb_error *
raft_read_metadata(const char *file_name, struct raft_metadata *md)
{
    struct raft *raft = raft_alloc(file_name);
    struct ovsdb_error *error = ovsdb_log_open(file_name, RAFT_MAGIC,
                                               OVSDB_LOG_READ_ONLY, -1,
                                               &raft->storage);
    if (error) {
        goto exit;
    }

    error = raft_read_header(raft);
    if (error) {
        goto exit;
    }

    md->sid = raft->sid;
    md->name = xstrdup(raft->name);
    md->local = xstrdup(raft->local_address);
    md->cid = raft->cid;

exit:
    if (error) {
        memset(md, 0, sizeof *md);
    }
    raft_close(raft);
    return error;
}

void
raft_metadata_destroy(struct raft_metadata *md)
{
    if (md) {
        free(md->name);
        free(md->local);
    }
}

static struct json *
raft_entry_to_json(const struct raft_entry *e)
{
    struct json *json = json_object_create();
    json_object_put_uint(json, "term", e->term);
    json_object_put(json,  e->type == RAFT_DATA ? "data" : "servers",
                    json_clone(e->data));
    return json;
}

static struct json *
raft_entry_to_json_with_index(const struct raft *raft, uint64_t index)
{
    ovs_assert(index >= raft->log_start && index < raft->log_end);
    struct json *json = raft_entry_to_json(&raft->log[index
                                                      - raft->log_start]);
    json_object_put_uint(json, "index", index);
    return json;
}

static uint64_t
parse_uint(struct ovsdb_parser *p, const char *name)
{
    const struct json *json = ovsdb_parser_member(p, name, OP_INTEGER);
    return json ? json_integer(json) : 0;
}

static bool
parse_boolean(struct ovsdb_parser *p, const char *name)
{
    const struct json *json = ovsdb_parser_member(p, name, OP_BOOLEAN);
    return json && json_boolean(json);
}

static const char *
parse_string__(struct ovsdb_parser *p, const char *name, bool optional)
{
    enum ovsdb_parser_types types = OP_STRING | (optional ? OP_OPTIONAL : 0);
    const struct json *json = ovsdb_parser_member(p, name, types);
    return json ? json_string(json) : NULL;
}

static const char *
parse_required_string(struct ovsdb_parser *p, const char *name)
{
    return parse_string__(p, name, false);
}

static const char *
parse_optional_string(struct ovsdb_parser *p, const char *name)
{
    return parse_string__(p, name, true);
}

static bool
parse_uuid__(struct ovsdb_parser *p, const char *name, bool optional,
             struct uuid *uuid)
{
    const char *s = parse_string__(p, name, optional);
    if (s) {
        if (uuid_from_string(uuid, s)) {
            return true;
        }
        ovsdb_parser_raise_error(p, "%s is not a valid UUID", name);
    }
    *uuid = UUID_ZERO;
    return false;
}

static struct uuid
parse_required_uuid(struct ovsdb_parser *p, const char *name)
{
    struct uuid uuid;
    parse_uuid__(p, name, false, &uuid);
    return uuid;
}

static bool
parse_optional_uuid(struct ovsdb_parser *p, const char *name,
                    struct uuid *uuid)
{
    return parse_uuid__(p, name, true, uuid);
}

static void
raft_server_destroy(struct raft_server *s)
{
    if (s) {
        jsonrpc_session_close(s->js);
        free(s->address);
        free(s);
    }
}

static void
raft_servers_destroy(struct hmap *servers)
{
    struct raft_server *s, *next;
    HMAP_FOR_EACH_SAFE (s, next, hmap_node, servers) {
        hmap_remove(servers, &s->hmap_node);
        raft_server_destroy(s);
    }
    hmap_destroy(servers);
}

static struct raft_server *
raft_server_add(struct hmap *servers, const struct uuid *sid,
                const char *address)
{
    struct raft_server *s = xzalloc(sizeof *s);
    s->sid = *sid;
    s->address = xstrdup(address);
    hmap_insert(servers, &s->hmap_node, uuid_hash(sid));
    return s;
}

static struct raft_server *
raft_server_clone(const struct raft_server *src)
{
    struct raft_server *dst = xmemdup(src, sizeof *src);
    dst->address = xstrdup(dst->address);
    return dst;
}

static void
raft_servers_clone(struct hmap *dst, const struct hmap *src)
{
    struct raft_server *s;
    hmap_init(dst);
    HMAP_FOR_EACH (s, hmap_node, src) {
        struct raft_server *s2 = raft_server_clone(s);
        hmap_insert(dst, &s2->hmap_node, uuid_hash(&s2->sid));
    }
}

static struct ovsdb_error * OVS_WARN_UNUSED_RESULT
raft_servers_from_json__(const struct json *json, struct hmap *servers)
{
    if (!json || json->type != JSON_OBJECT) {
        return ovsdb_syntax_error(json, NULL, "servers must be JSON object");
    } else if (shash_is_empty(json_object(json))) {
        return ovsdb_syntax_error(json, NULL, "must have at least one server");
    }

    /* Parse new servers. */
    struct shash_node *node;
    SHASH_FOR_EACH (node, json_object(json)) {
        /* Parse server UUID. */
        struct uuid sid;
        if (!uuid_from_string(&sid, node->name)) {
            return ovsdb_syntax_error(json, NULL, "%s is a not a UUID",
                                      node->name);
        }

        const struct json *address = node->data;
        struct ovsdb_error *error = raft_address_validate_json(address);
        if (error) {
            return error;
        }

        raft_server_add(servers, &sid, json_string(address));
    }

    return NULL;
}

static struct ovsdb_error * OVS_WARN_UNUSED_RESULT
raft_servers_from_json(const struct json *json, struct hmap *servers)
{
    hmap_init(servers);
    struct ovsdb_error *error = raft_servers_from_json__(json, servers);
    if (error) {
        raft_servers_destroy(servers);
    }
    return error;
}

static struct ovsdb_error * OVS_WARN_UNUSED_RESULT
raft_remotes_from_json(const struct json *json, struct sset *remotes)
{
    sset_init(remotes);

    const struct json_array *array = json_array(json);
    if (!array->n) {
        return ovsdb_syntax_error(json, NULL,
                                  "at least one remote address is required");
    }
    for (size_t i = 0; i < array->n; i++) {
        const struct json *address = array->elems[i];
        struct ovsdb_error *error = raft_address_validate_json(address);
        if (error) {
            return error;
        }
        sset_add(remotes, json_string(address));
    }
    return NULL;
}

static struct ovsdb_error * OVS_WARN_UNUSED_RESULT
raft_servers_validate_json(const struct json *json)
{
    struct hmap servers = HMAP_INITIALIZER(&servers);
    struct ovsdb_error *error = raft_servers_from_json__(json, &servers);
    raft_servers_destroy(&servers);
    return error;
}

static struct json *
raft_servers_to_json(const struct hmap *servers)
{
    struct json *json = json_object_create();
    struct raft_server *s;
    HMAP_FOR_EACH (s, hmap_node, servers) {
        char sid_s[UUID_LEN + 1];
        sprintf(sid_s, UUID_FMT, UUID_ARGS(&s->sid));
        json_object_put_string(json, sid_s, s->address);
    }
    return json;
}

static void
raft_servers_format(const struct hmap *servers, struct ds *ds)
{
    int i = 0;
    const struct raft_server *s;
    HMAP_FOR_EACH (s, hmap_node, servers) {
        if (i++) {
            ds_put_cstr(ds, ", ");
        }
        ds_put_format(ds, "%04x(%s)", uuid_prefix(&s->sid, 4), s->address);
    }
}

static const struct raft_entry *
raft_get_entry(const struct raft *raft, uint64_t index)
{
    ovs_assert(index >= raft->log_start);
    ovs_assert(index < raft->log_end);
    return &raft->log[index - raft->log_start];
}

static uint64_t
raft_get_term(const struct raft *raft, uint64_t index)
{
    return (index == raft->log_start - 1
            ? raft->prev_term
            : raft_get_entry(raft, index)->term);
}

static struct json *
raft_servers_for_index(const struct raft *raft, uint64_t index)
{
    ovs_assert(index >= raft->log_start - 1);
    ovs_assert(index < raft->log_end);

    const struct json *servers = NULL;
    for (uint64_t i = raft->log_start; i <= index; i++) {
        const struct raft_entry *e = raft_get_entry(raft, i);
        if (e->type == RAFT_SERVERS) {
            servers = e->data;
        }
    }
    return (servers
            ? json_clone(servers)
            : raft_servers_to_json(&raft->prev_servers));
}

static void
raft_set_servers(struct raft *raft, const struct hmap *new_servers,
                 enum vlog_level level)
{
    struct raft_server *s, *next;
    HMAP_FOR_EACH_SAFE (s, next, hmap_node, &raft->servers) {
        if (!raft_find_server__(new_servers, &s->sid)) {
            if (raft->me == s) {
                raft->me = NULL;
                /* XXX */
            }
            /* XXX raft->leader */
            /* XXX raft->remove_server */
            hmap_remove(&raft->servers, &s->hmap_node);
            VLOG(level, "server %04x removed from configuration",
                 uuid_prefix(&s->sid, 4));
            raft_server_destroy(s);
        }
    }

    HMAP_FOR_EACH_SAFE (s, next, hmap_node, new_servers) {
        if (!raft_find_server__(&raft->servers, &s->sid)) {
            VLOG(level, "server %04x added to configuration",
                 uuid_prefix(&s->sid, 4));

            struct raft_server *new = xzalloc(sizeof *new);
            new->sid = s->sid;
            new->address = xstrdup(s->address);
            new->voted = true;  /* XXX conservative */
            raft_server_init_leader(raft, new);
            hmap_insert(&raft->servers, &new->hmap_node, uuid_hash(&new->sid));

            if (uuid_equals(&raft->sid, &new->sid)) {
                raft->me = new;
            }
        }
    }
}

static struct ovsdb_error * OVS_WARN_UNUSED_RESULT
raft_entry_from_json(struct json *json, struct raft_entry *e)
{
    memset(e, 0, sizeof *e);

    struct ovsdb_parser p;
    ovsdb_parser_init(&p, json, "raft log entry");
    e->term = parse_uint(&p, "term");
    const struct json *servers_json = ovsdb_parser_member(
        &p, "servers", OP_OBJECT | OP_OPTIONAL);
    if (servers_json) {
        struct ovsdb_error *error = raft_servers_validate_json(servers_json);
        if (error) {
            return error;
        }
        e->type = RAFT_SERVERS;
        e->data = json_clone(servers_json);
    } else {
        const struct json *data = ovsdb_parser_member(&p, "data", OP_STRING);
        if (data) {
            e->type = RAFT_DATA;
            e->data = json_clone(data);
        }
    }

    struct ovsdb_error *error = ovsdb_parser_finish(&p);
    if (error) {
        json_destroy(e->data);
    }
    return error;
}

static struct raft_entry *
raft_add_entry(struct raft *raft,
               uint64_t term, enum raft_entry_type type, struct json *data)
{
    if (raft->log_end - raft->log_start >= raft->allocated_log) {
        raft->log = x2nrealloc(raft->log, &raft->allocated_log,
                               sizeof *raft->log);
    }

    struct raft_entry *entry = &raft->log[raft->log_end++ - raft->log_start];
    entry->type = type;
    entry->term = term;
    entry->data = data;
    return entry;
}

static struct ovsdb_error * OVS_WARN_UNUSED_RESULT
raft_write_entry(struct raft *raft,
                 uint64_t term, enum raft_entry_type type, struct json *data)
{
    /* XXX  when one write fails we need to make all subsequent writes fail (or
     * just not attempt them) since omitting some writes is fatal */

    raft_add_entry(raft, term, type, data);
    struct json *json = raft_entry_to_json_with_index(raft, raft->log_end - 1);
    struct ovsdb_error *error = ovsdb_log_write(raft->storage, json);
    json_destroy(json);

    if (error) {
        /* XXX? */
        json_destroy(raft->log[--raft->log_end].data);
    }

    return error;
}

static struct ovsdb_error * OVS_WARN_UNUSED_RESULT
raft_write_state(struct ovsdb_log *storage,
                 uint64_t term, const struct uuid *vote)
{
    struct json *json = json_object_create();
    json_object_put_uint(json, "term", term);
    if (vote && !uuid_is_zero(vote)) {
        json_object_put_format(json, "vote", UUID_FMT, UUID_ARGS(vote));
    }
    struct ovsdb_error *error = ovsdb_log_write(storage, json);
    json_destroy(json);

    return error;
}

static struct ovsdb_error * OVS_WARN_UNUSED_RESULT
parse_log_record(struct raft *raft, const struct json *entry)
{
    /* All log records include "term", plus at most one of:
     *
     *     - "index" and "data".
     *
     *     - "index" and "servers".
     *
     *     - "vote".
     */

    struct ovsdb_parser p;
    ovsdb_parser_init(&p, entry, "raft log entry");

    /* Parse "term".
     *
     * A Raft leader can replicate entries from previous terms to the other
     * servers in the cluster, retaining the original terms on those entries
     * (see section 3.6.2 "Committing entries from previous terms" for more
     * information), so it's OK for the term in a log record to precede the
     * current term. */
    uint64_t term = parse_uint(&p, "term");
    if (term > raft->current_term) {
        raft->current_term = term;
        raft->voted_for = UUID_ZERO;
    }

    /* Parse "vote". */
    struct uuid vote;
    if (parse_optional_uuid(&p, "vote", &vote)) {
        if (uuid_is_zero(&raft->voted_for)) {
            raft->voted_for = vote;
        } else if (!uuid_equals(&raft->voted_for, &vote)) {
            ovsdb_parser_raise_error(&p, "log entry term %"PRIu64 " votes for "
                                     "both %04x and %04x", term,
                                     uuid_prefix(&raft->voted_for, 4),
                                     uuid_prefix(&vote, 4));
        }
        goto done;
    }

    /* Parse "index". */
    const struct json *index_json = ovsdb_parser_member(
        &p, "index", OP_INTEGER | OP_OPTIONAL);
    if (!index_json) {
        goto done;
    }
    uint64_t index = json_integer(index_json);
    if (index < raft->log_end) {
        /* XXX log that the log gets truncated? */
        raft_truncate(raft, index);
    } else if (index > raft->log_end) {
        ovsdb_parser_raise_error(&p, "log entry index %"PRIu64" skips past "
                                 "expected %"PRIu64, index, raft->log_end);
        goto done;
    }

    /* Since there's an index, this is a log record that includes a Raft log
     * entry, as opposed to just advancing the term or marking a vote.
     * Therefore, the term must not precede the term of the previous log
     * entry. */
    uint64_t prev_term = (raft->log_end > raft->log_start
                          ? raft->log[raft->log_end - raft->log_start - 1].term
                          : raft->prev_term);
    if (term < prev_term) {
        ovsdb_parser_raise_error(&p, "log entry index %"PRIu64" term "
                                 "%"PRIu64" precedes previous entry's term "
                                 "%"PRIu64, index, term, prev_term);
        goto done;
    }

    /* Parse "servers" or "data"; exactly one must be present.*/
    const struct json *servers_json
        = ovsdb_parser_member(&p, "servers", OP_OBJECT | OP_OPTIONAL);
    if (servers_json) {
        struct ovsdb_error *error = raft_servers_validate_json(servers_json);
        if (error) {
            ovsdb_error_destroy(ovsdb_parser_finish(&p));
            return error;
        }
        raft_add_entry(raft, term, RAFT_SERVERS, json_clone(servers_json));
    } else {
        const struct json *data = ovsdb_parser_member(&p, "data", OP_STRING);
        if (data) {
            raft_add_entry(raft, term, RAFT_DATA, json_clone(data));
        }
    }

done:
    return ovsdb_parser_finish(&p);
}

static struct ovsdb_error * OVS_WARN_UNUSED_RESULT
raft_read_header(struct raft *raft)
{
    /* Read header record. */
    struct json *header;
    struct ovsdb_error *error = ovsdb_log_read(raft->storage, &header);
    if (error || !header) {
        /* Report error or end-of-file. */
        return error;
    }
    struct ovsdb_parser p;
    ovsdb_parser_init(&p, header, "raft header");

    /* Parse always-required fields. */
    raft->sid = parse_required_uuid(&p, "server_id");
    raft->name = nullable_xstrdup(parse_required_string(&p, "name"));
    raft->local_address = nullable_xstrdup(
        parse_required_string(&p, "address"));

    /* Parse "remotes", if present.
     *
     * If this is present, then this database file is for the special case of a
     * server that was created with "ovsdb-tool join-cluster" and has not yet
     * joined its cluster, */
    const struct json *remotes = ovsdb_parser_member(&p, "remotes",
                                                     OP_ARRAY | OP_OPTIONAL);
    if (remotes) {
        raft->joining = true;
        error = raft_remotes_from_json(remotes, &raft->remote_addresses);
    } else {
        /* Parse required set of servers. */
        const struct json *prev_servers_json = ovsdb_parser_member(
            &p, "prev_servers", OP_OBJECT);
        struct hmap prev_servers;
        error = raft_servers_from_json(prev_servers_json, &prev_servers);
        if (!error) {
            hmap_swap(&prev_servers, &raft->prev_servers);
        }
        raft_servers_destroy(&prev_servers);

        /* Parse term, index, and snapshot.  If any of these is present, all of
         * them must be. */
        const struct json *snapshot = ovsdb_parser_member(&p, "prev_data",
                                                          OP_ANY | OP_OPTIONAL);
        if (snapshot) {
            raft->prev_term = parse_uint(&p, "prev_term");
            raft->log_start = raft->log_end = parse_uint(&p, "prev_index") + 1;
            raft->commit_index = raft->log_start - 1;
            raft->last_applied = raft->log_start - 2;
            raft->snapshot = json_clone(snapshot);
            raft->snapshot_size = ovsdb_log_get_offset(raft->storage);
        }
    }

    /* Parse cluster ID.  If we're joining a cluster, this is optional,
     * otherwise it is mandatory. */
    parse_uuid__(&p, "cluster_id", raft->joining, &raft->cid);

    error = ovsdb_parser_finish(&p);
    json_destroy(header);
    return error;
}

static struct ovsdb_error * OVS_WARN_UNUSED_RESULT
raft_read_log(struct raft *raft)
{
    for (;;) {
        struct json *entry;
        struct ovsdb_error *error = ovsdb_log_read(raft->storage, &entry);
        if (!entry) {
            if (error) {
                /* We assume that the error is due to a partial write while
                 * appending to the file before a crash, so log it and
                 * continue. */
                char *error_string = ovsdb_error_to_string(error);
                VLOG_WARN("%s", error_string);
                free(error_string);
                ovsdb_error_destroy(error);
                error = NULL;
            }
            break;
        }

        error = parse_log_record(raft, entry);
        if (error) {
            return error;
        }
    }

    /* Set the most recent servers. */
    raft_get_servers_from_log(raft);

    return NULL;
}

static void
raft_reset_timer(struct raft *raft)
{
    unsigned int duration = (ELECTION_TIME_BASE_MSEC
                             + random_range(ELECTION_TIME_RANGE_MSEC));
    raft->election_base = time_msec();
    raft->election_timeout = raft->election_base + duration;
}

static void
raft_add_conn(struct raft *raft, struct jsonrpc_session *js)
{
    struct raft_conn *conn = xzalloc(sizeof *conn);
    ovs_list_push_back(&raft->conns, &conn->list_node);
    conn->js = js;
    conn->js_seqno = jsonrpc_session_get_seqno(conn->js);
}

/* Starts the local server in an existing Raft cluster, using the local copy of
 * the cluster's log in 'file_name'. */
struct ovsdb_error * OVS_WARN_UNUSED_RESULT
raft_open(const char *file_name, struct raft **raftp)
{
    struct raft *raft = raft_alloc(file_name);

    struct ovsdb_error *error = ovsdb_log_open(file_name, RAFT_MAGIC,
                                               OVSDB_LOG_READ_WRITE,
                                               -1, &raft->storage);
    if (error) {
        goto error;
    }

    raft->fsync_thread_running = true;
    raft->fsync_thread = ovs_thread_create("raft_fsync",
                                           raft_fsync_thread, raft);

    error = raft_read_header(raft);
    if (error) {
        goto error;
    }

    if (!raft->joining) {
        error = raft_read_log(raft);
        if (error) {
            goto error;
        }

        /* Find our own server.
         *
         * XXX It seems that this could fail if the server is restarted during
         * the process of removing it but before removal is committed, what to
         * do about that? */
        raft->me = raft_find_server__(&raft->servers, &raft->sid);
        if (!raft->me) {
            error = ovsdb_error(NULL, "server does not belong to cluster");
            goto error;
        }
    } else {
        raft->join_timeout = time_msec() + 1000;
        const char *remote;
        SSET_FOR_EACH (remote, &raft->remote_addresses) {
            raft_add_conn(raft, jsonrpc_session_open(remote, true));
        }
    }

    *raftp = raft;
    return NULL;

error:
    raft_close(raft);
    *raftp = NULL;
    return error;
}

bool
raft_is_joining(const struct raft *raft)
{
    return raft->joining;
}

/* If we're leader, try to transfer leadership to another server. */
void
raft_transfer_leadership(struct raft *raft)
{
    if (raft->role != RAFT_LEADER) {
        return;
    }

    struct raft_server *s;
    HMAP_FOR_EACH (s, hmap_node, &raft->servers) {
        if (!uuid_equals(&raft->sid, &s->sid)
            && jsonrpc_session_is_connected(s->js)
            && s->phase == RAFT_PHASE_STABLE) {
            union raft_rpc rpc = {
                .become_leader = {
                    .common = {
                        .type = RAFT_RPC_BECOME_LEADER,
                        .sid = s->sid,
                    },
                    .term = raft->current_term,
                }
            };
            raft_send__(raft, &rpc, s->js);
            break;
        }
    }
}

/* Send a RemoveServerRequest to the rest of the servers in the cluster.
 *
 * If we know which server is the leader, we can just send the request to it.
 * However, we might not know which server is the leader, and we might never
 * find out if the remove request was actually previously committed by a
 * majority of the servers (because in that case the new leader will not send
 * AppendRequests or heartbeats to us).  Therefore, we instead send
 * RemoveRequests to every server.  This theoretically has the same problem, if
 * the current cluster leader was not previously a member of the cluster, but
 * it seems likely to be more robust in practice.  */
static void
raft_send_remove_server_requests(struct raft *raft)
{
    VLOG_INFO("sending remove requests (joining=%s, leaving=%s)",
              raft->joining ? "true" : "false",
              raft->leaving ? "true" : "false");
    const struct raft_server *s;
    HMAP_FOR_EACH (s, hmap_node, &raft->servers) {
        if (s != raft->me) {
            union raft_rpc rpc = (union raft_rpc) {
                .server_request = {
                    .common = {
                        .type = RAFT_RPC_REMOVE_SERVER_REQUEST,
                        .sid = s->sid,
                    },
                    .sid = raft->sid,
                },
            };
            raft_send__(raft, &rpc, s->js);
        }
    }

    raft_reset_timer(raft);
}

void
raft_leave(struct raft *raft)
{
    ovs_assert(!raft->joining);
    if (raft->leaving) {
        return;
    }

    raft->leaving = true;
    raft_transfer_leadership(raft);
    raft_become_follower(raft);
    raft_send_remove_server_requests(raft);
    raft_reset_timer(raft);
}

bool
raft_is_leaving(const struct raft *raft)
{
    return raft->leaving;
}

bool
raft_left(const struct raft *raft)
{
    return raft->left;
}

void
raft_take_leadership(struct raft *raft)
{
    if (raft->role != RAFT_LEADER) {
        raft_start_election(raft);
    }
}

void
raft_close(struct raft *raft)
{
    if (!raft) {
        return;
    }

    raft_transfer_leadership(raft);
    raft_complete_all_commands(raft, RAFT_CMD_SHUTDOWN);

    ovs_mutex_lock(&raft->fsync_mutex);
    raft->fsync_next = UINT64_MAX;
    ovs_mutex_unlock(&raft->fsync_mutex);
    seq_change(raft->fsync_request);
    if (raft->fsync_thread_running) {
        xpthread_join(raft->fsync_thread, NULL);
    }

    ovsdb_log_close(raft->storage);

    raft_servers_destroy(&raft->servers);

    for (uint64_t index = raft->log_start; index < raft->log_end; index++) {
        struct raft_entry *e = &raft->log[index - raft->log_start];
        json_destroy(e->data);
    }
    free(raft->log);

    raft_servers_destroy(&raft->prev_servers);
    json_destroy(raft->snapshot);

    struct raft_conn *conn, *next;
    LIST_FOR_EACH_SAFE (conn, next, list_node, &raft->conns) {
        jsonrpc_session_close(conn->js);
        ovs_list_remove(&conn->list_node);
        free(conn);
    }

    raft_servers_destroy(&raft->add_servers);
    raft_server_destroy(raft->remove_server);

    sset_destroy(&raft->remote_addresses);
    free(raft->local_address);
    free(raft->name);
    free(raft->file_name);

    free(raft);
}

static bool
raft_receive_rpc(struct raft *raft, struct jsonrpc_session *js,
                 struct uuid *sid, union raft_rpc *rpc)
{
    struct jsonrpc_msg *msg = jsonrpc_session_recv(js);
    if (!msg) {
        return false;
    }

    struct ovsdb_error *error = raft_rpc_from_jsonrpc(raft, msg, rpc);
    if (error) {
        char *s = ovsdb_error_to_string(error);
        ovsdb_error_destroy(error);
        VLOG_INFO("%s: %s", jsonrpc_session_get_name(js), s);
        free(s);
        return false;
    }

    if (uuid_is_zero(sid)) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 5);
        *sid = rpc->common.sid;
        VLOG_INFO_RL(&rl, "%s: learned server ID %04x",
                     jsonrpc_session_get_name(js), uuid_prefix(sid, 4));
    } else if (!uuid_equals(sid, &rpc->common.sid)) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
        VLOG_WARN_RL(&rl, "%s: remote server ID changed from %04x to %04x",
                     jsonrpc_session_get_name(js),
                     uuid_prefix(sid, 4), uuid_prefix(&rpc->common.sid, 4));
    }

    return true;
}

static void
raft_send_add_server_request(struct raft *raft, struct jsonrpc_session *js)
{
    union raft_rpc rq = {
        .server_request = {
            .common = {
                .type = RAFT_RPC_ADD_SERVER_REQUEST,
                .sid = UUID_ZERO,
                .comment = NULL,
            },
            .sid = raft->sid,
            .address = raft->local_address,
        },
    };
    raft_send__(raft, &rq, js);
}

static void
raft_run_session(struct raft *raft,
                 struct jsonrpc_session *js, unsigned int *seqno,
                 struct uuid *sid)
{
    jsonrpc_session_run(js);

    bool just_connected = false;
    if (seqno) {
        unsigned int new_seqno = jsonrpc_session_get_seqno(js);
        if (new_seqno != *seqno && jsonrpc_session_is_connected(js)) {
            *seqno = new_seqno;
            just_connected = true;
        }
    }

    if (raft->joining
        && (just_connected || time_msec() >= raft->join_timeout)) {
        raft_send_add_server_request(raft, js);
    } else if (just_connected) {
        if (raft->leaving) {
            union raft_rpc rq = {
                .server_request = {
                    .common = {
                        .type = RAFT_RPC_REMOVE_SERVER_REQUEST,
                        .sid = *sid,
                    },
                    .sid = raft->sid,
                },
            };
            raft_send__(raft, &rq, js);
        } else {
            union raft_rpc rq = (union raft_rpc) {
                .common = {
                    .type = RAFT_RPC_HELLO_REQUEST,
                    .sid = *sid,
                },
            };
            raft_send__(raft, &rq, js);
        }
    }

    for (size_t i = 0; i < 50; i++) {
        union raft_rpc rpc;
        if (!raft_receive_rpc(raft, js, sid, &rpc)) {
            break;
        }

        if (!raft_rpc_is_heartbeat(&rpc)) {
            struct ds s = DS_EMPTY_INITIALIZER;
            raft_rpc_format(&rpc, &s);
            VLOG_INFO("<--%s", ds_cstr(&s));
            ds_destroy(&s);
        }

        raft_handle_rpc(raft, &rpc);
        raft_rpc_destroy(&rpc);
    }

    if (ovsdb_log_get_offset(raft->storage) == 0
        && !uuid_is_zero(&raft->cid)) {
        struct ovsdb_error *error = raft_write_header(raft->storage,
                                                      &raft->cid, &raft->sid);
        if (error) {
            /* XXX */
        }
    }
}

static void
raft_waiter_complete(struct raft *raft, struct raft_waiter *w)
{
    switch (w->type) {
    case RAFT_W_COMMAND:
        raft_update_match_index(raft, raft->me, w->command.index);
        break;

    case RAFT_W_APPEND:
        raft_send_append_reply(raft, w->append.rq, true, "log updated");
        break;

    case RAFT_W_VOTE:
        if (!uuid_is_zero(&raft->voted_for)
            && !uuid_equals(&raft->voted_for, &raft->sid)) {
            raft_send_vote_reply(raft, &raft->voted_for, true);
        }
        break;
    }
}

static void
raft_waiter_destroy(struct raft_waiter *w)
{
    if (!w) {
        return;
    }

    switch (w->type) {
    case RAFT_W_COMMAND:
        raft_command_unref(w->command.cmd);
        break;

    case RAFT_W_APPEND:
        free(w->append.rq);
        break;

    case RAFT_W_VOTE:
        break;
    }
    free(w);
}

static void
raft_waiters_run(struct raft *raft)
{
    if (ovs_list_is_empty(&raft->waiters)) {
        return;
    }

    ovs_mutex_lock(&raft->fsync_mutex);
    uint64_t cur = raft->fsync_cur;
    ovs_mutex_unlock(&raft->fsync_mutex);

    struct raft_waiter *w, *next;
    LIST_FOR_EACH_SAFE (w, next, list_node, &raft->waiters) {
        if (cur < w->fsync_seqno) {
            break;
        }
        raft_waiter_complete(raft, w);
        ovs_list_remove(&w->list_node);
        raft_waiter_destroy(w);
    }
}

static void
raft_waiters_wait(struct raft *raft)
{
    if (ovs_list_is_empty(&raft->waiters)) {
        return;
    }

    uint64_t complete = seq_read(raft->fsync_complete);

    ovs_mutex_lock(&raft->fsync_mutex);
    uint64_t cur = raft->fsync_cur;
    ovs_mutex_unlock(&raft->fsync_mutex);

    struct raft_waiter *w, *next;
    LIST_FOR_EACH_SAFE (w, next, list_node, &raft->waiters) {
        if (cur < w->fsync_seqno) {
            seq_wait(raft->fsync_complete, complete);
        } else {
            poll_immediate_wake();
        }
        break;
    }
}

static void
raft_set_term(struct raft *raft, uint64_t term, const struct uuid *vote)
{
    struct ovsdb_error *error = raft_write_state(raft->storage, term, vote);
    if (error) {
        /* XXX */
    }
    /* XXX need to commit before replying */
    raft->current_term = term;
    raft->voted_for = vote ? *vote : UUID_ZERO;
}

static void
raft_accept_vote(struct raft *raft, struct raft_server *s, bool granted)
{
    if (s->voted) {
        return;
    }
    s->voted = true;
    if (granted
        && ++raft->n_votes > hmap_count(&raft->servers) / 2) {
        raft_become_leader(raft);
    }
}

static void
raft_start_election(struct raft *raft)
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);

    ovs_assert(raft->role != RAFT_LEADER);
    ovs_assert(hmap_is_empty(&raft->commands));
    raft->role = RAFT_CANDIDATE;

    /* XXX what if we're not part of the server set? */

    raft_set_term(raft, raft->current_term + 1, &raft->sid);
    raft->n_votes = 0;

    if (!VLOG_DROP_INFO(&rl)) {
        long long int now = time_msec();
        if (now >= raft->election_timeout) {
            VLOG_INFO("term %"PRIu64": %lld ms timeout expired, "
                      "starting election",
                      raft->current_term, now - raft->election_base);
        } else {
            VLOG_INFO("term %"PRIu64": starting election", raft->current_term);
        }
    }
    raft_reset_timer(raft);

    struct raft_server *peer;
    HMAP_FOR_EACH (peer, hmap_node, &raft->servers) {
        peer->voted = false;
        if (peer == raft->me) {
            continue;
        }

        union raft_rpc rq = {
            .vote_request = {
                .common = {
                    .type = RAFT_RPC_VOTE_REQUEST,
                    .sid = peer->sid,
                },
                .term = raft->current_term,
                .last_log_index = raft->log_end - 1,
                .last_log_term = (
                    raft->log_end > raft->log_start
                    ? raft->log[raft->log_end - raft->log_start - 1].term
                    : raft->prev_term),
            },
        };
        raft_send(raft, &rq);
    }

    /* Vote for ourselves.
     * XXX only if we're not being removed? */
    raft_accept_vote(raft, raft->me, true);

    /* XXX how do we handle outstanding waiters? */
}

void
raft_run(struct raft *raft)
{
    if (raft->left) {
        return;
    }

    raft_waiters_run(raft);

    if (!raft->listener && time_msec() >= raft->listen_backoff) {
        char *paddr = raft_make_address_passive(raft->local_address);
        int error = pstream_open(paddr, &raft->listener, DSCP_DEFAULT);
        if (error) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
            VLOG_WARN_RL(&rl, "%s: listen failed (%s)",
                         paddr, ovs_strerror(error));
            raft->listen_backoff = time_msec() + 1000;
        }
        free(paddr);
    }

    if (raft->listener) {
        struct stream *stream;
        int error = pstream_accept(raft->listener, &stream);
        if (!error) {
            raft_add_conn(raft, jsonrpc_session_open_unreliably(
                              jsonrpc_open(stream), DSCP_DEFAULT));
        } else if (error != EAGAIN) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
            VLOG_WARN_RL(&rl, "%s: accept failed: %s",
                         pstream_get_name(raft->listener),
                         ovs_strerror(error));
        }
    }

    struct raft_server *s;
    HMAP_FOR_EACH (s, hmap_node, &raft->servers) {
        if (!s->js && !uuid_equals(&s->sid, &raft->sid)) {
            s->js = jsonrpc_session_open(s->address, true);
        }
        if (s->js) {
            raft_run_session(raft, s->js, &s->js_seqno, &s->sid);
        }
    }

    struct raft_conn *conn, *next;
    LIST_FOR_EACH_SAFE (conn, next, list_node, &raft->conns) {
        raft_run_session(raft, conn->js, &conn->js_seqno, &conn->sid);
        if (!jsonrpc_session_is_alive(conn->js)) {
            jsonrpc_session_close(conn->js);
            ovs_list_remove(&conn->list_node);
            free(conn);
        }
    }

    if (!raft->joining && time_msec() >= raft->election_timeout) {
        if (raft->leaving) {
            raft_send_remove_server_requests(raft);
        } else {
            raft_start_election(raft);
        }
    }

    if (raft->joining && time_msec() >= raft->join_timeout) {
        raft->join_timeout = time_msec() + 1000;
    }

    if (raft->role == RAFT_LEADER && time_msec() >= raft->ping_timeout) {
        /* XXX send only if idle */
        raft_send_heartbeats(raft);
    }
}

static void
raft_wait_session(struct jsonrpc_session *js)
{
    if (js) {
        jsonrpc_session_wait(js);
        jsonrpc_session_recv_wait(js);
    }
}

void
raft_wait(struct raft *raft)
{
    if (raft->left) {
        return;
    }

    raft_waiters_wait(raft);

    if (raft->listener) {
        pstream_wait(raft->listener);
    } else {
        poll_timer_wait_until(raft->listen_backoff);
    }

    struct raft_server *s;
    HMAP_FOR_EACH (s, hmap_node, &raft->servers) {
        raft_wait_session(s->js);
    }

    struct raft_conn *conn;
    LIST_FOR_EACH (conn, list_node, &raft->conns) {
        raft_wait_session(conn->js);
    }

    if (!raft->joining) {
        poll_timer_wait_until(raft->election_timeout);
    } else {
        poll_timer_wait_until(raft->join_timeout);
    }
    if (raft->role == RAFT_LEADER) {
        poll_timer_wait_until(raft->ping_timeout);
    }
}

static struct raft_waiter *
raft_waiter_create(struct raft *raft, enum raft_waiter_type type)
{
    ovs_mutex_lock(&raft->fsync_mutex);
    uint64_t seqno = ++raft->fsync_next;
    ovs_mutex_unlock(&raft->fsync_mutex);

    seq_change(raft->fsync_request);

    struct raft_waiter *w = xzalloc(sizeof *w);
    ovs_list_push_back(&raft->waiters, &w->list_node);
    w->fsync_seqno = seqno;
    w->type = type;
    return w;
}

const char *
raft_command_status_to_string(enum raft_command_status status)
{
    switch (status) {
    case RAFT_CMD_INCOMPLETE:
        return "operation still in progress";
    case RAFT_CMD_SUCCESS:
        return "success";
    case RAFT_CMD_NOT_LEADER:
        return "not leader";
    case RAFT_CMD_LOST_LEADERSHIP:
        return "lost leadership";
    case RAFT_CMD_SHUTDOWN:
        return "server shutdown";
    default:
        OVS_NOT_REACHED();
    }
}

static struct raft_command * OVS_WARN_UNUSED_RESULT
raft_command_execute__(struct raft *raft, enum raft_entry_type type,
                       const struct json *data)
{
    struct raft_command *cmd = xzalloc(sizeof *cmd);
    cmd->n_refs = 2;            /* One for client, one for raft->commands. */
    cmd->index = raft->log_end;
    cmd->status = RAFT_CMD_INCOMPLETE;
    hmap_insert(&raft->commands, &cmd->hmap_node, cmd->index);

    if (raft->role != RAFT_LEADER) {
        raft_command_complete(raft, cmd, RAFT_CMD_NOT_LEADER);
        return cmd;
    }

    /* Write to local log.
     *
     * XXX If this server is being removed from the configuration then we
     * should not write to the local log; see section 4.2.2.  Or we could
     * implement leadership transfer. */
    struct ovsdb_error *error = raft_write_entry(raft, raft->current_term,
                                                 type, json_clone(data));
    if (!error) {
        struct raft_waiter *w = raft_waiter_create(raft, RAFT_W_COMMAND);
        w->command.index = cmd->index;
    } else {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 5);
        char *s = ovsdb_error_to_string(error);
        VLOG_WARN_RL(&rl, "%s", s);
        free(s);
        ovsdb_error_destroy(error);

        /* XXX make this a hard failure if cluster has <=2 servers. */
    }

    /* Write to remote logs. */
    struct raft_server *s;
    HMAP_FOR_EACH (s, hmap_node, &raft->servers) {
        if (s != raft->me && s->next_index == raft->log_end - 1) {
            raft_send_append_request(raft, s, 1);
            s->next_index++;    /* XXX Is this a valid way to pipeline? */
        }
    }

    return cmd;
}

struct raft_command * OVS_WARN_UNUSED_RESULT
raft_command_execute(struct raft *raft, const struct json *data)
{
    return raft_command_execute__(raft, RAFT_DATA, data);
}

enum raft_command_status
raft_command_get_status(const struct raft_command *cmd)
{
    ovs_assert(cmd->n_refs > 0);
    return cmd->status;
}

void
raft_command_unref(struct raft_command *cmd)
{
    if (cmd) {
        ovs_assert(cmd->n_refs > 0);
        if (!--cmd->n_refs) {
            free(cmd);
        }
    }
}

void
raft_command_wait(const struct raft_command *cmd)
{
    if (cmd->status != RAFT_CMD_INCOMPLETE) {
        poll_immediate_wake();
    }
}

static void
raft_command_complete(struct raft *raft,
                      struct raft_command *cmd,
                      enum raft_command_status status)
{
    ovs_assert(cmd->status == RAFT_CMD_INCOMPLETE);
    ovs_assert(cmd->n_refs > 0);
    hmap_remove(&raft->commands, &cmd->hmap_node);
    cmd->status = status;
    raft_command_unref(cmd);
}

static void
raft_complete_all_commands(struct raft *raft, enum raft_command_status status)
{
    struct raft_command *cmd, *next;
    HMAP_FOR_EACH_SAFE (cmd, next, hmap_node, &raft->commands) {
        raft_command_complete(raft, cmd, status);
    }
}

static struct raft_command *
raft_find_command(struct raft *raft, uint64_t index)
{
    struct raft_command *cmd;

    HMAP_FOR_EACH_IN_BUCKET (cmd, hmap_node, index, &raft->commands) {
        if (cmd->index == index) {
            return cmd;
        }
    }
    return NULL;
}

static void
raft_rpc_destroy(union raft_rpc *rpc)
{
    if (!rpc) {
        return;
    }

    free(rpc->common.comment);

    switch (rpc->common.type) {
    case RAFT_RPC_HELLO_REQUEST:
        break;
    case RAFT_RPC_APPEND_REQUEST:
        for (size_t i = 0; i < rpc->append_request.n_entries; i++) {
            json_destroy(rpc->append_request.entries[i].data);
        }
        free(rpc->append_request.entries);
        break;
    case RAFT_RPC_APPEND_REPLY:
    case RAFT_RPC_VOTE_REQUEST:
    case RAFT_RPC_VOTE_REPLY:
        break;
    case RAFT_RPC_ADD_SERVER_REQUEST:
    case RAFT_RPC_REMOVE_SERVER_REQUEST:
        free(rpc->server_request.address);
        break;
    case RAFT_RPC_ADD_SERVER_REPLY:
    case RAFT_RPC_REMOVE_SERVER_REPLY:
        free(rpc->server_reply.leader_address);
        break;
    case RAFT_RPC_INSTALL_SNAPSHOT_REQUEST:
        raft_servers_destroy(&rpc->install_snapshot_request.last_servers);
        json_destroy(rpc->install_snapshot_request.data);
        break;
    case RAFT_RPC_INSTALL_SNAPSHOT_REPLY:
        break;
    case RAFT_RPC_BECOME_LEADER:
        break;
    }
}

/* raft_rpc_to/from_jsonrpc(). */

static void
raft_append_request_to_jsonrpc(const struct raft_append_request *rq,
                               struct json *args)
{
    json_object_put_uint(args, "term", rq->term);
    json_object_put_uint(args, "prev_log_index", rq->prev_log_index);
    json_object_put_uint(args, "prev_log_term", rq->prev_log_term);
    json_object_put_uint(args, "leader_commit", rq->leader_commit);

    struct json **entries = xmalloc(rq->n_entries * sizeof *entries);
    for (size_t i = 0; i < rq->n_entries; i++) {
        entries[i] = raft_entry_to_json(&rq->entries[i]);
    }
    json_object_put(args, "log", json_array_create(entries, rq->n_entries));
}

static void
raft_append_request_from_jsonrpc(struct ovsdb_parser *p,
                                 struct raft_append_request *rq)
{
    rq->term = parse_uint(p, "term");
    rq->prev_log_index = parse_uint(p, "prev_log_index");
    rq->prev_log_term = parse_uint(p, "prev_log_term");
    rq->leader_commit = parse_uint(p, "leader_commit");

    const struct json *log = ovsdb_parser_member(p, "log", OP_ARRAY);
    if (!log) {
        return;
    }
    const struct json_array *entries = json_array(log);
    rq->entries = xmalloc(entries->n * sizeof *rq->entries);
    rq->n_entries = 0;
    for (size_t i = 0; i < entries->n; i++) {
        struct ovsdb_error *error = raft_entry_from_json(entries->elems[i],
                                                         &rq->entries[i]);
        if (error) {
            ovsdb_parser_put_error(p, error);
            break;
        }
        rq->n_entries++;
    }
}

static void
raft_append_reply_to_jsonrpc(const struct raft_append_reply *rpy,
                             struct json *args)
{
    json_object_put_uint(args, "term", rpy->term);
    json_object_put_uint(args, "log_end", rpy->log_end);
    json_object_put_uint(args, "prev_log_index", rpy->prev_log_index);
    json_object_put_uint(args, "prev_log_term", rpy->prev_log_term);
    json_object_put_uint(args, "n_entries", rpy->n_entries);
    json_object_put(args, "success", json_boolean_create(rpy->success));
}

static void
raft_append_reply_from_jsonrpc(struct ovsdb_parser *p,
                               struct raft_append_reply *rpy)
{
    rpy->term = parse_uint(p, "term");
    rpy->log_end = parse_uint(p, "log_end");
    rpy->prev_log_index = parse_uint(p, "prev_log_index");
    rpy->prev_log_term = parse_uint(p, "prev_log_term");
    rpy->n_entries = parse_uint(p, "n_entries");
    rpy->success = parse_boolean(p, "success");
}

static void
raft_vote_request_to_jsonrpc(const struct raft_vote_request *rq,
                             struct json *args)
{
    json_object_put_uint(args, "term", rq->term);
    json_object_put_uint(args, "last_log_index", rq->last_log_index);
    json_object_put_uint(args, "last_log_term", rq->last_log_term);
}

static void
raft_vote_request_from_jsonrpc(struct ovsdb_parser *p,
                               struct raft_vote_request *rq)
{
    rq->term = parse_uint(p, "term");
    rq->last_log_index = parse_uint(p, "last_log_index");
    rq->last_log_term = parse_uint(p, "last_log_term");
}

static void
raft_vote_reply_to_jsonrpc(const struct raft_vote_reply *rpy,
                           struct json *args)
{
    json_object_put_uint(args, "term", rpy->term);
    json_object_put(args, "vote_granted",
                    json_boolean_create(rpy->vote_granted));
}

static void
raft_vote_reply_from_jsonrpc(struct ovsdb_parser *p,
                             struct raft_vote_reply *rpy)
{
    rpy->term = parse_uint(p, "term");
    rpy->vote_granted = parse_boolean(p, "vote_granted");
}

static void
raft_server_request_to_jsonrpc(const struct raft_server_request *rq,
                               struct json *args)
{
    json_object_put_format(args, "server_id", UUID_FMT, UUID_ARGS(&rq->sid));
    if (rq->address) {
        json_object_put_string(args, "address", rq->address);
    }
}

static void
raft_server_request_from_jsonrpc(struct ovsdb_parser *p,
                                 struct raft_server_request *rq)
{
    rq->sid = parse_required_uuid(p, "server_id");
    if (rq->common.type == RAFT_RPC_ADD_SERVER_REQUEST) {
        const struct json *json = ovsdb_parser_member(p, "address", OP_STRING);
        if (json) {
            rq->address = xstrdup(json_string(json));
        }
    }
}

static void
raft_server_reply_to_jsonrpc(const struct raft_server_reply *rpy,
                             struct json *args)
{
    json_object_put_string(args, "status",
                           raft_server_status_to_string(rpy->status));
    if (rpy->leader_address) {
        json_object_put_string(args, "leader_address", rpy->leader_address);
        json_object_put_format(args, "leader", UUID_FMT,
                               UUID_ARGS(&rpy->leader_sid));
    }
}

static void
raft_server_reply_from_jsonrpc(struct ovsdb_parser *p,
                               struct raft_server_reply *rpy)
{
    const char *status = parse_required_string(p, "status");
    if (status && !raft_server_status_from_string(status, &rpy->status)) {
        ovsdb_parser_raise_error(p, "unknown server status \"%s\"",
                                 status);
    }

    const char *leader_address = parse_optional_string(p, "leader_address");
    rpy->leader_address = leader_address ? xstrdup(leader_address) : NULL;
    if (rpy->leader_address) {
        rpy->leader_sid = parse_required_uuid(p, "leader");
    }
}

static void
raft_install_snapshot_request_to_jsonrpc(
    const struct raft_install_snapshot_request *rq, struct json *args)
{
    json_object_put_uint(args, "term", rq->term);
    json_object_put_uint(args, "last_index", rq->last_index);
    json_object_put_uint(args, "last_term", rq->last_term);
    json_object_put(args, "last_servers",
                    raft_servers_to_json(&rq->last_servers));

    json_object_put(args, "data", json_clone(rq->data));
}

static void
raft_install_snapshot_request_from_jsonrpc(
    struct ovsdb_parser *p, struct raft_install_snapshot_request *rq)
{
    const struct json *last_servers = ovsdb_parser_member(p, "last_servers",
                                                          OP_OBJECT);
    struct ovsdb_error *error = raft_servers_from_json(last_servers,
                                                       &rq->last_servers);
    if (error) {
        ovsdb_parser_put_error(p, error);
        return;
    }

    rq->term = parse_uint(p, "term");
    rq->last_index = parse_uint(p, "last_index");
    rq->last_term = parse_uint(p, "last_term");

    const struct json *data = ovsdb_parser_member(p, "data", OP_ANY);
    if (data) {
        rq->data = json_clone(data);
    }
}

static void
raft_install_snapshot_reply_to_jsonrpc(
    const struct raft_install_snapshot_reply *rpy, struct json *args)
{
    json_object_put_uint(args, "term", rpy->term);
    json_object_put_uint(args, "last_index", rpy->last_index);
    json_object_put_uint(args, "last_term", rpy->last_term);
}

static void
raft_install_snapshot_reply_from_jsonrpc(
    struct ovsdb_parser *p,
    struct raft_install_snapshot_reply *rpy)
{
    rpy->term = parse_uint(p, "term");
    rpy->last_index = parse_uint(p, "last_index");
    rpy->last_term = parse_uint(p, "last_term");
}

static struct jsonrpc_msg *
raft_rpc_to_jsonrpc(const struct raft *raft,
                    const union raft_rpc *rpc)
{
    struct json *args = json_object_create();
    if (!uuid_is_zero(&raft->cid)) {
        json_object_put_format(args, "cluster", UUID_FMT,
                               UUID_ARGS(&raft->cid));
    }
    if (!uuid_is_zero(&rpc->common.sid)) {
        json_object_put_format(args, "to", UUID_FMT,
                               UUID_ARGS(&rpc->common.sid));
    }
    json_object_put_format(args, "from", UUID_FMT, UUID_ARGS(&raft->sid));
    if (rpc->common.comment) {
        json_object_put_string(args, "comment", rpc->common.comment);
    }

    switch (rpc->common.type) {
    case RAFT_RPC_HELLO_REQUEST:
        break;
    case RAFT_RPC_APPEND_REQUEST:
        raft_append_request_to_jsonrpc(&rpc->append_request,
                                       args);
        break;
    case RAFT_RPC_APPEND_REPLY:
        raft_append_reply_to_jsonrpc(&rpc->append_reply, args);
        break;
    case RAFT_RPC_VOTE_REQUEST:
        raft_vote_request_to_jsonrpc(&rpc->vote_request, args);
        break;
    case RAFT_RPC_VOTE_REPLY:
        raft_vote_reply_to_jsonrpc(&rpc->vote_reply, args);
        break;
    case RAFT_RPC_ADD_SERVER_REQUEST:
        raft_server_request_to_jsonrpc(&rpc->server_request, args);
        break;
    case RAFT_RPC_ADD_SERVER_REPLY:
        raft_server_reply_to_jsonrpc(&rpc->server_reply, args);
        break;
    case RAFT_RPC_REMOVE_SERVER_REQUEST:
        raft_server_request_to_jsonrpc(&rpc->server_request, args);
        break;
    case RAFT_RPC_REMOVE_SERVER_REPLY:
        raft_server_reply_to_jsonrpc(&rpc->server_reply, args);
        break;
    case RAFT_RPC_INSTALL_SNAPSHOT_REQUEST:
        raft_install_snapshot_request_to_jsonrpc(
            &rpc->install_snapshot_request, args);
        break;
    case RAFT_RPC_INSTALL_SNAPSHOT_REPLY:
        raft_install_snapshot_reply_to_jsonrpc(
            &rpc->install_snapshot_reply, args);
        break;
    case RAFT_RPC_BECOME_LEADER:
        json_object_put_uint(args, "term", rpc->become_leader.term);
        break;
    default:
        OVS_NOT_REACHED();
    }

    return jsonrpc_create_notify(raft_rpc_type_to_string(rpc->common.type),
                                 json_array_create_1(args));
}

static struct ovsdb_error * OVS_WARN_UNUSED_RESULT
raft_rpc_from_jsonrpc(struct raft *raft,
                      const struct jsonrpc_msg *msg, union raft_rpc *rpc)
{
    memset(rpc, 0, sizeof *rpc);
    if (msg->type != JSONRPC_NOTIFY) {
        return ovsdb_error(NULL, "expecting notify RPC but received %s",
                           jsonrpc_msg_type_to_string(msg->type));
    }

    if (!raft_rpc_type_from_string(msg->method, &rpc->common.type)) {
        return ovsdb_error(NULL, "unknown method %s", msg->method);
    }

    if (json_array(msg->params)->n != 1) {
        return ovsdb_error(NULL,
                           "%s RPC has %"PRIuSIZE" parameters (expected 1)",
                           msg->method, json_array(msg->params)->n);
    }

    struct ovsdb_parser p;
    ovsdb_parser_init(&p, json_array(msg->params)->elems[0],
                      "raft %s RPC", msg->method);

    bool is_hello = rpc->common.type == RAFT_RPC_HELLO_REQUEST;
    bool is_add = rpc->common.type == RAFT_RPC_ADD_SERVER_REQUEST;

    struct uuid cid;
    if (parse_uuid__(&p, "cluster", is_add, &cid)
        && !uuid_equals(&cid, &raft->cid)) {
        if (uuid_is_zero(&raft->cid)) {
            raft->cid = cid;
            VLOG_INFO("learned cluster ID %04x", uuid_prefix(&cid, 4));
        } else {
            ovsdb_parser_raise_error(&p, "wrong cluster %04x (expected %04x)",
                                     uuid_prefix(&cid, 4),
                                     uuid_prefix(&raft->cid, 4));
        }
    }

    struct uuid to_sid;
    if (parse_uuid__(&p, "to", is_add || is_hello, &to_sid)
        && !uuid_equals(&to_sid, &raft->sid)) {
        ovsdb_parser_raise_error(&p, "misrouted message (addressed to %04x "
                                 " but we're %04x)",
                                 uuid_prefix(&to_sid, 4),
                                 uuid_prefix(&raft->sid, 4));
    }

    rpc->common.sid = parse_required_uuid(&p, "from");
    rpc->common.comment = nullable_xstrdup(
        parse_optional_string(&p, "comment"));

    switch (rpc->common.type) {
    case RAFT_RPC_HELLO_REQUEST:
        break;
    case RAFT_RPC_APPEND_REQUEST:
        raft_append_request_from_jsonrpc(&p, &rpc->append_request);
        break;
    case RAFT_RPC_APPEND_REPLY:
        raft_append_reply_from_jsonrpc(&p, &rpc->append_reply);
        break;
    case RAFT_RPC_VOTE_REQUEST:
        raft_vote_request_from_jsonrpc(&p, &rpc->vote_request);
        break;
    case RAFT_RPC_VOTE_REPLY:
        raft_vote_reply_from_jsonrpc(&p, &rpc->vote_reply);
        break;
    case RAFT_RPC_ADD_SERVER_REQUEST:
        raft_server_request_from_jsonrpc(&p, &rpc->server_request);
        break;
    case RAFT_RPC_ADD_SERVER_REPLY:
        raft_server_reply_from_jsonrpc(&p, &rpc->server_reply);
        break;
    case RAFT_RPC_REMOVE_SERVER_REQUEST:
        raft_server_request_from_jsonrpc(&p, &rpc->server_request);
        break;
    case RAFT_RPC_REMOVE_SERVER_REPLY:
        raft_server_reply_from_jsonrpc(&p, &rpc->server_reply);
        break;
    case RAFT_RPC_INSTALL_SNAPSHOT_REQUEST:
        raft_install_snapshot_request_from_jsonrpc(
            &p, &rpc->install_snapshot_request);
        break;
    case RAFT_RPC_INSTALL_SNAPSHOT_REPLY:
        raft_install_snapshot_reply_from_jsonrpc(
            &p, &rpc->install_snapshot_reply);
        break;
    case RAFT_RPC_BECOME_LEADER:
        rpc->become_leader.term = parse_uint(&p, "term");
        break;
    default:
        OVS_NOT_REACHED();
    }

    struct ovsdb_error *error = ovsdb_parser_finish(&p);
    if (error) {
        raft_rpc_destroy(rpc);
    }
    return error;
}

static void
raft_send_server_reply(struct raft *raft, bool add,
                       const struct uuid *sid, enum raft_server_status status)
{
    const struct raft_server *leader = raft_find_server(raft,
                                                        &raft->leader_sid);
    union raft_rpc rpy = {
        .server_reply = {
            .common = {
                .type = (add
                         ? RAFT_RPC_ADD_SERVER_REPLY
                         : RAFT_RPC_REMOVE_SERVER_REPLY),
                .sid = *sid,
            },
            .status = status,

            .leader_address = leader ? leader->address : NULL,
            .leader_sid = leader ? leader->sid : UUID_ZERO,
        }
    };
    raft_send(raft, &rpy);
}

static void
raft_send_add_server_reply(struct raft *raft, const struct uuid *sid,
                           enum raft_server_status status)
{
    return raft_send_server_reply(raft, true, sid, status);
}

static void
raft_send_remove_server_reply(struct raft *raft, const struct uuid *sid,
                              enum raft_server_status status)
{
    return raft_send_server_reply(raft, false, sid, status);
}

static void
raft_become_follower(struct raft *raft)
{
    raft->leader_sid = UUID_ZERO;
    if (raft->role == RAFT_FOLLOWER) {
        return;
    }

    raft->role = RAFT_FOLLOWER;
    raft_reset_timer(raft);

    /* Notify clients about lost leadership.
     *
     * We do not reverse our changes to 'raft->servers' because the new
     * configuration is already part of the log.  Possibly the configuration
     * log entry will not be committed, but until we know that we must use the
     * new configuration.  Our AppendEntries processing will properly update
     * the server configuration later, if necessary. */
    struct raft_server *s;
    HMAP_FOR_EACH (s, hmap_node, &raft->add_servers) {
        raft_send_add_server_reply(raft, &s->sid, RAFT_SERVER_LOST_LEADERSHIP);
    }
    if (raft->remove_server) {
        raft_send_remove_server_reply(raft, &raft->remove_server->reply_sid,
                                      RAFT_SERVER_LOST_LEADERSHIP);
        raft_server_destroy(raft->remove_server);
        raft->remove_server = NULL;
    }

    /* XXX how do we handle outstanding waiters? */
    raft_complete_all_commands(raft, RAFT_CMD_LOST_LEADERSHIP);
}

static void
raft_send_append_request(struct raft *raft,
                         struct raft_server *peer, unsigned int n)
{
    ovs_assert(raft->role == RAFT_LEADER);

    union raft_rpc rq = {
        .append_request = {
            .common = {
                .type = RAFT_RPC_APPEND_REQUEST,
                .sid = peer->sid,
                .comment = "heartbeat",
            },
            .term = raft->current_term,
            .prev_log_index = peer->next_index - 1,
            .prev_log_term = (peer->next_index - 1 >= raft->log_start
                              ? raft->log[peer->next_index - 1
                                          - raft->log_start].term
                              : raft->prev_term),
            .leader_commit = raft->commit_index,
            .entries = &raft->log[peer->next_index - raft->log_start],
            .n_entries = n,
        },
    };
    raft_send(raft, &rq);
}

static void
raft_send_heartbeats(struct raft *raft)
{
    struct raft_server *s;
    HMAP_FOR_EACH (s, hmap_node, &raft->servers) {
        if (s != raft->me) {
            /* XXX should also retransmit unacknowledged append requests */
            raft_send_append_request(raft, s, 0);
        }
    }
    raft->ping_timeout = time_msec() + PING_TIME_MSEC;
}

static void
raft_server_init_leader(struct raft *raft, struct raft_server *s)
{
    s->next_index = raft->log_end;
    s->match_index = 0;
    s->phase = RAFT_PHASE_STABLE;
}

static void
raft_become_leader(struct raft *raft)
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
    VLOG_INFO_RL(&rl, "term %"PRIu64": elected leader by %d+ of "
                 "%"PRIuSIZE" servers", raft->current_term,
                 raft->n_votes, hmap_count(&raft->servers));

    ovs_assert(raft->role != RAFT_LEADER);
    raft->role = RAFT_LEADER;
    raft->leader_sid = raft->sid;
    raft->election_timeout = LLONG_MAX;
    raft->ping_timeout = time_msec() + PING_TIME_MSEC;

    struct raft_server *s;
    HMAP_FOR_EACH (s, hmap_node, &raft->servers) {
        raft_server_init_leader(raft, s);
    }

    raft_send_heartbeats(raft);
}

/* Processes term 'term' received as part of RPC 'common'.  Returns true if the
 * caller should continue processing the RPC, false if the caller should reject
 * it due to a stale term. */
static bool
raft_receive_term__(struct raft *raft, const struct raft_rpc_common *common,
                    uint64_t term)
{
    /* Section 3.3 says:
     *
     *     Current terms are exchanged whenever servers communicate; if one
     *     server’s current term is smaller than the other’s, then it updates
     *     its current term to the larger value.  If a candidate or leader
     *     discovers that its term is out of date, it immediately reverts to
     *     follower state.  If a server receives a request with a stale term
     *     number, it rejects the request.
     */
    if (term > raft->current_term) {
        raft_set_term(raft, term, NULL);
        raft_become_follower(raft);
    } else if (term < raft->current_term) {
        VLOG_INFO("rejecting term %"PRIu64" < current term %"PRIu64" received "
                  "in %s message from server %04x",
                  term, raft->current_term,
                  raft_rpc_type_to_string(common->type),
                  uuid_prefix(&common->sid, 4));
        return false;
    }
    return true;
}

static void
raft_get_servers_from_log(struct raft *raft)
{
    for (uint64_t index = raft->log_end - 1; index >= raft->log_start;
         index--) {
        struct raft_entry *e = &raft->log[index - raft->log_start];
        if (e->type == RAFT_SERVERS) {
            struct hmap servers;
            struct ovsdb_error *error = raft_servers_from_json(e->data,
                                                               &servers);
            ovs_assert(!error);
            raft_set_servers(raft, &servers, VLL_INFO);
            raft_servers_destroy(&servers);
            return;
        }
    }
    raft_set_servers(raft, &raft->prev_servers, VLL_INFO);
}

/* Truncates the log, so that raft->log_end becomes 'new_end'.
 *
 * Doesn't write anything to disk.
 *
 * Returns true if any of the removed log entries were server configuration
 * entries, false otherwise. */
static bool
raft_truncate(struct raft *raft, uint64_t new_end)
{
    ovs_assert(new_end >= raft->log_start);

    bool servers_changed = false;
    while (raft->log_end > new_end) {
        struct raft_entry *entry = &raft->log[--raft->log_end
                                              - raft->log_start];
        if (entry->type == RAFT_SERVERS) {
            servers_changed = true;
        }
        json_destroy(entry->data);
    }
    return servers_changed;
}

static const struct raft_entry *
raft_peek_next_entry(struct raft *raft, enum raft_entry_type type)
{
    if (raft->commit_index > raft->last_applied
        && raft->last_applied + 1 >= raft->log_start) {
        const struct raft_entry *e = raft_get_entry(raft, raft->last_applied + 1);
        if (e->type == type) {
            return e;
        }
    }
    return NULL;
}

static const struct raft_entry *
raft_get_next_entry(struct raft *raft, enum raft_entry_type type)
{
    const struct raft_entry *e = raft_peek_next_entry(raft, type);
    if (e) {
        raft->last_applied++;
    }
    return e;
}

static void
raft_commit_servers(struct raft *raft)
{
    for (;;) {
        const struct raft_entry *e = raft_get_next_entry(raft, RAFT_SERVERS);
        if (!e) {
            break;
        }
#if 0
        VLOG_INFO("applying log index %"PRIu64": servers \"%s\"",
                  raft->last_applied, e->data);
#endif
        if (raft->role == RAFT_LEADER) {
            raft_run_reconfigure(raft);
        }
    }
}

static void
raft_update_commit_index(struct raft *raft, uint64_t new_commit_index)
{
    ovs_assert(new_commit_index >= raft->commit_index);
    raft->commit_index = new_commit_index;

    raft_commit_servers(raft);
}

/* This doesn't use rq->entries (but it does use rq->n_entries). */
static void
raft_send_append_reply(struct raft *raft, const struct raft_append_request *rq,
                       bool success, const char *comment)
{
    /* Figure 3.1: "If leaderCommit > commitIndex, set commitIndex =
     * min(leaderCommit, index of last new entry)" */
    if (success && rq->leader_commit > raft->commit_index) {
        raft_update_commit_index(
            raft, MIN(rq->leader_commit, rq->prev_log_index + rq->n_entries));
    }

    /* Send reply. */
    union raft_rpc reply = {
        .append_reply = {
            .common = {
                .type = RAFT_RPC_APPEND_REPLY,
                .sid = rq->common.sid,
                .comment = CONST_CAST(char *, comment),
            },
            .term = raft->current_term,
            .log_end = raft->log_end,
            .prev_log_index = rq->prev_log_index,
            .prev_log_term = rq->prev_log_term,
            .n_entries = rq->n_entries,
            .success = success,
        }
    };
    raft_send(raft, &reply);
}

/* If 'prev_log_index' exists in 'raft''s log, term 'prev_log_term', returns
 * NULL.  Otherwise, returns an explanation for the mismatch.  */
static const char *
match_index_and_term(const struct raft *raft,
                     uint64_t prev_log_index, uint64_t prev_log_term)
{
    if (prev_log_index < raft->log_start - 1) {
        return "mismatch before start of log";
    } else if (prev_log_index == raft->log_start - 1) {
        if (prev_log_term != raft->prev_term) {
            return "prev_term mismatch";
        }
    } else if (prev_log_index < raft->log_end) {
        if (raft->log[prev_log_index - raft->log_start].term
            != prev_log_term) {
            return "term mismatch";
        }
    } else {
        /* prev_log_index >= raft->log_end */
        return "mismatch past end of log";
    }
    return NULL;
}

/* Returns NULL on success, RAFT_IN_PROGRESS for an operation in progress,
 * otherwise a brief comment explaining failure. */
static void
raft_handle_append_entries(struct raft *raft,
                           const struct raft_append_request *rq,
                           uint64_t prev_log_index, uint64_t prev_log_term,
                           const struct raft_entry *entries,
                           unsigned int n_entries)
{
    /* Section 3.5: "When sending an AppendEntries RPC, the leader includes
     * the index and term of the entry in its log that immediately precedes
     * the new entries. If the follower does not find an entry in its log
     * with the same index and term, then it refuses the new entries." */
    const char *mismatch = match_index_and_term(raft, prev_log_index,
                                                prev_log_term);
    if (mismatch) {
        VLOG_INFO("rejecting append_request because previous entry "
                  "%"PRIu64",%"PRIu64" not in local log (%s)",
                  prev_log_term, prev_log_index, mismatch);
        raft_send_append_reply(raft, rq, false, mismatch);
        return;
    }

    /* Figure 3.1: "If an existing entry conflicts with a new one (same
     * index but different terms), delete the existing entry and all that
     * follow it." */
    unsigned int i;
    bool servers_changed = false;
    for (i = 0; ; i++) {
        if (i >= n_entries) {
            /* No change. */
            if (rq->common.comment
                && !strcmp(rq->common.comment, "heartbeat")) {
                raft_send_append_reply(raft, rq, true, "heartbeat");
            } else {
                raft_send_append_reply(raft, rq, true, "no change");
            }
            return;
        }

        uint64_t log_index = (prev_log_index + 1) + i;
        if (log_index >= raft->log_end) {
            break;
        }
        if (raft->log[log_index - raft->log_start].term != entries[i].term) {
            if (raft_truncate(raft, log_index)) {
                servers_changed = true;
            }
            break;
        }
    }

    /* Figure 3.1: "Append any entries not already in the log." */
    struct ovsdb_error *error = NULL;
    for (; i < n_entries; i++) {
        const struct raft_entry *entry = &entries[i];
        error = raft_write_entry(raft, entry->term, entry->type,
                                 json_clone(entry->data));
        if (error) {
            break;
        }
        if (entry->type == RAFT_SERVERS) {
            servers_changed = true;
        }
    }

    if (servers_changed) {
        raft_get_servers_from_log(raft);
    }

    if (error) {
        char *s = ovsdb_error_to_string(error);
        VLOG_ERR("%s", s);
        free(s);
        ovsdb_error_destroy(error);
        raft_send_append_reply(raft, rq, false, "I/O error");
        return;
    }

    struct raft_waiter *w = raft_waiter_create(raft, RAFT_W_APPEND);
    w->append.rq = xmemdup(rq, sizeof *rq);
    w->append.rq->entries = NULL;
    /* Reply will be sent later following waiter completion. */
}

static bool
raft_update_leader(struct raft *raft, const struct uuid *sid)
{
    if (raft->role == RAFT_LEADER && !uuid_equals(sid, &raft->sid)) {
        VLOG_ERR("this server is leader but server %04x claims to be",
                 uuid_prefix(sid, 4));
        return false;
    } else if (!uuid_equals(sid, &raft->leader_sid)) {
        if (!uuid_is_zero(&raft->leader_sid)) {
            VLOG_ERR("leader for term %"PRIu64" changed "
                     "from %04x to %04x",
                     raft->current_term,
                     uuid_prefix(&raft->leader_sid, 4),
                     uuid_prefix(sid, 4));
        } else {
            VLOG_INFO("server %04x is leader for term %"PRIu64,
                      uuid_prefix(sid, 4), raft->current_term);
        }
        raft->leader_sid = *sid;
    }
    return true;
}

static void
raft_handle_append_request__(struct raft *raft,
                             const struct raft_append_request *rq)
{
    if (!raft_receive_term__(raft, &rq->common, rq->term)) {
        /* Section 3.3: "If a server receives a request with a stale term
         * number, it rejects the request." */
        raft_send_append_reply(raft, rq, false, "stale term");
        return;
    }

    /* We do not check whether the server that sent the request is part of the
     * cluster.  As section 4.1 says, "A server accepts AppendEntries requests
     * from a leader that is not part of the server’s latest configuration.
     * Otherwise, a new server could never be added to the cluster (it would
     * never accept any log entries preceding the configuration entry that adds
     * the server)." */
    if (!raft_update_leader(raft, &rq->common.sid)) {
        raft_send_append_reply(raft, rq, false, "usurpsed leadership");
        return;
    }
    raft_reset_timer(raft);

    /* First check for the common case, where the AppendEntries request is
     * entirely for indexes covered by 'log_start' ... 'log_end - 1', something
     * like this:
     *
     *     rq->prev_log_index
     *       | first_entry_index
     *       |   |         nth_entry_index
     *       |   |           |
     *       v   v           v
     *         +---+---+---+---+
     *       T | T | T | T | T |
     *         +---+-------+---+
     *     +---+---+---+---+
     *   T | T | T | T | T |
     *     +---+---+---+---+
     *       ^               ^
     *       |               |
     *   log_start        log_end
     * */
    uint64_t first_entry_index = rq->prev_log_index + 1;
    uint64_t nth_entry_index = rq->prev_log_index + rq->n_entries;
    if (OVS_LIKELY(first_entry_index >= raft->log_start)) {
        raft_handle_append_entries(raft, rq,
                                   rq->prev_log_index, rq->prev_log_term,
                                   rq->entries, rq->n_entries);
        return;
    }

    /* Now a series of checks for odd cases, where the AppendEntries request
     * extends earlier than the beginning of our log, into the log entries
     * discarded by the most recent snapshot. */

    /*
     * Handle the case where the indexes covered by rq->entries[] are entirely
     * disjoint with 'log_start - 1' ... 'log_end - 1', as shown below.  So,
     * everything in the AppendEntries request must already have been
     * committed, and we might as well return true.
     *
     *     rq->prev_log_index
     *       | first_entry_index
     *       |   |         nth_entry_index
     *       |   |           |
     *       v   v           v
     *         +---+---+---+---+
     *       T | T | T | T | T |
     *         +---+-------+---+
     *                             +---+---+---+---+
     *                           T | T | T | T | T |
     *                             +---+---+---+---+
     *                               ^               ^
     *                               |               |
     *                           log_start        log_end
     */
    if (nth_entry_index < raft->log_start - 1) {
        raft_send_append_reply(raft, rq, true, "append before log start");
        return;
    }

    /*
     * Handle the case where the last entry in rq->entries[] has the same index
     * as 'log_start - 1', so we can compare their terms:
     *
     *     rq->prev_log_index
     *       | first_entry_index
     *       |   |         nth_entry_index
     *       |   |           |
     *       v   v           v
     *         +---+---+---+---+
     *       T | T | T | T | T |
     *         +---+-------+---+
     *                         +---+---+---+---+
     *                       T | T | T | T | T |
     *                         +---+---+---+---+
     *                           ^               ^
     *                           |               |
     *                       log_start        log_end
     *
     * There's actually a sub-case where rq->n_entries == 0, in which we
     * compare rq->prev_term:
     *
     *     rq->prev_log_index
     *       |
     *       |
     *       |
     *       v
     *       T
     *
     *         +---+---+---+---+
     *       T | T | T | T | T |
     *         +---+---+---+---+
     *           ^               ^
     *           |               |
     *       log_start        log_end
     */
    if (nth_entry_index == raft->log_start - 1) {
        if (rq->n_entries
            ? raft->prev_term == rq->entries[rq->n_entries - 1].term
            : raft->prev_term == rq->prev_log_term) {
            raft_send_append_reply(raft, rq, true, "no change");
        } else {
            raft_send_append_reply(raft, rq, false, "term mismatch");
        }
        return;
    }

    /*
     * We now know that the data in rq->entries[] overlaps the data in
     * raft->log[], as shown below, with some positive 'ofs':
     *
     *     rq->prev_log_index
     *       | first_entry_index
     *       |   |             nth_entry_index
     *       |   |               |
     *       v   v               v
     *         +---+---+---+---+---+
     *       T | T | T | T | T | T |
     *         +---+-------+---+---+
     *                     +---+---+---+---+
     *                   T | T | T | T | T |
     *                     +---+---+---+---+
     *                       ^               ^
     *                       |               |
     *                   log_start        log_end
     *
     *           |<-- ofs -->|
     *
     * We transform this into the following by trimming the first 'ofs'
     * elements off of rq->entries[], ending up with the following.  Notice how
     * we retain the term but not the data for rq->entries[ofs - 1]:
     *
     *                  first_entry_index + ofs - 1
     *                   | first_entry_index + ofs
     *                   |   |  nth_entry_index + ofs
     *                   |   |   |
     *                   v   v   v
     *                     +---+---+
     *                   T | T | T |
     *                     +---+---+
     *                     +---+---+---+---+
     *                   T | T | T | T | T |
     *                     +---+---+---+---+
     *                       ^               ^
     *                       |               |
     *                   log_start        log_end
     */
    uint64_t ofs = raft->log_start - first_entry_index;
    raft_handle_append_entries(raft, rq,
                               raft->log_start - 1, rq->entries[ofs - 1].term,
                               &rq->entries[ofs], rq->n_entries - ofs);
}

bool
raft_has_next_entry(const struct raft *raft_)
{
    struct raft *raft = CONST_CAST(struct raft *, raft_);
    if (raft->last_applied + 2 == raft->log_start) {
        return true;
    }
    return raft_peek_next_entry(raft, RAFT_DATA);
}

const struct json *
raft_next_entry(struct raft *raft, bool *is_snapshot)
{
    if (raft->last_applied + 2 == raft->log_start) {
        raft->last_applied++;
        *is_snapshot = true;
        return raft->snapshot;
    }

    const struct raft_entry *e = raft_get_next_entry(raft, RAFT_DATA);
    if (!e) {
        return NULL;
    }

    char *s = json_to_string(e->data, JSSF_SORT);
    VLOG_INFO("applying log index %"PRIu64": data \"%s\"",
              raft->last_applied, s);
    free(s);

    if (raft->role == RAFT_LEADER) {
        struct raft_command *cmd
            = raft_find_command(raft, raft->last_applied);
        if (cmd) {
            raft_command_complete(raft, cmd, RAFT_CMD_SUCCESS);
        }
    }

    raft_commit_servers(raft);

    *is_snapshot = false;       /* XXX */
    return e->data;
}

static void
raft_handle_append_request(struct raft *raft,
                           const struct raft_append_request *rq)
{
    raft_handle_append_request__(raft, rq);
}

static struct raft_server *
raft_find_peer(struct raft *raft, const struct uuid *uuid)
{
    struct raft_server *s = raft_find_server(raft, uuid);
    return s != raft->me ? s : NULL;
}

static struct raft_server *
raft_find_new_server(struct raft *raft, const struct uuid *uuid)
{
    return raft_find_server__(&raft->add_servers, uuid);
}

static void
raft_update_match_index(struct raft *raft, struct raft_server *s,
                        uint64_t min_index)
{
    if (s->match_index >= min_index) {
        return;
    }

    s->match_index = min_index;

    /* Figure 3.1: "If there exists an N such that N > commitIndex, a
     * majority of matchIndex[i] >= N, and log[N].term == currentTerm, set
     * commitIndex = N (sections 3.5 and 3.6)."
     *
     * This loop cannot just bail out when it comes across a log entry that
     * does not match the criteria.  For example, Figure 3.7(d2) shows a
     * case where the log entry for term 2 cannot be committed directly
     * (because it is not for the current term) but it can be committed as
     * a side effect of commit the entry for term 4 (the current term).
     * XXX Is there a more efficient way to do this? */
    for (uint64_t n = MAX(raft->commit_index + 1, raft->log_start);
         n < raft->log_end; n++) {
        if (raft->log[n - raft->log_start].term == raft->current_term) {
            size_t count = 0;
            struct raft_server *s2;
            HMAP_FOR_EACH (s2, hmap_node, &raft->servers) {
                if (s2->match_index >= n) {
                    count++;
                }
            }
            if (count > hmap_count(&raft->servers) / 2) {
                VLOG_INFO("%"PRIu64" committed to %"PRIuSIZE" servers, "
                          "applying", n, count);
                raft_update_commit_index(raft, n);
            }
        }
    }
}

static void
raft_send_install_snapshot_request(struct raft *raft,
                                   const struct raft_server *s,
                                   const char *comment)
{
    union raft_rpc rpc = {
        .install_snapshot_request = {
            .common = {
                .type = RAFT_RPC_INSTALL_SNAPSHOT_REQUEST,
                .sid = s->sid,
                .comment = CONST_CAST(char *, comment),
            },
            .term = raft->current_term,
            .last_index = raft->log_start - 1,
            .last_term = raft->prev_term,
            .last_servers = raft->prev_servers,
            .data = raft->snapshot,
        }
    };
    raft_send(raft, &rpc);
}

static void
raft_handle_append_reply(struct raft *raft,
                         const struct raft_append_reply *rpy)
{
    if (!raft_receive_term__(raft, &rpy->common, rpy->term)) {
        return;
    }
    if (raft->role != RAFT_LEADER) {
        VLOG_INFO("rejected append_reply (not leader)");
        return;
    }

    /* Most commonly we'd be getting an AppendEntries reply from a configured
     * server (e.g. a peer), but we can also get them from servers in the
     * process of being added. */
    struct raft_server *s = raft_find_peer(raft, &rpy->common.sid);
    if (!s) {
        s = raft_find_new_server(raft, &rpy->common.sid);
        if (!s) {
            VLOG_INFO("rejected append_reply from unknown server %04x",
                      uuid_prefix(&rpy->common.sid, 4));
            return;
        }
    }

    if (rpy->success) {
        /* Figure 3.1: "If successful, update nextIndex and matchIndex for
         * follower (section 3.5)." */
        uint64_t min_index = rpy->prev_log_index + rpy->n_entries + 1;
        if (s->next_index < min_index) {
            s->next_index = min_index;
        }
        raft_update_match_index(raft, s, min_index - 1);
    } else {
        /* Figure 3.1: "If AppendEntries fails because of log inconsistency,
         * decrement nextIndex and retry (section 3.5)."
         *
         * We also implement the optimization suggested in section 4.2.1:
         * "Various approaches can make nextIndex converge to its correct value
         * more quickly, including those described in Chapter 3. The simplest
         * approach to solving this particular problem of adding a new server,
         * however, is to have followers return the length of their logs in the
         * AppendEntries response; this allows the leader to cap the follower’s
         * nextIndex accordingly." */
        if (s->next_index > 0) {
            s->next_index = MIN(s->next_index - 1, rpy->log_end);
        } else {
            /* XXX log */
            VLOG_INFO("XXX");
        }
    }

    /*
     * Our behavior here must depend on the value of next_index relative to
     * log_start and log_end.  There are three cases:
     *
     *        Case 1       |    Case 2     |      Case 3
     *   <---------------->|<------------->|<------------------>
     *                     |               |
     *
     *                     +---+---+---+---+
     *                   T | T | T | T | T |
     *                     +---+---+---+---+
     *                       ^               ^
     *                       |               |
     *                   log_start        log_end
     */
    if (s->next_index < raft->log_start) {
        /* Case 1. */
        raft_send_install_snapshot_request(raft, s, NULL);
    } else if (s->next_index < raft->log_end) {
        /* Case 2. */
        raft_send_append_request(raft, s, 1);
    } else {
        /* Case 3. */
        if (s->phase == RAFT_PHASE_CATCHUP) {
            s->phase = RAFT_PHASE_CAUGHT_UP;
            raft_run_reconfigure(raft);
        }
    }
}

static int
raft_handle_vote_request__(struct raft *raft,
                           const struct raft_vote_request *rq)
{
    if (!raft_receive_term__(raft, &rq->common, rq->term)) {
        return false;
    }

    /* If we're waiting for our vote to be recorded persistently, don't
     * respond. */
    if (raft->vote_waiter) {
        return -1;
    }

    /* Figure 3.1: "If votedFor is null or candidateId, and candidate's vote is
     * at least as up-to-date as receiver's log, grant vote (sections 3.4,
     * 3.6)." */
    if (uuid_equals(&raft->voted_for, &rq->common.sid)) {
        /* Already voted for this candidate in this term.  Resend vote. */
        return true;
    } else if (!uuid_is_zero(&raft->voted_for)) {
        /* Already voted for different candidate in this term. */
        return false;
    }

    /* Section 3.6.1: "The RequestVote RPC implements this restriction: the RPC
     * includes information about the candidate’s log, and the voter denies its
     * vote if its own log is more up-to-date than that of the candidate.  Raft
     * determines which of two logs is more up-to-date by comparing the index
     * and term of the last entries in the logs.  If the logs have last entries
     * with different terms, then the log with the later term is more
     * up-to-date.  If the logs end with the same term, then whichever log is
     * longer is more up-to-date." */
    uint64_t last_term = (raft->log_end > raft->log_start
                          ? raft->log[raft->log_end - 1 - raft->log_start].term
                          : raft->prev_term);
    if (last_term > rq->last_log_term
        || (last_term == rq->last_log_term
            && raft->log_end - 1 > rq->last_log_index)) {
        /* Our log is more up-to-date than the peer's, so withhold vote. */
        return false;
    }

    /* Record a vote for the peer. */
    raft->voted_for = rq->common.sid;
    struct ovsdb_error *error = raft_write_state(raft->storage,
                                                 raft->current_term,
                                                 &raft->voted_for);
    if (error) {
        /* XXX */
    }

    raft_reset_timer(raft);

    raft->vote_waiter = raft_waiter_create(raft, RAFT_W_VOTE);
    return -1;
}

static void
raft_send_vote_reply(struct raft *raft, const struct uuid *dst,
                     bool vote_granted)
{
    union raft_rpc rpy = {
        .vote_reply = {
            .common = {
                .type = RAFT_RPC_VOTE_REPLY,
                .sid = *dst,
            },
            .term = raft->current_term,
            .vote_granted = vote_granted
        },
    };
    raft_send(raft, &rpy);
}

static void
raft_handle_vote_request(struct raft *raft,
                         const struct raft_vote_request *rq)
{
    int vote_granted = raft_handle_vote_request__(raft, rq);
    if (vote_granted >= 0) {
        raft_send_vote_reply(raft, &rq->common.sid, vote_granted);
    }
}

static void
raft_handle_vote_reply(struct raft *raft,
                       const struct raft_vote_reply *rpy)
{
    if (!raft_receive_term__(raft, &rpy->common, rpy->term)) {
        return;
    }

    if (raft->role != RAFT_CANDIDATE) {
        return;
    }

    struct raft_server *s = raft_find_peer(raft, &rpy->common.sid);
    if (s) {
        raft_accept_vote(raft, s, rpy->vote_granted);
    }
}

/* Returns true if 'raft''s log contains reconfiguration entries that have not
 * yet been committed. */
static bool
raft_has_uncommitted_configuration(const struct raft *raft)
{
    for (uint64_t i = raft->commit_index + 1; i < raft->log_end; i++) {
        ovs_assert(i >= raft->log_start);
        const struct raft_entry *e = &raft->log[i - raft->log_start];
        if (e->type == RAFT_SERVERS) {
            return true;
        }
    }
    return false;
}

static void
raft_log_reconfiguration(struct raft *raft)
{
    /* Add the reconfiguration to the log. */
    struct json *servers_json = raft_servers_to_json(&raft->servers);
    struct raft_command *cmd = raft_command_execute__(
        raft, RAFT_SERVERS, servers_json);
    json_destroy(servers_json);
    if (cmd) {
        /* XXX handle error */
    }
}

static void
raft_run_reconfigure(struct raft *raft)
{
    ovs_assert(raft->role == RAFT_LEADER);

    /* Reconfiguration only progresses when configuration changes commit. */
    if (raft_has_uncommitted_configuration(raft)) {
        return;
    }

    /* If we were waiting for a configuration change to commit, it's done. */
    struct raft_server *s;
    HMAP_FOR_EACH (s, hmap_node, &raft->servers) {
        if (s->phase == RAFT_PHASE_COMMITTING) {
            raft_send_add_server_reply(raft, &s->reply_sid, RAFT_SERVER_OK);
            s->phase = RAFT_PHASE_STABLE;
        }
    }
    if (raft->remove_server) {
        raft_send_remove_server_reply(raft, &raft->remove_server->reply_sid,
                                      RAFT_SERVER_OK);
        raft_server_destroy(raft->remove_server);
        raft->remove_server = NULL;
    }

    /* If a new server is caught up, add it to the configuration.  */
    HMAP_FOR_EACH (s, hmap_node, &raft->add_servers) {
        if (s->phase == RAFT_PHASE_CAUGHT_UP) {
            /* Move 's' from 'raft->add_servers' to 'raft->servers'. */
            hmap_remove(&raft->add_servers, &s->hmap_node);
            hmap_insert(&raft->servers, &s->hmap_node, uuid_hash(&s->sid));

            /* Mark 's' as waiting for commit. */
            s->phase = RAFT_PHASE_COMMITTING;

            raft_log_reconfiguration(raft);

            /* When commit completes we'll transition to RAFT_PHASE_STABLE and
             * send a RAFT_SERVER_OK reply. */

            return;
        }
    }

    /* Remove a server, if one is scheduled for removal. */
    HMAP_FOR_EACH (s, hmap_node, &raft->servers) {
        if (s->phase == RAFT_PHASE_REMOVE) {
            hmap_remove(&raft->servers, &s->hmap_node);
            raft->remove_server = s;

            raft_log_reconfiguration(raft);

            return;
        }
    }
}

static int
raft_handle_add_server_request__(struct raft *raft,
                                 const struct raft_server_request *rq)
{
    /* Figure 4.1: "1. Reply NOT_LEADER if not leader (section 6.2)." */
    if (raft->role != RAFT_LEADER) {
        return RAFT_SERVER_NOT_LEADER;
    }

    /* Check for an existing server. */
    struct raft_server *s = raft_find_server(raft, &rq->sid);
    if (s) {
        /* If the server is scheduled to be removed, cancel it. */
        if (s->phase == RAFT_PHASE_REMOVE) {
            s->phase = RAFT_PHASE_STABLE;
            raft_send_remove_server_reply(raft, &s->reply_sid,
                                          RAFT_SERVER_CANCELED);
            return RAFT_SERVER_OK;
        }

        /* If the server is being added, then it's in progress. */
        if (s->phase != RAFT_PHASE_STABLE) {
            return RAFT_SERVER_IN_PROGRESS;
        }

        /* Cannot add a server that is already part of the configuration. */
        return RAFT_SERVER_NO_OP;
    }

    /* Check for a server being removed. */
    if (raft->remove_server
        && uuid_equals(&rq->sid, &raft->remove_server->sid)) {
        return RAFT_SERVER_COMMITTING;
    }

    /* Check for a server already being added. */
    if (raft_find_new_server(raft, &rq->sid)) {
        return RAFT_SERVER_IN_PROGRESS;
    }

    /* Add server to 'add_servers'. */
    s = xzalloc(sizeof *s);
    hmap_insert(&raft->add_servers, &s->hmap_node, uuid_hash(&rq->sid));
    s->sid = rq->sid;
    raft_server_init_leader(raft, s);
    s->address = xstrdup(rq->address);
    s->reply_sid = rq->common.sid;
    s->phase = RAFT_PHASE_CATCHUP;

    /* Start sending the log.  If this is the first time we've tried to add
     * this server, then this will quickly degenerate into an InstallSnapshot
     * followed by a series of AddEntries, but if it's a retry of an earlier
     * AddRequest that was interrupted (e.g. by a timeout or a loss of
     * leadership) then it will gracefully resume populating the log.
     *
     * See the last few paragraphs of section 4.2.1 for further insight. */
    raft_send_append_request(raft, s, 0);

    return -1;
}

static void
raft_handle_add_server_request(struct raft *raft,
                               const struct raft_server_request *rq)
{
    int status = raft_handle_add_server_request__(raft, rq);
    if (status >= 0) {
        raft_send_add_server_reply(raft, &rq->common.sid, status);
    } else {
        /* Operation in progress, reply will be sent later. */
    }
}

static void
raft_handle_add_server_reply(struct raft *raft,
                             const struct raft_server_reply *rpc)
{
    if (rpc->status == RAFT_SERVER_OK || rpc->status == RAFT_SERVER_NO_OP) {
        if (raft->me) {
            raft->joining = false;

            /* Disable reconnect on the initial remote connections.  Some of
             * them might not even be correct. */
            struct raft_conn *conn;
            LIST_FOR_EACH (conn, list_node, &raft->conns) {
                jsonrpc_session_disable_reconnect(conn->js);
            }
        } else {
            /* XXX we're not really part of the cluster? */
        }
    }
}

static int
raft_handle_remove_server_request__(struct raft *raft,
                                    const struct raft_server_request *rq)
{
    /* Figure 4.1: "1. Reply NOT_LEADER if not leader (section 6.2)." */
    if (raft->role != RAFT_LEADER) {
        return RAFT_SERVER_NOT_LEADER;
    }

    /* If the server to remove is currently waiting to be added, cancel it. */
    struct raft_server *target = raft_find_new_server(raft, &rq->sid);
    if (target) {
        raft_send_remove_server_reply(raft, &target->reply_sid,
                                      RAFT_SERVER_CANCELED);
        hmap_remove(&raft->add_servers, &target->hmap_node);
        raft_server_destroy(target);
        return RAFT_SERVER_OK;
    }

    /* If the server isn't configured, report that. */
    target = raft_find_server(raft, &rq->sid);
    if (!target) {
        return RAFT_SERVER_NO_OP;
    }

    /* Check whether we're waiting for the addition of the server to commit. */
    if (target->phase == RAFT_PHASE_COMMITTING) {
        return RAFT_SERVER_COMMITTING;
    }

    /* Check whether the server is already scheduled for removal. */
    if (target->phase == RAFT_PHASE_REMOVE) {
        return RAFT_SERVER_IN_PROGRESS;
    }

    /* Make sure that if we remove this server then that at least one other
     * server will be left.  We don't count servers currently being added (in
     * 'add_servers') since those could fail. */
    struct raft_server *s;
    int n = 0;
    HMAP_FOR_EACH (s, hmap_node, &raft->servers) {
        if (s != target && s->phase != RAFT_PHASE_REMOVE) {
            n++;
        }
    }
    if (!n) {
        return RAFT_SERVER_EMPTY;
    }

    /* Mark the server for removal. */
    target->phase = RAFT_PHASE_REMOVE;
    target->reply_sid = rq->common.sid;

    raft_run_reconfigure(raft);
    return -1;
}

static void
raft_handle_remove_server_request(struct raft *raft,
                                  const struct raft_server_request *rq)
{
    int status = raft_handle_remove_server_request__(raft, rq);
    if (status >= 0) {
        raft_send_remove_server_reply(raft, &rq->common.sid, status);
    } else {
        /* Operation in progress, reply will be sent later. */
    }
}

static void
raft_handle_remove_server_reply(struct raft *raft OVS_UNUSED,
                                const struct raft_server_reply *rpc OVS_UNUSED)
{
    if (rpc->status == RAFT_SERVER_OK || rpc->status == RAFT_SERVER_NO_OP) {
        VLOG_INFO("left cluster");
        raft->leaving = false;
        raft->left = true;
    }
}

static struct ovsdb_error * OVS_WARN_UNUSED_RESULT
raft_write_snapshot(struct raft *raft, struct ovsdb_log *storage,
                    uint64_t new_log_start, const struct json *new_snapshot)
{
    ovs_assert(new_log_start >= raft->log_start);
    ovs_assert(new_log_start <= raft->log_end);
    ovs_assert(new_log_start <= raft->last_applied + 2);
    ovs_assert(new_log_start > raft->log_start
               ? new_snapshot != NULL
               : new_snapshot == NULL);

    /* Compose header record. */
    uint64_t prev_term = raft_get_term(raft, new_log_start - 1);
    uint64_t prev_index = new_log_start - 1;
    struct json *prev_servers = raft_servers_for_index(raft,
                                                       new_log_start - 1);

    /* Write snapshot record. */
    struct json *header = json_object_create();
    json_object_put_format(header, "server_id", UUID_FMT,
                           UUID_ARGS(&raft->sid));
    json_object_put_string(header, "name", raft->name);
    json_object_put(header, "prev_servers", prev_servers);
    if (!uuid_is_zero(&raft->cid)) {
        json_object_put_format(header, "cluster_id", UUID_FMT,
                               UUID_ARGS(&raft->cid));
    }
    if (raft->snapshot || new_snapshot) {
        json_object_put(header, "prev_term", json_integer_create(prev_term));
        json_object_put(header, "prev_index",
                        json_integer_create(prev_index));
        json_object_put(header, "prev_data", json_clone(new_snapshot
                                                        ? new_snapshot
                                                        : raft->snapshot));
    }
    struct ovsdb_error *error = ovsdb_log_write(storage, header);
    json_destroy(header);
    if (error) {
        return error;
    }
    raft->snapshot_size = ovsdb_log_get_offset(storage);

    /* Write log records. */
    for (uint64_t index = new_log_start; index < raft->log_end; index++) {
        struct json *json = raft_entry_to_json_with_index(raft, index);
        error = ovsdb_log_write(storage, json);
        json_destroy(json);
        if (error) {
            return error;
        }
    }

    /* Write term and vote (if any).
     *
     * The term is redundant if we wrote a log record for that term above.  The
     * vote, if any, is never redundant.
     */
    return raft_write_state(storage, raft->current_term, &raft->voted_for);
}

static struct ovsdb_error * OVS_WARN_UNUSED_RESULT
raft_save_snapshot(struct raft *raft,
                   uint64_t new_start, const struct json *new_snapshot)
{
    struct ovsdb_log *new_storage;
    struct ovsdb_error *error;
    error = ovsdb_log_replace_start(raft->storage, &new_storage);
    if (error) {
        return error;
    }

    error = raft_write_snapshot(raft, new_storage, new_start, new_snapshot);
    if (error) {
        ovsdb_log_replace_abort(new_storage);
        return error;
    }

    return ovsdb_log_replace_commit(raft->storage, new_storage);
}

static void
raft_handle_install_snapshot_request__(
    struct raft *raft, const struct raft_install_snapshot_request *rq)
{
    if (!raft_receive_term__(raft, &rq->common, rq->term)) {
        return;
    }

    raft_reset_timer(raft);

    uint64_t new_log_start = rq->last_index + 1;
    if (new_log_start < raft->log_start) {
        /* The new snapshot covers less than our current one, why bother? */
        return;
    } else if (new_log_start >= raft->log_end) {
        /* The new snapshot starts past the end of our current log, so discard
         * all of our current log.
         *
         * XXX make sure that last_term is not a regression*/
        raft->log_start = raft->log_end = new_log_start;
    } else {
        /* The new snapshot starts in the middle of our log, so discard the
         * first 'new_log_start - raft->log_start' entries in the log.
         *
         * XXX we can validate last_term and last_servers exactly */
        memmove(&raft->log[0], &raft->log[new_log_start - raft->log_start],
                (raft->log_end - new_log_start) * sizeof *raft->log);
        raft->log_start = new_log_start;
    }
    raft->commit_index = raft->log_start - 1;
    if (raft->last_applied < raft->commit_index) {
        raft->last_applied = raft->log_start - 2;
    }

    raft->prev_term = rq->last_term;
    raft_servers_destroy(&raft->prev_servers);
    raft_servers_clone(&raft->prev_servers, &rq->last_servers);

    /* install snapshot */
    json_destroy(raft->snapshot);
    raft->snapshot = json_clone(rq->data);

    struct ovsdb_error *error = raft_save_snapshot(raft,
                                                   raft->log_start, NULL);
    if (error) {
        char *error_s = ovsdb_error_to_string(error);
        VLOG_WARN("could not save snapshot: %s", error_s);
        free(error_s);

        /* XXX handle error */
    }
}

static void
raft_handle_install_snapshot_request(
    struct raft *raft, const struct raft_install_snapshot_request *rq)
{
    raft_handle_install_snapshot_request__(raft, rq);

    union raft_rpc rpy = {
        .install_snapshot_reply = {
            .common = {
                .type = RAFT_RPC_INSTALL_SNAPSHOT_REPLY,
                .sid = rq->common.sid,
            },
            .term = raft->current_term,
            .last_index = rq->last_index,
            .last_term = rq->last_term,
        },
    };
    raft_send(raft, &rpy);
}

static void
raft_handle_install_snapshot_reply(
    struct raft *raft, const struct raft_install_snapshot_reply *rpy)
{
    if (!raft_receive_term__(raft, &rpy->common, rpy->term)) {
        return;
    }

    /* We might get an InstallSnapshot reply from a configured server (e.g. a
     * peer) or a server in the process of being added. */
    struct raft_server *s = raft_find_peer(raft, &rpy->common.sid);
    if (!s) {
        s = raft_find_new_server(raft, &rpy->common.sid);
        if (!s) {
            /* XXX log */
            return;
        }
    }

    if (rpy->last_index != raft->log_start - 1 ||
        rpy->last_term != raft->prev_term) {
        VLOG_INFO("cluster %04x: server %04x installed "
                  "out-of-date snapshot, starting over",
                  uuid_prefix(&raft->cid, 4), uuid_prefix(&s->sid, 4));
        raft_send_install_snapshot_request(raft, s,
                                           "installed obsolete snapshot");
        return;
    }

    VLOG_INFO("cluster %04x: installed snapshot on server %04x "
              " up to %"PRIu64":%"PRIu64,
              uuid_prefix(&raft->cid, 4), uuid_prefix(&s->sid, 4),
              rpy->last_term, rpy->last_index);
    s->next_index = raft->log_end;
    raft_send_append_request(raft, s, 0);
}

bool
raft_should_snapshot(const struct raft *raft)
{
    if (raft->joining) {
        return false;
    }

    /* If it has been at least SNAPSHOT_TIME_BASE_MSEC (plus up to
     * SNAPSHOT_TIME_RANGE_MSEC) ms since the last time we took a snapshot, and
     * if there are at least 100 log entries, and if the log is at least 10 MB,
     * and the log is at least 4x the size of the previous snapshot, then we
     * should take a snapshot. */
    off_t log_size = ovsdb_log_get_offset(raft->storage);
    return (time_msec() >= raft->next_snapshot
            && raft->last_applied - raft->log_start >= 100
            && log_size >= 10 * 1024 * 1024
            && log_size / 4 >= raft->snapshot_size);
}

static bool
raft_try_store_snapshot(struct raft *raft, const struct json *new_snapshot)
{
    if (raft->joining) {
        VLOG_WARN("cannot store a snapshot while joining");
        return false;
    }

    if (raft->last_applied < raft->log_start) {
        VLOG_WARN("not storing a duplicate snapshot");
        return false;
    }

    uint64_t new_log_start = raft->last_applied + 1;
    struct ovsdb_error *error = raft_save_snapshot(raft, new_log_start,
                                                   new_snapshot);
    if (error) {
        char *error_s = ovsdb_error_to_string(error);
        VLOG_WARN("saving snapshot failed (%s)", error_s);
        free(error_s);
        ovsdb_error_destroy(error);
        return false;
    }

    raft->prev_term = raft_get_term(raft, new_log_start - 1);
    json_destroy(raft->snapshot);
    raft->snapshot = json_clone(new_snapshot);

    struct json *prev_servers_json = raft_servers_for_index(raft,
                                                            new_log_start - 1);
    struct hmap prev_servers;
    error = raft_servers_from_json(prev_servers_json, &prev_servers);
    if (error) {
        /* XXX */
        return false;
    }
    json_destroy(prev_servers_json);
    hmap_swap(&prev_servers, &raft->prev_servers);
    raft_servers_destroy(&prev_servers);

    memmove(&raft->log[0], &raft->log[new_log_start - raft->log_start],
            (raft->log_end - new_log_start) * sizeof *raft->log);
    raft->log_start = new_log_start;
    return true;
}

void
raft_store_snapshot(struct raft *raft, const struct json *data)
{
    bool ok = raft_try_store_snapshot(raft, data);
    raft_schedule_snapshot(raft, !ok);
}

static void
raft_handle_become_leader(struct raft *raft,
                          const struct raft_become_leader *rq)
{
    if (!raft_receive_term__(raft, &rq->common, rq->term)) {
        return;
    }

    if (raft->role == RAFT_FOLLOWER) {
        VLOG_INFO("received leadership transfer from %04x in term %"PRIu64,
                  uuid_prefix(&rq->common.sid, 4), rq->term);
        raft_start_election(raft);
    }
}

static void
raft_handle_rpc(struct raft *raft, const union raft_rpc *rpc)
{
    switch (rpc->common.type) {
    case RAFT_RPC_HELLO_REQUEST:
        break;
    case RAFT_RPC_APPEND_REQUEST:
        raft_handle_append_request(raft, &rpc->append_request);
        break;
    case RAFT_RPC_APPEND_REPLY:
        raft_handle_append_reply(raft, &rpc->append_reply);
        break;
    case RAFT_RPC_VOTE_REQUEST:
        raft_handle_vote_request(raft, &rpc->vote_request);
        break;
    case RAFT_RPC_VOTE_REPLY:
        raft_handle_vote_reply(raft, &rpc->vote_reply);
        break;
    case RAFT_RPC_ADD_SERVER_REQUEST:
        raft_handle_add_server_request(raft, &rpc->server_request);
        break;
    case RAFT_RPC_ADD_SERVER_REPLY:
        raft_handle_add_server_reply(raft, &rpc->server_reply);
        break;
    case RAFT_RPC_REMOVE_SERVER_REQUEST:
        raft_handle_remove_server_request(raft, &rpc->server_request);
        break;
    case RAFT_RPC_REMOVE_SERVER_REPLY:
        raft_handle_remove_server_reply(raft, &rpc->server_reply);
        break;
    case RAFT_RPC_INSTALL_SNAPSHOT_REQUEST:
        raft_handle_install_snapshot_request(raft,
                                             &rpc->install_snapshot_request);
        break;
    case RAFT_RPC_INSTALL_SNAPSHOT_REPLY:
        raft_handle_install_snapshot_reply(raft, &rpc->install_snapshot_reply);
        break;
    case RAFT_RPC_BECOME_LEADER:
        raft_handle_become_leader(raft, &rpc->become_leader);
        break;
    default:
        OVS_NOT_REACHED();
    }
}

static void
raft_format_append_request(const struct raft_append_request *rq,
                           struct ds *s)
{
    ds_put_format(s, " term=%"PRIu64, rq->term);
    ds_put_format(s, " prev_log_index=%"PRIu64, rq->prev_log_index);
    ds_put_format(s, " prev_log_term=%"PRIu64, rq->prev_log_term);
    ds_put_format(s, " leader_commit=%"PRIu64, rq->leader_commit);
    ds_put_format(s, " n_entries=%u", rq->n_entries);
}

static void
raft_format_append_reply(const struct raft_append_reply *rpy, struct ds *s)
{
    ds_put_format(s, " term=%"PRIu64, rpy->term);
    ds_put_format(s, " log_end=%"PRIu64, rpy->log_end);
    ds_put_format(s, " %s", rpy->success ? "success" : "failure");
}

static void
raft_format_vote_request(const struct raft_vote_request *rq, struct ds *s)
{
    ds_put_format(s, " term=%"PRIu64, rq->term);
    ds_put_format(s, " last_log_index=%"PRIu64, rq->last_log_index);
    ds_put_format(s, " last_log_term=%"PRIu64, rq->last_log_term);
}

static void
raft_format_vote_reply(const struct raft_vote_reply *rpy, struct ds *s)
{
    ds_put_format(s, " term=%"PRIu64, rpy->term);
    ds_put_format(s, " vote_granted=%s", rpy->vote_granted ? "true" : "false");
}

static void
raft_format_add_server_request(const struct raft_server_request *rq,
                               struct ds *s)
{
    ds_put_format(s, " server=%04x(%s)",
                  uuid_prefix(&rq->sid, 4), rq->address);
}

static void
raft_format_server_reply(const struct raft_server_reply *rpy, struct ds *s)
{
    ds_put_format(s, " status=%s", raft_server_status_to_string(rpy->status));
    if (rpy->leader_address) {
        ds_put_format(s, " leader=%04x(%s)",
                      uuid_prefix(&rpy->leader_sid, 4), rpy->leader_address);
    }
}

static void
raft_format_remove_server_request(const struct raft_server_request *rq,
                                  struct ds *s)
{
    ds_put_format(s, " server=%04x", uuid_prefix(&rq->sid, 4));
}

static void
raft_format_install_snapshot_request(
    const struct raft_install_snapshot_request *rq, struct ds *s)
{
    ds_put_format(s, " term=%"PRIu64, rq->term);
    ds_put_format(s, " last_index=%"PRIu64, rq->last_index);
    ds_put_format(s, " last_term=%"PRIu64, rq->last_term);
    ds_put_cstr(s, " last_servers=");
    raft_servers_format(&rq->last_servers, s);
}

static void
raft_format_install_snapshot_reply(
    const struct raft_install_snapshot_reply *rpy, struct ds *s)
{
    ds_put_format(s, " term=%"PRIu64, rpy->term);
}

static void
raft_format_become_leader(const struct raft_become_leader *rq, struct ds *s)
{
    ds_put_format(s, " term=%"PRIu64, rq->term);
}

static void
raft_rpc_format(const union raft_rpc *rpc, struct ds *s)
{
    ds_put_format(s, "%04x %s", uuid_prefix(&rpc->common.sid, 4),
                  raft_rpc_type_to_string(rpc->common.type));
    if (rpc->common.comment) {
        ds_put_format(s, " \"%s\"", rpc->common.comment);
    }
    ds_put_char(s, ':');

    switch (rpc->common.type) {
    case RAFT_RPC_HELLO_REQUEST:
        break;
    case RAFT_RPC_APPEND_REQUEST:
        raft_format_append_request(&rpc->append_request, s);
        break;
    case RAFT_RPC_APPEND_REPLY:
        raft_format_append_reply(&rpc->append_reply, s);
        break;
    case RAFT_RPC_VOTE_REQUEST:
        raft_format_vote_request(&rpc->vote_request, s);
        break;
    case RAFT_RPC_VOTE_REPLY:
        raft_format_vote_reply(&rpc->vote_reply, s);
        break;
    case RAFT_RPC_ADD_SERVER_REQUEST:
        raft_format_add_server_request(&rpc->server_request, s);
        break;
    case RAFT_RPC_ADD_SERVER_REPLY:
        raft_format_server_reply(&rpc->server_reply, s);
        break;
    case RAFT_RPC_REMOVE_SERVER_REQUEST:
        raft_format_remove_server_request(&rpc->server_request, s);
        break;
    case RAFT_RPC_REMOVE_SERVER_REPLY:
        raft_format_server_reply(&rpc->server_reply, s);
        break;
    case RAFT_RPC_INSTALL_SNAPSHOT_REQUEST:
        raft_format_install_snapshot_request(&rpc->install_snapshot_request,
                                             s);
        break;
    case RAFT_RPC_INSTALL_SNAPSHOT_REPLY:
        raft_format_install_snapshot_reply(&rpc->install_snapshot_reply, s);
        break;
    case RAFT_RPC_BECOME_LEADER:
        raft_format_become_leader(&rpc->become_leader, s);
        break;
    default:
        OVS_NOT_REACHED();
    }
}

static bool
raft_rpc_is_heartbeat(const union raft_rpc *rpc)
{
    return ((rpc->common.type == RAFT_RPC_APPEND_REQUEST
             || rpc->common.type == RAFT_RPC_APPEND_REPLY)
             && rpc->common.comment
             && !strcmp(rpc->common.comment, "heartbeat"));
}


static void
raft_send__(struct raft *raft, const union raft_rpc *rpc,
            struct jsonrpc_session *js)
{
    if (!raft_rpc_is_heartbeat(rpc)) {
        struct ds s = DS_EMPTY_INITIALIZER;
        raft_rpc_format(rpc, &s);
        VLOG_INFO("-->%s", ds_cstr(&s));
        ds_destroy(&s);
    }

    jsonrpc_session_send(js, raft_rpc_to_jsonrpc(raft, rpc));
}

static void
raft_send(struct raft *raft, const union raft_rpc *rpc)
{
    if (uuid_equals(&rpc->common.sid, &raft->sid)) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
        VLOG_WARN_RL(&rl, "attempting to send RPC to self");
        return;
    }

    struct raft_server *s = raft_find_peer(raft, &rpc->common.sid);
    if (s && s->js && jsonrpc_session_is_connected(s->js)) {
        raft_send__(raft, rpc, s->js);
        return;
    }

    struct raft_conn *conn;
    LIST_FOR_EACH (conn, list_node, &raft->conns) {
        if (uuid_equals(&conn->sid, &rpc->common.sid)
            && jsonrpc_session_is_connected(conn->js)) {
            raft_send__(raft, rpc, conn->js);
            return;
        }
    }

    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
    VLOG_WARN_RL(&rl, "%04x: no connection, cannot send RPC",
                 uuid_prefix(&rpc->common.sid, 4));
}
