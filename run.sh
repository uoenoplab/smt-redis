#!/bin/bash -x

rm -f *.db server_*.txt

#iface="enp23s0f0np0"
iface="enp1s0f0np0"

first_core=16
last_core=16
queues=63
tls_hw="off"

#/root/setup_cores.sh $((last_core + 1)) 32

ethtool -K "$iface" tls-hw-tx-offload $tls_hw
ethtool -C "$iface" adaptive-rx off rx-usecs 5 rx-frames 1
ethtool -L "$iface" combined $queues

for id in `seq -f "%02g" $first_core $last_core`; do
	taskset -c $id ./src/redis-server ./redis.conf --homa-port "50$id" --homals-port "60$id" --port "70$id" --tls-port "80$id" --tcpktls-port "90$id" > "server_${id}.txt" 2>&1 &
done
wait
