#!/usr/bin/env python3
"""Plain matplotlib figures from results/ into figures/ (300 dpi PNG), and the README's tables
(printed as Markdown). Every point is the median over rounds; bars span min to max.
Run: uv run --with matplotlib python plot.py"""
import csv, os, re, sys
from statistics import median

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "results/xl170-upstream-1c59d7b6")
OUT = os.path.join(HERE, "figures")
os.makedirs(OUT, exist_ok=True)
# label, CSV name suffix, transport column, colour, line style. "-tcpclean" files were taken with
# homa.ko unloaded (tcpclean.sh on); the others in Homa's official config, which TCP then shares.
LINES = [("Homa", "", "homa", "C0", "-"), ("TCP, stock", "-tcpclean", "tcp", "C1", "-"),
         ("TCP in Homa's config", "", "tcp", "C2", "--")]
# workload, title, results file (b, d, e, f ran later, with the Scan-capable YCSB-C build)
WORKLOADS = [("workloada", "a: 50% read, 50% update", "ycsb-xl170"), ("workloadb", "b: 95% read, 5% update", "ycsb-xl170-bdef"),
             ("workloadc", "c: 100% read", "ycsb-xl170"), ("workloadd", "d: 95% read (latest), 5% insert", "ycsb-xl170-bdef"),
             ("workloade", "e: 95% scan of up to 100 records, 5% insert", "ycsb-xl170-bdef"),
             ("workloadf", "f: 50% read, 50% read-modify-write", "ycsb-xl170-bdef")]


def rows(name, suffix, tr, **match):
    """Rows of results/<name><suffix>.csv for transport tr matching **match. A mixload row with bad
    replies (not the value asked for: the data was missing) is not a measurement: skipped, reported."""
    path = os.path.join(RES, f"{name}{suffix}.csv")
    out = [r for r in csv.DictReader(open(path)) if r["transport"] == tr
           and all(r[k] == str(v) for k, v in match.items())] if os.path.exists(path) else []
    for r in out:
        if r.get("bad", "0") != "0":
            print(f"skipped {name}{suffix}.csv round {r['round']}: {r['bad']} bad replies", file=sys.stderr)
    return [r for r in out if r.get("bad", "0") == "0"]


def us_per_op(r, busy, kops):
    """CPU microseconds per operation: 1% of one CPU is 10 ms/s, over kops thousand ops/s."""
    return float(r[busy]) * 10 / kops


def stat(rs, f):
    v = [f(r) for r in rs]
    return (median(v), min(v), max(v)) if v else (float("nan"),) * 3


def curve(ax, xs, pts, label, color, ls):
    """pts: x -> (median, min, max); plots only the x values that have data."""
    xs = [x for x in xs if x in pts and pts[x][0] == pts[x][0]]
    med = [pts[x][0] for x in xs]
    ax.errorbar(xs, med, yerr=[[m - pts[x][1] for m, x in zip(med, xs)], [pts[x][2] - m for m, x in zip(med, xs)]],
                marker="o", ms=4, capsize=3, color=color, ls=ls, label=label)


def style(ax, xlabel, ylabel, logx=True, logy=False, xs=None):
    if logx:
        ax.set_xscale("log", base=2 if xs and max(xs) <= 64 else 10)
        if xs:
            ax.set_xticks(xs, [str(x) for x in xs])
    if logy:
        ax.set_yscale("log")
    else:
        ax.set_ylim(bottom=0)
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    ax.grid(True, alpha=0.3)


def save(fig, name, title, note=None):
    fig.suptitle(title)
    if note:
        fig.text(0.5, 0.005, note, ha="center", fontsize=8)
    fig.tight_layout(rect=(0, 0.025 if note else 0, 1, 1))
    fig.savefig(os.path.join(OUT, name), dpi=300)
    plt.close(fig)
    print(f"\n![{title}](figures/{name})")


def table(header, body):
    print("\n| " + " | ".join(header) + " |\n|" + "---|" * len(header))
    for row in body:
        print("| " + " | ".join(row) + " |")


fmt = lambda s, d=1: f"{s[0]:.{d}f}"

# --- YCSB-C: one operation per request ---------------------------------------------------------
THREADS = [1, 2, 4, 8, 16]
ycsb = {(w, lab, t): rows(f, suf, tr, workload=w, threads=t)
        for w, _, f in WORKLOADS for lab, suf, tr, _, _ in LINES for t in THREADS}
