#!/bin/bash
# Profile-guided optimization, in three steps:
#   1. BUILD_DIR=$REPO/out/build/android-arm64-pgogen PGO=generate STRIP=1 build_native.sh
#      then build_apk.sh and install.sh: the recording build. Play for a few
#      minutes; it writes its counters to files/pgo every 60 seconds.
#   2. pgo.sh              pulls the counters and merges them into out/pgo/liberty.profdata
#   3. BUILD_DIR=$REPO/out/build/android-arm64-pgo PGO=$REPO/out/pgo/liberty.profdata STRIP=1 build_native.sh
#      then build_apk.sh and install.sh: the optimized build.
# pgo.sh --clear removes the counters on the device (before a new recording).
set -euo pipefail
. "$(dirname "$0")/env.sh"
DEVICE_DIR=files/pgo
if [ "${1:-}" = "--clear" ]; then
  adbs shell "run-as $PKG sh -c 'rm -f $DEVICE_DIR/*.profraw'"
  exit 0
fi
OUT="$REPO/out/pgo"
mkdir -p "$OUT/raw"
rm -f "$OUT"/raw/*.profraw
FILES_ON_DEVICE=$(adbs shell "run-as $PKG ls $DEVICE_DIR" | tr -d '\r' | grep '\.profraw$' || true)
[ -n "$FILES_ON_DEVICE" ] || { echo "no .profraw in $DEVICE_DIR: is the recording build installed, and did it run for a minute?"; exit 1; }
for name in $FILES_ON_DEVICE; do
  adbs exec-out "run-as $PKG cat $DEVICE_DIR/$name" > "$OUT/raw/$name"
  echo "$name $(stat -c %s "$OUT/raw/$name") bytes"
done
"$NDK_BIN/llvm-profdata" merge -o "$OUT/liberty.profdata" "$OUT"/raw/*.profraw
ls -la "$OUT/liberty.profdata"
