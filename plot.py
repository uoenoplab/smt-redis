# plot.py: latency.png, cpu.png, throughput.png and the README tables from results/*.csv
# (uv run --with matplotlib python plot.py)
import csv, collections, statistics as st
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

TR = {"homa": ("Homa", "tab:red"), "tcp": ("TCP", "tab:blue")}
WL = {"c52": "c52 (values 17 B-3.6 KB)", "c53": "c53 (values 8 B-35 KB)"}
NS = (1024, 8192, 24576)

g = collections.defaultdict(list)
for f in ("homa", "tcp"):
    for r in csv.DictReader(open(f"results/fleet-{f}.csv")):
        g[(r["workload"], int(r["clients"]), int(r["offered"]), r["transport"])].append(r)
D = {k: {c: st.median(float(r[c] or 0) for r in v) for c in v[0] if c not in ("workload", "transport")}
     for k, v in g.items()}
LOADS = sorted({k[2] for k in D})
ok = lambda m: m["ops"] >= 0.98 * m["offered"]  # the server kept up
cpu = lambda m: m["server_busy_sum"] / 100 / m["ops"] * 1e6  # node0 CPU us per request

# latency: GET p50 and p99 vs offered load
fig, axs = plt.subplots(2, len(NS), figsize=(13, 7.5), sharey="row")
for i, w in enumerate(WL):
    for j, n in enumerate(NS):
        ax = axs[i][j]
        for tr, (name, col) in TR.items():
            for c, ls, lab in (("get_p99_us", "-", "p99"), ("get_p50_us", "--", "p50")):
                pts = [(L / 1000, D[(w, n, L, tr)]) for L in LOADS if ok(D[(w, n, L, tr)])]
                ax.plot([x for x, _ in pts], [m[c] for _, m in pts], ls, marker="o", ms=3, color=col,
                        label=f"{name} {lab}")
        ax.set_xlim(15, 105); ax.set_yscale("log"); ax.grid(alpha=.3)
        ax.set_title(f"{WL[w]}, {n:,} clients", fontsize=10)
        if i == 1: ax.set_xlabel("offered load (k requests/s)")
        if j == 0: ax.set_ylabel("GET latency (us)")
axs[0][0].legend(fontsize=8)
fig.suptitle("GET p50 and p99 vs load, at the loads each transport sustains (achieved within 2% of offered)")
fig.tight_layout(); fig.savefig("latency.png", dpi=300)

# CPU per request at 20k and 40k requests/s (sustained everywhere)
fig, axs = plt.subplots(1, 2, figsize=(11, 4), sharey=True)
for ax, w in zip(axs, WL):
    k = 0
    for L, hatch in ((20000, ""), (40000, "//")):
        for tr, (name, col) in TR.items():
            v = [cpu(D[(w, n, L, tr)]) for n in NS]
            bars = ax.bar([x + (k - 1.5) * .2 for x in range(len(NS))], v, .2, color=col, hatch=hatch,
                          edgecolor="white", label=f"{name}, {L // 1000}k requests/s")
            ax.bar_label(bars, fmt="%.0f", fontsize=7); k += 1
    ax.set_xticks(range(len(NS)), [f"{n:,}" for n in NS]); ax.set_xlabel("clients")
    ax.set_title(WL[w], fontsize=10); ax.grid(axis="y", alpha=.3)
axs[0].set_ylabel("node0 CPU per request (us)"); axs[0].legend(fontsize=7)
fig.suptitle("Server CPU per request (all node0 CPUs, MPERF)")
fig.tight_layout(); fig.savefig("cpu.png", dpi=300)

# throughput at 100k offered: the most each transport sustains
fig, axs = plt.subplots(1, 2, figsize=(11, 4), sharey=True)
for ax, w in zip(axs, WL):
    for b, (tr, (name, col)) in enumerate(TR.items()):
        bars = ax.bar([x + (b - .5) * .4 for x in range(len(NS))], [D[(w, n, 100000, tr)]["ops"] / 1000 for n in NS],
                      .4, color=col, label=name)
        ax.bar_label(bars, fmt="%.0f", fontsize=7)
    ax.axhline(100, ls="--", color="gray", lw=1)
    ax.set_xticks(range(len(NS)), [f"{n:,}" for n in NS]); ax.set_xlabel("clients")
    ax.set_title(WL[w], fontsize=10); ax.grid(axis="y", alpha=.3)
axs[0].set_ylabel("achieved (k requests/s)"); axs[0].legend(fontsize=8, loc="lower left")
fig.suptitle("Throughput at 100k requests/s offered (dashed)")
fig.tight_layout(); fig.savefig("throughput.png", dpi=300)

