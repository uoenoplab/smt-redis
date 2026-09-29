/* mixload: open-loop, mixed-size GET load against Redis over TCP or Homa, for
 * head-of-line blocking and per-connection cost.
 *
 *   mixload <ip> <port> tcp|homa <endpoints> <ops/s> <secs> <big-fraction>
 *
 * Each request is GET small or, with probability <big-fraction>, GET big (preload
 * both keys). Arrivals are Poisson; a request's latency runs from its scheduled
 * arrival to its reply, so client-side queueing counts (no coordinated omission).
 * tcp: <endpoints> connections, each pipelined, replies in order per connection.
 * homa: <endpoints> sockets, any number of RPCs in flight on each, responses
 * matched by RPC id (receive pool: the kernel minimum + 64 bpages, room for ~12
 * big replies). A request goes to the least recently used endpoint with nothing
 * outstanding if there is one (so all endpoints stay active), else to the one
 * with the fewest. One thread sends and receives, spinning on both transports,
 * so neither side's client sleeps; while it handles a reply, the requests due are
 * sent late, and lag_* says how late. The first second is not recorded.
 * Output, one CSV line (latencies in us):
 *   transport,endpoints,rate,secs,big_frac,sent,done,bad,small_p50,small_p99,
 *   small_p999,big_p50,big_p99,max_outstanding,lag_p99,lag_max
 * Environment: MIXSEED seeds the arrival schedule (default 1; give each of several
 * concurrent clients its own); LATDUMP names a file that gets every recorded latency,
 * one "s|b|l <us>" line each (small, big, send lag), for pooling several clients.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include "hiredis.h"
#include "homa_user.h"

#define WARMUP 1.0
#define DRAIN 2.0
#define MAPBITS 20 /* outstanding Homa RPCs, indexed by id/2 (ids step by 2 per host) */

typedef struct { double t; int big; } req_t;
typedef struct {
    int fd, out;                   /* socket; requests outstanding */
    redisReader *rd;
    req_t *q; int qh, qn, qcap;    /* tcp: outstanding, in send order */
    char *wbuf; size_t wlen, wcap; /* tcp: bytes not yet written */
    uint8_t *region; size_t rsize; /* homa: receive pool */
    struct homa_recvmsg_args ctl;
} ep_t;
typedef struct { uint64_t id; req_t r; int ep; } slot_t;

static const char SMALL[] = "*2\r\n$3\r\nGET\r\n$5\r\nsmall\r\n", BIG[] = "*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n";
static int homa, n_ep, *idle, idle_h, n_idle, rot, max_out, n_wpend; /* idle: FIFO ring of endpoints */
static ep_t *eps;
static slot_t *map;
static req_t *backlog; static int bl_h, bl_n, bl_cap; static int *bl_ep;
static struct sockaddr_in dst;
static double *lat[3]; static long n_lat[3], cap_lat[3], sent, done, bad; /* [2]: send lag */
static double t_rec;

static double now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static void die(const char *what) { perror(what); exit(1); }

/* Readers run without object functions: a reply is just its type, so parsing
 * never copies a (512 KB) value into a reply object, on either transport. */
static void complete(ep_t *e, req_t r, void *rep, double t) {
    if ((uintptr_t)rep != REDIS_REPLY_STRING) bad++;
    if (r.t >= t_rec && n_lat[r.big] < cap_lat[r.big]) lat[r.big][n_lat[r.big]++] = (t - r.t) * 1e6;
    done++;
    if (--e->out == 0) idle[(idle_h + n_idle++) % n_ep] = (int)(e - eps);
}

static ep_t *pick(void) {
    if (n_idle) { n_idle--; int i = idle[idle_h]; idle_h = (idle_h + 1) % n_ep; return &eps[i]; } /* least recently used */
    int best = rot = (rot + 1) % n_ep;
    for (int i = 0; i < n_ep; i++) { int j = (rot + i) % n_ep; if (eps[j].out < eps[best].out) best = j; }
    return &eps[best];
}

static void flush(ep_t *e) {
    while (e->wlen) {
        ssize_t n = write(e->fd, e->wbuf, e->wlen);
        if (n < 0) { if (errno == EAGAIN) break; die("write"); }
        memmove(e->wbuf, e->wbuf + n, e->wlen - n); e->wlen -= n;
    }
}

/* Homa: 0 if sent, -1 if out of send memory (caller retries later). */
static int homa_send(ep_t *e, req_t r) {
    uint64_t id;
    const char *cmd = r.big ? BIG : SMALL; size_t len = r.big ? sizeof(BIG) - 1 : sizeof(SMALL) - 1;
    if (homa_user_send(e->fd, cmd, len, (struct sockaddr *)&dst, sizeof(dst), &id) < 0) {
        if (errno == EAGAIN) return -1;
        die("homa send");
    }
    slot_t *s = &map[(id >> 1) & ((1 << MAPBITS) - 1)];
    if (s->id) { fprintf(stderr, "rpc map collision\n"); exit(1); }
    *s = (slot_t){ id, r, (int)(e - eps) };
    return 0;
}

