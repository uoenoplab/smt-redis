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

static ConnectionType CT_HomaLs;

static const char *connHomaLsGetType(connection *conn) {
    UNUSED(conn);

    return CONN_TYPE_HOMALS;
}

static void connHomaLsEventHandler(struct aeEventLoop *el, int fd, void *clientData, int mask) {
//serverLog(LL_NOTICE, "connHomaLsEventHandler");
    connectionTypeHomaLs()->ae_handler(el, fd, clientData, mask);
}

static int connHomaLsAddr(connection *conn, char *ip, size_t ip_len, int *port, int remote) {
//serverLog(LL_NOTICE, "connHomaLsAddr");
    return connectionTypeHomaLs()->addr(conn, ip, ip_len, port, remote);
}

static int connHomaLsIsLocal(connection *conn) {
    UNUSED(conn);

    return 1; /* Homa socket is always local connection */
}

static int connHomaLsListen(connListener *listener) {
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
            serverLog(LL_WARNING, "Failed opening HomaLs socket: %s", server.neterr);
            exit(1);
        }

        if (bind(fd, (struct sockaddr*)addr, sizeof(struct sockaddr_in)) == -1) {
            serverLog(LL_WARNING, "Couldn't bind HomaLs: %s", strerror(errno));
            exit(1);
        }

        if (init_recv_args_per_conn(fd, (char**)&listener->priv) != 0) {
            serverLog(LL_WARNING, "Couldn't init HomaLs recv buffer: %s", strerror(errno));
            exit(1);
        }

        anetNonBlock(NULL, fd);
        anetCloexec(fd);
        listener->fd[listener->count++] = fd;
serverLog(LL_NOTICE, "HomaLs now listening to fd=%d binded to port=%d", fd, listener->port);
    }

    return C_OK;
}

static connection *connCreateHomaLs(void) {
    connection *conn = zcalloc(sizeof(connection));
    conn->type = &CT_HomaLs;
    conn->fd = -1;
    //conn->iovcnt = IOV_MAX;
    conn->iovcnt = 1024;
//serverLog(LL_NOTICE, "connCreateHoma");
    return conn;
}

static connection *connCreateAcceptedHomaLs(int fd, void *priv) {
    UNUSED(priv);
    connection *conn = connCreateHomaLs();
    conn->fd = fd;
    conn->state = CONN_STATE_ACCEPTING;
    serverLog(LL_NOTICE,"Accepted connection to HomaLS fd=%d", fd);
    return conn;
}

static void connHomaLsAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cfd, max = MAX_ACCEPTS_PER_CALL;
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    // TODO somehow add this conn to a data structure
    serverLog(LL_NOTICE,"Accepting connection to HomaLS fd=%d", fd);
    acceptCommonHandler(connCreateAcceptedHomaLs(fd, NULL),0,NULL);  // we dont care about special handling for now
}

static void connHomaLsShutdown(connection *conn) {
    connectionTypeHoma()->shutdown(conn);
}

static void connHomaLsClose(connection *conn) {
// TODO connectionless, never close server FD. instead remove from data structure
    return connectionTypeHoma()->close(conn);
}

static int connHomaLsAccept(connection *conn, ConnectionCallbackFunc accept_handler) {
// TODO add server fd to epoll loop again -> CTRL_MOD, should be fine?
    return connectionTypeHoma()->accept(conn, accept_handler);
}

static int connHomaLsWrite(connection *conn, const void *data, size_t data_len) {
    return connectionTypeHoma()->write(conn, data, data_len);
}

static int connHomaLsWritev(connection *conn, const struct iovec *iov, int iovcnt) {
    return connectionTypeHoma()->writev(conn, iov, iovcnt);
}

static int connHomaLsRead(connection *conn, void *buf, size_t buf_len) {
    return connectionTypeHoma()->read(conn, buf, buf_len);
}

static int connHomaLsSetWriteHandler(connection *conn, ConnectionCallbackFunc func, int barrier) {
    return connectionTypeHoma()->set_write_handler(conn, func, barrier);
}

static int connHomaLsSetReadHandler(connection *conn, ConnectionCallbackFunc func) {
    return connectionTypeHoma()->set_read_handler(conn, func);
}

static const char *connHomaLsGetLastError(connection *conn) {
    return strerror(conn->last_errno);
}

static ssize_t connHomaLsSyncWrite(connection *conn, char *ptr, ssize_t size, long long timeout) {
//serverLog(LL_NOTICE, "connHomaLsSyncWrite");
    return syncWrite(conn->fd, ptr, size, timeout);
}

static ssize_t connHomaLsSyncRead(connection *conn, char *ptr, ssize_t size, long long timeout) {
//serverLog(LL_NOTICE, "connHomaLsSyncRead");
    return syncRead(conn->fd, ptr, size, timeout);
}

static ssize_t connHomaLsSyncReadLine(connection *conn, char *ptr, ssize_t size, long long timeout) {
//serverLog(LL_NOTICE, "connHomaLsSyncReadLine");
    return syncReadLine(conn->fd, ptr, size, timeout);
}

static ConnectionType CT_HomaLs = {
    /* connection type */
    .get_type = connHomaLsGetType,

    /* connection type initialize & finalize & configure */
    .init = NULL,
    .cleanup = NULL,
    .configure = NULL,

    /* ae & accept & listen & error & address handler */
    .ae_handler = connHomaLsEventHandler,
    .accept_handler = connHomaLsAcceptHandler,
    .addr = connHomaLsAddr,
    .is_local = connHomaLsIsLocal,
    .listen = connHomaLsListen,

    /* create/shutdown/close connection */
    .conn_create = connCreateHomaLs,
    .conn_create_accepted = connCreateAcceptedHomaLs,
    .shutdown = connHomaLsShutdown,
    .close = connHomaLsClose,

    /* connect & accept */
    .connect = NULL,
    .blocking_connect = NULL,
    .accept = connHomaLsAccept,

    /* IO */
    .write = connHomaLsWrite,
    .writev = connHomaLsWritev,
    .read = connHomaLsRead,
    .set_write_handler = connHomaLsSetWriteHandler,
    .set_read_handler = connHomaLsSetReadHandler,
    .get_last_error = connHomaLsGetLastError,
    .sync_write = connHomaLsSyncWrite,
    .sync_read = connHomaLsSyncRead,
    .sync_readline = connHomaLsSyncReadLine,

    /* pending data */
    .has_pending_data = NULL,
    .process_pending_data = NULL,
};

int RedisRegisterConnectionTypeHomaLs(void)
{
    return connTypeRegister(&CT_HomaLs);
}
