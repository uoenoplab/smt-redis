/* Extracted from anet.c to work properly with Hiredis error reporting.
 *
 * Copyright (c) 2009-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2014, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2015, Matt Stancliff <matt at genges dot com>,
 *                     Jan-Erik Rediger <janerik at fnordig dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fmacros.h"
#include <sys/types.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>

#include "net.h"
#include "sds.h"
#include "sockcompat.h"
#include "win32.h"

/* Defined in hiredis.c */
void __redisSetError(redisContext *c, int type, const char *str);

int redisContextUpdateCommandTimeout(redisContext *c, const struct timeval *timeout);

void redisNetClose(redisContext *c) {
    if (c && c->fd != REDIS_INVALID_FD) {
        close(c->fd);
        c->fd = REDIS_INVALID_FD;
    }
}

ssize_t redisNetRead(redisContext *c, char *buf, size_t bufcap) {
    ssize_t nread = recv(c->fd, buf, bufcap, 0);
    if (nread == -1) {
        if ((errno == EWOULDBLOCK && !(c->flags & REDIS_BLOCK)) || (errno == EINTR)) {
            /* Try again later */
            return 0;
        } else if(errno == ETIMEDOUT && (c->flags & REDIS_BLOCK)) {
            /* especially in windows */
            __redisSetError(c, REDIS_ERR_TIMEOUT, "recv timeout");
            return -1;
        } else {
            __redisSetError(c, REDIS_ERR_IO, strerror(errno));
            return -1;
        }
    } else if (nread == 0) {
        __redisSetError(c, REDIS_ERR_EOF, "Server closed the connection");
        return -1;
    } else {
        return nread;
    }
}

ssize_t redisNetWrite(redisContext *c) {
    ssize_t nwritten;

    nwritten = send(c->fd, c->obuf, hi_sdslen(c->obuf), 0);
    if (nwritten < 0) {
        if ((errno == EWOULDBLOCK && !(c->flags & REDIS_BLOCK)) || (errno == EINTR)) {
            /* Try again */
            return 0;
        } else {
            __redisSetError(c, REDIS_ERR_IO, strerror(errno));
            return -1;
        }
    }

    return nwritten;
}

static void __redisSetErrorFromErrno(redisContext *c, int type, const char *prefix) {
    int errorno = errno;  /* snprintf() may change errno */
    char buf[128] = { 0 };
    size_t len = 0;

    if (prefix != NULL)
        len = snprintf(buf,sizeof(buf),"%s: ",prefix);
    strerror_r(errorno, (char *)(buf + len), sizeof(buf) - len);
    __redisSetError(c,type,buf);
}

static int redisSetReuseAddr(redisContext *c) {
    int on = 1;
    if (setsockopt(c->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
        redisNetClose(c);
        return REDIS_ERR;
    }
    return REDIS_OK;
}

static int redisCreateSocket(redisContext *c, int type) {
    redisFD s;
    if ((s = socket(type, SOCK_STREAM, 0)) == REDIS_INVALID_FD) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
        return REDIS_ERR;
    }
    c->fd = s;
    if (type == AF_INET) {
        if (redisSetReuseAddr(c) == REDIS_ERR) {
            return REDIS_ERR;
        }
    }
    return REDIS_OK;
}

static int redisSetBlocking(redisContext *c, int blocking) {
#ifndef _WIN32
    int flags;

    /* Set the socket nonblocking.
     * Note that fcntl(2) for F_GETFL and F_SETFL can't be
     * interrupted by a signal. */
    if ((flags = fcntl(c->fd, F_GETFL)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"fcntl(F_GETFL)");
        redisNetClose(c);
        return REDIS_ERR;
    }

    if (blocking)
        flags &= ~O_NONBLOCK;
    else
        flags |= O_NONBLOCK;

    if (fcntl(c->fd, F_SETFL, flags) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"fcntl(F_SETFL)");
        redisNetClose(c);
        return REDIS_ERR;
    }
