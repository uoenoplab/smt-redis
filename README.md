# Redis over Homa vs TCP: a client fleet against one Redis shard

One single-threaded Redis shard serving 1,024 to 24,576 independent clients over Homa and over
TCP, on 2x CloudLab Utah xl170 (25 Gb/s), with request sizes and mixes from the Twitter
cache trace.

## Reasoning

A production Redis shard serves a fleet of application processes. Most use synchronous client
libraries (redis-py, Jedis, hiredis) with connection pools: each client is one connection with one
request in flight, and clients send independently of each other. In that regime:

- **Batching** cannot happen: a client never has a second request queued behind the first, so
  neither explicit pipelining nor TCP's coalescing of several requests per read applies. (Over one
  busy connection TCP does batch, and there Homa, one message per request, costs more.)
- **Serialization inside a client** cannot happen either: Redis executes one client's commands in
  order, but each client has only one outstanding. Across clients Redis is out of order, so the
  transport's ordering matters only between clients.

What remains are the two properties in which the transports differ across clients, and the
experiment is built to expose them:

| Homa property | Pays off when | Cancelled when | Here |
|---|---|---|---|
| Connectionless: the server's kernel state does not grow with the number of clients (one socket for all; TCP keeps one socket per client) | many clients | few clients each sending densely, where TCP batches by itself | 1,024-24,576 clients, one request in flight each |
| Message-based, shortest remaining message first: a small reply is not queued behind another client's large one | sizes mixed across clients, egress the bottleneck | ordering inside one client, or a single-threaded endpoint that must process a large message whole | c53 mixes 8 B to 35 KB |
| Receiver-driven grants: incast without loss | many senders replying to one receiver | (needs more than two machines) | not tested |

## The experiment

