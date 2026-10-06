#!/bin/bash
# need-ack-probe.sh (node1, Homa's config): 20 short-lived `redis-cli --homa ping`, each ending with a
# QUIT it does not wait for, against a fresh redis-server, with node1's net.ipv4.icmp_ratelimit at
# 1000 (the default) and 0, twice each. Prints the NEED_ACK packets/s node0 sends before, right after
# and 4 s after. Leaves icmp_ratelimit at 0 (the setting of every other run).
R=~/smt-redis
rate() { ssh -4 node0 'm() { awk "\$1 == \"packets_sent_NEED_ACK\" {s += \$2} END {print s + 0}" /proc/net/homa_metrics; }; a=$(m); sleep 2; b=$(m); echo $(( (b - a) / 2 ))'; }
ssh -4 node0 "cd $R && ./src/redis-cli -p 6379 shutdown nosave >/dev/null 2>&1; for i in \$(seq 100); do pgrep -x redis-server >/dev/null || break; sleep 0.1; done
  tmux kill-session -t rsrv 2>/dev/null; tmux new-session -d -s rsrv './src/redis-server --port 6379 --homa-port 2000 --bind 0.0.0.0 --protected-mode no --save \"\" --io-threads 1'; sleep 1"
for lim in 1000 0 1000 0; do
  sudo sysctl -qw net.ipv4.icmp_ratelimit=$lim; sleep 12; b=$(rate)
  for i in $(seq 20); do $R/src/redis-cli --homa -h 10.0.1.1 -p 2000 ping > /dev/null; done
  echo "icmp_ratelimit=$lim: NEED_ACK/s before $b, right after $(rate), 4 s later $(rate)"
done
sudo sysctl -qw net.ipv4.icmp_ratelimit=0; ssh -4 node0 "cd $R && ./src/redis-cli -p 6379 shutdown nosave"