WORKLOADS = [x for x in WORKLOADS if ycsb[(x[0], "Homa", 1)]]  # those measured
kt = lambda r: float(r["ktps"])
fig, axes = plt.subplots((len(WORKLOADS) + 2) // 3, min(3, len(WORKLOADS)), figsize=(15, 4.5 * ((len(WORKLOADS) + 2) // 3)), squeeze=False)
for ax, (w, title, _) in zip(axes.flat, WORKLOADS):
    for lab, _, _, c, ls in LINES:
        curve(ax, THREADS, {t: stat(ycsb[(w, lab, t)], kt) for t in THREADS}, lab, c, ls)
    style(ax, "YCSB client threads", "Throughput (KTPS)", xs=THREADS)
    ax.set_title(title)
axes.flat[0].legend()
save(fig, "ycsb-throughput.png", "YCSB-C over Redis on 2x xl170 (5 rounds)")

fig, axes = plt.subplots(2, 2, figsize=(11, 8))
for row, (w, title, _) in enumerate([x for x in WORKLOADS if x[0] in ("workloada", "workloadc")]):
    for col, (side, busy) in enumerate([("server", "server_busy_sum"), ("client", "client_busy_sum")]):
        ax = axes[row][col]
        for lab, _, _, c, ls in LINES:
            curve(ax, THREADS, {t: stat(ycsb[(w, lab, t)], lambda r: us_per_op(r, busy, kt(r))) for t in THREADS}, lab, c, ls)
        style(ax, "YCSB client threads", f"{side.capitalize()} CPU per operation (us)", xs=THREADS)
        ax.set_title(f"{title} ({side} node, all 20 CPUs)")
axes[0][0].legend()
save(fig, "ycsb-cpu-per-op.png", "YCSB-C: CPU time per operation (5 rounds)")
table(["Workload", "Threads"] + [f"{l} KTPS" for l, *_ in LINES] + ["Homa / stock TCP", "Server us/op (Homa / stock TCP)", "Client us/op (Homa / stock TCP)"],
      [[w, str(t)] + [(lambda m, d: f"{m[0]:.{d}f} ({m[1]:.{d}f}-{m[2]:.{d}f})")(stat(ycsb[(w, l, t)], kt), 2 if stat(ycsb[(w, l, t)], kt)[0] < 10 else 1)
                       for l, *_ in LINES]
       + [f"{stat(ycsb[(w, 'Homa', t)], kt)[0] / stat(ycsb[(w, 'TCP, stock', t)], kt)[0]:.2f}"]
       + [" / ".join(fmt(stat(ycsb[(w, l, t)], lambda r: us_per_op(r, b, kt(r)))) for l in ("Homa", "TCP, stock")) for b in ("server_busy_sum", "client_busy_sum")]
       for w, *_ in WORKLOADS for t in THREADS])

# --- Batching: redis-benchmark -P ---------------------------------------------------------------
PIPES = [1, 2, 4, 8, 16, 32, 64]
batch = {(t, lab, p): rows("batch", suf, tr, test=t, pipeline=p) for t in ("get", "set") for lab, suf, tr, _, _ in LINES for p in PIPES}
if any(batch.values()):
    fig, axes = plt.subplots(2, 2, figsize=(11, 8))
    for col, t in enumerate(("get", "set")):
        for lab, _, _, c, ls in LINES:
            curve(axes[0][col], PIPES, {p: stat(batch[(t, lab, p)], lambda r: float(r["rps"]) / 1e6) for p in PIPES}, lab, c, ls)
            curve(axes[1][col], PIPES, {p: stat(batch[(t, lab, p)], lambda r: float(r["p99_ms"]) * 1000) for p in PIPES}, lab, c, ls)
        style(axes[0][col], "Pipeline depth (-P)", "Throughput (M requests/s)", xs=PIPES)
        style(axes[1][col], "Pipeline depth (-P)", "p99 latency of a request (us)", xs=PIPES)
        axes[0][col].set_title(f"{t.upper()}, 100 B values, 4 processes x 16 clients")
        axes[1][col].set_title(f"{t.upper()}: p99 (median over the 4 processes)")
    axes[0][0].legend()
    save(fig, "batch.png", "Batching: redis-benchmark pipeline depth (3 rounds)")
    table(["Test", "-P"] + [f"{l} M req/s" for l, *_ in LINES] + ["Homa / stock TCP"],
          [[t.upper(), str(p)] + [fmt(stat(batch[(t, l, p)], lambda r: float(r["rps"]) / 1e6), 2) for l, *_ in LINES]
           + [f"{stat(batch[(t, 'Homa', p)], lambda r: float(r['rps']))[0] / stat(batch[(t, 'TCP, stock', p)], lambda r: float(r['rps']))[0]:.2f}"]
           for t in ("get", "set") for p in PIPES])

# --- Head-of-line blocking: mixload, small (100 B) and big (512 KB) GETs, open loop ----------------
RATES = {"0.01": [10000, 20000, 40000, 60000, 80000], "0.1": [5000, 10000, 15000, 20000, 30000]}
HOL = [("Homa, 1 socket", "", "homa", 1, "C0", "-"), ("Homa branch tip, 1 socket", "-tip", "homa", 1, "C0", ":"),
       ("TCP stock, 1 connection", "-tcpclean", "tcp", 1, "C1", "-"),
       ("TCP stock, 64 connections", "-tcpclean", "tcp", 64, "C1", "-."),
       ("TCP in Homa's config, 1 connection", "", "tcp", 1, "C2", "--")]
# the tagged Homa code collapses at 80k GET/s with 1% big GETs (p99 ~2 s): cap the axis, note it
CAP_US, CAP_NOTE = 1e4, "Axis capped at 10 ms: the tagged Homa code collapses off the scale at 80k GET/s with 1% big GETs."
hol = lambda name, suf, tr, n, f, rate: rows(name, suf, tr, endpoints=n, big_frac=f, rate=rate)
fig, axes = plt.subplots(2, 2, figsize=(11, 8))
for col, f in enumerate(RATES):
    xs = [r / 1000 for r in RATES[f]]
    for row, (field, ylabel) in enumerate([("small_p99_us", "Small GET p99 (us)"), ("big_p50_us", "Big GET p50 (us)")]):
        for lab, suf, tr, n, c, ls in HOL:
            curve(axes[row][col], xs, {r / 1000: stat(hol("mixload-hol", suf, tr, n, f, r), lambda x: float(x[field])) for r in RATES[f]}, lab, c, ls)
        style(axes[row][col], "Offered load (k GET/s, Poisson)", ylabel, logx=False, logy=True)
        axes[row][col].set_ylim(top=CAP_US)
        axes[row][col].set_title(f"{float(f):.0%} of GETs are 512 KB, the rest 100 B")
axes[0][0].legend(fontsize=8)
save(fig, "hol.png", "Head-of-line blocking: mixed GET sizes, pipelined (3 rounds)", CAP_NOTE)
table(["Big GETs", "k GET/s"] + [f"{l}: small p99 us" for l, *_ in HOL] + ["Big GET p50 us: Homa / Homa tip / TCP stock"],
      [[f"{float(f):.0%}", str(r // 1000)] + [f"{stat(hol('mixload-hol', s, t, n, f, r), lambda x: float(x['small_p99_us']))[0]:.0f}" for _, s, t, n, _, _ in HOL]
       + [" / ".join(f"{stat(hol('mixload-hol', s, t, 1, f, r), lambda x: float(x['big_p50_us']))[0]:.0f}" for s, t in (("", "homa"), ("-tip", "homa"), ("-tcpclean", "tcp")))]
       for f in RATES for r in RATES[f]])

# How late the (single-threaded) client sent requests against their schedule
if "lag_p99_us" in (hol("mixload-hol", "", "homa", 1, "0.01", 10000) or [{}])[0]:
    table(["Big GETs", "k GET/s"] + [f"{l}: send lag p99 us" for l, *_ in HOL],
          [[f"{float(f):.0%}", str(r // 1000)] + [f"{stat(hol('mixload-hol', s, t, n, f, r), lambda x: float(x['lag_p99_us']))[0]:.0f}"
                                                   for _, s, t, n, _, _ in HOL] for f in RATES for r in RATES[f]])

# --- Many clients: the hol mix split over K concurrent clients, one endpoint each -----------------
KS = [1, 4, 8, 16]
ML = [("Homa branch tip", "-tip", "homa", "C0"), ("TCP, stock", "-tcpclean", "tcp", "C1")]
mc = lambda suf, tr, k, f, r, fld: stat(rows("mixload-hol", suf, tr, endpoints=1, big_frac=f, rate=r) if k == 1 else
                                        rows("mixload-multi", suf, tr, clients=k, big_frac=f, rate=r), lambda x: float(x[fld]))
fig, axes = plt.subplots(1, 2, figsize=(11, 4.5))
for col, f in enumerate(RATES):
    xs = [r / 1000 for r in RATES[f]]
    for lab, suf, tr, c in ML:
        for k, ls in ((1, "--"), (16, "-")):
            curve(axes[col], xs, {r / 1000: mc(suf, tr, k, f, r, "small_p99_us") for r in RATES[f]},
                  f"{lab}, {k} client{'s' if k > 1 else ''}", c, ls)
    style(axes[col], "Offered load, all clients (k GET/s, Poisson)", "Small GET p99 (us)", logx=False, logy=True)
    axes[col].set_title(f"{float(f):.0%} of GETs are 512 KB")
axes[0].legend(fontsize=8)
save(fig, "multi.png", "Head-of-line blocking with 1 or 16 independent clients (3 rounds)")
table(["Big GETs", "k GET/s"] + [f"{k} client{'s' if k > 1 else ''}: small p99 us, Homa tip / TCP stock" for k in KS] + ["16 clients: big GET p50 us, Homa / TCP"],
      [[f"{float(f):.0%}", str(r // 1000)] + [" / ".join(f"{mc(s_, t_, k, f, r, 'small_p99_us')[0]:.0f}" for _, s_, t_, _ in ML) for k in KS]
       + [" / ".join(f"{mc(s_, t_, 16, f, r, 'big_p50_us')[0]:.0f}" for _, s_, t_, _ in ML)] for f in RATES for r in RATES[f]])

# --- Per-request cost: 100 B GETs only over one endpoint, 10k-80k GET/s ------------------------
# All at the branch tip, TCP included.
RLINES = [("Homa, branch tip", "", "homa", "C0", ":"), ("TCP, stock", "-tcpclean", "tcp", "C1", "-"),
          ("TCP in Homa's config", "", "tcp", "C2", "--")]
RRATES = [10000, 20000, 40000, 60000, 80000]
kops = lambda r: float(r["done"]) / float(r["secs"]) / 1000
rpc = {(l, n): rows("mixload-rpc", s, t, rate=n) for l, s, t, _, _ in RLINES for n in RRATES}
per = lambda *cols: lambda r: sum(float(r[c]) for c in cols) / (4 * kops(r) * 1000)  # perf counts over 4 s
fig, axes = plt.subplots(1, 2, figsize=(11, 4.5))
for ax, (fn, ylabel) in zip(axes, [(lambda r: us_per_op(r, "server_busy_max", kops(r)), "Redis-core CPU per GET (us)"),
                                   (per("syscalls"), "redis-server system calls per GET")]):
    for lab, _, _, c, ls in RLINES:
        curve(ax, [n / 1000 for n in RRATES], {n / 1000: stat(rpc[(lab, n)], fn) for n in RRATES}, lab, c, ls)
    style(ax, "Offered load (k GET/s, Poisson)", ylabel, logx=False)
    ax.set_ylim(bottom=0)
axes[0].legend(fontsize=8)
save(fig, "rpc.png", "Per-request cost: 100 B GETs over one endpoint (3 rounds)")
table(["k GET/s"] + [f"{l}: Redis core / node us, syscalls (recv / send / epoll_wait) per GET, p99 us" for l, *_ in RLINES],
      [[str(n // 1000)] + [f"{fmt(stat(rpc[(l, n)], lambda r: us_per_op(r, 'server_busy_max', kops(r))))} / "
                           f"{fmt(stat(rpc[(l, n)], lambda r: us_per_op(r, 'server_busy_sum', kops(r))))}, "
                           f"{stat(rpc[(l, n)], per('syscalls'))[0]:.2f} ({stat(rpc[(l, n)], per('recvmsg', 'read'))[0]:.2f} / "
                           f"{stat(rpc[(l, n)], per('sendmsg', 'write', 'writev'))[0]:.2f} / {stat(rpc[(l, n)], per('epoll_wait'))[0]:.2f}), "
                           f"{stat(rpc[(l, n)], lambda r: float(r['small_p99_us']))[0]:.0f}" for l, *_ in RLINES] for n in RRATES])

# Where the Redis core's time goes at 40k GET/s (prof-core.sh): perf samples by symbol, grouped by
# layer (first match wins) and scaled to the core's busy time per GET; idle-loop samples dropped
LAYERS = [("idle", r"intel_idle|cpuidle|menu_select|do_idle|poll_idle|nohz|arch_cpu_idle|mwait|flush_smp_call"),
          ("IOMMU (translated mode)", r"clflush_cache_range|dma_pte|domain_mapping|iommu|iova|^qi_|cache_tag|dma_map|dma_unmap|__intel_|domain_flush|fq_ring|fq_flush"),
          ("Homa timetrace", r"^tt_"), ("sch_homa", r"^homa_qdisc"), ("Homa", r"^_*homa_"), ("NIC driver", r"mlx5"),
          ("TCP/IP, qdisc", r"tcp|^_*ip_|ipv4|inet|^_*sk_|sock|skb|napi|netif|^net_|^_*dev_|^sch_|qdisc|fq_codel|^eth_|^raw_|udp|rps|gro|dst_|neigh|xmit|page_pool|csum|xps|netdev|mtu"),
          ("system calls, epoll, copies", r"syscall|SYSRET|sys_|^ep_|epoll|fdget|fput|__fget|^aa_|security_|apparmor|copy|rep_movs|check_object|check_heap|check_stack_object|virt_addr_valid|iov|import_|exit_to_user|fpregs|vfs_|file_perm|rw_verify"),
          ("scheduler, IRQ, timers, locks", r"sched|psi_|update_|dequeue|enqueue|pick_|finish_task|switch|rcu|irq|softirq|bh_enable|handle_|hrtimer|tick|timer|ktime|clock|wake|ttwu|select_task|spin|mutex|timekeeping|tsc|task_|account|cgroup|preempt|asm_|resched|entity|set_next|put_prev|vruntime|record_times|calc_|cpu_util|reweight|native_"),
          ("memory allocation", r"kmem_cache|slab|kmalloc|kfree|memset|memcpy|alloc_pages|__alloc|free_unref|^page_|folio|memcg|obj_cgroup|objcg|refill_obj|__free|kmemdup")]
# Redis functions that parse RESP requests or encode RESP replies (LTO inlines the parser into them)
RESP = (r"^processInputBuffer|processMultibulk|processInlineBuffer|string2ll|lpStringToInt64|sdssubstr|createStringObject|"
        r"createEmbeddedString|sdsnewlen|sdsnewplacement|trimClientQueryBuffer|resetClientInternal|freeClientArgv|"
        r"acquirePendingCommand|sdsIncrLen|addReply|_addReplyToBuffer|tryAvoidBulkStrCopy|ll2string|setDeferred|prepareClientToWrite")
def profile(name, rate=40000):
    lines = open(os.path.join(RES, name)).read().splitlines()
    us, out = float(lines[0].split()[1]) * 1e4 / rate, {}
    for line in lines[1:]:
        pct, dso, _, sym = line.split()[:4]
        layer = (("Redis: RESP parsing, reply encoding" if re.search(RESP, sym) else "Redis: commands, event loop")
                 if "redis-server" in dso else "libc" if "libc" in dso else
                 next((n for n, rx in LAYERS if re.search(rx, sym)), "other kernel"))
        out[layer] = out.get(layer, 0) + float(pct.rstrip("%"))
    busy = sum(v for k, v in out.items() if k != "idle")
    return {k: v / busy * us for k, v in out.items()}, us
PROF = [(l, profile(f)) for l, f in (("Homa, branch tip", "prof-core-homa.txt"), ("TCP, stock", "prof-core-tcp-tcpclean.txt"),
                                     ("TCP in Homa's config", "prof-core-tcp.txt"))]
table(["Redis-core us per GET at 40k GET/s"] + [l for l, _ in PROF],
      [[k] + [f"{p.get(k, 0):.2f}" for _, (p, _) in PROF] for k in ["Redis: RESP parsing, reply encoding", "Redis: commands, event loop", "libc"] + [n for n, _ in LAYERS[1:]] + ["other kernel"]]
      + [["total (busy-cores.sh)"] + [f"{us:.1f}" for _, (_, us) in PROF]])

# --- Connection count: 30k small GET/s spread over N endpoints -----------------------------------
EPS = [16, 1000, 5000, 25000]
# plus Homa at the branch tip (idle-peer reaper in 10 ms slices, replies sent without a copy)
SLINES = LINES + [("Homa, branch tip", "-tip", "homa", "C0", ":")]
scale = {(lab, n): rows("mixload-scale", suf, tr, endpoints=n) for lab, suf, tr, _, _ in SLINES for n in EPS}
# kernel slab growth per endpoint; below 1000 endpoints the run-to-run slab noise swamps it
slab = lambda r: (float(r["slab_kb"]) - float(r["base_slab_kb"])) / float(r["endpoints"]) if int(r["endpoints"]) >= 1000 else float("nan")
fig, axes = plt.subplots(2, 2, figsize=(11, 8))
for ax, (fn, ylabel) in zip(axes.flat, [(lambda r: us_per_op(r, "server_busy_sum", kops(r)), "Server CPU per GET (us, all 20 CPUs)"),
                                        (slab, "Server kernel slab per endpoint (KB)"),
                                        (lambda r: float(r["small_p99_us"]), "GET p99 (us)"),
                                        (lambda r: float(r["small_p999_us"]), "GET p99.9 (us)")]):
    for lab, _, _, c, ls in SLINES:
        curve(ax, EPS, {n: stat(scale[(lab, n)], fn) for n in EPS}, lab, c, ls)
    style(ax, "TCP connections / Homa sockets", ylabel, xs=EPS)
axes[0][0].legend()
save(fig, "scale.png", "Connection count: 30k GET/s (100 B) over N endpoints (3 rounds)")
table(["Endpoints"] + [f"{l}: server us/op (node / Redis core), p99 / p99.9 us, slab KB/endpoint" for l, *_ in SLINES],
      [[str(n)] + [f"{fmt(stat(scale[(l, n)], lambda r: us_per_op(r, 'server_busy_sum', kops(r))))} / {fmt(stat(scale[(l, n)], lambda r: us_per_op(r, 'server_busy_max', kops(r))))}, "
                   f"{stat(scale[(l, n)], lambda r: float(r['small_p99_us']))[0]:.0f} / {stat(scale[(l, n)], lambda r: float(r['small_p999_us']))[0]:.0f}, "
                   f"{stat(scale[(l, n)], slab)[0]:.1f}".replace("nan", "-") for l, *_ in SLINES] for n in EPS])

# --- Cost of one 512 KB reply: 1000 GET/s of only small vs only big values ------------------------
cost = lambda l, s, t, f, busy: stat(rows("mixload-bigcost", s, t, big_frac=f), lambda r: us_per_op(r, busy, kops(r)))[0]
table(["", "Extra server CPU per 512 KB reply, whole node (us)", "... on the busiest CPU, the Redis core (us)", "Big GET p50, alone (us)"],
      [[l, f"{cost(l, s, t, '1', 'server_busy_sum') - cost(l, s, t, '0', 'server_busy_sum'):.0f}",
        f"{cost(l, s, t, '1', 'server_busy_max') - cost(l, s, t, '0', 'server_busy_max'):.0f}",
        f"{stat(rows('mixload-bigcost', s, t, big_frac='1'), lambda r: float(r['big_p50_us']))[0]:.0f}"] for l, s, t, _, _ in SLINES])

# --- The transport alone: cp_node, one message + a 100 B reply, one RPC outstanding -------------
cp = lambda proto, size, f: stat([r for r in csv.DictReader(open(os.path.join(RES, "cpnode.csv")))
                                  if r["protocol"] == proto and r["size"] == str(size)], lambda r: float(r[f]))[0]
table(["Message", "Homa P50 / P99 (us)", "TCP in Homa's config P50 / P99 (us)", "Homa / TCP (P50)"],
      [[{100: "100 B", 65536: "64 KB", 524288: "512 KB"}[n], f"{cp('homa', n, 'p50_us'):.0f} / {cp('homa', n, 'p99_us'):.0f}",
        f"{cp('tcp', n, 'p50_us'):.0f} / {cp('tcp', n, 'p99_us'):.0f}", f"{cp('homa', n, 'p50_us') / cp('tcp', n, 'p50_us'):.2f}"]
       for n in (100, 65536, 524288)])
