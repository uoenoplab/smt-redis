# plot.py: the figures and README tables from results/*.csv (uv run --with matplotlib python plot.py)
#   clients-*.csv: 4-24,576 clients at 20k and 60k requests/s; load-*.csv: 1,024-24,576 clients, 20k-100k
import csv, collections, statistics as st
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

TR = {"homa": "Homa", "tcp": "TCP"}
WL = {"c52": "c52 (values 17 B-3.6 KB)", "c53": "c53 (values 8 B-35 KB)"}

def load(name):  # (workload, threads, clients, offered, transport) -> median of each column over rounds
    g = collections.defaultdict(list)
    for tr in TR:
        for r in csv.DictReader(open(f"results/{name}-{tr}.csv")):
            g[(r["workload"], int(r["threads"]), int(r["clients"]), int(r["offered"]), r["transport"])].append(r)
    return {k: {c: st.median(float(r[c] or 0) for r in v) for c in v[0] if c not in ("workload", "transport")}
            for k, v in g.items()}

C, D, P = load("clients"), load("load"), load("capacity")
# the server kept up with the offered load: achieved within 0.5% (3 sd of a Poisson count over 20 s at
# 20k/s); a point short of it has a backlog, and its latency (from the arrival) grows toward N / achieved
ok = lambda m: m["ops"] >= 0.995 * m["offered"]
cpu = lambda m: m["server_busy_sum"] / 100 / m["ops"] * 1e6  # node0 CPU us per request
CT, CL = sorted({k[1] for k in C}), sorted({k[3] for k in C})

COL = {"homa": "#d62728", "tcp": "#1f77b4"}
plt.rcParams.update({"font.size": 11, "lines.linewidth": 2.2, "lines.markersize": 6})

def ends(ax, xs, vs, tr, top, ls="-", label=None):
    # a value over the axis is drawn at the top and labelled
    ax.plot(xs, [min(v, top) for v in vs], ls, marker="o", color=COL[tr], label=label or TR[tr])
    for x, v in zip(xs, vs):
        if v > top: ax.annotate(f"{v / 1000:.1f} ms", (x, top), textcoords="offset points", xytext=(5, -12),
                                fontsize=9, color=COL[tr])

# clients sweep, one figure per workload: rows CPU / p50 / p99, columns load; one line per transport
# and client thread count (4, 8, 12 threads: solid is 8, the only one that spans 8 to 24,576 clients)
LS = {4: ":", 8: "-", 12: "--"}
ROWS = (("CPU per request (us)", cpu), ("GET p50 (us)", lambda m: m["get_p50_us"]), ("GET p99 (us)", lambda m: m["get_p99_us"]))
for w in WL:
    fig, axs = plt.subplots(len(ROWS), len(CL), figsize=(11, 10), sharex=True)
    for i, (lab, f) in enumerate(ROWS):
        for j, L in enumerate(CL):
            ax = axs[i][j]
            ser = {(tr, t): sorted((k[2], f(m)) for k, m in C.items() if k[:2] == (w, t) and k[3] == L and k[4] == tr and ok(m))
                   for tr in TR for t in CT}
            vals = sorted(v for pts in ser.values() for _, v in pts)
            top = vals[-1] * 1.12 if vals[-1] <= 4 * vals[-2] else vals[-2] * 1.3
            for (tr, t), pts in ser.items():
                if pts: ends(ax, [n for n, _ in pts], [v for _, v in pts], tr, top, LS[t], f"{TR[tr]}, {t} threads")
            last = max(n for pts in ser.values() for n, _ in pts)
            if last < 24576: ax.text(.97, .06, f"neither keeps up\nbeyond {last:,} clients", transform=ax.transAxes,
                                     ha="right", fontsize=9, color="gray")
            ax.set_xscale("log"); ax.set_ylim(0, top); ax.grid(alpha=.3)
            if i == 0: ax.set_title(f"{L // 1000}k requests/s")
            if j == 0: ax.set_ylabel(lab)
            if i == len(ROWS) - 1: ax.set_xlabel("clients")
    axs[0][0].legend(fontsize=8, ncol=2)
    fig.suptitle(f"{WL[w]}: server CPU and GET latency vs clients")
    fig.tight_layout(); fig.savefig(f"{w}-clients.png", dpi=300)

