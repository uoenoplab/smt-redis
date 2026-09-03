/* homa.c - Homa transport connection type for Redis (server side).
 *
 * Homa is a connectionless, message-oriented datacenter transport. One Homa
 * socket serves every peer, and each request/reply is a self-contained RPC
 * identified by (peer address, rpc id).
 *
 * Peer demux: the listen fd's readable handler is a permanent dispatcher that
 * drains ready RPCs with recvmsg and hands each payload to a per-peer redis
 * client (looked up by peer ip:port). Peer connections have fd == -1: they
 * never touch the event loop — the dispatcher copies the whole message into
 * the peer's querybuf and drives processInputBuffer directly. Per peer only one
 * RPC is in flight at a time: further RPCs wait in a FIFO with their own bytes,
 * which join the querybuf only when the RPC becomes current, so one reply never
 * mixes two RPCs' commands. Replies are coalesced and sent as the RPC's single
 * response message from the pending-data hook, once the RPC is done
 * (homaRpcDone). Peer-internal ordering and session state (MULTI/SELECT/...)
 * thus behave like a TCP connection -- until the peer is reaped for being idle
 * (see homaReapIdlePeers).
 *
 * Most non-IO callbacks (event handler, address, accept, sync IO) are
 * identical to plain TCP, so we delegate them to the socket connection type.
 */

#include "server.h"
#include "connection.h"
#include "connhelpers.h"
#include "anet.h"
#include "homa_user.h"
#include <arpa/inet.h>

static ConnectionType CT_Homa;

/* Each Homa/SMT listen socket mmaps its own receive pool and keeps its own
 * peers; there are at most two listeners (--homa-port and --smt-port). */
#define HOMA_MAX_LISTENERS 2
typedef struct homa_pool {
    int fd;
    uint8_t *region;
    size_t region_size;
    connListener *listener;
    struct homa_recvmsg_args ctl;   /* bpage recycling carried across recvmsg */
    dict *peers;                    /* homaPeerKey(addr) -> homa_connection */
    list *pending;                  /* peers the pending-data hook must visit */
    int tx_blocked;                 /* out of send memory: waiting for EPOLLOUT */
} homa_pool;
static homa_pool homa_pools[HOMA_MAX_LISTENERS];
static int homa_pools_count = 0;

/* Per-peer connection. `c` MUST be first so a `connection *` can be cast to
 * `homa_connection *` (same trick as tls.c). c.fd is always -1: the peer
 * connection is a pure bookkeeping object driven by the dispatcher. */
typedef struct homa_connection {
    connection c;
    struct sockaddr_in peer;      /* this peer's address */
    homa_pool *pool;              /* listener it arrived on */
    uint64_t cur_id;              /* rpc id currently being processed/replied */
    int reply_pending;            /* cur_id not answered yet */
    sds reply_buf;                /* coalesced reply pieces for cur_id */
    list *rpc_queue;              /* homa_rpc: RPCs waiting behind cur_id */
    listNode *pending_node;       /* in pool->pending, or NULL */
} homa_connection;

/* An RPC that arrived while its peer was still busy with the previous one.
 * Its bytes stay here, not in the querybuf, until it becomes current: the
 * querybuf must only ever hold the current RPC's commands, or
 * processInputBuffer would answer several RPCs in one reply. */
typedef struct homa_rpc {
    uint64_t id;
    sds msg;
} homa_rpc;

static void homaRpcFree(void *ptr) {
    homa_rpc *rpc = ptr;
    sdsfree(rpc->msg);
    zfree(rpc);
}

static const char *connHomaGetType(connection *conn) {
    UNUSED(conn);
    return CONN_TYPE_HOMA;
}

/* -------------------------------------------------------------------------
 * Peer table
 * ------------------------------------------------------------------------- */

/* A peer is its ip:port, packed into the key pointer itself (64-bit, like
 * Homa); keys then compare by value (dictType.keyCompare == NULL). */
static void *homaPeerKey(const struct sockaddr_in *addr) {
    return (void *)(((uintptr_t)addr->sin_addr.s_addr << 16) | addr->sin_port);
}

static uint64_t homaPeerKeyHash(const void *key) {
    return dictGenHashFunction(&key, sizeof(key));
}

static dictType homaPeerDictType = { .hashFunction = homaPeerKeyHash };

