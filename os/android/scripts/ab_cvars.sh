#!/bin/bash
# Compares live settings in the running game, one configuration after another.
#   ab_cvars.sh <seconds> "<label>=<--cvar=value ...>" ...
#   ab_cvars.sh 15 "spin=--gta4_frame_wait_sleep=false" "sleep=--gta4_frame_wait_sleep=true" \
#                  "spin=--gta4_frame_wait_sleep=false"
# Per configuration: new frames per second (from the frame heartbeat), the
# worker's wait for the recorder, draws, clock limits and per-thread CPU.
# Repeat configurations (A B A): the phone's thermal state drifts within a
# minute and changes the clocks, which a single A/B cannot tell from the setting.
set -uo pipefail
. "$(dirname "$0")/env.sh"
[ $# -ge 2 ] || { echo "usage: ab_cvars.sh <seconds> \"<label>=<--cvar=value ...>\" ..."; exit 1; }
PID=$(adbs shell pidof "$GAME_PROC" | tr -d '\r')
[ -n "$PID" ] || { echo "game not running"; exit 1; }
OUT="$(mktemp -d)"
SECS=$1; shift
for config in "$@"; do
  label=${config%%=*}; cvars=${config#*=}
  printf '%s\n' $cvars | adbs shell "cat > $FILES/live_cvars.txt"
  sleep 4
  adbs logcat -c
  adbs shell top -H -b -n 2 -d "$SECS" -p "$PID" -o TID,%CPU,CMD -s 2 > "$OUT/top.txt"
  adbs logcat -d -s LibertyRecomp:W > "$OUT/log.txt"
  echo "=== $label ($cvars)"
  grep "heartbeat" "$OUT/log.txt" | awk '
    { split($2,t,":"); s=t[1]*3600+t[2]*60+t[3]; f=$NF; sub("frame=","",f);
      if (!n++) {s0=s; f0=f} s1=s; f1=f }
    END { if (s1>s0) printf "fps %.1f over %.1fs\n", (f1-f0)/(s1-s0), s1-s0 }'
  grep "pipeline: 120" "$OUT/log.txt" | sed 's/.*handoff-wait=/handoff-wait=/; s/ types.*//' | tr '\n' ' '; echo
  grep " vk: " "$OUT/log.txt" | sed 's/.*draws=/draws=/; s/ .*//' | tr '\n' ' '; echo
  grep "thermal:" "$OUT/log.txt" | tail -1 | sed 's/.*android-status/status/; s/ yields.*//'
  grep "performance-hint: core" "$OUT/log.txt" | tail -1 | sed 's/.*core:/core:/'
  grep "thread-pinning" "$OUT/log.txt" | tail -2 | sed 's/.*thread-pinning/thread-pinning/'
  awk '/TID/{n++} n==2' "$OUT/top.txt" | sed -n '2,9p' | awk '{printf "%s %s %s | ", $2, $3, $4} END {print ""}'
done
adbs shell "rm -f $FILES/live_cvars.txt"
