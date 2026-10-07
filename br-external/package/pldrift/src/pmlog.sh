#!/bin/sh
# pmlog - log CPU1 control-cycle timing from the PL performance monitor,
# one reading per minute.
#
# usage: pmlog LABEL MINUTES [CSVFILE]
#
# Each minute the monitor is cleared, left for 60 s, then its max and min
# registers are read. The monitor keeps only extremes, so one reading per
# minute gives MINUTES maximums per run instead of a single value, which is
# what makes runs comparable (method from the 1 Oct 2026 lab session).
#
# Registers (performance entity in design_top.vhd, 5 ns per count):
#   0x800C0000  pmonctrl: write 0x00030003 then 0x00000003 to clear and run
#   0x800C0100  util max   CPU1 busy time in a control cycle
#   0x800C0104  util min
#   0x800C0200  lat max    latency monitor
#   0x800C0204  lat min
#
# Prints one readable line per minute; with CSVFILE also appends
# time_utc,label,minute,util_max_ns,util_min_ns,lat_max_ns,lat_min_ns.

LABEL=$1
N=$2
CSV=$3
case "$N" in
    ''|*[!0-9]*) echo "usage: pmlog LABEL MINUTES [CSVFILE]" >&2; exit 2 ;;
esac
[ -n "$LABEL" ] || { echo "usage: pmlog LABEL MINUTES [CSVFILE]" >&2; exit 2; }
command -v devmem >/dev/null || { echo "pmlog: devmem not found" >&2; exit 1; }

if [ -n "$CSV" ] && [ ! -s "$CSV" ]; then
    echo "time_utc,label,minute,util_max_ns,util_min_ns,lat_max_ns,lat_min_ns" > "$CSV"
fi

i=1
while [ "$i" -le "$N" ]; do
    devmem 0x800C0000 32 0x00030003
    devmem 0x800C0000 32 0x00000003
    sleep 60
    umax=$(( $(devmem 0x800C0100) * 5 ))
    umin=$(( $(devmem 0x800C0104) * 5 ))
    lmax=$(( $(devmem 0x800C0200) * 5 ))
    lmin=$(( $(devmem 0x800C0204) * 5 ))
    t=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    printf '%s %-16s min %2d/%d  util_max %5d ns  util_min %5d ns  lat_max %5d ns  lat_min %5d ns\n' \
        "$t" "$LABEL" "$i" "$N" "$umax" "$umin" "$lmax" "$lmin"
    [ -n "$CSV" ] && echo "$t,$LABEL,$i,$umax,$umin,$lmax,$lmin" >> "$CSV"
    i=$((i + 1))
done
