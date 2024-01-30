/* ==========================================================================
 * unix.c - unix socket connection implementation
 * --------------------------------------------------------------------------
 * Copyright (C) 2022  zhenwei pi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the
 * following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 * NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 * ==========================================================================
 */
 
#include "homa.h"
#include "homa_hl.h"

#include "server.h"
#include "connection.h"

static ConnectionType CT_Homa;

static const char *connHomaGetType(connection *conn) {
    UNUSED(conn);

    return CONN_TYPE_HOMA;
}

static void connHomaEventHandler(struct aeEventLoop *el, int fd, void *clientData, int mask) {
serverLog(LL_NOTICE, "connHomaEventHandler");
    connectionTypeTcp()->ae_handler(el, fd, clientData, mask);
}

static int connHomaAddr(connection *conn, char *ip, size_t ip_len, int *port, int remote) {
serverLog(LL_NOTICE, "connHomaAddr");
    return connectionTypeTcp()->addr(conn, ip, ip_len, port, remote);
}

static int connHomaIsLocal(connection *conn) {
    UNUSED(conn);

    return 1; /* Homa socket is always local connection */
}

static int connHomaListen(connListener *listener) {
    int fd;
    mode_t *perm = (mode_t *)listener->priv;

    if (listener->bindaddr_count == 0)
        return C_OK;

    /* currently listener->bindaddr_count is always 1, we still use a loop here in case Redis supports multi Homa socket in the future */
    for (int j = 0; j < listener->bindaddr_count; j++) {
        struct sockaddr_in *addr = zcalloc(sizeof(struct sockaddr_in)); // TODO where to free this?
        memset(addr, 0, sizeof(struct sockaddr_in));
        addr->sin_family = AF_INET;
        addr->sin_addr.s_addr = INADDR_ANY;
        addr->sin_port = htons(listener->port);

	fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_HOMA);
        if (fd == ANET_ERR) {
            serverLog(LL_WARNING, "Failed opening Homa socket: %s", server.neterr);
            exit(1);
        }

        if (bind(fd, (struct sockaddr*)addr, sizeof(struct sockaddr_in)) == -1) {
            serverLog(LL_WARNING, "Couldn't bind Homa: %s", strerror(errno));
            exit(1);
        }

        if (init_recv_args(fd, addr, sizeof(struct sockaddr_in)) < 0) {
            serverLog(LL_WARNING, "Couldn't init Homa recv buffer: %s", strerror(errno));
            exit(1);
        }

        anetNonBlock(NULL, fd);
        anetCloexec(fd);
        listener->fd[listener->count++] = fd;
serverLog(LL_NOTICE, "Homa now listening to fd=%d binded to port=%d", fd, listener->port);
    }

    return C_OK;
}

static connection *connCreateHoma(void) {
    connection *conn = zcalloc(sizeof(connection));
    conn->type = &CT_Homa;
    conn->fd = -1;
    //conn->iovcnt = IOV_MAX;
    conn->iovcnt = 1024;
serverLog(LL_NOTICE, "connCreateHoma");
    return conn;
}

static connection *connCreateAcceptedHoma(int fd, void *priv) {
    UNUSED(priv);
    connection *conn = connCreateHoma();
    conn->fd = fd;
    conn->state = CONN_STATE_ACCEPTING;
    serverLog(LL_NOTICE,"Accepted connection to homa fd=%d", fd);
    return conn;
}

static void connHomaAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cfd, max = MAX_ACCEPTS_PER_CALL;
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    // TODO somehow add this conn to a data structure
    serverLog(LL_NOTICE,"Accepting connection to homa fd=%d", fd);
    acceptCommonHandler(connCreateAcceptedHoma(fd, NULL),0,NULL);  // we dont care about special handling for now
}

static void connHomaShutdown(connection *conn) {
serverLog(LL_NOTICE, "connSetHomaShutdown");
    connectionTypeHoma()->shutdown(conn);
}

static void connHomaClose(connection *conn) {
serverLog(LL_NOTICE, "connSetHomaClose");
// TODO connectionless, never close server FD. instead remove from data structure
    //connectionTypeTcp()->close(conn);
}

static int connHomaAccept(connection *conn, ConnectionCallbackFunc accept_handler) {
printf("connHomaAccept\n");
// TODO add server fd to epoll loop again -> CTRL_MOD, should be fine?
    return connectionTypeTcp()->accept(conn, accept_handler);
}

static int connHomaWrite(connection *conn, const void *data, size_t data_len) {
serverLog(LL_NOTICE, "connHomaWrite");
    int ret = homa_reply(conn->fd, data, data_len, (sockaddr_in_union *)conn->saddr, control.id);
    if (ret < 0) {
        serverLog(LL_WARNING, "connHomaWrite: homa_reply error: %s", strerror(errno));
    }
    else
        ret = data_len;

    connHomaClose(conn);
    return ret;
}

