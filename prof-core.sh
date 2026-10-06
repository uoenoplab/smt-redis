#!/bin/bash
# prof-core.sh <suffix> <transport...>: where the Redis core's time goes. For each transport, 100 B
# GETs at RATE (default 40k/s) over one endpoint from mixload (node1 cpu 8) while perf samples
# node0 CPU 7, the Redis core, for 5 s; writes prof-core-<transport><suffix>.txt (perf report by
# symbol; its first line is the core's busy %) and prints mixload's row. R picks the Redis build.
# perf is the image's linux-tools one (the /usr/bin/perf wrapper refuses the mainline kernel).
SRV=10.0.1.1; R=${R:-~/smt-redis}; RATE=${RATE:-40000}; suf=$1; shift
PERF='sudo $(ls /usr/lib/linux-tools/*/perf | head -1)'
HOMA=$([[ " $* " == *" homa "* ]] && echo "--homa-port 2000")
for tr in "$@"; do  # each on a fresh redis-server
  ssh -4 node0 "cd $R && ./src/redis-cli -p 6379 shutdown nosave >/dev/null 2>&1
    for i in \$(seq 100); do pgrep -x redis-server >/dev/null || break; sleep 0.1; done; tmux kill-session -t rsrv 2>/dev/null
    tmux new-session -d -s rsrv 'taskset -c 7 ./src/redis-server --port 6379 $HOMA --bind 0.0.0.0 --protected-mode no --save \"\" --io-threads 1'
    for i in \$(seq 50); do ./src/redis-cli ping 2>/dev/null | grep -q PONG && break; sleep 0.1; done; ./src/redis-cli ping" | grep -q PONG || { echo "server start failed" >&2; exit 1; }
  head -c 100 /dev/zero | tr '\0' s | $R/src/redis-cli -h $SRV -x set small >/dev/null
  taskset -c 8 ~/mixload $SRV $([ $tr = homa ] && echo 2000 || echo 6379) $tr 1 $RATE 15 0 > /tmp/prof.out 2> /tmp/prof.err & p=$!
  sleep 5
  ssh -4 node0 "$PERF record -q -C 7 -F 4999 -o /tmp/prof.data -- ~/busy-cores.sh 7 5 2>/dev/null
    $PERF report -q -i /tmp/prof.data --no-children --sort dso,sym --stdio 2>/dev/null | grep '%'" > prof-core-$tr$suf.txt
  wait $p; echo "$tr$suf $(cat /tmp/prof.out)"
done