/* Queue the peer for the pending-data hook (connHomaProcessPendingData). */
static void homaPeerMarkPending(homa_connection *hc) {
    if (hc->pending_node)
        return;
    listAddNodeTail(hc->pool->pending, hc);
    hc->pending_node = listLast(hc->pool->pending);
}

/* Find this peer's redis client, or admit a new peer exactly like an accepted
 * TCP connection (acceptCommonHandler: maxclients, protected mode, stats,
 * module events). Its first RPC `id` is made current before admission so that
 * a rejection (-ERR/-DENIED, written with connWrite) goes out as its reply.
 * Returns NULL if the peer was rejected. */
static homa_connection *homaPeerGet(homa_pool *pool, struct aeEventLoop *el,
                                    struct sockaddr_in *addr, uint64_t id) {
    homa_connection *hc = dictFetchValue(pool->peers, homaPeerKey(addr));
    if (hc)
        return hc;

    hc = zcalloc(sizeof(*hc));
    hc->c.type = pool->listener->ct; /* CT_Homa or CT_Smt, for accurate reporting */
    hc->c.fd = -1;
    hc->c.iovcnt = IOV_MAX;
    hc->c.el = el;
    hc->c.state = CONN_STATE_ACCEPTING;
    hc->peer = *addr;
    hc->pool = pool;
    hc->cur_id = id;
    hc->reply_pending = 1;
    hc->rpc_queue = listCreate();
    listSetFreeMethod(hc->rpc_queue, homaRpcFree);
    dictAdd(pool->peers, homaPeerKey(addr), hc);

    /* A rejection closes the connection: hold a ref so hc stays valid. */
    connIncrRefs(&hc->c);
    acceptCommonHandler(&hc->c, 0, NULL);
    connDecrRefs(&hc->c);
    if (hc->c.flags & CONN_FLAG_CLOSE_SCHEDULED) {
        connClose(&hc->c); /* sends the -ERR */
        return NULL;
    }
    client *c = connGetPrivateData(&hc->c);
    if (c->flags & CLIENT_CLOSE_ASAP)
        return NULL; /* -DENIED: sent by the pending-data hook, then freed */
    hc->reply_pending = 0; /* admitted: the caller runs the RPC */
    return hc;
}

/* -------------------------------------------------------------------------
 * Listen
 * ------------------------------------------------------------------------- */

/* Homa binds a port, never an address (homa_bind ignores sin_addr), so a Homa
 * socket listens on every interface. Only allow that when `bind` exposes TCP
 * on every IPv4 interface too, rather than silently widen what it restricts. */
int homaBindAllowed(char **bindaddr, int count) {
    for (int j = 0; j < count; j++) {
        const char *a = bindaddr[j] + (bindaddr[j][0] == '-');
        if (!strcmp(a, "*") || !strcmp(a, "0.0.0.0"))
            return 1;
    }
    return 0;
}

static int connHomaListen(connListener *listener) {
    if (!homaBindAllowed(listener->bindaddr, listener->bindaddr_count)) {
        serverLog(LL_WARNING, "Homa can't bind an address, so port %d would listen on "
                  "every interface: add '*' or '0.0.0.0' to 'bind'", listener->port);
        return C_ERR;
    }
    /* Peers are fed on the main thread by the dispatcher; an IO thread taking
     * one over would process the same client concurrently. */
    if (server.io_threads_num > 1) {
        serverLog(LL_WARNING, "Homa port %d does not support io-threads > 1", listener->port);
        return C_ERR;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_HOMA);
    if (fd == -1) {
        serverLog(LL_WARNING, "Failed opening Homa socket: %s", strerror(errno));
        return C_ERR;
    }

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(listener->port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        serverLog(LL_WARNING, "Couldn't bind Homa port %d: %s", listener->port, strerror(errno));
        close(fd);
        return C_ERR;
    }

    uint8_t *region;
    size_t region_size;
    if (homa_user_init_recv_buffer(fd, &region, &region_size, HOMA_RECV_BPAGES) < 0) {
        serverLog(LL_WARNING, "Couldn't init Homa receive buffer: %s", strerror(errno));
        close(fd);
        return C_ERR;
    }

    anetNonBlock(NULL, fd);
    anetCloexec(fd);
    listener->fd[listener->count++] = fd;
    homa_pool *pool = &homa_pools[homa_pools_count++];
    pool->fd = fd;
    pool->region = region;
    pool->region_size = region_size;
    pool->listener = listener;
    pool->peers = dictCreate(&homaPeerDictType);
    pool->pending = listCreate();
    serverLog(LL_NOTICE, "Homa listening on port %d (fd=%d)", listener->port, fd);
    return C_OK;
}

