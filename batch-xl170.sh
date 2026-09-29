#!/bin/bash
# batch-xl170.sh > batch.csv: batching curve on xl170 (runs on node1 against redis-server on node0).
# redis-benchmark GET and SET of a 100 B value on one key, pipeline depth -P {1,2,4,8,16,32,64},
# from 4 benchmark processes x 16 clients pinned to node1 cpus 8-11 (one redis-benchmark process is
# single-threaded and would be the bottleneck), 3 rounds; server-node and client-node CPU sampled
# mid-run. redis-server pinned to node0 cpu7. TRANSPORTS as in ycsb-xl170.sh: "homa tcp" (default)
# in the official config state, "tcp" alone for the stock-TCP baseline after tcpclean.sh on.
# Output: round,transport,test,pipeline,requests,rps,p50_ms,p99_ms,server_busy_sum,server_busy_max,
#         client_busy_sum,client_busy_max (rps summed over the 4 processes, latencies their median)
SRV=10.0.1.1; R=~/smt-redis; SPIN=7; CPUS=(8 9 10 11); TRANSPORTS=${TRANSPORTS:-homa tcp}
HOMA=$([[ " $TRANSPORTS " == *" homa "* ]] && echo "--homa-port 2000")
[ -n "$HOMA" ] && for h in node0 node1; do ssh -4 $h 'bash -lc "config rps" >/dev/null'; done
start() {  # a fresh redis-server on node0 for every run
  ssh -4 node0 "cd $R && ./src/redis-cli -p 6379 shutdown nosave >/dev/null 2>&1
    for i in \$(seq 100); do pgrep -x redis-server >/dev/null || break; sleep 0.1; done; tmux kill-session -t rsrv 2>/dev/null
    tmux new-session -d -s rsrv 'taskset -c $SPIN ./src/redis-server --port 6379 $HOMA --bind 0.0.0.0 --protected-mode no --save \"\" --io-threads 1'
    for i in \$(seq 50); do ./src/redis-cli ping 2>/dev/null | grep -q PONG && break; sleep 0.1; done; ./src/redis-cli ping" | grep -q PONG || { echo "server start failed" >&2; exit 1; }
  head -c 100 /dev/zero | tr '\0' v | $R/src/redis-cli -h $SRV -x set key:__rand_int__ >/dev/null  # GET's key
}
echo "round,transport,test,pipeline,requests,rps,p50_ms,p99_ms,server_busy_sum,server_busy_max,client_busy_sum,client_busy_max"
for r in 1 2 3; do for t in get set; do for P in 1 2 4 8 16 32 64; do for tr in $TRANSPORTS; do
  start; n=$(( P * 200000 < 5000000 ? P * 200000 : 5000000 ))          # per process: ~8-15 s a run
  f=$([ $tr = homa ] && echo "--homa -p 2000" || echo "-p 6379")
  for c in "${CPUS[@]}"; do
    taskset -c $c $R/src/redis-benchmark $f -h $SRV -c 16 -n $n -P $P -d 100 -t $t --csv > /tmp/rb.$c 2>&1 &
  done
  sleep 3; ssh -4 node0 "~/busy-cores.sh 0-19 3" > /tmp/rb.srv & s=$!
  cli=$(~/busy-cores.sh 0-19 3); wait                                  # the sampler and all 4 benchmarks
  col() { cat /tmp/rb.{8,9,10,11} | awk -F'"' -v f=$1 '$2 ~ /^(GET|SET)$/ {print $f}'; }  # 4: rps, 10: p50, 14: p99
  med() { col $1 | sort -g | awk '{v[NR] = $1} END {print (v[int((NR+1)/2)] + v[int(NR/2)+1]) / 2}'; }
  echo "$r,$tr,$t,$P,$((4 * n)),$(col 4 | awk '{s += $1} END {printf "%.0f", s}'),$(med 10),$(med 14),$(tr ' ' , < /tmp/rb.srv),${cli// /,}"
done; done; done; done
