#!/bin/bash
# need-ack-probe.sh <redis dir> <runs>: after each 10 s run of 30k GET/s over 25,000 Homa sockets, node0's
# homa_timer CPU and per-second NEED_ACK sends / RPC timeouts, and node1's NEED_ACKs received / ACKs sent.
R=$1; SRV=10.0.1.1; ulimit -n 200000
m() { awk -v k="$1" '$1 == k {s += $2} END {print s + 0}' /proc/net/homa_metrics; }
ssh -4 node0 "cd $R && ./src/redis-cli -p 6379 shutdown nosave >/dev/null 2>&1; sleep 0.5
  tmux new-session -d -s rsrv 'ulimit -n 200000; taskset -c 7 ./src/redis-server --port 6379 --homa-port 2000 --bind 0.0.0.0 --protected-mode no --save \"\" --io-threads 1 --maxclients 100000'
  sleep 1; ./src/redis-cli ping" | grep -q PONG || { echo "server start failed"; exit 1; }
head -c 100 /dev/zero | tr '\0' s | $R/src/redis-cli -h $SRV -x set small >/dev/null
N0='m() { awk -v k="$1" "\$1 == k {s += \$2} END {print s + 0}" /proc/net/homa_metrics; }
a=$(m packets_sent_NEED_ACK); t=$(m rpc_timeouts); sleep 5; b=$(m packets_sent_NEED_ACK); u=$(m rpc_timeouts)
echo "timer $(top -bn2 -d 1 -H -p $(pgrep -x homa_timer) | awk "/^ *[0-9]/{v=\$9} END{print v}")% need_ack/s $(( (b - a) / 5 )) timeouts/s $(( (u - t) / 5 ))"'
for i in $(seq $2); do
  taskset -c 8,9 ~/mixload $SRV 2000 homa 25000 30000 10 0 > /tmp/lp.out 2> /tmp/lp.err
  for s in 0 30 60; do [ $s -gt 0 ] && sleep 25; r0=$(m packets_rcvd_NEED_ACK); a0=$(m packets_sent_ACK)
    n=$(ssh -4 node0 "$N0"); echo "run $i +${s}s: node0 $n | node1 need_ack in/s $(( ($(m packets_rcvd_NEED_ACK) - r0) / 7 )) ack out/s $(( ($(m packets_sent_ACK) - a0) / 7 ))"; done
done