#else
    u_long mode = blocking ? 0 : 1;
    if (ioctl(c->fd, FIONBIO, &mode) == -1) {
        __redisSetErrorFromErrno(c, REDIS_ERR_IO, "ioctl(FIONBIO)");
        redisNetClose(c);
        return REDIS_ERR;
    }
#endif /* _WIN32 */
    return REDIS_OK;
}

int redisKeepAlive(redisContext *c, int interval) {
    int val = 1;
    redisFD fd = c->fd;

#ifndef _WIN32
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) == -1){
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }

    val = interval;

#if defined(__APPLE__) && defined(__MACH__)
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &val, sizeof(val)) < 0) {
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }
#else
#if defined(__GLIBC__) && !defined(__FreeBSD_kernel__)
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val)) < 0) {
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }

    val = interval/3;
    if (val == 0) val = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val)) < 0) {
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }

    val = 3;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)) < 0) {
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }
#endif
#endif
#else
    int res;

    res = win32_redisKeepAlive(fd, interval * 1000);
    if (res != 0) {
        __redisSetError(c, REDIS_ERR_OTHER, strerror(res));
        return REDIS_ERR;
    }
#endif
    return REDIS_OK;
}

int redisSetTcpNoDelay(redisContext *c) {
    int yes = 1;
    if (setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(TCP_NODELAY)");
        redisNetClose(c);
        return REDIS_ERR;
    }
    return REDIS_OK;
}

int redisContextSetTcpUserTimeout(redisContext *c, unsigned int timeout) {
    int res;
#ifdef TCP_USER_TIMEOUT
    res = setsockopt(c->fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &timeout, sizeof(timeout));
#else
    res = -1;
    errno = ENOTSUP;
    (void)timeout;
#endif
    if (res == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(TCP_USER_TIMEOUT)");
        redisNetClose(c);
        return REDIS_ERR;
    }
    return REDIS_OK;
}

#define __MAX_MSEC (((LONG_MAX) - 999) / 1000)

static int redisContextTimeoutMsec(redisContext *c, long *result)
{
    const struct timeval *timeout = c->connect_timeout;
    long msec = -1;

    /* Only use timeout when not NULL. */
    if (timeout != NULL) {
        if (timeout->tv_usec > 1000000 || timeout->tv_sec > __MAX_MSEC) {
            __redisSetError(c, REDIS_ERR_IO, "Invalid timeout specified");
            *result = msec;
            return REDIS_ERR;
        }

        msec = (timeout->tv_sec * 1000) + ((timeout->tv_usec + 999) / 1000);

        if (msec < 0 || msec > INT_MAX) {
            msec = INT_MAX;
        }
    }

    *result = msec;
    return REDIS_OK;
}

static int redisContextWaitReady(redisContext *c, long msec) {
    struct pollfd   wfd[1];

    wfd[0].fd     = c->fd;
    wfd[0].events = POLLOUT;

    if (errno == EINPROGRESS) {
        int res;

        if ((res = poll(wfd, 1, msec)) == -1) {
            __redisSetErrorFromErrno(c, REDIS_ERR_IO, "poll(2)");
            redisNetClose(c);
            return REDIS_ERR;
        } else if (res == 0) {
            errno = ETIMEDOUT;
            __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
            redisNetClose(c);
            return REDIS_ERR;
        }

        if (redisCheckConnectDone(c, &res) != REDIS_OK || res == 0) {
            redisCheckSocketError(c);
            return REDIS_ERR;
        }

        return REDIS_OK;
    }

    __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
    redisNetClose(c);
    return REDIS_ERR;
}

int redisCheckConnectDone(redisContext *c, int *completed) {
    int rc = connect(c->fd, (const struct sockaddr *)c->saddr, c->addrlen);
    if (rc == 0) {
        *completed = 1;
        return REDIS_OK;
    }
    int error = errno;
    if (error == EINPROGRESS) {
        /* must check error to see if connect failed.  Get the socket error */
        int fail, so_error;
        socklen_t optlen = sizeof(so_error);
        fail = getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen);
        if (fail == 0) {
            if (so_error == 0) {
                /* Socket is connected! */
                *completed = 1;
                return REDIS_OK;
            }
            /* connection error; */
            errno = so_error;
            error = so_error;
        }
    }
    switch (error) {
    case EISCONN:
        *completed = 1;
        return REDIS_OK;
    case EALREADY:
    case EWOULDBLOCK:
        *completed = 0;
        return REDIS_OK;
    default:
        return REDIS_ERR;
    }
}

