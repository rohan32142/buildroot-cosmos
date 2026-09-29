#!/bin/sh
# labup - per-session bring-up for the Cosmos XZQ10.
# usage: labup [IP/PREFIX]     (default 192.168.50.2/24)
# Stops dhcpcd, sets a static address, shows the clock, starts the DAQ.

IP=${1:-192.168.50.2/24}

killall dhcpcd 2>/dev/null
ip link set eth0 up
if ip -4 addr show eth0 | grep -q "inet ${IP%/*}/"; then
    echo "labup: eth0 already has ${IP}"
else
    ip addr add "$IP" dev eth0 && echo "labup: eth0 set to ${IP}"
fi

echo "labup: board UTC is $(date -u '+%Y-%m-%d %H:%M:%S')  (compare with the laptop)"

if adcmon start; then
    echo "labup: DAQ running"
else
    echo "labup: adcmon start FAILED" >&2
    exit 1
fi