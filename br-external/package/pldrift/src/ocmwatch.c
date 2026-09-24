#!/bin/sh
# ocmwatch - print OCM ring record 0 each time the PL rewrites it.
#
# Record 0 is rewritten once per ring pass: 20,000 Hz / 2048 records, about
# 9.77 times per second. Use it to check that ADC channels are changing.
# Inputs above about 5 Hz alias (a 100 Hz input appears at about 2.34 Hz).
#
# Columns: timestamp low word, then ch0..ch5 as ADC codes (raw int16 >> 4).
# An open input reads -1 (raw 0xFFF0). Stop with Ctrl+C.

P=""
IDLE=0
WARNED=0
while true; do
  T=$(devmem 0xFFFC0000 32)
  if [ "$T" = "$P" ]; then
    IDLE=$((IDLE + 1))
    if [ $IDLE -gt 300 ] && [ $WARNED -eq 0 ]; then
      echo "ocmwatch: record 0 not changing; is the DAQ started?" >&2
      WARNED=1
    fi
    continue
  fi
  P=$T
  IDLE=0
  WARNED=0
  W0=$(devmem 0xFFFC0008 32)
  W1=$(devmem 0xFFFC000C 32)
  W2=$(devmem 0xFFFC0010 32)
  out=""
  for w in $W0 $W1 $W2; do
    for s in 0 16; do
      c=$(( (w >> s) & 0xFFFF ))
      [ $c -ge 32768 ] && c=$(( c - 65536 ))
      out="$out$(printf '%7d' $(( c >> 4 )))"
    done
  done
  echo "$T$out"
done