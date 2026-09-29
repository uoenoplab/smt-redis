#!/bin/bash
# mixload-xl170.sh hol|scale|bigcost|rpc|multi > <mode>.csv: open-loop mixload experiments on xl170 (runs on
# node1 against redis-server on node0, which it restarts with room for 100k clients). Official config
# state (config rps on both nodes); redis-server pinned to node0 cpu7, mixload (one thread) to node1
# cpu 8, both off the NAPI cores. Keys: small = 100 B, big = 512 KB.
#  hol:   small/big GET mix (big fraction 1% and 10%) x offered load x {TCP over 1, 2, 4, 16, 64
#         pipelined connections, Homa over 1 socket}, 3 rounds of 10 s, configurations interleaved.
#  Every run starts on a fresh redis-server.
#  multi: the hol mix and loads, split over K = 4, 8, 16 concurrent clients, each one mixload
#         process with one endpoint on its own node1 CPU and its own seed; latencies pooled.
#  scale: small GETs at 30k ops/s spread over {16, 1000, 5000, 25000} TCP connections or Homa
#         sockets, 3 rounds; server-node CPU and memory sampled mid-run and before the run (base_*).
#  bigcost: 1000 ops/s of only small or only big GETs over 1 endpoint, 3 rounds, sampled like
#         scale: the server CPU a 512 KB reply costs on each transport.
#  rpc:   small GETs only over 1 endpoint at 10k-80k ops/s, 3 rounds: server CPU and redis-server's
#         system calls (perf, 4 s mid-run) per GET, i.e. how many requests one read or write carries.
#         It stops below Homa's capacity over one socket (~81k GET/s): overloading Homa can wedge
#         sch_homa (a NIC TX timeout desyncs its queue estimate), after which it holds back TCP.
# Needs ~/mixload (built against ~/smt-redis/deps) on node1 and ~/busy-cores.sh on node0. R picks
# the Redis build (default ~/smt-redis, the same path on both nodes).
# TRANSPORTS (default "homa tcp") picks the legs; without homa it is the stock-TCP baseline
# (run tcpclean.sh on first).
SRV=10.0.1.1; R=${R:-~/smt-redis}; M=~/mixload; SPIN=7; CPIN=8; TRANSPORTS=${TRANSPORTS:-homa tcp}
HOL="transport,endpoints,rate,secs,big_frac,sent,done,bad,small_p50_us,small_p99_us,small_p999_us,big_p50_us,big_p99_us,max_outstanding,lag_p99_us,lag_max_us"
SNAP='i=$(~/smt-redis/src/redis-cli info); f() { echo "$i" | sed -n "s/^$1:\([0-9]*\).*/\1/p"; }
echo "$(f connected_clients) $(f used_memory) $(f mem_clients_normal) $(awk "/^TCP:/{print \$3, \$NF}" /proc/net/sockstat) $(awk "/^Slab:/{print \$2}" /proc/meminfo)"'
MEM="clients,used_memory,mem_clients_normal,tcp_inuse,tcp_mem_pages,slab_kb"
# TRANSPORTS without homa = the stock-TCP baseline (after tcpclean.sh on): no Homa listener, no RPS.
HOMA=$([[ " $TRANSPORTS " == *" homa "* ]] && echo "--homa-port 2000")
[ -n "$HOMA" ] && for h in node0 node1; do ssh -4 $h 'bash -lc "config rps" >/dev/null'; done
keys() {  # (re)load the two values
  head -c 100 /dev/zero | tr '\0' s | $R/src/redis-cli -h $SRV -x set small >/dev/null
  head -c 524288 /dev/zero | tr '\0' b | $R/src/redis-cli -h $SRV -x set big >/dev/null
}
start() {  # (re)start redis-server on node0 with the two values
  ssh -4 node0 "cd $R && ./src/redis-cli -p 6379 shutdown nosave >/dev/null 2>&1
    for i in \$(seq 100); do pgrep -x redis-server >/dev/null || break; sleep 0.1; done; tmux kill-session -t rsrv 2>/dev/null
    tmux new-session -d -s rsrv 'ulimit -n 200000; taskset -c $SPIN ./src/redis-server --port 6379 $HOMA --bind 0.0.0.0 --protected-mode no --save \"\" --io-threads 1 --maxclients 100000'
    for i in \$(seq 50); do ./src/redis-cli ping 2>/dev/null | grep -q PONG && break; sleep 0.1; done; ./src/redis-cli ping" | grep -q PONG || { echo "server start failed" >&2; exit 1; }
  keys; [ "$($R/src/redis-cli -h $SRV strlen big)" = 524288 ] || { echo "keys not loaded" >&2; exit 1; }
}
start
ulimit -n 200000
mix() {  # transport endpoints ops/s secs big_frac, in the background; waits until it is set up
  taskset -c $CPIN $M $SRV $([ $1 = homa ] && echo 2000 || echo 6379) "$@" > /tmp/mixload.out 2> /tmp/mixload.err & p=$!
  until grep -q ready /tmp/mixload.err || ! kill -0 $p 2>/dev/null; do sleep 0.1; done
}
checked() {  # after a run: replies that were not the value (bad > 0) mean the data went missing
  if [ "$(cut -d, -f8 /tmp/mixload.out)" != 0 ]; then
    echo "$(date -u +%T) bad replies in [$(cut -d, -f1-5 /tmp/mixload.out)]: $($R/src/redis-cli -h $SRV info server | grep -o 'uptime_in_seconds:[0-9]*'), dbsize $($R/src/redis-cli -h $SRV dbsize), strlen big $($R/src/redis-cli -h $SRV strlen big)" >&2
    keys
  fi
}
gone() {  # wait (<= 90 s) until only our own INFO client is connected, then (<= 2 min) until node0 is
  # idle again: after 25,000 Homa sockets close, its homa_timer thread stays busy for about a minute
  for i in $(seq 90); do [ "$(ssh -4 node0 "$SNAP" | cut -d' ' -f1)" -le 1 ] && break; sleep 1; done
  for i in $(seq 120); do ssh -4 node0 "~/busy-cores.sh 0-19 1" | awk '{exit !($1 < 15)}' && return; done
}
sampled() {  # round + mix's arguments: one run on a fresh server, CPU and memory sampled 2 s in.
  # A fresh server per run: with the tagged code, a run of 25,000 Homa sockets could leave RPCs
  # behind in the server's socket that Homa's timer then scans every tick until the socket closes.
  local r=$1; shift
  start; gone; base=$(ssh -4 node0 "$SNAP")
  mix "$@"; sleep 2
  mid=$(ssh -4 node0 "~/busy-cores.sh 0-19 4; $SNAP" | tr '\n' ' ')
  wait $p; echo "$r,$(cat /tmp/mixload.out),$(echo $mid $base | tr ' ' ',')"; grep -v ready /tmp/mixload.err >&2
}
SAMPLED="round,$HOL,server_busy_sum,server_busy_max,$MEM,$(echo base_${MEM//,/,base_})"
# redis-server's system calls while busy-cores.sh samples; perf from the image's linux-tools (the
# /usr/bin/perf wrapper refuses a kernel it has no package for)
SC="syscalls recvmsg sendmsg read write writev epoll_wait"
PERF='sudo $(ls /usr/lib/linux-tools/*/perf | head -1) stat -x, -o /tmp/rpc-perf.csv -e raw_syscalls:sys_enter'"$(for c in ${SC#* }; do printf ,syscalls:sys_enter_$c; done)"' -p $(pgrep -x redis-server) -- ~/busy-cores.sh 0-19 4
grep -v "^#" /tmp/rpc-perf.csv | cut -d, -f1 | tr "\n" " "'
case $1 in
hol)   echo "round,$HOL"
       for r in 1 2 3; do for f in 0.01 0.1; do
         for rate in $([ $f = 0.01 ] && echo 10000 20000 40000 60000 80000 || echo 5000 10000 15000 20000 30000); do
           for ep in "tcp 1" "tcp 2" "tcp 4" "tcp 16" "tcp 64" "homa 1"; do
             [[ " $TRANSPORTS " == *" ${ep% *} "* ]] || continue
             start; mix $ep $rate 10 $f; wait $p; echo "$r,$(cat /tmp/mixload.out)"; grep -v ready /tmp/mixload.err >&2; checked
           done
         done
       done; done ;;