int redisCheckSocketError(redisContext *c) {
    int err = 0, errno_saved = errno;
    socklen_t errlen = sizeof(err);

    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &errlen) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"getsockopt(SO_ERROR)");
        return REDIS_ERR;
    }

    if (err == 0) {
        err = errno_saved;
    }

    if (err) {
        errno = err;
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
        return REDIS_ERR;
    }

    return REDIS_OK;
}

int redisContextSetTimeout(redisContext *c, const struct timeval tv) {
    const void *to_ptr = &tv;
    size_t to_sz = sizeof(tv);

    if (redisContextUpdateCommandTimeout(c, &tv) != REDIS_OK) {
        __redisSetError(c, REDIS_ERR_OOM, "Out of memory");
        return REDIS_ERR;
    }
    if (setsockopt(c->fd,SOL_SOCKET,SO_RCVTIMEO,to_ptr,to_sz) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(SO_RCVTIMEO)");
        return REDIS_ERR;
    }
    if (setsockopt(c->fd,SOL_SOCKET,SO_SNDTIMEO,to_ptr,to_sz) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(SO_SNDTIMEO)");
        return REDIS_ERR;
    }
    return REDIS_OK;
}

int redisContextUpdateConnectTimeout(redisContext *c, const struct timeval *timeout) {
    /* Same timeval struct, short circuit */
    if (c->connect_timeout == timeout)
        return REDIS_OK;

    /* Allocate context timeval if we need to */
    if (c->connect_timeout == NULL) {
        c->connect_timeout = hi_malloc(sizeof(*c->connect_timeout));
        if (c->connect_timeout == NULL)
            return REDIS_ERR;
    }

    memcpy(c->connect_timeout, timeout, sizeof(*c->connect_timeout));
    return REDIS_OK;
}

int redisContextUpdateCommandTimeout(redisContext *c, const struct timeval *timeout) {
    /* Same timeval struct, short circuit */
    if (c->command_timeout == timeout)
        return REDIS_OK;

    /* Allocate context timeval if we need to */
    if (c->command_timeout == NULL) {
        c->command_timeout = hi_malloc(sizeof(*c->command_timeout));
        if (c->command_timeout == NULL)
            return REDIS_ERR;
    }

    memcpy(c->command_timeout, timeout, sizeof(*c->command_timeout));
    return REDIS_OK;
}

