#!/bin/sh
ip link add br0 type bridge
ip link set eth0 master br0
ip tuntap add dev tap0 mode tap user $(whoami)
ip link set tap0 master br0
ip link set dev br0 up
ip link set dev tap0 up
ifconfig
# Shell script to configure networking for docker
