#!/bin/sh
# soakrun - OCM drain tests for one lab session.
#   T0: stall injection 50/90/150/250 ms, checks missed counts against expected
#   T2: SCHED_FIFO baseline, no injected stalls
# usage: soakrun [BOARD] [T2_MINUTES]      (defaults: rohan 20)
# Logs go to /tmp during runs and are copied to /mnt/sd/soak/<UTC time>/ at the end.
# Env for bench testing only: T0SEC (60), T0PER (10), T2SEC, OCMSOAK_EXTRA (e.g. "-m ring.bin -a 0")

BOARD=${1:-rohan}
T2MIN=${2:-20}
T0SEC=${T0SEC:-60}
T0PER=${T0PER:-10}
TMP=/tmp/soak
OUT=/mnt/sd/soak/$(date -u +%Y%m%dT%H%M)
NSTALL=$(( (T0SEC - 1) / T0PER ))
FAIL=0

mkdir -p "$TMP" || exit 1

missed_of() { sed -n 's/^# summary elapsed_s=.* missed=\([0-9]*\) .*/\1/p' "$1"; }

t0() {  # t0 STALL_MS LAPS_PER_STALL
    f="$TMP/soak_${BOARD}_T0_j$1.csv"
    exp=$(( NSTALL * $2 * 2048 ))
    echo "== T0 stall $1 ms: ${T0SEC} s, ${NSTALL} stalls, expect missed=${exp}"
    ocmsoak $OCMSOAK_EXTRA -d "$T0SEC" -j "$1" -J "$T0PER" -o "$f" -t "T0 stall $1 $BOARD" >/dev/null 2>&1
    got=$(missed_of "$f")
    if [ "$got" = "$exp" ]; then
        echo "   PASS missed=$got"
    else
        echo "   CHECK missed=${got:-none} (expected $exp)"
        FAIL=1
    fi
}

t0 50 0
t0 90 0
t0 150 1
t0 250 2

T2SEC=${T2SEC:-$(( T2MIN * 60 ))}
if [ "$T2SEC" -gt 0 ]; then
    echo "== T2 SCHED_FIFO baseline: ${T2SEC} s (Ctrl+C ends it early and still writes the summary)"
    f="$TMP/soak_${BOARD}_T2_fifo.csv"
    ocmsoak $OCMSOAK_EXTRA -d "$T2SEC" -f 50 -o "$f" -t "T2 idle FIFO 50 $BOARD" >/dev/null
    [ "$(missed_of "$f")" = "0" ] || FAIL=1
else
    echo "== T2 skipped"
fi

mkdir -p "$OUT" && cp "$TMP"/soak_"$BOARD"_*.csv "$OUT"/ && sync
echo "== logs copied to $OUT"
ls -l "$OUT"
[ "$FAIL" = 0 ] && echo "soakrun: all checks passed" || echo "soakrun: see CHECK lines above"
exit "$FAIL"