static int _redisContextConnectTcp(redisContext *c, const char *addr, int port,
                                   const struct timeval *timeout,
                                   const char *source_addr) {
    redisFD s;
    int rv, n;
    char _port[6];  /* strlen("65535"); */
    struct addrinfo hints, *servinfo, *bservinfo, *p, *b;
    int blocking = (c->flags & REDIS_BLOCK);
    int reuseaddr = (c->flags & REDIS_REUSEADDR);
    int reuses = 0;
    long timeout_msec = -1;

    servinfo = NULL;
    c->connection_type = REDIS_CONN_TCP;
    c->tcp.port = port;

    /* We need to take possession of the passed parameters
     * to make them reusable for a reconnect.
     * We also carefully check we don't free data we already own,
     * as in the case of the reconnect method.
     *
     * This is a bit ugly, but atleast it works and doesn't leak memory.
     **/
    if (c->tcp.host != addr) {
        hi_free(c->tcp.host);

        c->tcp.host = hi_strdup(addr);
        if (c->tcp.host == NULL)
            goto oom;
    }

    if (timeout) {
        if (redisContextUpdateConnectTimeout(c, timeout) == REDIS_ERR)
            goto oom;
    } else {
        hi_free(c->connect_timeout);
        c->connect_timeout = NULL;
    }

    if (redisContextTimeoutMsec(c, &timeout_msec) != REDIS_OK) {
        goto error;
    }

    if (source_addr == NULL) {
        hi_free(c->tcp.source_addr);
        c->tcp.source_addr = NULL;
    } else if (c->tcp.source_addr != source_addr) {
        hi_free(c->tcp.source_addr);
        c->tcp.source_addr = hi_strdup(source_addr);
    }

    snprintf(_port, 6, "%d", port);
    memset(&hints,0,sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    /* DNS lookup. To use dual stack, set both flags to prefer both IPv4 and
     * IPv6. By default, for historical reasons, we try IPv4 first and then we
     * try IPv6 only if no IPv4 address was found. */
    if (c->flags & REDIS_PREFER_IPV6 && c->flags & REDIS_PREFER_IPV4)
        hints.ai_family = AF_UNSPEC;
    else if (c->flags & REDIS_PREFER_IPV6)
        hints.ai_family = AF_INET6;
    else
        hints.ai_family = AF_INET;

    rv = getaddrinfo(c->tcp.host, _port, &hints, &servinfo);
    if (rv != 0 && hints.ai_family != AF_UNSPEC) {
        /* Try again with the other IP version. */
        hints.ai_family = (hints.ai_family == AF_INET) ? AF_INET6 : AF_INET;
        rv = getaddrinfo(c->tcp.host, _port, &hints, &servinfo);
    }
    if (rv != 0) {
        __redisSetError(c, REDIS_ERR_OTHER, gai_strerror(rv));
        return REDIS_ERR;
    }
    for (p = servinfo; p != NULL; p = p->ai_next) {
addrretry:
        if ((s = socket(p->ai_family,p->ai_socktype,p->ai_protocol)) == REDIS_INVALID_FD)
            continue;

        c->fd = s;
        if (redisSetBlocking(c,0) != REDIS_OK)
            goto error;
        if (c->tcp.source_addr) {
            int bound = 0;
            /* Using getaddrinfo saves us from self-determining IPv4 vs IPv6 */
            if ((rv = getaddrinfo(c->tcp.source_addr, NULL, &hints, &bservinfo)) != 0) {
                char buf[128];
                snprintf(buf,sizeof(buf),"Can't get addr: %s",gai_strerror(rv));
                __redisSetError(c,REDIS_ERR_OTHER,buf);
                goto error;
            }

            if (reuseaddr) {
                n = 1;
                if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*) &n,
                               sizeof(n)) < 0) {
                    freeaddrinfo(bservinfo);
                    goto error;
                }
            }

            for (b = bservinfo; b != NULL; b = b->ai_next) {
                if (bind(s,b->ai_addr,b->ai_addrlen) != -1) {
                    bound = 1;
                    break;
                }
            }
            freeaddrinfo(bservinfo);
            if (!bound) {
                char buf[128];
                snprintf(buf,sizeof(buf),"Can't bind socket: %s",strerror(errno));
                __redisSetError(c,REDIS_ERR_OTHER,buf);
                goto error;
            }
        }

        /* For repeat connection */
        hi_free(c->saddr);
        c->saddr = hi_malloc(p->ai_addrlen);
        if (c->saddr == NULL)
            goto oom;

        memcpy(c->saddr, p->ai_addr, p->ai_addrlen);
        c->addrlen = p->ai_addrlen;

        if (connect(s,p->ai_addr,p->ai_addrlen) == -1) {
            if (errno == EHOSTUNREACH) {
                redisNetClose(c);
                continue;
            } else if (errno == EINPROGRESS) {
                if (blocking) {
                    goto wait_for_ready;
                }
                /* This is ok.
                 * Note that even when it's in blocking mode, we unset blocking
                 * for `connect()`
                 */
            } else if (errno == EADDRNOTAVAIL && reuseaddr) {
                if (++reuses >= REDIS_CONNECT_RETRIES) {
                    goto error;
                } else {
                    redisNetClose(c);
                    goto addrretry;
                }
            } else {
                wait_for_ready:
                if (redisContextWaitReady(c,timeout_msec) != REDIS_OK)
                    goto error;
                if (redisSetTcpNoDelay(c) != REDIS_OK)
                    goto error;
            }
        }
        if (blocking && redisSetBlocking(c,1) != REDIS_OK)
            goto error;

        c->flags |= REDIS_CONNECTED;
        rv = REDIS_OK;
        goto end;
    }
    if (p == NULL) {
        char buf[128];
        snprintf(buf,sizeof(buf),"Can't create socket: %s",strerror(errno));
        __redisSetError(c,REDIS_ERR_OTHER,buf);
        goto error;
    }

