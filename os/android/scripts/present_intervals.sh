#!/bin/bash
# What reaches the screen: the intervals between presents of the game's layer,
# from SurfaceFlinger.
#   present_intervals.sh [seconds]   (default 4; SurfaceFlinger keeps 128 frames)
# This counts host presents, not new game frames: the presenter repaints at
# the frame limit and repeats the last image when the game is slower. Compare
# with the "fps" of ab_cvars.sh or the "pipeline: 120 frames" lines.
set -euo pipefail
. "$(dirname "$0")/env.sh"
SECS="${1:-4}"
OUT="$(mktemp -d)"
adbs shell dumpsys SurfaceFlinger --list | tr -d '\r' | grep -i "$PKG" > "$OUT/layers.txt" || true
# Android 16 lists "RequestedLayerState{<id> SurfaceView[...](BLAST)#<n> parentId=...}".
LAYER=$(sed -n "s/^RequestedLayerState{\(.*SurfaceView\[$PKG[^ ]*(BLAST)#[0-9]*\).*/\1/p" "$OUT/layers.txt" | head -1)
[ -n "$LAYER" ] || LAYER=$(grep -E "^SurfaceView\[$PKG/.*\(BLAST\)" "$OUT/layers.txt" | head -1 || true)
[ -n "$LAYER" ] || { echo "no game layer; candidates:"; cat "$OUT/layers.txt"; exit 1; }
adbs shell "dumpsys SurfaceFlinger --latency-clear '$LAYER'" >/dev/null
sleep "$SECS"
adbs shell "dumpsys SurfaceFlinger --latency '$LAYER'" | tr -d '\r' > "$OUT/sf.txt"
head -1 "$OUT/sf.txt" | awk '{printf "refresh period %.2f ms\n", $1/1e6}'
awk 'NR>1 && NF>=3 && $2>0 && $2<9e18 { if (p) { d=($2-p)/1e6; n++; s+=d; b=int(d/4.1667+0.5); h[b]++; if (d>mx) mx=d } p=$2 }
  END { if (!n) { print "no frames"; exit }
        printf "%d presents, mean %.1f ms (%.1f per second), max %.1f ms\n", n, s/n, 1000*n/s, mx
        print "interval histogram (ms: presents):"
        for (i=1;i<=60;i++) if (h[i]) printf "  %5.1f: %d\n", i*4.1667, h[i] }' "$OUT/sf.txt"