static void issue(req_t r) {
    ep_t *e = pick();
    if (++e->out > max_out) max_out = e->out;
    sent++;
    if (homa) {
        if (bl_n == 0 && homa_send(e, r) == 0) return;
        if (bl_n == bl_cap) { fprintf(stderr, "send backlog full\n"); exit(1); }
        int k = (bl_h + bl_n++) % bl_cap; backlog[k] = r; bl_ep[k] = (int)(e - eps);
        return;
    }
    if (e->qn == e->qcap) {
        req_t *q = malloc(sizeof(req_t) * 2 * e->qcap);
        for (int i = 0; i < e->qn; i++) q[i] = e->q[(e->qh + i) % e->qcap];
        free(e->q); e->q = q; e->qh = 0; e->qcap *= 2;
    }
    e->q[(e->qh + e->qn++) % e->qcap] = r;
    const char *cmd = r.big ? BIG : SMALL; size_t len = r.big ? sizeof(BIG) - 1 : sizeof(SMALL) - 1;
    if (e->wlen + len > e->wcap) e->wbuf = realloc(e->wbuf, e->wcap = 2 * (e->wlen + len));
    memcpy(e->wbuf + e->wlen, cmd, len); e->wlen += len;
    flush(e);
    if (e->wlen) n_wpend++;
}

/* At most 16 reads (64 KB each, parsed as they come, so the reader's buffer
 * stays small) or 16 messages per call: epoll is level-triggered, and one busy
 * endpoint must not starve the others or the arrival schedule. */
static void readable(ep_t *e) {
    static char buf[1 << 16];
    void *rep;
    if (!homa) {
        for (int k = 0; k < 16; k++) {
            ssize_t n = read(e->fd, buf, sizeof(buf));
            if (n < 0 && errno == EAGAIN) break;
            if (n <= 0) die("read");
            if (redisReaderFeed(e->rd, buf, n) != REDIS_OK) die("reader");
            double t = now();
            while (redisReaderGetReply(e->rd, &rep) == REDIS_OK && rep) {
                req_t r = e->q[e->qh]; e->qh = (e->qh + 1) % e->qcap; e->qn--;
                complete(e, r, rep, t);
            }
        }
        return;
    }
    for (int k = 0; k < 16; k++) {
        struct sockaddr_in from;
        struct msghdr h = { .msg_name = &from, .msg_namelen = sizeof(from),
                            .msg_control = &e->ctl, .msg_controllen = sizeof(e->ctl) };
        e->ctl.id = 0; /* any RPC; also returns the previous message's bpages */
        ssize_t n = recvmsg(e->fd, &h, MSG_DONTWAIT);
        if (n < 0) { if (errno == EAGAIN) break; die("homa recvmsg"); }
        slot_t *s = &map[(e->ctl.id >> 1) & ((1 << MAPBITS) - 1)];
        if (s->id != e->ctl.id) { fprintf(stderr, "unknown rpc id\n"); exit(1); }
        for (uint32_t i = 0; i < e->ctl.num_bpages; i++) {
            size_t len = i == e->ctl.num_bpages - 1 ? (size_t)n - (size_t)i * HOMA_BPAGE_SIZE : HOMA_BPAGE_SIZE;
            if (redisReaderFeed(e->rd, (char *)e->region + e->ctl.bpage_offsets[i], len) != REDIS_OK) die("reader");
        }
        rep = NULL;
        if (redisReaderGetReply(e->rd, &rep) != REDIS_OK) die("reader");
        req_t r = s->r; s->id = 0;
        complete(e, r, rep, now());
    }
}

static int cmpd(const void *a, const void *b) { double d = *(double *)a - *(double *)b; return (d > 0) - (d < 0); }
static double pct(int k, double p) {
    return n_lat[k] ? lat[k][(long)(p * (n_lat[k] - 1))] : NAN;
}