oom:
    __redisSetError(c, REDIS_ERR_OOM, "Out of memory");
error:
    rv = REDIS_ERR;
end:
    if(servinfo) {
        freeaddrinfo(servinfo);
    }

    return rv;  // Need to return REDIS_OK if alright
}

int redisContextConnectTcp(redisContext *c, const char *addr, int port,
                           const struct timeval *timeout) {
    return _redisContextConnectTcp(c, addr, port, timeout, NULL);
}

int redisContextConnectBindTcp(redisContext *c, const char *addr, int port,
                               const struct timeval *timeout,
                               const char *source_addr) {
    return _redisContextConnectTcp(c, addr, port, timeout, source_addr);
}

int redisContextConnectUnix(redisContext *c, const char *path, const struct timeval *timeout) {
#ifndef _WIN32
    int blocking = (c->flags & REDIS_BLOCK);
    struct sockaddr_un *sa;
    long timeout_msec = -1;

    if (redisCreateSocket(c,AF_UNIX) < 0)
        return REDIS_ERR;
    if (redisSetBlocking(c,0) != REDIS_OK)
        return REDIS_ERR;

    c->connection_type = REDIS_CONN_UNIX;
    if (c->unix_sock.path != path) {
        hi_free(c->unix_sock.path);

        c->unix_sock.path = hi_strdup(path);
        if (c->unix_sock.path == NULL)
            goto oom;
    }

    if (timeout) {
        if (redisContextUpdateConnectTimeout(c, timeout) == REDIS_ERR)
            goto oom;
    } else {
        hi_free(c->connect_timeout);
        c->connect_timeout = NULL;
    }

    if (redisContextTimeoutMsec(c,&timeout_msec) != REDIS_OK)
        return REDIS_ERR;

    /* Don't leak sockaddr if we're reconnecting */
    if (c->saddr) hi_free(c->saddr);

    sa = (struct sockaddr_un*)(c->saddr = hi_malloc(sizeof(struct sockaddr_un)));
    if (sa == NULL)
        goto oom;

    c->addrlen = sizeof(struct sockaddr_un);
    sa->sun_family = AF_UNIX;
    strncpy(sa->sun_path, path, sizeof(sa->sun_path) - 1);
    if (connect(c->fd, (struct sockaddr*)sa, sizeof(*sa)) == -1) {
        if (errno == EINPROGRESS && !blocking) {
            /* This is ok. */
        } else {
            if (redisContextWaitReady(c,timeout_msec) != REDIS_OK)
                return REDIS_ERR;
        }
    }

    /* Reset socket to be blocking after connect(2). */
    if (blocking && redisSetBlocking(c,1) != REDIS_OK)
        return REDIS_ERR;

    c->flags |= REDIS_CONNECTED;
    return REDIS_OK;
#else
    /* We currently do not support Unix sockets for Windows. */
    /* TODO(m): https://devblogs.microsoft.com/commandline/af_unix-comes-to-windows/ */
    errno = EPROTONOSUPPORT;
    return REDIS_ERR;
#endif /* _WIN32 */
oom:
    __redisSetError(c, REDIS_ERR_OOM, "Out of memory");
    return REDIS_ERR;
}

