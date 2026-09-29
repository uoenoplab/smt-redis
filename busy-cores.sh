#!/bin/bash
# busy-cores.sh <cpu list, e.g. 7 or 0-3,5-13> <secs>: "sum max" of per-CPU busy % over secs, as
# MPERF/TSC (turbostat's Busy%: the share of time each CPU executed, not halted). /proc/stat samples
# ticks, and an otherwise idle CPU has its tick stopped, so it misses most interrupt work there.
# perf is the image's linux-tools one (the /usr/bin/perf wrapper refuses the mainline kernel).
cpus=" "; for part in ${1//,/ }; do for ((i=${part%-*}; i<=${part#*-}; i++)); do cpus+="$i "; done; done
sudo $(ls /usr/lib/linux-tools/*/perf | head -1) stat -a -A -x, -e msr/mperf/,msr/tsc/ -- sleep "$2" 2>&1 |
  awk -F, -v cpus="$cpus" '{c = substr($1, 4); v[c, $4] = $2}
    END {n = split(cpus, l, " "); for (i = 1; i <= n; i++) {p = 100 * v[l[i], "msr/mperf/"] / v[l[i], "msr/tsc/"]; s += p; if (p > m) m = p}
         printf "%.1f %.1f\n", s, m}'
