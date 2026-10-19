#!/bin/bash
# memtier-ab.sh (node1, Homa's config): memtier 9006af8 (counts a request from its send) against 7f8b5a3
# (from its arrival), interleaved, 3 pairs, at two points past saturation: c53 with 1,024 clients and c52
# with 24,576, both at 100k requests/s. Output: results/memtier-ab.csv (a memtier column, then
# fleet-xl170.sh's columns).
cd ~ && set -eo pipefail
hdr=1
for i in 1 2 3; do
  for m in 9006af8 7f8b5a3; do
    M=~/memtier_benchmark/memtier_benchmark; [ $m = 9006af8 ] && M=~/memtier-9006af8/memtier_benchmark
    for wc in "c53 64" "c52 1536"; do
      set -- $wc
      out=$(M=$M WORKLOADS=$1 THREADS=16 CPT=$2 LOADS=100000 ROUNDS=1 TRANSPORTS=homa bash fleet-xl170.sh)
      [ $hdr ] && { echo "memtier,$(head -1 <<< "$out")"; hdr=; }
      tail -n +2 <<< "$out" | sed "s|^|$m,|"
    done
  done
done
