#!/bin/sh
# labsuite - the streaming perturbation test plan, run back to back.
#
# usage: labsuite PREFIX [MINUTES]
#
#   PREFIX   lab session label used in every RUN_ID, e.g. L2
#   MINUTES  measured minutes per run (default 10)
#
# Runs, in order (RUN_ID = PREFIX-Rnn-name, also used as the adcscope
# session tag so each run can be found in InfluxDB):
#   R01-idle        no load
#   R02-scope5k     adcscope, 5 kHz waveform
#   R03-scope20k    adcscope, 20 kHz waveform (every sample)
#   R04-idle        no load again, to check the baseline has not moved
#   R05-scope20k    20 kHz repeated, since worst cases vary between runs
#
# adcscope options common to every run come from SCOPE_ARGS, default
#   -c 0,1,2,3 -R 2 -W 2:0 -B scope
# e.g. SCOPE_ARGS="-c 0,1 -R 0 -B scope" labsuite L2
#
# With 10-minute runs and a 30 s warm-up the suite takes about 55 minutes.
# Ctrl+C stops the current run; completed runs are already saved.

P=$1
M=${2:-10}
[ -n "$P" ] || { echo "usage: labsuite PREFIX [MINUTES]" >&2; exit 2; }
SCOPE_ARGS=${SCOPE_ARGS:--c 0,1,2,3 -R 2 -W 2:0 -B scope}

echo "labsuite: prefix $P, $M min per run, adcscope args: $SCOPE_ARGS"
echo "labsuite: start $(date -u +%H:%M:%S) UTC, about $(( (M * 60 + 45) * 5 / 60 )) min in total"

run() {
    id=$1; shift
    echo; echo "===== $id ($(date -u +%H:%M:%S) UTC)"
    perfrun "$id" "$M" "$@" || echo "labsuite: $id did not complete" >&2
}

# shellcheck disable=SC2086
run "$P-R01-idle"
run "$P-R02-scope5k"  adcscope $SCOPE_ARGS -r 5000            -s "$P-R02-scope5k"
run "$P-R03-scope20k" adcscope $SCOPE_ARGS -r 20000 -f 250    -s "$P-R03-scope20k"
run "$P-R04-idle"
run "$P-R05-scope20k" adcscope $SCOPE_ARGS -r 20000 -f 250    -s "$P-R05-scope20k"

echo; echo "===== labsuite done ($(date -u +%H:%M:%S) UTC)"
for d in /mnt/sd/runs/"$P"-R0*; do
    [ -f "$d/summary.txt" ] || continue
    printf '%-18s ' "${d##*/}"
    awk -F': ' '/^cpu1_util_max_ns/{u=$2} /^cpu0_busy_pct/{c=$2} /^eth0_tx/{x=$2}
        END{printf "CPU1 max %s | CPU0 busy %s%% | eth0 %s MB/s\n", u, c, x}' "$d/summary.txt"
done
