#!/bin/bash
# tcpclean.sh on|off: host state of node0 and node1 for the TCP baseline. on: stop redis-server
# and unload homa.ko, taking all of Homa out of TCP's path (sch_homa and its pacer, RPS/RFS, the TCP
# GRO hook homa.ko installs at load); qdisc back to the kernel default (mq + fq_codel), RSS only. NIC
# coalescing, governor and C-states stay as the official config set them. off: `config default`
# again (reloads ~/bin/homa.ko). Prints each node's resulting state.
for h in node0 node1; do ssh -4 $h "
  ~/smt-redis/src/redis-cli -p 6379 shutdown nosave >/dev/null 2>&1; tmux kill-session -t rsrv 2>/dev/null
  if [ '$1' = on ]; then
    bash -lc 'config reset_qdisc' >/dev/null
    for q in /sys/class/net/ens1f1np1/queues/rx-*; do echo 0 | sudo tee \$q/rps_cpus \$q/rps_flow_cnt >/dev/null; done
    sudo sysctl -qw net.core.rps_sock_flow_entries=0; sudo rmmod homa
  else
    # config default has once left a node without sch_homa and RPS: retry until all 20 TX queues have it
    for i in 1 2 3; do bash -lc 'config default' >/dev/null 2>&1
      [ \$(tc qdisc show dev ens1f1np1 | grep -c 'qdisc homa') = 20 ] && break; done
  fi
  echo \$(hostname -s): homa=\$(cat /sys/module/homa/srcversion 2>/dev/null || echo unloaded) \
    qdisc=\$(tc qdisc show dev ens1f1np1 | awk '{print \$2}' | sort | uniq -c | awk '{printf \"%sx%s \", \$1, \$2}') \
    rps=\$(cat /sys/class/net/ens1f1np1/queues/rx-0/rps_cpus) rfs=\$(sysctl -n net.core.rps_sock_flow_entries) \
    coalesce=\$(ethtool -c ens1f1np1 | awk '/^rx-usecs:|^rx-frames:|^tx-usecs:/{printf \"%s%s \", \$1, \$2}')"; done
