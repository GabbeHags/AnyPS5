#!/bin/bash
# Usage: WORK=<scratch dir> CAPTURE=<capture dir> skipscan.sh <hex program address>...
# Replays the capture once per address with APS5_SKIP_SHADER set and prints the average colour of the second replayed frame.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
WORK=${WORK:?set WORK to a scratch directory}
CAPTURE=${CAPTURE:?set CAPTURE to the frame capture directory}
export PATH="$REPO/build/core/libs/libs/unpatched:$PATH" ANYPS5_SHADER_CACHE_DIR="$WORK/cache"
for a in "$@"; do
  O=$WORK/skip/$a; rm -rf "$O"; mkdir -p "$O"
  APS5_SKIP_SHADER=$a timeout 900 "$REPO/build/core/libs/prx/libSceAgcDriver/agc_frame_replay.exe" "$CAPTURE" --frames "$O" > "$O/replay.log" 2>&1
  code=$?
  if [ -f "$O/replay-1.bmp" ]; then r=$(python -I "$HERE/bmp.py" "$O/replay-1.bmp" | sed 's/.*avg rgb //'); else r="no frame"; fi
  echo "$a exit=$code rgb=$r"
done