# load sweep, one figure per workload: rows CPU / p50 / p99, columns offered load, x = clients; a point
# only where the transport kept up, the load it achieved otherwise written at the bottom
NS, LOADS = sorted({k[2] for k in D}), sorted({k[3] for k in D})
for w in WL:
    WLOADS = [L for L in LOADS if any(ok(D[(w, 16, n, L, tr)]) for n in NS for tr in TR)]  # a load nobody keeps up
    fig, axs = plt.subplots(len(ROWS), len(WLOADS), figsize=(3.6 * len(WLOADS), 10), sharex=True, squeeze=False)
    for i, (lab, f) in enumerate(ROWS):                                                     # with: see throughput.png
        for j, L in enumerate(WLOADS):
            ax = axs[i][j]
            ser = {tr: [(n, f(D[(w, 16, n, L, tr)])) for n in NS if ok(D[(w, 16, n, L, tr)])] for tr in TR}
            vals = sorted(v for pts in ser.values() for _, v in pts) or [1, 1]
            top = vals[-1] * 1.12 if len(vals) < 2 or vals[-1] <= 4 * vals[-2] else vals[-2] * 1.3
            for tr, pts in ser.items():
                if pts: ends(ax, [n for n, _ in pts], [v for _, v in pts], tr, top)
            ax.set_xscale("log"); ax.set_xlim(NS[0] / 1.5, NS[-1] * 1.5); ax.set_ylim(0, top); ax.grid(alpha=.3)
            sat = [f"{TR[tr]} {n:,}" for tr in TR for n in NS if not ok(D[(w, 16, n, L, tr)])] if i == len(ROWS) - 1 else []
            if sat:  # in the corner (bottom first) with the fewest points under it
                inv = ax.transAxes.inverted()
                xy = [inv.transform(ax.transData.transform((n, min(v, top)))) for pts in ser.values() for n, v in pts]
                h = .04 * (len(sat) + 1)  # the text's box: 40% wide, one line ~4% of the panel
                cx, cy = min(((x, y) for y in (0, 1) for x in (0, 1)),
                             key=lambda c: (sum(abs(px - c[0]) < .4 and abs(py - c[1]) < h for px, py in xy), c[1]))
                ax.text(.03 + .94 * cx, .04 + .92 * cy, "saturated\n" + "\n".join(sat), transform=ax.transAxes,
                        fontsize=7, color="gray", ha="right" if cx else "left", va="top" if cy else "bottom")
            if i == 0: ax.set_title(f"{L // 1000}k requests/s")
            if j == 0: ax.set_ylabel(lab)
            if i == len(ROWS) - 1: ax.set_xlabel("clients (16 threads)")
    axs[0][0].legend()
    fig.suptitle(f"{WL[w]}: server CPU and GET latency vs clients, at each offered load")
    fig.tight_layout(); fig.savefig(f"{w}-load.png", dpi=300)

# capacity: achieved at 100k offered, vs clients (memtier 9006af8, which counts every completion sent
# after the warm-up; 7f8b5a3 dates a request by its arrival and drops the backlog that waited through it)
fig, axs = plt.subplots(1, 2, figsize=(11, 4), sharey=True)
for ax, w in zip(axs, WL):
    ys = {tr: [P[(w, 16, n, 100000, tr)]["ops"] / 1000 for n in NS] for tr in TR}
    for tr in TR:  # each label on the side away from the other transport's point
        ax.plot(NS, ys[tr], "-o", color=COL[tr], label=TR[tr])
        other = [v for t, v in ys.items() if t != tr][0]
        for x, y, o in zip(NS, ys[tr], other):
            up = y > o or (y == o and tr == "tcp")
            ax.annotate(f"{y:.0f}", (x, y), textcoords="offset points", xytext=(0, 6 if up else -14),
                        ha="center", fontsize=8, color=COL[tr])
    ax.axhline(100, ls="--", color="gray", lw=1); ax.set_xscale("log"); ax.set_ylim(0, 110)
    ax.set_xlabel("clients (16 threads)"); ax.set_title(WL[w]); ax.grid(alpha=.3)
axs[0].set_ylabel("achieved (k requests/s)"); axs[0].legend(loc="lower left")
fig.suptitle("Capacity: throughput at 100k requests/s offered (dashed)")
fig.tight_layout(); fig.savefig("throughput.png", dpi=300)

# README tables
cell = lambda m: f"{m['get_p50_us']:.0f} / {m['get_p99_us']:.0f}, {cpu(m):.1f}" if ok(m) else "saturated"
print("=== clients ===")
for w in WL:
    print(f"\n**{w}**: GET p50 / p99 (us), node0 CPU per request (us)\n")
    print("| threads x clients per thread | clients | " + " | ".join(f"{L // 1000}k {TR[tr]}" for L in CL for tr in TR) + " |")
    print("|---:" * (2 * len(CL) + 2) + "|")
    for t, n in sorted({(k[1], k[2]) for k in C if k[0] == w}):
        print(f"| {t} x {n // t} | {n:,} | " + " | ".join(cell(C[(w, t, n, L, tr)]) for L in CL for tr in TR) + " |")
print("=== load ===")
for w in WL:
    print(f"\n**{w}**: GET p50 / p99 (us), node0 CPU per request (us)\n")
    print("| clients | load | Homa | TCP |\n|---:|---:|---:|---:|")
    for n in NS:
        for L in LOADS:
            print(f"| {n:,} | {L // 1000}k | {cell(D[(w, 16, n, L, 'homa')])} | {cell(D[(w, 16, n, L, 'tcp')])} |")
print("=== capacity ===\n\nAchieved at 100k requests/s offered (k requests/s)\n")
print("| clients | " + " | ".join(f"{w} {TR[tr]}" for w in WL for tr in TR) + " |\n|---:" + "|---:" * 2 * len(WL) + "|")
for n in NS:
    print(f"| {n:,} | " + " | ".join(f"{P[(w, 16, n, 100000, tr)]['ops'] / 1000:.1f}" for w in WL for tr in TR) + " |")
rows = [r for name in ("clients", "load") for tr in TR for r in csv.DictReader(open(f"results/{name}-{tr}.csv"))
        if int(r["offered"]) <= 40000]
print(f"\nRuns at 20k-40k offered: {len(rows)}; largest deviation of achieved from offered: "
      f"{max(abs(float(r['ops']) / int(r['offered']) - 1) for r in rows) * 100:.1f}%")