static int connHomaWritev(connection *conn, const struct iovec *iov, int iovcnt) {
    char client_ip[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &conn->saddr->sin_addr, client_ip, INET_ADDRSTRLEN) == NULL) {
        serverLog(LL_NOTICE, "Couldn't convert client address to string (inet_ntop): %s", strerror(errno));
        return -1;
    }

    ssize_t nwritten = 0;
    for (size_t i = 0; i < iovcnt; i++) {
        serverLog(LL_NOTICE, "iov[%d] len=%ld %.8s", i, iov[i].iov_len, (char*)iov[i].iov_base);
        nwritten += iov[i].iov_len;
    }

    serverLog(LL_NOTICE, "sending %ld bytes to fd=%d (ip %s, port %hu, iovcnt %d, rpcid %ld, num_bpages %d):",
        nwritten, conn->fd, client_ip, ntohs(conn->saddr->sin_port), iovcnt, control.id, control.num_bpages);

    int ret = homa_replyv(conn->fd, iov, iovcnt, (sockaddr_in_union*)conn->saddr, control.id);
    if (ret < 0) {
        serverLog(LL_WARNING, "Homa replyv error: %s", strerror(errno));
	exit(1);
	return -1;
    }
    serverLog(LL_NOTICE, "connHomaWritev control.id=%ld ret=%d nwritten=%ld", control.id, ret, nwritten);
    return nwritten;
}

static int connHomaRead(connection *conn, void *buf, size_t buf_len) {
serverLog(LL_NOTICE, "connHomaRead");
    uint64_t *rpcid = &control.id;
    ssize_t reqlen = 0;
    int ret = 0;

    control.flags = HOMA_RECVMSG_REQUEST;
    control.id = 0;

    reqlen = recvmsg(conn->fd, &hdr, 0);

    if (reqlen <= 0) {
        serverLog(LL_WARNING, "Couldn't receive Homa msg: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_in *client_addr = (struct sockaddr_in*)hdr.msg_name;
    if (!conn->saddr) {
        conn->saddr = zmalloc(sizeof(struct sockaddr_in));
        memcpy(conn->saddr, client_addr, sizeof(struct sockaddr_in));
    }

    char client_ip[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &(client_addr->sin_addr), client_ip, INET_ADDRSTRLEN) == NULL) {
        serverLog(LL_NOTICE, "Couldn't convert client address to string (inet_ntop): %s", strerror(errno));
        return -1;
    }
    serverLog(LL_NOTICE, "Server recv (ip %s, port %hu, reqlen %ld, rpcid %ld, num_bpages %d):",
        client_ip, ntohs(client_addr->sin_port), reqlen, *rpcid, control.num_bpages);
    if (buf_len < reqlen) {
        serverLog(LL_WARNING, "connHomaRead buf_len=%ld but read=%ld", buf_len, reqlen);
        return -1;
    }

    memcpy(conn->saddr, client_addr, sizeof(struct sockaddr_in));
    memcpy(buf, &recv_buf_region[control.bpage_offsets[0]], reqlen);
    serverLog(LL_NOTICE, "%s", (char*)buf);

    return reqlen;
}

static int connHomaSetWriteHandler(connection *conn, ConnectionCallbackFunc func, int barrier) {
serverLog(LL_NOTICE, "connHomaSetWriteHandler");
    return connectionTypeTcp()->set_write_handler(conn, func, barrier);
}

static int connHomaSetReadHandler(connection *conn, ConnectionCallbackFunc func) {
serverLog(LL_NOTICE, "connHomaReadHandler");
    return connectionTypeTcp()->set_read_handler(conn, func);
}

static const char *connHomaGetLastError(connection *conn) {
    return strerror(conn->last_errno);
}

static ssize_t connHomaSyncWrite(connection *conn, char *ptr, ssize_t size, long long timeout) {
serverLog(LL_NOTICE, "connHomaSyncWrite");
    return syncWrite(conn->fd, ptr, size, timeout);
}

static ssize_t connHomaSyncRead(connection *conn, char *ptr, ssize_t size, long long timeout) {
serverLog(LL_NOTICE, "connHomaSyncRead");
    return syncRead(conn->fd, ptr, size, timeout);
}

static ssize_t connHomaSyncReadLine(connection *conn, char *ptr, ssize_t size, long long timeout) {
serverLog(LL_NOTICE, "connHomaSyncReadLine");
    return syncReadLine(conn->fd, ptr, size, timeout);
}

static ConnectionType CT_Homa = {
    /* connection type */
    .get_type = connHomaGetType,

    /* connection type initialize & finalize & configure */
    .init = NULL,
    .cleanup = NULL,
    .configure = NULL,

    /* ae & accept & listen & error & address handler */
    .ae_handler = connHomaEventHandler,
    .accept_handler = connHomaAcceptHandler,
    .addr = connHomaAddr,
    .is_local = connHomaIsLocal,
    .listen = connHomaListen,

    /* create/shutdown/close connection */
    .conn_create = connCreateHoma,
    .conn_create_accepted = connCreateAcceptedHoma,
    .shutdown = connHomaShutdown,
    .close = connHomaClose,

    /* connect & accept */
    .connect = NULL,
    .blocking_connect = NULL,
    .accept = connHomaAccept,

    /* IO */
    .write = connHomaWrite,
    .writev = connHomaWritev,
    .read = connHomaRead,
    .set_write_handler = connHomaSetWriteHandler,
    .set_read_handler = connHomaSetReadHandler,
    .get_last_error = connHomaGetLastError,
    .sync_write = connHomaSyncWrite,
    .sync_read = connHomaSyncRead,
    .sync_readline = connHomaSyncReadLine,

    /* pending data */
    .has_pending_data = NULL,
    .process_pending_data = NULL,
};

int RedisRegisterConnectionTypeHoma(void)
{
    return connTypeRegister(&CT_Homa);
}
