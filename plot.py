# plot.py: the figures and README tables from results/*.csv (uv run --with matplotlib python plot.py)
#   clients-*.csv: 4-24,576 clients at 20k and 60k requests/s; load-*.csv: 1,024-24,576 clients, 20k-100k
import csv, collections, statistics as st
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

TR = {"homa": "Homa", "tcp": "TCP"}
WL = {"c52": "c52 (values 17 B-3.6 KB)", "c53": "c53 (values 8 B-35 KB)"}
SHADE = {"homa": ("#f4a582", "#d6604d", "#67001f"), "tcp": ("#92c5de", "#4393c3", "#053061")}
MARK = ("o", "s", "^")

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

def by_clients(ax, w, L, f):
    # one line per client thread count, so a line varies only the clients per thread; linear axis from 0,
    # and a point over 4x the panel's next largest is drawn at the top with its value
    lines = [(tr, t, col, mk, sorted((k[2], f(m)) for k, m in C.items()
                                     if k[0] == w and k[1] == t and k[3] == L and k[4] == tr and ok(m)))
             for tr in TR for t, col, mk in zip(CT, SHADE[tr], MARK)]
    vals = sorted(v for *_, pts in lines for _, v in pts)
    top = vals[-1] * 1.1 if vals[-1] <= 4 * vals[-2] else vals[-2] * 1.25
    for tr, t, col, mk, pts in lines:
        ax.plot([n for n, _ in pts], [min(v, top) for _, v in pts], "-", marker=mk, ms=4, color=col,
                label=f"{TR[tr]}, {t} threads")
        for n, v in pts:
            if v > top:
                ax.annotate(f"{v / 1000:.0f} ms", (n, top), textcoords="offset points", xytext=(6, -10), fontsize=7, color=col)
    ax.set_xscale("log"); ax.set_ylim(0, top); ax.grid(alpha=.3)
    ax.set_title(f"{WL[w]}, {L // 1000}k requests/s", fontsize=9)

# 1. server CPU per request vs clients
fig, axs = plt.subplots(2, len(CL), figsize=(11, 7.5), sharex=True)
for i, w in enumerate(WL):
    for j, L in enumerate(CL):
        by_clients(axs[i][j], w, L, cpu)
        if j == 0: axs[i][j].set_ylabel("node0 CPU per request (us)")
for ax in axs[-1]: ax.set_xlabel("clients")
axs[0][0].legend(fontsize=7, ncol=2)
fig.suptitle("Server CPU per request vs clients (one line per client thread count)")
fig.tight_layout(); fig.savefig("cpu-vs-clients.png", dpi=300)

# 2. GET p50 and p99 vs clients
fig, axs = plt.subplots(4, len(CL), figsize=(11, 13), sharex=True)
for i, (w, c, lab) in enumerate((w, c, lab) for w in WL for c, lab in (("get_p50_us", "p50"), ("get_p99_us", "p99"))):
    for j, L in enumerate(CL):
        by_clients(axs[i][j], w, L, lambda m: m[c])
        axs[i][j].set_title(f"{WL[w]}, {L // 1000}k requests/s: GET {lab}", fontsize=9)
        if j == 0: axs[i][j].set_ylabel(f"GET {lab} (us)")
for ax in axs[-1]: ax.set_xlabel("clients")
axs[0][0].legend(fontsize=7, ncol=2)
fig.suptitle("GET latency vs clients (one line per client thread count; loads each transport sustains)")
fig.tight_layout(); fig.savefig("latency-vs-clients.png", dpi=300)

# 3. GET p50 and p99 vs offered load, 1,024-24,576 clients
NS, LOADS = sorted({k[2] for k in D if k[2] >= 1024}), sorted({k[3] for k in D})
fig, axs = plt.subplots(2, len(NS), figsize=(13, 7.5), sharey="row")
for i, w in enumerate(WL):
    for j, n in enumerate(NS):
        ax = axs[i][j]
        for tr in TR:
            for c, ls, lab in (("get_p99_us", "-", "p99"), ("get_p50_us", "--", "p50")):
                pts = [(L / 1000, D[(w, 16, n, L, tr)][c]) for L in LOADS if ok(D[(w, 16, n, L, tr)])]
                ax.plot([x for x, _ in pts], [v for _, v in pts], ls, marker="o", ms=3, color=SHADE[tr][2],
                        label=f"{TR[tr]} {lab}")
        ax.set_xlim(15, 105); ax.set_yscale("log"); ax.grid(alpha=.3)
        ax.set_title(f"{WL[w]}, {n:,} clients", fontsize=10)
        if i == 1: ax.set_xlabel("offered load (k requests/s)")
        if j == 0: ax.set_ylabel("GET latency (us)")
axs[0][0].legend(fontsize=8)
fig.suptitle("GET p50 and p99 vs load, up to the load each transport sustains (16 client threads)")
fig.tight_layout(); fig.savefig("latency-vs-load.png", dpi=300)

# 4. throughput at 100k offered
fig, axs = plt.subplots(1, 2, figsize=(11, 4), sharey=True)
for ax, w in zip(axs, WL):
    for b, tr in enumerate(TR):
        bars = ax.bar([x + (b - .5) * .4 for x in range(len(NS))], [D[(w, 16, n, 100000, tr)]["ops"] / 1000 for n in NS],
                      .4, color=SHADE[tr][2], label=TR[tr])
        ax.bar_label(bars, fmt="%.0f", fontsize=7)
    ax.axhline(100, ls="--", color="gray", lw=1)
    ax.set_xticks(range(len(NS)), [f"{n:,}" for n in NS]); ax.set_xlabel("clients")
    ax.set_title(WL[w], fontsize=10); ax.grid(axis="y", alpha=.3)
axs[0].set_ylabel("achieved (k requests/s)"); axs[0].legend(fontsize=8, loc="lower left")
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
