#!/bin/bash
# hoststate.sh homa|tcp: print node0's and node1's host state and check it is the one the runs need;
# exit 1 with the reason otherwise. homa: Homa's config (homa.ko loaded, sch_homa on all 20 TX queues,
# RPS/RFS on) and nothing holding homa.ko beyond those 20 qdiscs. tcp: TCP alone (homa.ko unloaded, no
# sch_homa, RPS/RFS off). Both: no memtier_benchmark left on node1. A process with a Homa socket open
# keeps homa.ko in use, which makes rmmod and then config default fail, so the extra references are
# reported, with the processes that hold Homa sockets.
[ "$1" = homa ] || [ "$1" = tcp ] || { echo "usage: hoststate.sh homa|tcp" >&2; exit 2; }
rc=0
for h in node0 node1; do ssh -4 $h "
  q=\$(tc qdisc show dev ens1f1np1 | grep -c 'qdisc homa'); refs=\$(lsmod | awk '\$1==\"homa\"{print \$3}')
  rps=\$(cat /sys/class/net/ens1f1np1/queues/rx-0/rps_cpus); rfs=\$(sysctl -n net.core.rps_sock_flow_entries)
  echo \$(hostname -s): homa=\$(cat /sys/module/homa/srcversion 2>/dev/null || echo unloaded) refs=\${refs:--} \
    homa_qdiscs=\$q rps=\$rps rfs=\$rfs \
    coalesce=\$(ethtool -c ens1f1np1 | awk '/^rx-usecs:|^rx-frames:|^tx-usecs:/{printf \"%s%s \", \$1, \$2}')
  bad=
  if [ $1 = homa ]; then
    [ -e /sys/module/homa ] || bad=\"\$bad homa.ko not loaded;\"
    [ \$q = 20 ] || bad=\"\$bad \$q of 20 TX queues with sch_homa;\"
    [ \$rfs = 32768 ] && [ \$(( 16#\$rps )) != 0 ] || bad=\"\$bad RPS/RFS off;\"
    [ ! -e /sys/module/homa ] || [ \"\$refs\" = 20 ] || bad=\"\$bad homa.ko used by \$refs, not by the 20 qdiscs alone (Homa sockets open; Homa clients and servers here: \$(pgrep -a 'memtier|redis|mixload|cp_node|ycsbc' | tr '\n' ' '));\"
  else
    [ ! -e /sys/module/homa ] || bad=\"\$bad homa.ko still loaded (used by \$refs);\"
    [ \$q = 0 ] || bad=\"\$bad sch_homa on \$q TX queues;\"
    [ \$rfs = 0 ] && [ \$(( 16#\$rps )) = 0 ] || bad=\"\$bad RPS/RFS on;\"
  fi
  [ \$(hostname -s) != node1 ] || ! p=\$(pidof memtier_benchmark) || bad=\"\$bad memtier_benchmark left running (pid \$(echo \$p));\"
  [ -z \"\$bad\" ] || { echo \"\$(hostname -s): not in the $1 state:\$bad\" >&2; exit 1; }" || rc=1; done
exit $rc
