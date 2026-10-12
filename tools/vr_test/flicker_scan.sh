#!/usr/bin/env bash
# Flicker scan: each level in a run of its own at once (vr_run.sh), its
# screenshots written to /dev/shm each frame and scanned as they come
# (flickscan.py: pixels that change and change back over three frames).
# Refer to README.md.
#   flicker_scan.sh <out dir> <seconds> <jitter mm> <level>...
# Environment: as vr_run.sh's, and EXTRA (more HALO_*=value settings) and
# PYTHON (one with numpy; default python3).
set -u
out=$1; secs=$2; jitter=$3; shift 3
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"; out=$(cd "$out" && pwd)
slot=${SLOT:-$(($$ % 1000))}
for level in "$@"; do
	slot=$((slot + 1))
	shots=/dev/shm/halo-shots-$$-$slot; rm -rf $shots; mkdir -p $shots/shots
	(
		SLOT=$slot "$here/vr_run.sh" $shots $level $secs HALO_VR_TEST_JITTER=$jitter HALO_TEST_INPUT=look:1 \
			HALO_VR_RESOLUTION_SCALE=0.4 HALO_SCREENSHOT_DIR=/out/shots HALO_SCREENSHOT_EVERY=1 ${EXTRA:-} > /dev/null 2>&1
		cp $shots/game.log "$out/$level.log"
		touch $shots.done
	) &
	${PYTHON:-python3} "$here/flickscan.py" $shots/shots "$out/$level" $shots.done > "$out/$level.scan" 2>&1 &
done
wait
rm -rf /dev/shm/halo-shots-$$-*
for level in "$@"; do echo "$level: $(tail -1 "$out/$level.scan")"; done
