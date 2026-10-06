# plot.py: fleet-p99.png, fleet-cpu.png, sweep-p99.png and README tables from results/*.csv (uv run --with matplotlib python plot.py)
import csv, collections, statistics as st
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

TR = {"homa": ("Homa", "tab:red"), "tcp": ("stock TCP", "tab:blue")}
WL = {"c52": "c52 (values 17 B-3.6 KB)", "c53": "c53 (values 8 B-35 KB)"}

def load(prefix):
    rows = []
    for f in ("homa", "stocktcp"):
        rows += list(csv.DictReader(open(f"results/{prefix}-{f}.csv")))
    g = collections.defaultdict(list)
    for r in rows:
        g[(r["workload"], int(r.get("threads") or 16), int(r["clients"]), int(r["offered"]), r["transport"])].append(r)
    return {k: {c: st.median(float(r[c] or 0) for r in v) for c in v[0] if c not in ("workload", "transport")} | {"n": len(v)}
            for k, v in g.items()}

def ok(m):  # the server kept up: achieved within 2% of offered
    return m["ops"] >= 0.98 * m["offered"]

fleet, sweep = load("fleet"), load("sweep")
NS, LOADS = sorted({k[2] for k in fleet}), sorted({k[3] for k in fleet})

# fleet: GET p99 vs offered load, one panel per (workload, clients); hollow = server did not keep up
fig, axs = plt.subplots(2, len(NS), figsize=(4 * len(NS), 7), sharey=True)
for i, w in enumerate(WL):
    for j, n in enumerate(NS):
        ax = axs[i][j]
        for tr, (name, col) in TR.items():
            pts = [(L, fleet[(w, 16, n, L, tr)]) for L in LOADS if (w, 16, n, L, tr) in fleet]
            ax.plot([L / 1000 for L, _ in pts], [m["get_p99_us"] for _, m in pts], color=col, label=name)
            for L, m in pts:
                ax.plot(L / 1000, m["get_p99_us"], "o", color=col, mfc=col if ok(m) else "white")
        ax.set_yscale("log"); ax.set_title(f"{WL[w]}, {n} clients", fontsize=9); ax.grid(alpha=.3)
        if i == 1: ax.set_xlabel("offered load (k requests/s)")
        if j == 0: ax.set_ylabel("GET p99 (us)")
axs[0][0].legend(fontsize=8)
fig.suptitle("Fleet: GET p99 vs load (hollow: achieved < 98% of offered)")
fig.tight_layout(); fig.savefig("fleet-p99.png", dpi=300)

# fleet: node0 CPU per request vs clients at each load the server sustains
fig, axs = plt.subplots(1, 2, figsize=(10, 4))
for ax, w in zip(axs, WL):
    for tr, (name, col) in TR.items():
        for L, ls in ((20000, ":"), (40000, "-")):
            pts = [(n, fleet[(w, 16, n, L, tr)]) for n in NS]
            ax.plot([n for n, _ in pts], [m["server_busy_sum"] / 100 / m["ops"] * 1e6 for _, m in pts], ls,
                    marker="o", color=col, label=f"{name}, {L // 1000}k requests/s")
    ax.set_xscale("log"); ax.set_ylim(bottom=0); ax.grid(alpha=.3); ax.set_title(WL[w], fontsize=9)
    ax.set_xlabel("clients")
axs[0].set_ylabel("node0 CPU per request (us)"); axs[0].legend(fontsize=8)
fig.suptitle("Fleet: server CPU per request vs clients (MPERF, all node0 CPUs)")
fig.tight_layout(); fig.savefig("fleet-cpu.png", dpi=300)

