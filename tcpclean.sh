#!/bin/bash
# tcpclean.sh on|off: switch node0 and node1 between TCP alone and Homa's config. on: stop
# redis-server and unload homa.ko, taking all of Homa out of TCP's path (sch_homa and its pacer, RPS/RFS,
# the TCP GRO hook homa.ko installs at load); qdisc back to the kernel default (mq + fq_codel), RSS only.
# NIC coalescing, governor and C-states stay as the official config set them. off: `config default`
# again (reloads ~/bin/homa.ko). Then hoststate.sh prints the state and exits 1 unless it is the one
# asked for (rmmod fails while any Homa socket is open, and config default then fails at insmod).
[ "$1" = on ] || [ "$1" = off ] || { echo "usage: tcpclean.sh on|off" >&2; exit 2; }
for h in node0 node1; do ssh -4 $h "
  ~/smt-redis/src/redis-cli -p 6379 shutdown nosave >/dev/null 2>&1; tmux kill-session -t rsrv 2>/dev/null
  if [ $1 = on ]; then
    bash -lc 'config reset_qdisc' >/dev/null
    for q in /sys/class/net/ens1f1np1/queues/rx-*; do echo 0 | sudo tee \$q/rps_cpus \$q/rps_flow_cnt >/dev/null; done
    sudo sysctl -qw net.core.rps_sock_flow_entries=0; [ ! -e /sys/module/homa ] || sudo rmmod homa
  else
    bash -lc 'config default' >/dev/null
  fi"; done
bash "$(dirname "$0")/hoststate.sh" $([ $1 = on ] && echo tcp || echo homa)
