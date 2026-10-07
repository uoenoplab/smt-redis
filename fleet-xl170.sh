#!/bin/bash
# fleet-xl170.sh > fleet.csv: a fleet of independent clients against one Redis shard (runs on node1
# against redis-server on node0). memtier_benchmark (built with --homa, --rate-poisson, --sample-mix)
# emulates the fleet: T threads x C clients, each client one TCP connection or one Homa socket with one
# request in flight, its requests a Poisson process of LOAD/N per second. A request's latency counts
# from its arrival, so the time it waits behind its client's request in flight is included. Request mix
# and value sizes come from the Twitter cache trace (Yang et al., OSDI'20): c52, the busiest cluster
# whose mean value exceeds 100 B (third by request rate; values 17 B-3.6 KB, 7% writes), and c53 (values
# 8 B-35 KB, a quarter of the GETs over 16 KB, 13% writes). Keys are drawn
# uniformly from 100,000, all preloaded, so every GET hits.
# Every run starts on a fresh redis-server (pinned to node0 CPU 7) once node0 is idle, preloads the
# keys over TCP, warms the clients up for 5 s (not counted), measures 20 s, and samples both nodes' CPU
# and each node's homa_timer kthread (Homa's per-host timer, which visits every socket) for 4 s mid-run.
# Every check that fails stops the sweep with the reason (die): the host state before each run
# (hoststate.sh: Homa's config, or TCP alone when TRANSPORTS is "tcp"), node0 going idle, the server
# start, the preload, memtier's exit status and output, misses and errors, and the CPU samples.
# TRANSPORTS (default "homa tcp"; "tcp" alone after tcpclean.sh on), WORKLOADS, THREADS, CPT (clients
# per thread), LOADS (requests/s, all clients together), ROUNDS; M: the memtier_benchmark binary.
# Output: round,workload,transport,threads,clients,offered,ops,sets,gets,get_p50_us,get_p99_us,get_p999_us,
#         set_p99_us,misses,errors,server_busy_sum,server_busy_max,client_busy_sum,client_busy_max,
#         server_homa_timer_pct,client_homa_timer_pct
SRV=10.0.1.1; R=${R:-~/smt-redis}; M=${M:-~/memtier_benchmark/memtier_benchmark}; SPIN=7; MC=0-3,5-13,15-18
TRANSPORTS=${TRANSPORTS:-homa tcp}; WORKLOADS=${WORKLOADS:-c52 c53}; THREADS=${THREADS:-16}; CPT=${CPT:-8 64 512 1536}
LOADS=${LOADS:-20000 40000 60000 80000 100000}; ROUNDS=${ROUNDS:-3}; T=20; W=5; KEYS=100000
declare -A SIZES=(
  [c52]="17:38,27:5751,38:199,54:39,70:23,117:10,151:240,216:383,306:531,434:732,607:1172,753:605,1149:220,1543:51,2403:3,3648:5"
  [c53]="8:717,16:304,24:251,40:713,56:310,72:215,98:241,128:73,231:41,280:91,432:322,528:79,912:1009,1136:388,1749:30,2574:41,3432:73,5049:125,7326:514,9801:323,13695:1594,18711:386,26961:1592,35343:567")
declare -A RATIO=([c52]=7:93 [c53]=13:87)
set -o pipefail
die() { echo "fleet-xl170.sh: $*" >&2; exit 1; }
HOMA=$([[ " $TRANSPORTS " == *" homa "* ]] && echo "--homa-port 2000")
MODE=$([ -n "$HOMA" ] && echo homa || echo tcp)   # TCP alone runs on hosts without homa.ko
STATE="$(dirname "$0")/hoststate.sh"
bash "$STATE" $MODE >&2 || die "hosts not in the $MODE state"
# Homa's timer thread visits every socket of its host each tick: with the whole fleet on node1 it
# needs most of a CPU. Give it CPU 19 on both nodes, which memtier (MC) and redis-server leave free.
[ $MODE = tcp ] || for h in node0 node1; do
  ssh -4 $h 'sudo taskset -pc 19 $(pgrep -x homa_timer) > /dev/null' || die "$h: cannot pin homa_timer"; done
