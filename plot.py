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
            g[(r["workload"], int(r.get("threads") or 16), int(r["clients"]), int(r["offered"]), r["transport"])].append(r)
    return {k: {c: st.median(float(r[c] or 0) for r in v) for c in v[0] if c not in ("workload", "transport")}
            for k, v in g.items()}

C, D = load("clients"), load("load")
ok = lambda m: m["ops"] >= 0.98 * m["offered"]  # the server kept up with the offered load
cpu = lambda m: m["server_busy_sum"] / 100 / m["ops"] * 1e6  # node0 CPU us per request
CT, CL = sorted({k[1] for k in C}), sorted({k[3] for k in C})

COL = {"homa": "#d62728", "tcp": "#1f77b4"}
plt.rcParams.update({"font.size": 11, "lines.linewidth": 2.2, "lines.markersize": 6})
FT = 8  # the thread count whose series spans 8 to 24,576 clients; 4 and 12 threads are in the tables

def ends(ax, xs, vs, tr, top):
    # a value over the axis is drawn at the top and labelled
    ax.plot(xs, [min(v, top) for v in vs], "-o", color=COL[tr], label=TR[tr])
    for x, v in zip(xs, vs):
        if v > top: ax.annotate(f"{v / 1000:.1f} ms", (x, top), textcoords="offset points", xytext=(5, -12),
                                fontsize=9, color=COL[tr])

# clients sweep, one figure per workload: rows CPU / p50 / p99, columns load
ROWS = (("CPU per request (us)", cpu), ("GET p50 (us)", lambda m: m["get_p50_us"]), ("GET p99 (us)", lambda m: m["get_p99_us"]))
for w in WL:
    fig, axs = plt.subplots(len(ROWS), len(CL), figsize=(11, 10), sharex=True)
    for i, (lab, f) in enumerate(ROWS):
        for j, L in enumerate(CL):
            ax = axs[i][j]
            ser = {tr: sorted((k[2], f(m)) for k, m in C.items() if k[:2] == (w, FT) and k[3] == L and k[4] == tr and ok(m))
                   for tr in TR}
            vals = sorted(v for pts in ser.values() for _, v in pts)
            top = vals[-1] * 1.12 if vals[-1] <= 4 * vals[-2] else vals[-2] * 1.3
            for tr, pts in ser.items():
                ends(ax, [n for n, _ in pts], [v for _, v in pts], tr, top)
            last = max(n for pts in ser.values() for n, _ in pts)
            if last < 24576: ax.text(.97, .06, f"neither keeps up\nbeyond {last:,} clients", transform=ax.transAxes,
                                     ha="right", fontsize=9, color="gray")
            ax.set_xscale("log"); ax.set_ylim(0, top); ax.grid(alpha=.3)
            if i == 0: ax.set_title(f"{L // 1000}k requests/s")
            if j == 0: ax.set_ylabel(lab)
            if i == len(ROWS) - 1: ax.set_xlabel("clients (8 threads)")
    axs[0][0].legend()
    fig.suptitle(f"{WL[w]}: server CPU and GET latency vs clients")
    fig.tight_layout(); fig.savefig(f"{w}-clients.png", dpi=300)

# load sweep, one figure per workload: grouped bars, rows p50 / p99, columns clients
NS, LOADS = sorted({k[2] for k in D if k[2] >= 1024}), sorted({k[3] for k in D})
for w in WL:
    fig, axs = plt.subplots(2, len(NS), figsize=(13, 7), sharey="row")
    for i, (c, lab) in enumerate((("get_p50_us", "GET p50 (us)"), ("get_p99_us", "GET p99 (us)"))):
        for j, n in enumerate(NS):
            ax = axs[i][j]
            for b, tr in enumerate(TR):
                for x, L in enumerate(LOADS):
                    m, xb = D[(w, 16, n, L, tr)], x + (b - .5) * .4
                    if ok(m): ax.bar(xb, m[c], .4, color=COL[tr], label=TR[tr] if x == 0 else None)
                    else: ax.text(xb, 0, f" saturated at {m['ops'] / 1000:.0f}k", rotation=90, va="bottom",
                                  ha="center", fontsize=8, color=COL[tr])
            ax.set_xticks(range(len(LOADS)), [f"{L // 1000}k" for L in LOADS]); ax.set_xlim(-.6, len(LOADS) - .3)
            ax.grid(axis="y", alpha=.3)
            if i == 0: ax.set_title(f"{n:,} clients")
            if i == 1: ax.set_xlabel("offered load (requests/s)")
            if j == 0: ax.set_ylabel(lab)
    axs[0][0].legend()
    fig.suptitle(f"{WL[w]}: GET latency vs load (16 client threads)")
    fig.tight_layout(); fig.savefig(f"{w}-load.png", dpi=300)

# throughput at 100k offered
fig, axs = plt.subplots(1, 2, figsize=(11, 4), sharey=True)
for ax, w in zip(axs, WL):
    for b, tr in enumerate(TR):
        bars = ax.bar([x + (b - .5) * .4 for x in range(len(NS))], [D[(w, 16, n, 100000, tr)]["ops"] / 1000 for n in NS],
                      .4, color=COL[tr], label=TR[tr])
        ax.bar_label(bars, fmt="%.0f", fontsize=9)
    ax.axhline(100, ls="--", color="gray", lw=1)
    ax.set_xticks(range(len(NS)), [f"{n:,}" for n in NS]); ax.set_xlabel("clients")
    ax.set_title(WL[w]); ax.grid(axis="y", alpha=.3)
axs[0].set_ylabel("achieved (k requests/s)"); axs[0].legend(loc="lower left")
fig.suptitle("Throughput at 100k requests/s offered (dashed)")
fig.tight_layout(); fig.savefig("throughput.png", dpi=300)

# README tables
cell = lambda m: f"{m['get_p50_us']:.0f} / {m['get_p99_us']:.0f}, {cpu(m):.1f}" if ok(m) else f"saturated at {m['ops'] / 1000:.0f}k"
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
rows = [r for name in ("clients", "load") for tr in TR for r in csv.DictReader(open(f"results/{name}-{tr}.csv"))
        if int(r["offered"]) <= 40000]
print(f"\nRuns at 20k-40k offered: {len(rows)}; largest deviation of achieved from offered: "
      f"{max(abs(float(r['ops']) / int(r['offered']) - 1) for r in rows) * 100:.1f}%")
