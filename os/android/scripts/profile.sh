#!/bin/bash
# CPU profile of the running game with call graphs and function names.
#   profile.sh syms              push the unstripped libraries (once per build)
#   profile.sh record [seconds]  record every thread with frame-pointer call graphs
#   profile.sh threads           CPU by thread
#   profile.sh top <thread> [n]  self time by function
#   profile.sh incl <thread> [n] inclusive time by function
#   profile.sh callers <thread> <symbol> [n]
# Thread names: GtaRecorder, SDLThread (command worker and command retirement),
# "Main XThread", and the XThread<address> names "threads" prints.
set -uo pipefail
. "$(dirname "$0")/env.sh"
DATA=/data/local/tmp/perf.data
SYMS=/data/local/tmp/syms
REPORT="simpleperf report -i $DATA --symdir $SYMS"
case "${1:-}" in
  syms)
    adbs shell "mkdir -p $SYMS"
    for lib in "$REX_OUT_DIR/librexgpu-gta4-native.so" "$REX_OUT_DIR/librexruntime.so" \
               "$BUILD_DIR/glue/gta4-recomp/libmain.so"; do
      adbs push "$(winpath "$lib")" "$SYMS/" | tail -1
    done
    ;;
  record)
    PID=$(adbs shell pidof "$GAME_PROC" | tr -d '\r')
    [ -n "$PID" ] || { echo "game not running"; exit 1; }
    adbs shell "rm -f $DATA; simpleperf record --app $PKG -p $PID -e cpu-clock -f 2000 --call-graph fp --duration ${2:-10} -o $DATA" 2>&1 | tail -2
    ;;
  threads)
    adbs shell "$REPORT --sort comm -n" 2>/dev/null | sed -n '/Overhead/,$p' | head -16
    ;;
  top)
    adbs shell "$REPORT --comms '$2' --sort symbol -n" 2>/dev/null | sed -n '/Overhead/,$p' | head -"${3:-40}" | cut -c1-170
    ;;
  incl)
    adbs shell "$REPORT --comms '$2' --children --sort symbol -n" 2>/dev/null | sed -n '/Children/,$p' | head -"${3:-60}" | cut -c1-190
    ;;
  callers)
    adbs shell "$REPORT --comms '$2' --symbols '$3' -g caller --sort symbol" 2>/dev/null | sed -n '/Overhead/,$p' | head -"${4:-60}" | cut -c1-190
    ;;
  *)
    sed -n '2,10p' "$0"
    ;;
esac
