/* Minimal user-space helpers for talking Homa from Redis.
 *
 * Header-only (static inline) so the server (src/homa.c) and the client
 * (deps/hiredis) can share one copy without a separate library. The uapi
 * (homa.h) is vendored verbatim from the Homa kernel tree and MUST match the
 * loaded homa module's ABI (SO_HOMA_RCVBUF, struct layouts).
 */
#ifndef HOMA_USER_H
#define HOMA_USER_H

/* _GNU_SOURCE must be set before system headers (for MAP_ANONYMOUS); consumers
 * that include this header late must also set it on the compiler command line. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <unistd.h>

#include "homa.h"

/* Homa has no zero-length messages, so the server answers an RPC whose
 * commands produced no output (CLIENT REPLY OFF/SKIP, a command split across
 * RPCs) with this single byte, which clients drop: a RESP reply is >= 3 bytes. */
#define HOMA_USER_NO_REPLY '\0'

/* Receive-pool sizes, in 64 KB bpages; each socket mmaps its own. A server socket
 * receives from every peer at once. A client context has one response in flight,
 * so it takes the kernel's minimum (homa_pool_set_region): a private bpage per
 * possible CPU (_SC_NPROCESSORS_CONF, i.e. nr_cpu_ids) plus two full-size messages. */
#ifndef HOMA_RECV_BPAGES
#define HOMA_RECV_BPAGES 512
#endif
static inline unsigned homa_user_client_bpages(void) {
    long cpus = sysconf(_SC_NPROCESSORS_CONF);
    return (unsigned)(cpus > 0 ? cpus : 1) + 2 * HOMA_MAX_BPAGES;
}

/* mmap a receive-buffer region of `bpages` bpages and register it with the
 * socket. Returns 0 on success and fills *region / *size; -1 on error. */
static inline int homa_user_init_recv_buffer(int sockfd, uint8_t **region,
                                             size_t *size, unsigned bpages) {
    struct homa_rcvbuf_args args;

    *size = (size_t)bpages * HOMA_BPAGE_SIZE;
    *region = mmap(NULL, *size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    if (*region == MAP_FAILED)
        return -1;

    args.start = (uint64_t)(uintptr_t)*region;
    args.length = *size;
    if (setsockopt(sockfd, IPPROTO_HOMA, SO_HOMA_RCVBUF, &args, sizeof(args)) < 0) {
        munmap(*region, *size);
        return -1;
    }
    return 0;
}

/* Copy bytes [msg_off, msg_len) of a just-received message out of the
 * (possibly multi-bpage) receive pool into a flat buffer, at most dstcap
 * bytes. Returns bytes copied (0 if the message is already fully drained),
 * or -1 on invalid arguments. */
static inline ssize_t homa_user_copy_msg(void *dst, size_t dstcap,
                                         const uint8_t *region, size_t msg_off,
                                         ssize_t msg_len, uint32_t num_bpages,
                                         const uint32_t *bpage_offsets) {
    ssize_t copied = 0;
    size_t skip = msg_off;

    if (msg_len < 0 || msg_off > (size_t)msg_len)
        return -1;
    for (uint32_t i = 0; i < num_bpages && (size_t)copied < dstcap; i++) {
        size_t avail = (i == num_bpages - 1) ? (size_t)msg_len - (size_t)i * HOMA_BPAGE_SIZE
                                             : (size_t)HOMA_BPAGE_SIZE;
        if (skip >= avail) {
            skip -= avail;
            continue;
        }
        size_t chunk = avail - skip;
        if (chunk > dstcap - (size_t)copied)
            chunk = dstcap - (size_t)copied;
        memcpy((uint8_t *)dst + copied, region + bpage_offsets[i] + skip, chunk);
        copied += chunk;
        skip = 0;
    }
    return copied;
}

/* Send a request (client side). Returns the syscall result; *id gets the RPC
 * id to match the response against. */
static inline ssize_t homa_user_send(int sockfd, const void *buf, size_t len,
                                     const struct sockaddr *dest,
                                     socklen_t destlen, uint64_t *id) {
    struct homa_sendmsg_args args = { 0 };
    struct iovec vec = { .iov_base = (void *)buf, .iov_len = len };
    struct msghdr hdr = {
        .msg_name = (void *)dest,
        .msg_namelen = destlen,
        .msg_iov = &vec,
        .msg_iovlen = 1,
        .msg_control = &args,
        .msg_controllen = 0,
    };
    ssize_t ret = sendmsg(sockfd, &hdr, 0);
    if (ret >= 0 && id)
        *id = args.id;
    return ret;
}

/* Send a reply for a previously received request (server side). */
static inline ssize_t homa_user_reply(int sockfd, const struct iovec *iov,
                                      int iovcnt, const struct sockaddr *dest,
                                      socklen_t destlen, uint64_t id) {
    struct homa_sendmsg_args args = { .id = id };
    struct msghdr hdr = {
        .msg_name = (void *)dest,
        .msg_namelen = destlen,
        .msg_iov = (struct iovec *)iov,
        .msg_iovlen = iovcnt,
        .msg_control = &args,
        .msg_controllen = 0,
    };
    return sendmsg(sockfd, &hdr, 0);
}

#endif /* HOMA_USER_H */