/* ---- Homa transport (message-based, connectionless) -------------------
 *
 * Each command is one Homa RPC: redisHomaWrite sends the output buffer as a
 * request, redisHomaBufferRead receives the whole response and feeds it to the
 * reply parser straight from the receive pool. One in-flight RPC per context
 * (id=0 receives it and recycles the previous message's bpages).
 */
#include <arpa/inet.h>
#include "alloc.h"
#include "homa_user.h"

typedef struct homaClientState {
    uint8_t *region;
    size_t region_size;
    struct homa_recvmsg_args ctl;
    struct sockaddr_in dest;
    int inflight;   /* a request was sent and its response not read yet */
} homaClientState;

/* free_privctx hook: redisFree calls it even when privctx is NULL. */
static void redisHomaFree(void *privctx) {
    homaClientState *h = privctx;
    if (h == NULL)
        return;
    munmap(h->region, h->region_size);
    hi_free(h);
}

/* Receive one whole response and feed it to the reply parser directly from the
 * receive pool, one bpage fragment at a time: no staging buffer, and the whole
 * message is consumed at once, so event-loop users never wait on an epoll
 * event for a remainder. Called by redisBufferRead in place of funcs->read. */
int redisHomaBufferRead(redisContext *c) {
    homaClientState *h = c->privctx;
    struct sockaddr_in from;
    struct msghdr hdr = {0};

    /* Homa ignores SO_RCVTIMEO: honour command_timeout (0 = none) with poll(). */
    if ((c->flags & REDIS_BLOCK) && c->command_timeout) {
        long ms = c->command_timeout->tv_sec * 1000 + c->command_timeout->tv_usec / 1000;
        struct pollfd p = { .fd = c->fd, .events = POLLIN };
        int r = ms > 0 ? poll(&p, 1, (int)ms) : 1;
        if (r == 0) {
            __redisSetError(c, REDIS_ERR_TIMEOUT, "recv timeout");
            return REDIS_ERR;
        }
        if (r < 0) {
            if (errno == EINTR)
                return REDIS_OK;
            __redisSetError(c, REDIS_ERR_IO, NULL);
            return REDIS_ERR;
        }
    }

    h->ctl.id = 0; /* receive any (one in flight) + recycle prior bpages */
    hdr.msg_name = &from;
    hdr.msg_namelen = sizeof(from);
    hdr.msg_control = &h->ctl;
    hdr.msg_controllen = sizeof(h->ctl);

    ssize_t n = recvmsg(c->fd, &hdr, 0);
    if (n < 0) {
        if ((errno == EWOULDBLOCK && !(c->flags & REDIS_BLOCK)) || errno == EINTR)
            return REDIS_OK; /* recoverable */
        __redisSetError(c, REDIS_ERR_IO, NULL);
        return REDIS_ERR;
    }
    h->inflight = 0;
    if (n == 1 && h->region[h->ctl.bpage_offsets[0]] == HOMA_USER_NO_REPLY)
        return REDIS_OK; /* an RPC that produced no reply (see homa_user.h) */
    for (uint32_t i = 0; i < h->ctl.num_bpages; i++) {
        size_t len = (i == h->ctl.num_bpages - 1) ? (size_t)n - (size_t)i * HOMA_BPAGE_SIZE
                                                  : HOMA_BPAGE_SIZE;
        if (redisReaderFeed(c->reader, (char *)h->region + h->ctl.bpage_offsets[i],
                            len) != REDIS_OK) {
            __redisSetError(c, c->reader->err, c->reader->errstr);
            return REDIS_ERR;
        }
    }
    return REDIS_OK;
}

