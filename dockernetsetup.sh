#!/bin/sh
ip link add br0 type bridge
ip link set dev eth0 down
ip addr flush dev eth0 
ip link set dev eth0 up
ip link set eth0 master br0
ip tuntap add dev tap0 mode tap user $(whoami)
ip link set tap0 master br0
ip link set dev br0 up
ip addr add 172.17.0.2/24 dev br0
ip addr add 172.17.0.3/24 dev tap0
ip link set promisc on tap0
ip link set dev tap0 up
ip route add default via 172.17.0.1
iptables -t nat -L
ifconfig
# Shell script to configure networking for docker