/* -------------------------------------------------------------------------
 * Dispatcher + per-peer processing
 * ------------------------------------------------------------------------- */

/* Make id the peer's current RPC and run its commands, whose bytes are
 * already in the querybuf. Their replies reach hc->reply_buf through
 * connHomaWrite; the pending-data hook answers the RPC once it is done, even
 * if it produced no output. May free hc. */
static void homaPeerRun(homa_connection *hc, uint64_t id) {
    client *c = connGetPrivateData(&hc->c);
    hc->cur_id = id;
    hc->reply_pending = 1;
    c->lastinteraction = server.unixtime;
    if (processInputBuffer(c) == C_OK)
        homaPeerMarkPending(hc);
}

/* Start the peer's next queued RPC, if any. May free hc. */
static void homaPeerNext(homa_connection *hc) {
    listNode *ln = listFirst(hc->rpc_queue);
    if (ln == NULL)
        return;
    homa_rpc *rpc = listNodeValue(ln);
    uint64_t id = rpc->id;
    client *c = connGetPrivateData(&hc->c);
    if (c->querybuf == NULL)
        c->querybuf = sdsempty();
    c->querybuf = sdscatsds(c->querybuf, rpc->msg);
    listDelNode(hc->rpc_queue, ln); /* frees rpc */
    homaPeerRun(hc, id);
}

/* Handle one just-received RPC. Its bytes must leave the receive pool now
 * (the next recvmsg recycles its bpages): straight into the querybuf if the
 * peer is idle, else into a queued homa_rpc. */
static void homaPeerOffer(homa_pool *pool, struct aeEventLoop *el,
                          struct sockaddr_in *peer, ssize_t len) {
    homa_connection *hc = homaPeerGet(pool, el, peer, pool->ctl.id);
    if (hc == NULL)
        return; /* rejected: the error is its reply */

    if (hc->reply_pending) {
        homa_rpc *rpc = zmalloc(sizeof(*rpc));
        rpc->id = pool->ctl.id;
        rpc->msg = sdsnewlen(SDS_NOINIT, len);
        homa_user_copy_msg(rpc->msg, len, pool->region, 0, len,
                           pool->ctl.num_bpages, pool->ctl.bpage_offsets);
        listAddNodeTail(hc->rpc_queue, rpc);
        return;
    }

    client *c = connGetPrivateData(&hc->c);
    if (c->querybuf == NULL)
        c->querybuf = sdsempty();
    size_t qblen = sdslen(c->querybuf);
    c->querybuf = sdsMakeRoomFor(c->querybuf, len);
    homa_user_copy_msg(c->querybuf + qblen, len, pool->region, 0, len,
                       pool->ctl.num_bpages, pool->ctl.bpage_offsets);
    sdsIncrLen(c->querybuf, len);
    homaPeerRun(hc, pool->ctl.id); /* may free hc — nothing touched after */
}

/* Bound the work done in one event-loop turn (same role as
 * max_new_conns_per_cycle for TCP accepts): leftover RPCs re-fire the fd. */
#define HOMA_DISPATCH_MAX 64

static void connHomaDispatchHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(mask);
    connListener *listener = privdata;

    int pi;
    for (pi = 0; pi < homa_pools_count; pi++)
        if (homa_pools[pi].fd == fd)
            break;
    if (pi == homa_pools_count)
        return;
    homa_pool *pool = &homa_pools[pi];

    for (int i = 0; i < HOMA_DISPATCH_MAX; i++) {
        struct sockaddr_in peer;
        /* id=0: receive any ready RPC. num_bpages/bpage_offsets carried in
         * the pool return the previous message's bpages to Homa for reuse. */
        pool->ctl.id = 0;
        struct msghdr hdr = { 0 };
        hdr.msg_name = &peer;
        hdr.msg_namelen = sizeof(peer);
        hdr.msg_control = &pool->ctl;
        hdr.msg_controllen = sizeof(pool->ctl);

        ssize_t n = recvmsg(fd, &hdr, 0);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EINTR)
                serverLog(LL_WARNING, "Homa recvmsg on port %d failed: %s",
                          listener->port, strerror(errno));
            return;
        }
        homaPeerOffer(pool, el, &peer, n);
    }
}