scale) echo "$SAMPLED"
       for r in 1 2 3; do for n in 16 1000 5000 25000; do for tr in $TRANSPORTS; do sampled $r $tr $n 30000 10 0; done; done; done ;;
bigcost) echo "$SAMPLED"
       for r in 1 2 3; do for f in 0 1; do for tr in $TRANSPORTS; do sampled $r $tr 1 1000 10 $f; done; done; done ;;
rpc)   echo "round,$HOL,server_busy_sum,server_busy_max,${SC// /,}"
       for r in 1 2 3; do for rate in 10000 20000 40000 60000 80000; do for tr in $TRANSPORTS; do
         # an overloaded run leaves a backlog: start only once the Redis core is idle again
         for i in $(seq 30); do ssh -4 node0 "~/busy-cores.sh $SPIN 1" | awk '{exit !($2 < 5)}' && break; done
         start; mix $tr 1 $rate 10 0; sleep 2; mid=$(ssh -4 node0 "$PERF")
         wait $p; echo "$r,$(cat /tmp/mixload.out),$(echo $mid | tr ' ' ',')"; grep -v ready /tmp/mixload.err >&2; checked
       done; done; done ;;
multi) echo "round,transport,clients,rate,secs,big_frac,sent,done,bad,small_p50_us,small_p99_us,small_p999_us,big_p50_us,big_p99_us,lag_p99_us"
       CL=(5 6 7 8 9 10 11 12 13 15 16 17 18 19 0 1)   # node1 CPUs, off NAPI core 4 and its sibling 14
       sum() { cat /tmp/mx.*.out | awk -F, -v c=$1 '{s += $c} END {print s}'; }
       pct() { awk -v k=$1 '$1 == k {print $2}' /tmp/mx.*.lat | sort -g | awk -v p=$2 '{v[NR] = $1} END {print NR ? v[int(p * (NR - 1)) + 1] : "nan"}'; }
       for r in 1 2 3; do for f in 0.01 0.1; do
         for rate in $([ $f = 0.01 ] && echo 10000 20000 40000 60000 80000 || echo 5000 10000 15000 20000 30000); do
           for k in 4 8 16; do for tr in $TRANSPORTS; do
             start; for i in "${!CL[@]}"; do : > /tmp/mx.$i.out; : > /tmp/mx.$i.lat; : > /tmp/mx.$i.err; done  # only the k in use refill
             port=$([ $tr = homa ] && echo 2000 || echo 6379)
             for i in $(seq 0 $((k - 1))); do
               MIXSEED=$((i + 1)) LATDUMP=/tmp/mx.$i.lat taskset -c ${CL[$i]} $M $SRV $port $tr 1 $((rate / k)) 10 $f > /tmp/mx.$i.out 2> /tmp/mx.$i.err &
             done; wait
             echo "$r,$tr,$k,$rate,10,$f,$(sum 6),$(sum 7),$(sum 8),$(pct s .5),$(pct s .99),$(pct s .999),$(pct b .5),$(pct b .99),$(pct l .99)"
             cat /tmp/mx.*.err | grep -v ready >&2
           done; done
         done
       done; done ;;
*)     echo "usage: $0 hol|scale|bigcost|rpc|multi" >&2; exit 2 ;;
esac
