#!/bin/bash
# homa-timer-busy.sh <secs>: busy % of the homa_timer kthread over secs (nothing without homa.ko). Homa's
# timer visits every socket on the host each tick, so its cost grows with the sockets a host holds.
p=$(pgrep -x homa_timer); [ -z "$p" ] && { sleep $1; echo; exit; }
a=$(awk '{print $14 + $15}' /proc/$p/stat); sleep $1; b=$(awk '{print $14 + $15}' /proc/$p/stat)
echo $(( (b - a) * 100 / $1 / $(getconf CLK_TCK) ))