int main(int argc, char **argv) {
    if (argc != 8) { fprintf(stderr, "usage: mixload <ip> <port> tcp|homa <endpoints> <ops/s> <secs> <big-fraction>\n"); return 2; }
    homa = !strcmp(argv[3], "homa");
    n_ep = atoi(argv[4]);
    double rate = atof(argv[5]), secs = atof(argv[6]), big_frac = atof(argv[7]);
    dst = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons(atoi(argv[2])) };
    if (inet_pton(AF_INET, argv[1], &dst.sin_addr) != 1) { fprintf(stderr, "bad ip\n"); return 2; }

    int ep = epoll_create1(0);
    eps = calloc(n_ep, sizeof(ep_t)); idle = malloc(sizeof(int) * n_ep);
    for (int i = 0; i < n_ep; i++) {
        ep_t *e = &eps[i];
        e->rd = redisReaderCreate();
        e->rd->fn = NULL;   /* replies are bare types: see complete() */
        e->rd->maxbuf = 0;  /* keep the buffer between replies instead of freeing it */
        if (homa) {
            if ((e->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_HOMA)) < 0) die("homa socket");
            if (homa_user_init_recv_buffer(e->fd, &e->region, &e->rsize, homa_user_client_bpages() + 64) < 0) die("homa rcvbuf");
        } else {
            int one = 1;
            if ((e->fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) die("socket");
            if (connect(e->fd, (struct sockaddr *)&dst, sizeof(dst)) < 0) die("connect");
            setsockopt(e->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            e->q = malloc(sizeof(req_t) * (e->qcap = 16));
        }
        fcntl(e->fd, F_SETFL, fcntl(e->fd, F_GETFL) | O_NONBLOCK);
        struct epoll_event ev = { .events = EPOLLIN, .data.u32 = i };
        if (epoll_ctl(ep, EPOLL_CTL_ADD, e->fd, &ev) < 0) die("epoll_ctl");
        idle[n_idle++] = i;
    }
    if (homa) {
        map = calloc(1 << MAPBITS, sizeof(slot_t));
        backlog = malloc(sizeof(req_t) * (bl_cap = 1 << 16)); bl_ep = malloc(sizeof(int) * bl_cap);
    }
    for (int k = 0; k < 3; k++) lat[k] = malloc(sizeof(double) * (cap_lat[k] = (long)(rate * secs * (k == 1 ? big_frac : 1) * 1.2) + 1000));

    fprintf(stderr, "ready\n"); /* endpoints set up; the timed run starts now */
    srand48(getenv("MIXSEED") ? atol(getenv("MIXSEED")) : 1);
    double t0 = now(), t_next = t0, t_end = t0 + secs;
    t_rec = t0 + WARMUP;
    struct epoll_event evs[256];
    for (double t = t0; t < t_end || (sent > done && t < t_end + DRAIN); t = now()) {
        while (bl_n && homa_send(&eps[bl_ep[bl_h]], backlog[bl_h]) == 0) { bl_h = (bl_h + 1) % bl_cap; bl_n--; }
        for (; t_next <= t && t_next < t_end; t_next += -log(1 - drand48()) / rate) {
            if (t_next >= t_rec && n_lat[2] < cap_lat[2]) lat[2][n_lat[2]++] = (t - t_next) * 1e6;
            issue((req_t){ t_next, drand48() < big_frac });
        }
        int n = epoll_wait(ep, evs, 256, 0);
        for (int i = 0; i < n; i++) readable(&eps[evs[i].data.u32]);
        if (n_wpend) {
            n_wpend = 0;
            for (int i = 0; i < n_ep; i++) if (eps[i].wlen) { flush(&eps[i]); n_wpend += eps[i].wlen > 0; }
        }
    }
    /* QUIT every endpoint, so the server frees it now: Homa has no FIN (fire and
     * forget, like a closing hiredis context); on TCP the server then closes first,
     * which leaves TIME_WAIT on its side, not on our ephemeral ports. */
    static const char quit[] = "*1\r\n$4\r\nQUIT\r\n";
    for (int i = 0; i < n_ep; i++)
        if (homa) homa_user_send(eps[i].fd, quit, sizeof(quit) - 1, (struct sockaddr *)&dst, sizeof(dst), NULL);
        else if (write(eps[i].fd, quit, sizeof(quit) - 1) < 0) die("write quit");
    for (int i = 0; !homa && i < n_ep; i++) { /* read to EOF (any unread replies, then +OK) */
        struct timeval tv = { 1, 0 }; char b[1 << 16];
        fcntl(eps[i].fd, F_SETFL, fcntl(eps[i].fd, F_GETFL) & ~O_NONBLOCK);
        setsockopt(eps[i].fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        while (read(eps[i].fd, b, sizeof(b)) > 0);
    }
    for (int k = 0; k < 3; k++) qsort(lat[k], n_lat[k], sizeof(double), cmpd);
    if (getenv("LATDUMP")) {
        FILE *f = fopen(getenv("LATDUMP"), "w");
        if (!f) die("LATDUMP");
        for (int k = 0; k < 3; k++) for (long i = 0; i < n_lat[k]; i++) fprintf(f, "%c %.1f\n", "sbl"[k], lat[k][i]);
        fclose(f);
    }
    printf("%s,%d,%.0f,%.0f,%g,%ld,%ld,%ld,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%.1f,%.1f\n", argv[3], n_ep, rate, secs, big_frac,
           sent, done, bad, pct(0, .5), pct(0, .99), pct(0, .999), pct(1, .5), pct(1, .99), max_out,
           pct(2, .99), n_lat[2] ? lat[2][n_lat[2] - 1] : NAN);
    return sent != done;
}