/* -------------------------------------------------------------------------
 * Reply path
 * ------------------------------------------------------------------------- */

/* The current RPC is done once all its commands have run -- none blocked,
 * waiting to be re-run after an unblock, or still executing (a script
 * yielding to the event loop) -- and all their replies sit in reply_buf. */
static int homaRpcDone(homa_connection *hc, client *c) {
    return hc->reply_pending && c && !clientHasPendingReplies(c) &&
           !(c->flags & (CLIENT_BLOCKED | CLIENT_UNBLOCKED | CLIENT_EXECUTING_COMMAND));
}

/* Answer cur_id with the buffered reply as its single response message, or
 * with HOMA_USER_NO_REPLY if its commands wrote nothing (CLIENT REPLY OFF/SKIP,
 * a command split across RPCs): Homa has no zero-length messages. A reply over
 * Homa's message limit becomes one error, so the client is not left waiting
 * (a client that pipelined several commands in the RPC still misses the rest).
 * Returns C_ERR, reply kept, when the socket is out of send memory (EAGAIN). */
static int homaSendReply(homa_connection *hc) {
    static const char too_big[] = "-ERR reply exceeds Homa's 1000000-byte message limit\r\n";
    char none = HOMA_USER_NO_REPLY;
    size_t len = hc->reply_buf ? sdslen(hc->reply_buf) : 0;
    struct iovec vec = { .iov_base = len ? hc->reply_buf : &none, .iov_len = len ? len : 1 };
    if (len > HOMA_MAX_MESSAGE_LENGTH)
        vec = (struct iovec){ .iov_base = (void *)too_big, .iov_len = sizeof(too_big) - 1 };
    ssize_t ret = homa_user_reply(hc->pool->fd, &vec, 1, (struct sockaddr *)&hc->peer,
                                  sizeof(hc->peer), hc->cur_id);
    if (ret < 0 && errno == EAGAIN)
        return C_ERR;
    if (ret < 0 || len > HOMA_MAX_MESSAGE_LENGTH) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &hc->peer.sin_addr, ip, sizeof(ip));
        serverLog(LL_WARNING, "Homa reply to %s:%u id=%llu (%zu bytes): %s",
                  ip, ntohs(hc->peer.sin_port), (unsigned long long)hc->cur_id, len,
                  ret < 0 ? strerror(errno) : "over the message limit, sent an error instead");
    }
    if (len)
        sdsclear(hc->reply_buf);
    hc->reply_pending = 0;
    return C_OK;
}

/* Homa reports EPOLLOUT once a socket that ran out of send memory has room again. */
static void homaPoolWritable(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(mask);
    homa_pool *pool = privdata;
    pool->tx_blocked = 0;
    aeDeleteFileEvent(el, fd, AE_WRITABLE);
}

/* Buffer a reply piece; it is sent when the RPC is done. Replies for
 * different RPCs never share a buffer: cur_id only advances after the previous
 * RPC has been answered. Output while no RPC is pending (a pub/sub push) has
 * no RPC to carry it and is dropped. */
static int connHomaWrite(connection *conn, const void *data, size_t data_len) {
    homa_connection *hc = (homa_connection *)conn;
    if (!hc->reply_pending)
        return data_len;
    if (hc->reply_buf == NULL)
        hc->reply_buf = sdsempty();
    hc->reply_buf = sdscatlen(hc->reply_buf, data, data_len);
    homaPeerMarkPending(hc);
    return data_len;
}

static int connHomaWritev(connection *conn, const struct iovec *iov, int iovcnt) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++)
        total += iov[i].iov_len;
    for (int i = 0; i < iovcnt; i++)
        connHomaWrite(conn, iov[i].iov_base, iov[i].iov_len);
    return total;
}

/* Peer connections never read from a fd; the dispatcher feeds them. */
static int connHomaRead(connection *conn, void *buf, size_t buf_len) {
    UNUSED(conn);
    UNUSED(buf);
    UNUSED(buf_len);
    return -1;
}

/* -------------------------------------------------------------------------
 * Pending-data hooks (send replies of done RPCs)
 * ------------------------------------------------------------------------- */

static int connHomaHasPendingData(struct aeEventLoop *el) {
    UNUSED(el);
    for (int i = 0; i < homa_pools_count; i++)
        if (listLength(homa_pools[i].pending) && !homa_pools[i].tx_blocked)
            return 1;
    return 0;
}