| Element | Choice |
|---|---|
| Server | one redis-server, `--io-threads 1`, pinned to node0 CPU 7 |
| Clients | memtier_benchmark, 16 threads x 64 / 512 / 1,536 clients (N = 1,024 / 8,192 / 24,576); each client one TCP connection or one Homa socket, one request in flight (`--pipeline=1`) |
| Arrivals | open loop: each client's requests are a Poisson process of LOAD/N per second; LOAD = 20k-100k requests/s |
| Requests | value sizes and SET:GET ratio of two Twitter clusters (Yang et al., OSDI'20, CMU PDL `sample100`); 100,000 keys, all preloaded, drawn uniformly (every GET hits) |
| Run | fresh redis-server once node0 is idle; preload; 5 s warm-up (not counted); 20 s measured; CPU sampled 4 s mid-run as MPERF/TSC on every CPU; 3 rounds, medians |

| Workload | Values (weighted by requests) | SET:GET | Cluster |
|---|---|---|---|
| c52 | 17 B-3.6 KB, mostly 27 B | 7:93 | busiest cluster whose mean value exceeds 100 B (third of 54 by request rate) |
| c53 | 8 B-35 KB, a fifth of the GETs over 16 KB | 13:87 | values over four orders of magnitude |

## Results

![latency](latency.png)
![cpu](cpu.png)
![throughput](throughput.png)

- **Server CPU**: Homa needs 18-44% less node0 CPU per request at every client count and load
  both sustain (the most at low load and many clients), and its cost does not grow with the client count (c52 at 40k: 19.3-19.9 us over
  Homa, 30.2-35.3 over TCP; c53 at 40k: 25.6-26.3 against 35.2-45.3).
- **c52 (small values)**: Homa's GET p50 is 20-40% lower and its p99 0-40% lower at every load
  both sustain; at 8,192 clients Homa sustains 100k requests/s (p99 391 us) while TCP
  saturates at 86k.
- **c53 (mixed sizes)**: at 20k and 40k requests/s Homa's p50 is 8-30% lower and the p99s are
  within 15%; at 1,024 clients and 60k TCP is ahead (p50 151 vs 231 us, p99 535 vs 943); TCP saturates later at
  every client count (78k vs 64k, 60k vs 57k, 55k vs 51k). The single Redis core sets capacity,
  and Homa does more work on it per message (one `recvmsg` and one `sendmsg` per message, a
  multi-KB message copied in one call) although it does less on the node as a whole.
- **Offered load reached**: every run at 20k and 40k requests/s achieved its offered load within
  0.4%; points where the server fell more than 2% short are marked * (x in the figure).

**c52** (* = the server did not keep up; achieved k requests/s in brackets)

| clients | load | Homa p50 / p99 (us) | TCP p50 / p99 (us) | node0 CPU per request, Homa / TCP (us) |
|---:|---:|---:|---:|---:|
| 1,024 | 20k | 55 / 167 | 87 / 183 | 25.9 / 42.7 |
| 1,024 | 40k | 47 / 143 | 71 / 167 | 19.3 / 30.2 |
| 1,024 | 60k | 55 / 167 | 71 / 183 | 16.8 / 23.5 |
| 1,024 | 80k | 63 / 207 | 79 / 239 | 14.8 / 19.3 |
| 1,024 | 100k | 87 / 303 | 143 / 503 | 13.4 / 16.3 |
| 8,192 | 20k | 55 / 183 | 87 / 199 | 26.0 / 46.1 |
| 8,192 | 40k | 55 / 151 | 71 / 183 | 19.4 / 35.1 |
| 8,192 | 60k | 55 / 191 | 71 / 223 | 16.9 / 28.4 |
| 8,192 | 80k | 71 / 255 | 95 / 335 | 15.0 / 24.0 |
| 8,192 | 100k | 95 / 391 | 94719 / 97279 * (86k) | - |
| 24,576 | 20k | 55 / 215 | 87 / 215 | 26.5 / 46.0 |
| 24,576 | 40k | 55 / 175 | 79 / 207 | 19.9 / 35.3 |
| 24,576 | 60k | 63 / 207 | 79 / 279 | 17.1 / 28.5 |
| 24,576 | 80k | 79 / 375 | 111 / 583 | 15.2 / 24.1 |
| 24,576 | 100k | 264191 / 303103 * (88k) | 274431 / 286719 * (87k) | - |

**c53** (* = the server did not keep up; achieved k requests/s in brackets)

| clients | load | Homa p50 / p99 (us) | TCP p50 / p99 (us) | node0 CPU per request, Homa / TCP (us) |
|---:|---:|---:|---:|---:|
| 1,024 | 20k | 79 / 239 | 103 / 247 | 33.5 / 47.8 |
| 1,024 | 40k | 95 / 311 | 103 / 279 | 25.6 / 35.2 |
| 1,024 | 60k | 231 / 943 | 151 / 535 | 21.2 / 28.0 |
| 1,024 | 80k | 15999 / 17151 * (64k) | 12991 / 14911 * (78k) | - |
| 1,024 | 100k | 16063 / 17279 * (64k) | 13055 / 14399 * (78k) | - |
| 8,192 | 20k | 79 / 271 | 111 / 295 | 34.1 / 58.9 |
| 8,192 | 40k | 103 / 399 | 119 / 447 | 26.1 / 43.6 |
| 8,192 | 60k | 135167 / 142335 * (57k) | 128511 / 166911 * (58k) | - |
| 8,192 | 80k | 144383 / 149503 * (56k) | 134143 / 136191 * (61k) | - |
| 8,192 | 100k | 144383 / 150527 * (57k) | 135167 / 137215 * (60k) | - |
| 24,576 | 20k | 87 / 295 | 111 / 303 | 34.7 / 59.1 |
| 24,576 | 40k | 103 / 487 | 127 / 575 | 26.3 / 45.3 |
| 24,576 | 60k | 413695 / 544767 * (52k) | 409599 / 505855 * (54k) | - |
| 24,576 | 80k | 417791 / 532479 * (52k) | 440319 / 444415 * (55k) | - |
| 24,576 | 100k | 415743 / 528383 * (51k) | 438271 / 446463 * (55k) | - |

Runs at 20k and 40k offered: 96; largest deviation of achieved from offered: 0.4%

## Reproduce

| Item | Value |
|---|---|
| Nodes | 2x CloudLab Utah xl170 (`small-lan` profile, LAN on 10.0.1.x): node0 = 10.0.1.1 (Redis), node1 = 10.0.1.2 (clients) |
| Hardware | Intel Xeon E5-2640 v4 (10 cores / 20 threads), Mellanox ConnectX-4 25 Gb/s (`ens1f1np1`) |
| OS | Ubuntu 24.04, mainline kernel 6.17.8-061708-generic, `mitigations=off`, governor `performance` |
| Homa | PlatformLab/HomaModule `main` @ `1c59d7b6` |
| Redis | uoenoplab/smt-redis tag `homa-6.17.8-xl170-20261006` (`f749cd4dd`): Redis 8.10.1 with a Homa transport |
| Load generator | uoenoplab/memtier_benchmark branch `homa` (`9006af8`, on redis/memtier_benchmark `7a6394e`) |

### Build (both nodes for Homa and Redis, node1 for memtier)

```bash
git clone https://github.com/PlatformLab/HomaModule && cd HomaModule && git checkout 1c59d7b6
cp -r cloudlab/bin/. ~/bin/ && make -j20 CC=gcc-14 && make -j20 -C util
~/bin/install_homa 2          # from node0: copies homa.ko and tools to both nodes, runs "config default"
git clone -b homa-6.17.8-xl170-20261006 https://github.com/uoenoplab/smt-redis ~/smt-redis && make -C ~/smt-redis -j20
# node1
sudo apt-get install -y build-essential autoconf automake libpcre3-dev libevent-dev pkg-config zlib1g-dev libssl-dev
git clone -b homa https://github.com/uoenoplab/memtier_benchmark ~/memtier_benchmark
cd ~/memtier_benchmark && git checkout 9006af8 && autoreconf -ivf && ./configure && make -j16
```

### Host configuration of each protocol

| | Homa (official CloudLab config) | TCP (kernel defaults, Homa removed) |
|---|---|---|
| Module | `homa.ko` loaded; `num_priorities 8`, `link_mbps 25000`, `unsched_bytes 60000`, `max_incoming 480000`, `max_gso_size 10000`, `max_nic_est_backlog_usecs 5` | `homa.ko` unloaded (`rmmod homa`) |
| qdisc | `mq` with `sch_homa` on all 20 TX queues | `mq` with `fq_codel` (kernel default) |
| RPS / RFS | RPS on every RX queue (mask `fffff`), `rps_sock_flow_entries 32768`, `rps_flow_cnt 2048` | off (RSS only) |
| NIC | `ethtool -C`: `adaptive-rx off rx-usecs 0 rx-frames 1 adaptive-tx off tx-usecs 5`; `-K ntuple off` | same (left as Homa's config set it) |
| Command | `bash -lc "config default"` on each node (`tcpclean.sh off`) | `tcpclean.sh on`: `config reset_qdisc`; `rps_cpus`/`rps_flow_cnt` 0; `rps_sock_flow_entries 0`; `rmmod homa` |
| redis-server | `redis-server --port 6379 --homa-port 2000 --bind 0.0.0.0 --protected-mode no --save "" --io-threads 1 --maxclients 100000` | the same without `--homa-port 2000` |
| Client | `memtier ... -p 2000 --homa` | `memtier ... -p 6379` |

Both: `net.ipv4.icmp_ratelimit=0` on both nodes (a Homa server RPC whose client closed is otherwise
probed every 1 ms until an ICMP gets through); node1 `ip_local_port_range "1024 65535"`,
`tcp_tw_reuse 1`; `ulimit -n 200000`; the `homa_timer` kthread pinned to CPU 19 on both nodes;
NAPI was node0 CPU 1 and node1 CPU 4, so redis-server runs on node0 CPU 7 and memtier on node1 CPUs
0-3, 5-13, 15-18.

The memtier command of one run (c52, 8,192 clients, 60k requests/s; `--rate-poisson` = LOAD/N):

```bash
memtier_benchmark -s 10.0.1.1 -p 2000 --homa --protocol=redis -t 16 -c 512 --pipeline=1 \
  --rate-poisson=7.32421875 --ratio=7:93 --key-pattern=R:R --key-minimum=1 --key-maximum=100000 \
  --data-size-list=17:38,27:5751,38:199,54:39,70:23,117:10,151:240,216:383,306:531,434:732,607:1172,753:605,1149:220,1543:51,2403:3,3648:5 \
  --sample-mix --randomize --distinct-client-seed --warmup=5 --test-time=20 --no-per-second-percentiles --hide-histogram
```

The preload before it, over TCP: the same with `-p 6379 -t 4 -c 8 --ratio=1:0 --key-pattern=P:P -n allkeys`.
memtier options added on the `homa` branch: `--homa` (each client one Homa socket, one RPC in
flight), `--rate-poisson` (Poisson arrivals on absolute times), `--sample-mix` (each request's
type and size drawn by `--ratio` and the `--data-size-list` weights), `--warmup`,
`--no-per-second-percentiles` (per-client per-second percentile summaries stall worker threads
at thousands of clients).

### Run (node1, about 3 h)

```bash
for h in node0 node1; do scp busy-cores.sh homa-timer-busy.sh $h:; done
tmux new -d -s fleet 'TRANSPORTS=homa CPT="64 512 1536" bash fleet-xl170.sh > fleet-homa.csv
  bash tcpclean.sh on; TRANSPORTS=tcp CPT="64 512 1536" bash fleet-xl170.sh > fleet-tcp.csv; bash tcpclean.sh off'
uv run --with matplotlib python plot.py
```

Check the host state that `tcpclean.sh` prints before each block: both nodes `20xhoma`,
`rps=fffff` for Homa; `20xfq_codel`, `rps=00000`, `homa=unloaded` for TCP.

| File | What |
|---|---|
| `fleet-xl170.sh` | driver: fresh server, preload, memtier, CPU sampling; one CSV row per run |
| `tcpclean.sh` | `on`: TCP; `off`: Homa's config |
| `busy-cores.sh`, `homa-timer-busy.sh` | per-CPU busy % (MPERF/TSC); busy % of the `homa_timer` kthread |
| `results/fleet-{homa,tcp}.csv` | one row per run; `server_busy_sum` is the sum of node0's per-CPU busy % |
| `plot.py` | the figures and tables |

## Limitations

- **One client host.** node1 holds every Homa socket; `homa_timer`, which visits every socket each
  tick, takes 40% of a CPU at 8,192 clients and 68% at 24,576, and saturates its CPU (96%) at
  24,576 clients and 100k requests/s, so that point measures the client host, not Redis.
- Latency is timed from the send of each request (memtier `9006af8`), not from its arrival; with
  one request in flight per client and at most 100k/1,024 = 98 requests/s per client, a request
  rarely waits behind its client's previous one.
- Near saturation the CPU per request of both transports is inflated; only sustained points are
  compared. What limits Homa at saturation (node0's busiest CPU 87-91% busy, against 94-96% for
  TCP) is not established.
- The TCP runs ran as one block after the Homa runs (`homa.ko` can only be unloaded with no
  Homa socket open). Keys are drawn uniformly, not with the trace's skew; every write is a SET.
