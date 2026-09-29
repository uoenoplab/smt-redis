#!/bin/bash
# YCSB-C over Redis, Homa vs TCP, on xl170 (runs on node1; server on node0): ROUNDS rounds
# of threads x workload x transport (transports interleaved), with whole-node CPU sampled
# mid-run on both nodes (Homa runs part of its softirq work off the application cores, so
# counting only the pinned cores would undercount it). ROUNDS=1 is the bottleneck diagnostic.
# Official config state for both transports (RPS on); server pinned to node0 cpu7 (off the
# NAPI core cpu1); ycsbc may use every node1 CPU except the NAPI core 4 and its sibling 14.
# Needs ~/busy-cores.sh on both nodes. TRANSPORTS (default "homa tcp") picks the legs; without
# homa it is the stock-TCP baseline (run tcpclean.sh on first).
# Output: round,transport,workload,threads,ops,ktps,server_busy_sum,server_busy_max,
#         client_busy_sum,client_busy_max (busy % over all 20 CPUs of a node; sum in cores x 100)
SRV=10.0.1.1; R=~/smt-redis; Y=${Y:-~/smt-ycsb-c}; SPIN=7; YCPUS=0-3,5-13,15-19
THREADS=${THREADS:-1 2 4 8 16}; WORKLOADS=${WORKLOADS:-workloada workloadc}; ROUNDS=${ROUNDS:-1}
TRANSPORTS=${TRANSPORTS:-homa tcp}
# TRANSPORTS without homa = the stock-TCP baseline (after tcpclean.sh on): no Homa listener, no RPS.
HOMA=$([[ " $TRANSPORTS " == *" homa "* ]] && echo "--homa-port 2000")
[ -n "$HOMA" ] && for h in node0 node1; do ssh -4 $h 'bash -lc "config rps" >/dev/null'; done
start() {  # a fresh redis-server on node0 for every run
  ssh -4 node0 "cd $R && ./src/redis-cli -p 6379 shutdown nosave >/dev/null 2>&1
    for i in \$(seq 100); do pgrep -x redis-server >/dev/null || break; sleep 0.1; done; tmux kill-session -t rsrv 2>/dev/null
    tmux new-session -d -s rsrv 'taskset -c $SPIN ./src/redis-server --port 6379 $HOMA --bind 0.0.0.0 --protected-mode no --save \"\" --io-threads 1'
    for i in \$(seq 50); do ./src/redis-cli ping 2>/dev/null | grep -q PONG && break; sleep 0.1; done; ./src/redis-cli ping" | grep -q PONG || { echo "server start failed" >&2; exit 1; }
}
echo "round,transport,workload,threads,ops,ktps,server_busy_sum,server_busy_max,client_busy_sum,client_busy_max"
for r in $(seq 1 $ROUNDS); do for w in $WORKLOADS; do for t in $THREADS; do for tr in $TRANSPORTS; do
  start; port=$([ $tr = homa ] && echo 2000 || echo 6379)
  case $t in 1) ops=500000;; 2) ops=800000;; 4) ops=1500000;; *) ops=2000000;; esac  # run phase >= ~15 s
  [ $w = workloade ] && ops=$((ops / 50))                # a scan reads ~50 records: ~1k ops/s
  sed "s/^operationcount=.*/operationcount=$ops/" $Y/workloads/$w.spec > /tmp/ycsb-diag.spec
  : > /tmp/ycsb-diag.out
  (cd $Y && exec taskset -c $YCPUS ./ycsbc -db redis -threads $t -P /tmp/ycsb-diag.spec \
     -host $SRV -port $port -transport $tr -slaves 0 > /tmp/ycsb-diag.out 2>&1) & p=$!
  until grep -q '^# Loading records' /tmp/ycsb-diag.out || ! kill -0 $p 2>/dev/null; do sleep 0.2; done
  sleep 1                                               # sample the run phase only
  ssh -4 node0 "~/busy-cores.sh 0-19 4" > /tmp/ycsb-diag.srv & s=$!
  cli=$(~/busy-cores.sh 0-19 4); wait $s; srv=$(cat /tmp/ycsb-diag.srv)
  wait $p; ktps=$(tail -1 /tmp/ycsb-diag.out | awk '{print $NF}')
  echo "$r,$tr,$w,$t,$ops,$ktps,${srv// /,},${cli// /,}"
done; done; done; done
