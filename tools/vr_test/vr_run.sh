#!/usr/bin/env bash
# One headless run of the VR build on a simulated headset (halo-xrtest-monado),
# on an NVIDIA GPU. Refer to README.md.
#   vr_run.sh <out dir> <map> <seconds> [HALO_*=value...]
# <map>: "b30", "levels\b30\b30", or "menu". Environment:
#   HALO_MAPS  the folder with the game's maps (required)
#   HALO_GAME  the folder with halo and libSDL3.so.0 (default: this
#              repository's build/linux_arm64)
#   HALO_BIN   the executable's name there (default: halo)
#   HALO_XRTEST_IMAGE  the image (default: halo-xrtest-monado:latest)
#   SLOT       a number unique among the runs at once (default: from the PID)
#   XRT_COMPOSITOR_NULL_FPS, XRT_COMPOSITOR_NULL_VIEW_SIZE  the headset's
#              refresh rate and eye size (default: 72 and 1728, a Steam Frame's)
# The game's log is <out>/game.log, Monado's <out>/monado.log;
# HALO_SCREENSHOT_DIR=/out/shots puts the screenshots in <out>/shots.
set -eu
out=$1; map=$2; secs=$3; shift 3
repo=$(cd "$(dirname "$0")/../.." && pwd)
game=$(cd "${HALO_GAME:-$repo/build/linux_arm64}" && pwd)
maps=$(cd "${HALO_MAPS:?set HALO_MAPS to the folder with the maps}" && pwd)
mkdir -p "$out/shots"; out=$(cd "$out" && pwd)
mounts=()
# (after a reboot the NVIDIA runtime can lose its EGL and Vulkan files)
for f in /usr/share/glvnd/egl_vendor.d/10_nvidia.json /usr/share/vulkan/icd.d/nvidia_icd.json; do
	[ -f $f ] && mounts+=(-v $f:$f:ro)
done
exec timeout $((secs + 120)) docker run --rm --gpus all -e NVIDIA_DRIVER_CAPABILITIES=all "${mounts[@]}" \
	-e HALO_BIN=${HALO_BIN:-halo} -e XRT_COMPOSITOR_NULL_FPS=${XRT_COMPOSITOR_NULL_FPS:-72} \
	-e XRT_COMPOSITOR_NULL_VIEW_SIZE=${XRT_COMPOSITOR_NULL_VIEW_SIZE:-1728} \
	-u $(id -u):$(id -g) -v "$game":/game:ro -v "$maps":/maps:ro -v "$out":/out \
	${HALO_XRTEST_IMAGE:-halo-xrtest-monado:latest} halo-vr-run ${SLOT:-$(($$ % 1000))} "$map" $secs "$@"
