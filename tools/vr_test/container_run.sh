#!/usr/bin/env bash
# Inside halo-xrtest-monado (with --gpus all on an NVIDIA host): the VR build
# on Monado's simulated headset, the null compositor and the game on the GPU,
# no display. vr_run.sh starts it; refer to README.md.
#   halo-vr-run <slot> <map> <seconds> [HALO_*=value...]
# /game: the folder with halo and libSDL3.so.0 (HALO_BIN names another
# executable there); /maps: the game's maps; /out: the logs, and config.toml
# and the saved games after the run. <map>: "b30" (a campaign level), a map
# path ("levels\b30\b30"), or "menu" (the main menu, with /out/save and
# /out/config.in.toml to start from, if there).
set -u
slot=$1; map=$2; secs=$3; shift 3
export HOME=/tmp XDG_RUNTIME_DIR=/tmp/xdg-xr$slot
mkdir -p $XDG_RUNTIME_DIR && chmod 700 $XDG_RUNTIME_DIR
export XRT_COMPOSITOR_NULL=1 SIMULATED_ENABLE=1 \
	XRT_COMPOSITOR_NULL_FPS=${XRT_COMPOSITOR_NULL_FPS:-72} XRT_COMPOSITOR_NULL_VIEW_SIZE=${XRT_COMPOSITOR_NULL_VIEW_SIZE:-1728}
(sleep 100000 | /usr/local/bin/monado-service > /out/monado.log 2>&1) &
sleep 3
run=/tmp/run$slot; rm -rf $run; mkdir -p $run; cd $run
ln -s /maps maps; cp /game/${HALO_BIN:-halo} /game/libSDL3.so.0 .
mkdir -p /tmp/.local/share
if [ "$map" = menu ]; then
	[ -d /out/save ] && cp -a /out/save /tmp/.local/share/halo-linux
else
	case $map in *\\*) ;; *) map="levels\\$map\\$map" ;; esac
	printf 'map_name %s\n' "$map" > init.txt
fi
[ -f /out/config.in.toml ] && cp /out/config.in.toml config.toml
env XR_RUNTIME_JSON=/usr/local/share/openxr/1/openxr_monado.json SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy HALO_NO_AUDIO=1 \
	HALO_UPDATE_ANSWER=no HALO_VR_FORCE_RENDER=1 HALO_EXIT_AFTER=$secs "$@" HALO_NET_ONLINE=false ./${HALO_BIN:-halo} > /out/game.log 2>&1
status=$?
cp config.toml debug.txt /out/ 2>/dev/null
[ -d /tmp/.local/share/halo-linux ] && { rm -rf /out/save; cp -a /tmp/.local/share/halo-linux /out/save; }
if [ -f debug.txt ] && grep -n 'EXCEPTION' debug.txt >&2; then
	echo "VR run failed: EXCEPTION in debug.txt" >&2
	[ "$status" -ne 0 ] || status=1
fi
echo "exit $status"
exit "$status"
