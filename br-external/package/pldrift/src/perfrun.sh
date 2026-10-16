#!/bin/sh
# perfrun - one perturbation test run, with everything saved to a run folder.
#
# usage: [INTERVAL=s] perfrun RUN_ID MINUTES [COMMAND ...]
#
#   RUN_ID   name for the run and its folder, e.g. L2-R03-scope20k
#   MINUTES  measured minutes
#   INTERVAL seconds per pmlog reading (default 60); INTERVAL=1 gives
#            one reading per second. Passed to pmlog through the environment.
#   COMMAND  optional load to run during the measurement, e.g.
#            adcscope -c 0,1,2,3 -B scope -r 20000 -f 250 -s L2-R03-scope20k
#            Leave it out for an idle run.
#
# Sequence:
#   1. record the system state (sys.txt)
#   2. start COMMAND in the background (output to cmd.log)
#   3. wait WARMUP seconds (default 30) so start-up effects are excluded
#      from the steady-state figures; set WARMUP=0 to include them
#   4. pmlog for MINUTES (pm.csv, pm.txt), with CPU0 load and eth0 traffic
#      measured over the same window
#   5. stop COMMAND with SIGTERM so it writes its final scope_meta
#   6. write summary.txt and copy the folder to /mnt/sd/runs/RUN_ID
#
# Files are written to /tmp during the run (RAM). The SD copy happens after
# the measurement, because SD writes were found to disturb CPU1 on 1 Oct.
#
# The eth0 tx byte counter in /proc/net/dev is 32-bit on this kernel and
# wraps at 4 GiB. The summary corrects for one wrap, so eth0_tx_mb_per_s is
# right as long as less than 4 GiB is sent during the measurement (about
# 79 min at 0.9 MB/s, 25 min at 2.8 MB/s).
#
# CPU1 should be running its controller (e.g. feedLUT_noTimerReset.elf)
# throughout; sys.txt records whether its OCM outputs were changing.

ID=$1
MIN=$2
case "$MIN" in
    ''|*[!0-9]*) echo "usage: perfrun RUN_ID MINUTES [COMMAND ...]" >&2; exit 2 ;;