static ssize_t redisHomaWrite(redisContext *c) {
    homaClientState *h = c->privctx;
    size_t len = hi_sdslen(c->obuf);
    if (len > HOMA_MAX_MESSAGE_LENGTH) {
        __redisSetError(c, REDIS_ERR_OTHER, "Request exceeds Homa's 1000000-byte message limit");
        return -1;
    }
    /* One RPC in flight: Homa may complete a later, smaller response first
     * (SRPT), and responses are matched to commands by arrival order. A blocking
     * context reads the outstanding response into the reader first; a
     * nonblocking one reports nothing written until it has read it. */
    if (h->inflight) {
        if (!(c->flags & REDIS_BLOCK))
            return 0;
        if (redisHomaBufferRead(c) != REDIS_OK)
            return -1;
        if (h->inflight)
            return 0; /* EINTR: the caller retries */
    }
    ssize_t ret = homa_user_send(c->fd, c->obuf, len,
                                 (struct sockaddr *)&h->dest, sizeof(h->dest), NULL);
    if (ret < 0) {
        if ((errno == EWOULDBLOCK && !(c->flags & REDIS_BLOCK)) || errno == EINTR)
            return 0;
        __redisSetError(c, REDIS_ERR_IO, NULL);
        return -1;
    }
    h->inflight = 1;
    /* Homa sendmsg sends the whole message atomically and returns 0 on
     * success; report the full length so hiredis clears its output buffer. */
    return (ssize_t)len;
}

/* Homa has no FIN: tell the server this peer is gone, as a TCP close would, so
 * its session (db, MULTI, AUTH user) is freed now instead of at the idle reap.
 * Otherwise a later socket that reuses the client port would inherit it. QUIT is
 * sent fire-and-forget; if it is lost the server still reaps the peer. */
static void redisHomaClose(redisContext *c) {
    static const char quit[] = "*1\r\n$4\r\nQUIT\r\n";
    homaClientState *h = c->privctx;
    if (c->fd != REDIS_INVALID_FD && h)
        homa_user_send(c->fd, quit, sizeof(quit) - 1,
                       (struct sockaddr *)&h->dest, sizeof(h->dest), NULL);
    redisNetClose(c);
}

static redisContextFuncs redisContextHomaFuncs = {
    .close = redisHomaClose,
    .free_privctx = redisHomaFree,
    .async_read = NULL, /* async is refused in redisAsyncConnectWithOptions */
    .async_write = NULL,
    .read = NULL, /* reads go through redisHomaBufferRead */
    .write = redisHomaWrite,
};

int redisContextConnectHoma(redisContext *c, const char *addr, int port,
                            const struct timeval *timeout) {
    (void)timeout;
    homaClientState *h;
    int fd;

    /* TODO: c->tcp.host/port are not recorded and redisReconnect has no Homa
     * branch, so a Homa context cannot be reconnected (TCP/Unix can). */
    c->connection_type = REDIS_CONN_HOMA;

    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_HOMA);
    if (fd == REDIS_INVALID_FD) {
        __redisSetError(c, REDIS_ERR_IO, "Cannot create Homa socket");
        return REDIS_ERR;
    }

    h = hi_calloc(1, sizeof(*h));
    if (h == NULL) {
        close(fd);
        __redisSetError(c, REDIS_ERR_OOM, "Out of memory");
        return REDIS_ERR;
    }
    if (homa_user_init_recv_buffer(fd, &h->region, &h->region_size, homa_user_client_bpages()) < 0) {
        hi_free(h);
        close(fd);
        __redisSetError(c, REDIS_ERR_IO, "Cannot init Homa receive buffer");
        return REDIS_ERR;
    }

    h->dest.sin_family = AF_INET;
    h->dest.sin_port = htons(port);
    if (inet_pton(AF_INET, addr, &h->dest.sin_addr) != 1) {
        munmap(h->region, h->region_size);
        hi_free(h);
        close(fd);
        __redisSetError(c, REDIS_ERR_OTHER, "Invalid Homa server address");
        return REDIS_ERR;
    }

    c->fd = fd;
    c->privctx = h;
    c->funcs = &redisContextHomaFuncs;

    /* Honor non-blocking mode so the fd can be driven by an event loop; the
     * read/write callbacks then retry on EWOULDBLOCK, as redisNetRead/Write do. */
    if (!(c->flags & REDIS_BLOCK) && redisSetBlocking(c, 0) != REDIS_OK)
        return REDIS_ERR;

    c->flags |= REDIS_CONNECTED;
    return REDIS_OK;
}