# README tables
for w in WL:
    print(f"\n**{w}** (latency only where the achieved load is within 2% of the offered)\n")
    print("| clients | load | Homa p50 / p99 (us) | TCP p50 / p99 (us) | node0 CPU per request, Homa / TCP (us) |")
    print("|---:|---:|---:|---:|---:|")
    for n in NS:
        for L in LOADS:
            h, t = D[(w, n, L, "homa")], D[(w, n, L, "tcp")]
            f = lambda m: f"{m['get_p50_us']:.0f} / {m['get_p99_us']:.0f}" if ok(m) else f"saturated at {m['ops'] / 1000:.0f}k"
            c = f"{cpu(h):.1f} / {cpu(t):.1f}" if ok(h) and ok(t) else "-"
            print(f"| {n:,} | {L // 1000}k | {f(h)} | {f(t)} | {c} |")
rows = [r for f in ("homa", "tcp") for r in csv.DictReader(open(f"results/fleet-{f}.csv")) if int(r["offered"]) <= 40000]
print(f"\nRuns at 20k and 40k offered: {len(rows)}; largest deviation of achieved from offered: "
      f"{max(abs(float(r['ops']) / int(r['offered']) - 1) for r in rows) * 100:.1f}%")

# few clients (4-768): GET p50/p99 and CPU per request vs clients, at 20k and 60k requests/s
S = collections.defaultdict(list)
for f in ("homa", "tcp"):
    for r in csv.DictReader(open(f"results/sweep-{f}.csv")):
        S[(r["workload"], int(r["clients"]), int(r["offered"]), r["transport"])].append(r)
S = {k: {c: st.median(float(r[c] or 0) for r in v) for c in v[0] if c not in ("workload", "transport")}
     for k, v in S.items()}
SN, SL = sorted({k[1] for k in S}), sorted({k[2] for k in S})
fig, axs = plt.subplots(2, len(SL), figsize=(11, 7.5), sharex=True)
for i, w in enumerate(WL):
    for j, L in enumerate(SL):
        ax = axs[i][j]
        for tr, (name, col) in TR.items():
            for c, ls, lab in (("get_p99_us", "-", "p99"), ("get_p50_us", "--", "p50")):
                pts = [(n, S[(w, n, L, tr)][c]) for n in SN if ok(S[(w, n, L, tr)])]
                ax.plot([n for n, _ in pts], [v for _, v in pts], ls, marker="o", ms=3, color=col, label=f"{name} {lab}")
        ax.set_xscale("log"); ax.set_yscale("log"); ax.grid(alpha=.3)
        ax.set_title(f"{WL[w]}, {L // 1000}k requests/s", fontsize=10)
        if i == 1: ax.set_xlabel("clients")
        if j == 0: ax.set_ylabel("GET latency (us)")
axs[0][0].legend(fontsize=8)
fig.suptitle("Few clients: GET p50 and p99 vs clients, at the loads each transport sustains")
fig.tight_layout(); fig.savefig("few-latency.png", dpi=300)
fig, axs = plt.subplots(1, 2, figsize=(11, 4), sharey=True)
for ax, w in zip(axs, WL):
    for tr, (name, col) in TR.items():
        for L, ls in zip(SL, (":", "-")):
            pts = [(n, cpu(S[(w, n, L, tr)])) for n in SN if ok(S[(w, n, L, tr)])]
            ax.plot([n for n, _ in pts], [v for _, v in pts], ls, marker="o", ms=3, color=col, label=f"{name}, {L // 1000}k requests/s")
    ax.set_xscale("log"); ax.set_ylim(bottom=0); ax.grid(alpha=.3); ax.set_title(WL[w], fontsize=10); ax.set_xlabel("clients")
axs[0].set_ylabel("node0 CPU per request (us)"); axs[0].legend(fontsize=8)
fig.suptitle("Few clients: server CPU per request vs clients")
fig.tight_layout(); fig.savefig("few-cpu.png", dpi=300)
print("\n=== few clients ===")
for w in WL:
    print(f"\n**{w}**: GET p50 / p99 (us) and node0 CPU per request (us), Homa | TCP\n")
    print("| clients | " + " | ".join(f"{L // 1000}k Homa | {L // 1000}k TCP" for L in SL) + " |")
    print("|---:" * (2 * len(SL) + 1) + "|")
    for n in SN:
        cells = []
        for L in SL:
            for tr in TR:
                m = S[(w, n, L, tr)]
                cells.append(f"{m['get_p50_us']:.0f} / {m['get_p99_us']:.0f}, {cpu(m):.1f}" if ok(m) else f"saturated at {m['ops'] / 1000:.0f}k")
        print(f"| {n} | " + " | ".join(cells) + " |")