# sweep: GET p99 vs clients at each load, few clients
TS = sorted({k[1] for k in sweep}); SL = sorted({k[3] for k in sweep})
fig, axs = plt.subplots(2, len(SL), figsize=(5 * len(SL), 7), sharey="row")
for i, w in enumerate(WL):
    for j, L in enumerate(SL):
        ax = axs[i][j]
        for tr, (name, col) in TR.items():
            pts = sorted((k[2], m) for k, m in sweep.items() if k[0] == w and k[3] == L and k[4] == tr)
            ax.plot([n for n, _ in pts], [m["get_p99_us"] for _, m in pts], "o", color=col, label=name,
                    mfc="none")
        ax.set_xscale("log"); ax.set_yscale("log"); ax.grid(alpha=.3)
        ax.set_title(f"{WL[w]}, {L // 1000}k requests/s", fontsize=9)
        if i == 1: ax.set_xlabel("clients (threads x clients per thread)")
        if j == 0: ax.set_ylabel("GET p99 (us)")
axs[0][0].legend(fontsize=8)
fig.suptitle("Few clients: GET p99 vs clients (latency from arrival)")
fig.tight_layout(); fig.savefig("sweep-p99.png", dpi=300)

# README tables
def cell(m, c):
    return "-" if m is None else f"{m[c]:.0f}" + ("" if ok(m) else "*")
for w in WL:
    print(f"\n{w}: GET p99 us, Homa / stock TCP (* achieved < 98% of offered)\n")
    print("| clients | " + " | ".join(f"{L // 1000}k" for L in LOADS) + " |")
    print("|---" * (len(LOADS) + 1) + "|")
    for n in NS:
        print(f"| {n} | " + " | ".join(f"{cell(fleet.get((w, 16, n, L, 'homa')), 'get_p99_us')} / "
                                        f"{cell(fleet.get((w, 16, n, L, 'tcp')), 'get_p99_us')}" for L in LOADS) + " |")
for w in WL:
    print(f"\n{w}: achieved k requests/s at the highest offered load ({LOADS[-1] // 1000}k) and node0 CPU us per request at 40k, Homa / stock TCP\n")
    print("| clients | achieved | node0 CPU us/request at 40k | busiest node0 CPU % at 40k |\n|---|---|---|---|")
    for n in NS:
        a = [fleet[(w, 16, n, LOADS[-1], t)]["ops"] / 1000 for t in TR]
        c = [fleet[(w, 16, n, 40000, t)] for t in TR]
        print(f"| {n} | {a[0]:.1f} / {a[1]:.1f} | " + " / ".join(f"{m['server_busy_sum'] / 100 / m['ops'] * 1e6:.1f}" for m in c)
              + " | " + " / ".join(f"{m['server_busy_max']:.0f}" for m in c) + " |")
for w in WL:
    print(f"\n{w}: few clients, GET p99 us, Homa / stock TCP\n")
    print("| threads x clients per thread | clients | " + " | ".join(f"{L // 1000}k" for L in SL) + " |")
    print("|---" * (len(SL) + 2) + "|")
    for k in sorted({(k[1], k[2]) for k in sweep if k[0] == w}, key=lambda x: (x[0], x[1])):
        t, n = k
        print(f"| {t} x {n // t} | {n} | " + " | ".join(f"{cell(sweep.get((w, t, n, L, 'homa')), 'get_p99_us')} / "
                                                         f"{cell(sweep.get((w, t, n, L, 'tcp')), 'get_p99_us')}" for L in SL) + " |")
for w in WL:
    print(f"\n{w}: few clients, node0 CPU us per request, Homa / stock TCP\n")
    print("| clients | " + " | ".join(f"{L // 1000}k" for L in SL) + " |")
    print("|---" * (len(SL) + 1) + "|")
    for t, n in sorted({(k[2], k[1]) for k in sweep if k[0] == w}):
        f = lambda m: f"{m['server_busy_sum'] / 100 / m['ops'] * 1e6:.1f}"
        print(f"| {t} | " + " | ".join(f"{f(sweep[(w, n, t, L, 'homa')])} / {f(sweep[(w, n, t, L, 'tcp')])}" for L in SL) + " |")