/* Homa is connectionless: a peer that goes away sends nothing, so without
 * this its client (and its slot against maxclients) would live forever. Reap
 * peers idle longer than `timeout`, or HOMA_PEER_IDLE_TIMEOUT when timeout is 0
 * (the default "never", which only works for TCP because TCP has FIN), with
 * the same exemptions as clientsCronHandleTimeout(). Trade-off: a peer that
 * comes back after that long gets a fresh client and has lost its session
 * state (SELECT, AUTH, MULTI). Runs at most once per second. */
#define HOMA_PEER_IDLE_TIMEOUT 60 /* seconds */
extern int ProcessingEventsWhileBlocked; /* networking.c */
static void homaReapIdlePeers(void) {
    static time_t last;
    /* Like clientsCron, never free clients while serving events from inside a
     * blocking operation (loading, long scripts). */
    if (ProcessingEventsWhileBlocked || server.unixtime == last)
        return;
    last = server.unixtime;
    time_t idle = server.maxidletime ? (time_t)server.maxidletime : HOMA_PEER_IDLE_TIMEOUT;
    for (int i = 0; i < homa_pools_count; i++) {
        dictIterator *di = dictGetSafeIterator(homa_pools[i].peers); /* freeClient deletes */
        dictEntry *de;
        while ((de = dictNext(di))) {
            homa_connection *hc = dictGetVal(de);
            client *c = connGetPrivateData(&hc->c);
            if (c && !(c->flags & (CLIENT_SLAVE | CLIENT_BLOCKED | CLIENT_PUBSUB)) &&
                !mustObeyClient(c) && server.unixtime - c->lastinteraction > idle)
                freeClient(c);
        }
        dictReleaseIterator(di);
    }
}

/* Visit only the peers queued by homaPeerRun/connHomaWrite. A peer whose RPC
 * is not done yet (blocked, say) leaves the list; its next write re-queues it.
 * Always pop the head and hold no other node: handling one peer may free
 * others (CLIENT KILL) or re-queue itself at the tail. */
static int connHomaProcessPendingData(struct aeEventLoop *el) {
    int processed = 0;
    homaReapIdlePeers();
    for (int i = 0; i < homa_pools_count; i++) {
        homa_pool *pool = &homa_pools[i];
        list *pending = pool->pending;
        if (pool->tx_blocked)
            continue;
        for (unsigned long n = listLength(pending); n && listLength(pending); n--) {
            listNode *ln = listFirst(pending);
            homa_connection *hc = listNodeValue(ln);
            listDelNode(pending, ln);
            hc->pending_node = NULL;
            client *c = connGetPrivateData(&hc->c);
            /* Redis stops writing after NET_MAX_WRITES_PER_EVENT (64 KB) and
             * would normally wait for a writable event — which peer
             * connections (fd == -1) never get. Drain the remainder here. */
            if (c && clientHasPendingReplies(c)) {
                writeToClient(c, 0);
                processed = 1;
            }
            if (homaRpcDone(hc, c)) {
                if (homaSendReply(hc) == C_ERR) { /* out of send memory: resume on EPOLLOUT */
                    homaPeerMarkPending(hc);
                    pool->tx_blocked = 1;
                    aeCreateFileEvent(el, pool->fd, AE_WRITABLE, homaPoolWritable, pool);
                    break;
                }
                homaPeerNext(hc); /* may free hc */
                processed = 1;
            }
        }
    }
    return processed;
}

/* -------------------------------------------------------------------------
 * Close / misc
 * ------------------------------------------------------------------------- */

static void connHomaClose(connection *conn) {
    if (connHasRefs(conn)) {
        conn->flags |= CONN_FLAG_CLOSE_SCHEDULED;
        return;
    }
    homa_connection *hc = (homa_connection *)conn;
    /* fd is always -1 (nothing in the event loop, nothing to close); just
     * unlink the peer. The shared listen socket stays put. */
    dictDelete(hc->pool->peers, homaPeerKey(&hc->peer));
    if (hc->pending_node)
        listDelNode(hc->pool->pending, hc->pending_node);
    /* Answer every RPC once, so none is left waiting in Homa: the current one
     * with what was already written, like close() on a TCP socket (-ERR/-DENIED
     * on admission, +OK to QUIT), the queued ones -- never run -- with NO_REPLY.
     * Out of send memory here, a response is lost: the peer is going away. */
    if (hc->reply_pending)
        homaSendReply(hc);
    for (listNode *ln; (ln = listFirst(hc->rpc_queue)); listDelNode(hc->rpc_queue, ln)) {
        hc->cur_id = ((homa_rpc *)listNodeValue(ln))->id;
        homaSendReply(hc);
    }
    listRelease(hc->rpc_queue);
    sdsfree(hc->reply_buf);
    zfree(conn);
}


