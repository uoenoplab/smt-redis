#!/bin/bash
# cpnode-xl170.sh > cpnode.csv: the transport alone, no Redis: Homa's cp_node RTT for one message of
# 100 B / 64 KB / 512 KB and a 100 B reply (--one-way), one RPC outstanding, Homa and TCP in Homa's
# official config, 3 rounds of 10 s. Server on node0 CPUs 7,8, client on node1 CPUs 8,9: each side
# needs two CPUs, since a Homa receiver thread polls (poll_usecs) and would starve its sender on one.
# Output: round,protocol,size,p50_us,p99_us
echo "round,protocol,size,p50_us,p99_us"
for r in 1 2 3; do for proto in homa tcp; do
  ssh -4 node0 "pkill -x cp_node; taskset -c 7,8 ~/bin/cp_node server --ports 1 --port-threads 1 --protocol $proto </dev/null >/tmp/cps.log 2>&1 &"
  sleep 1
  for size in 100 65536 524288; do
    set -- $(timeout 12 taskset -c 8,9 ~/bin/cp_node client --protocol $proto --servers 0 --id 1 --server-ports 1 --ports 1 \
      --port-receivers 1 --workload $size --one-way --client-max 1 2>&1 | grep -oE 'P50 [0-9.]+ P99 [0-9.]+' | tail -3 | head -1)
    echo "$r,$proto,$size,$2,$4"
  done
  ssh -4 node0 "pkill -x cp_node"
done; done