ulimit -n 200000 || die "ulimit -n 200000"
# 25k client-side TCP connections per run, closed by the client: room in the port range, and TIME_WAIT
# ports reused for new outgoing connections
sudo sysctl -qw net.ipv4.ip_local_port_range="1024 65535" net.ipv4.tcp_tw_reuse=1 || die "sysctl"
start() {  # a fresh redis-server once node0 is idle (Homa's timer may still be busy with closed clients)
  local i ok=
  for i in $(seq 120); do ssh -4 node0 "~/busy-cores.sh 0-19 1" | awk '{exit !($1 < 15)}' && { ok=1; break; }; done
  [ -n "$ok" ] || die "node0 not idle after 120 s"
  ssh -4 node0 "cd $R && ./src/redis-cli -p 6379 shutdown nosave >/dev/null 2>&1
    for i in \$(seq 100); do pgrep -x redis-server >/dev/null || break; sleep 0.1; done; tmux kill-session -t rsrv 2>/dev/null"
  # the old server's Homa socket may take a moment to go; then the hosts must be as the runs need them
  for i in $(seq 60); do bash "$STATE" $MODE > /dev/null 2>&1 && break; sleep 1; done
  bash "$STATE" $MODE > /dev/null || die "hosts left the $MODE state"
  ssh -4 node0 "cd $R && tmux new-session -d -s rsrv 'ulimit -n 200000; taskset -c $SPIN ./src/redis-server --port 6379 $HOMA --bind 0.0.0.0 --protected-mode no --save \"\" --io-threads 1 --maxclients 100000'
    for i in \$(seq 50); do ./src/redis-cli ping 2>/dev/null | grep -q PONG && break; sleep 0.1; done; ./src/redis-cli ping" | grep -q PONG || die "server start failed"
}
mt() {  # workload, then memtier's own arguments
  local w=$1; shift
  taskset -c $MC $M -s $SRV --protocol=redis --key-minimum=1 --key-maximum=$KEYS --data-size-list=${SIZES[$w]} \
    --sample-mix --randomize --distinct-client-seed --no-per-second-percentiles --hide-histogram "$@" 2>&1; }
us() { awk -v t=$1 -v c=$2 '$1 == t {printf "%.1f", $c * 1000}' /tmp/fleet.out; }  # memtier prints ms
echo "round,workload,transport,threads,clients,offered,ops,sets,gets,get_p50_us,get_p99_us,get_p999_us,set_p99_us,misses,errors,server_busy_sum,server_busy_max,client_busy_sum,client_busy_max,server_homa_timer_pct,client_homa_timer_pct"
for r in $(seq $ROUNDS); do for w in $WORKLOADS; do for t in $THREADS; do for c in $CPT; do for load in $LOADS; do for tr in $TRANSPORTS; do
  n=$((t * c)); start
  mt $w -p 6379 -t 4 -c 8 --ratio=1:0 --key-pattern=P:P -n allkeys > /dev/null || die "preload: memtier failed"
  [ "$($R/src/redis-cli -h $SRV dbsize)" = $KEYS ] || die "preload: dbsize is not $KEYS"
  rm -f /tmp/fleet.busy /tmp/fleet.cbusy
  ( sleep $((W + 8)); ssh -4 node0 "~/homa-timer-busy.sh 4 > /tmp/ht & ~/busy-cores.sh 0-19 4; wait; cat /tmp/ht" | tr '\n' ' ' > /tmp/fleet.busy ) &
  ( sleep $((W + 8)); ~/homa-timer-busy.sh 4 > /tmp/fleet.ht & ~/busy-cores.sh 0-19 4 | tr '\n' ' ' > /tmp/fleet.cbusy; wait; cat /tmp/fleet.ht >> /tmp/fleet.cbusy ) &
  mt $w $([ $tr = homa ] && echo "-p 2000 --homa" || echo "-p 6379") -t $t -c $c --pipeline=1 --warmup=$W --test-time=$T \
     --rate-poisson=$(echo "$load / $n" | bc -l) --ratio=${RATIO[$w]} --key-pattern=R:R > /tmp/fleet.out \
     || die "memtier failed ($w $tr $n clients $load/s): $(tail -3 /tmp/fleet.out)"
  wait
  grep -q '^Totals' /tmp/fleet.out || die "no Totals in memtier's output ($w $tr $n clients $load/s)"
  ! grep -qi error /tmp/fleet.out || die "memtier reported errors: $(grep -i error /tmp/fleet.out | head -3)"
  [ "$(awk '$1 == "Gets" {print $4 + 0}' /tmp/fleet.out)" = 0 ] || die "GET misses: the preload did not cover the keys"
  for f in /tmp/fleet.busy /tmp/fleet.cbusy; do [ $(wc -w < $f) -ge 2 ] || die "CPU sample missing: $f"; done
  echo "$r,$w,$tr,$t,$n,$load,$(awk '$1 == "Totals" {print $2}' /tmp/fleet.out),$(awk '$1 == "Sets" {print $2}' /tmp/fleet.out),$(awk '$1 == "Gets" {print $2}' /tmp/fleet.out),$(us Gets 6),$(us Gets 7),$(us Gets 8),$(us Sets 7),$(awk '$1 == "Gets" {print $4}' /tmp/fleet.out),$(grep -ci error /tmp/fleet.out),$(f() { set -- $(cat $1); echo $1,$2; }; f /tmp/fleet.busy),$(f() { set -- $(cat $1); echo $1,$2; }; f /tmp/fleet.cbusy),$(awk '{print $3}' /tmp/fleet.busy),$(awk '{print $3}' /tmp/fleet.cbusy)"
done; done; done; done; done; done
ssh -4 node0 "cd $R && ./src/redis-cli -p 6379 shutdown nosave >/dev/null 2>&1"