static void connHomaShutdown(connection *conn) {
    UNUSED(conn); /* nothing to half-close on a connectionless socket */
}

/* --- delegated-to-TCP callbacks --------------------------------------- */

static void connHomaEventHandler(struct aeEventLoop *el, int fd, void *clientData, int mask) {
    connectionTypeTcp()->ae_handler(el, fd, clientData, mask);
}

static int connHomaAddr(connection *conn, char *ip, size_t ip_len, int *port, int remote) {
    homa_connection *hc = (homa_connection *)conn;
    if (conn->fd == -1 && remote) { /* peer connection: answer from the stash */
        if (ip)
            inet_ntop(AF_INET, &hc->peer.sin_addr, ip, ip_len);
        if (port)
            *port = ntohs(hc->peer.sin_port);
        return C_OK;
    }
    return connectionTypeTcp()->addr(conn, ip, ip_len, port, remote);
}

/* Loopback peers only, like connSocketIsLocal(): this gates protected mode. */
static int connHomaIsLocal(connection *conn) {
    homa_connection *hc = (homa_connection *)conn;
    return (ntohl(hc->peer.sin_addr.s_addr) >> 24) == 127;
}

static int connHomaAccept(connection *conn, ConnectionCallbackFunc accept_handler) {
    return connectionTypeTcp()->accept(conn, accept_handler);
}

/* Peer connections have fd == -1 and are driven by the dispatcher: record
 * the handler (redis expects it) but never register anything with the event
 * loop. */
static int connHomaSetReadHandler(connection *conn, ConnectionCallbackFunc func) {
    if (conn->fd == -1) {
        conn->read_handler = func;
        return C_OK;
    }
    return connectionTypeTcp()->set_read_handler(conn, func);
}

static int connHomaSetWriteHandler(connection *conn, ConnectionCallbackFunc func, int barrier) {
    if (conn->fd == -1) {
        conn->write_handler = func;
        if (func && barrier)
            conn->flags |= CONN_FLAG_WRITE_BARRIER;
        return C_OK;
    }
    return connectionTypeTcp()->set_write_handler(conn, func, barrier);
}

static int connHomaRebindEventLoop(connection *conn, aeEventLoop *el) {
    return connectionTypeTcp()->rebind_event_loop(conn, el);
}

static const char *connHomaGetLastError(connection *conn) {
    return strerror(conn->last_errno);
}

static ssize_t connHomaSyncWrite(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncWrite(conn->fd, ptr, size, timeout);
}

static ssize_t connHomaSyncRead(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncRead(conn->fd, ptr, size, timeout);
}

static ssize_t connHomaSyncReadLine(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncReadLine(conn->fd, ptr, size, timeout);
}

/* ------------------------------------------------------------------------- */

static ConnectionType CT_Homa = {
    .get_type = connHomaGetType,

    .init = NULL,
    .cleanup = NULL,
    .configure = NULL,

    .ae_handler = connHomaEventHandler,
    .accept_handler = connHomaDispatchHandler,
    .addr = connHomaAddr,
    .is_local = connHomaIsLocal,
    .listen = connHomaListen,

    .conn_create = NULL,
    .conn_create_accepted = NULL,
    .shutdown = connHomaShutdown,
    .close = connHomaClose,

    .connect = NULL,
    .blocking_connect = NULL,
    .accept = connHomaAccept,

    .unbind_event_loop = NULL,
    .rebind_event_loop = connHomaRebindEventLoop,

    .write = connHomaWrite,
    .writev = connHomaWritev,
    .read = connHomaRead,
    .set_write_handler = connHomaSetWriteHandler,
    .set_read_handler = connHomaSetReadHandler,
    .get_last_error = connHomaGetLastError,
    .sync_write = connHomaSyncWrite,
    .sync_read = connHomaSyncRead,
    .sync_readline = connHomaSyncReadLine,

    .has_pending_data = connHomaHasPendingData,
    .process_pending_data = connHomaProcessPendingData,
};

int RedisRegisterConnectionTypeHoma(void) {
    return connTypeRegister(&CT_Homa);
}