esac
case "$ID" in
    ''|*/*|*' '*) echo "perfrun: RUN_ID must be one word without /" >&2; exit 2 ;;
esac
shift 2
WARMUP=${WARMUP:-30}
INTERVAL=${INTERVAL:-60}
case "$INTERVAL" in
    ''|*[!0-9]*|0) echo "perfrun: INTERVAL must be a whole number of seconds, 1 or more" >&2; exit 2 ;;
esac
export INTERVAL
D=/tmp/runs/$ID
SD=/mnt/sd/runs/$ID
if [ -e "$D" ] || [ -e "$SD" ]; then
    echo "perfrun: $ID already exists in /tmp/runs or /mnt/sd/runs; pick a new RUN_ID" >&2
    exit 1
fi
mkdir -p "$D" || exit 1

cpu_stat() { awk '/^cpu /{b=$2+$3+$4+$7+$8+$9; print b, b+$5+$6}' /proc/stat; }
tx_bytes() { awk -F'[: ]+' '/eth0:/{for(i=1;i<=NF;i++) if($i=="eth0:"||$i=="eth0") {print $(i+9); exit}}' /proc/net/dev; }

# ---- 1. system state
{
    echo "run_id: $ID"
    echo "start_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "minutes: $MIN"
    echo "warmup_s: $WARMUP"
    echo "interval_s: $INTERVAL"
    echo "command: ${*:-(none, idle run)}"
    echo "kernel: $(uname -r)"
    echo "uptime: $(cat /proc/uptime)"
    echo "loadavg: $(cat /proc/loadavg)"
    echo "bitstream_revision: $(devmem 0x80000004)"
    a=$(devmem 0xFFFFF800 16); sleep 0.05 2>/dev/null || sleep 1; b=$(devmem 0xFFFFF800 16)
    if [ "$a" != "$b" ]; then echo "cpu1_outputs_changing: yes ($a, $b)"; else echo "cpu1_outputs_changing: NO ($a, $b)"; fi
    echo "eth0: $(ip -4 -o addr show eth0 2>/dev/null | awk '{print $4}')"
    echo "processes:"; ps 2>/dev/null | grep -v '\[' | sed 's/^/  /'
    echo "adcmon.conf:"; sed 's/^/  /' /mnt/sd/adcmon.conf 2>/dev/null
} > "$D/sys.txt"
grep -q "cpu1_outputs_changing: NO" "$D/sys.txt" &&
    echo "perfrun: WARNING CPU1 outputs at 0xFFFFF800 are not changing; is the controller running?" >&2

# ---- 2. load
PID=
if [ $# -gt 0 ]; then
    "$@" > "$D/cmd.log" 2>&1 &
    PID=$!
    echo "perfrun: $ID started '$*' (pid $PID)"
fi

# ---- 3. warm-up
[ "$WARMUP" -gt 0 ] && { echo "perfrun: warm-up ${WARMUP} s"; sleep "$WARMUP"; }
if [ -n "$PID" ] && ! kill -0 "$PID" 2>/dev/null; then
    echo "perfrun: command exited during warm-up; see $D/cmd.log" >&2
    tail -5 "$D/cmd.log" >&2
    exit 1
fi

# ---- 4. measurement
set -- $(cpu_stat); b0=$1; t0=$2
x0=$(tx_bytes); s0=$(date +%s)
echo "perfrun: measuring for $MIN min"
pmlog "$ID" "$MIN" "$D/pm.csv" | tee "$D/pm.txt"
set -- $(cpu_stat); b1=$1; t1=$2
x1=$(tx_bytes); s1=$(date +%s)
alive=yes
[ -n "$PID" ] && ! kill -0 "$PID" 2>/dev/null && alive=NO

# ---- 5. stop the load
if [ -n "$PID" ]; then
    kill -TERM "$PID" 2>/dev/null
    i=0; while kill -0 "$PID" 2>/dev/null && [ $i -lt 20 ]; do sleep 1; i=$((i + 1)); done
    kill -0 "$PID" 2>/dev/null && kill -KILL "$PID" 2>/dev/null
    wait "$PID" 2>/dev/null
fi

# ---- 6. summary
el=$((s1 - s0)); [ "$el" -gt 0 ] || el=1
# median, max and min of a pm.csv column, using sort so that runs with many
# readings (INTERVAL=1 gives 3600 per hour) stay fast on the board
colstats() {
    tail -n +2 "$D/pm.csv" | cut -d, -f"$1" | sort -n | awk '
        { a[NR] = $1 }
        END { n = NR; if (!n) { print "0 0 0"; exit }
              m = (n % 2) ? a[(n + 1) / 2] : (a[n / 2] + a[n / 2 + 1]) / 2
              printf "%d %d %d\n", m, a[n], a[1] }'
}
set -- $(colstats 4); umed=$1; umaxx=$2; umaxmin=$3
set -- $(colstats 6); lmed=$1; lmaxx=$2
awk -v id="$ID" -v b0="$b0" -v t0="$t0" -v b1="$b1" -v t1="$t1" -v x0="${x0:-0}" -v x1="${x1:-0}" \
    -v el="$el" -v alive="$alive" -v hascmd="${PID:+1}" -v min="$MIN" -v iv="$INTERVAL" \
    -v umed="$umed" -v umaxx="$umaxx" -v umaxmin="$umaxmin" -v lmed="$lmed" -v lmaxx="$lmaxx" -F, '
    NR > 1 { n++; if ($5 < umin || n == 1) umin = $5 }
    END {
        printf "run_id: %s\nminutes_measured: %d\nreadings: %d\ninterval_s: %d\n", id, min, n, iv
        printf "cpu1_util_max_ns: median %d, max %d, min-of-reading-maxima %d\n", umed, umaxx, umaxmin
        printf "cpu1_util_min_ns: %d\n", umin
        printf "lat_max_ns: median %d, max %d\n", lmed, lmaxx
        printf "cpu0_busy_pct: %.1f\n", (t1 > t0) ? 100 * (b1 - b0) / (t1 - t0) : 0
        # 32-bit tx counter: add 2^32 if it wrapped once during the measurement
        tx = x1 - x0; if (tx < 0) tx += 4294967296
        printf "eth0_tx_mb_per_s: %.3f\n", tx / el / 1e6
        if (hascmd) printf "command_alive_at_end: %s\n", alive
    }' "$D/pm.csv" > "$D/summary.txt"
[ -s "$D/cmd.log" ] && grep 'queue max' "$D/cmd.log" | tail -1 | sed 's/^adcscope: /adcscope_last_meta: /' >> "$D/summary.txt"
echo "end_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$D/sys.txt"

echo "----- $ID summary"
cat "$D/summary.txt"
[ "$alive" = NO ] && echo "perfrun: WARNING the command stopped before the end of the measurement" >&2

mkdir -p /mnt/sd/runs && cp -r "$D" /mnt/sd/runs/ && sync && echo "perfrun: saved to $SD"