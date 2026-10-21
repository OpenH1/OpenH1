#!/bin/bash
# capture.sh <run-name> <original|openh1> <scenario> <script> <seconds> [extra init lines]
#
# One side of a comparison, offscreen, a BMP a frame: the Original (the Classic
# preset, one frame a tick, the Xbox's 640x480) or OpenH1 (Enhanced, frames between
# ticks, at SIZE). Settings: TREE (a built linux_x86_64 tree), MAPS, RUNS, SIZE,
# PORT (the console's, distinct for runs side by side), CONSOLE (commands sent with
# the HUD's, as "(cheat_all_weapons) "), FT (OpenH1's frame time),
# SEED, TRACE_SECONDS (write the sim trace to data/trace.txt and quit then).
set -u
TREE=${TREE:-$HOME/halo-capture} MAPS=${MAPS:-$HOME/halo-x86-maps/maps}
RUNS=${RUNS:-$HOME/halo-capture-runs} SIZE=${SIZE:-1280x960}
name=$1 side=$2 scenario=$3 script=$4 seconds=$5 extra=${6:-}
root=$RUNS/$name
rm -rf "$root"; mkdir -p "$root/data" "$root/saves" "$root/frames"
ln -s "$MAPS" "$root/data/maps"
# (NETTEST: a debug.network_test session, "local:<map>:<variant>" for a local
# game with bots, instead of the scenario loaded directly; its HUD is hidden
# once the network test logs the game, HUD_AFTER)
if [ -n "${NETTEST:-}" ]; then
  printf '%s' "$extra" > "$root/data/init.txt"; export HALO_NETWORK_TEST=$NETTEST
  HUD_AFTER=${HUD_AFTER:-network test: tick}
else
  printf 'map_name %s\n%s' "$scenario" "$extra" > "$root/data/init.txt"
fi
export HALO_DATA_ROOT=$root/data HALO_SAVE_ROOT=$root/saves HALO_NET_ONLINE=false \
  HALO_DISPLAY_MODE=windowed HALO_NO_AUDIO=1 HALO_UPDATE_ANSWER=no HALO_NO_CLIPBOARD=1 \
  HALO_EXIT_AFTER=$seconds HALO_SCREENSHOT_DIR=$root/frames HALO_SCREENSHOT_EVERY=1 \
  HALO_TEST_INPUT="script:$script" HALO_HDR=off SDL_VIDEO_DRIVER=offscreen \
  HALO_TELNET_CONSOLE=true HALO_TELNET_CONSOLE_PORT=${PORT:-2399}
if [ "$side" = original ]; then
  export HALO_GRAPHICS=classic HALO_INTERPOLATION=false HALO_RESOLUTION_SCALING=original \
    HALO_WINDOW_SIZE=$SIZE HALO_FRAME_TIME=0.0333333
else
  export HALO_GRAPHICS=enhanced HALO_INTERPOLATION=true HALO_RESOLUTION_SCALING=native \
    HALO_WINDOW_SIZE=$SIZE HALO_FRAME_TIME=${FT:-0.0166667}
fi
[ -n "${SEED:-}" ] && export HALO_RANDOM_SEED=$SEED
# (a relative path, which lands in the data root: an absolute one cannot be opened)
[ -n "${TRACE_SECONDS:-}" ] && export HALO_TRACE=trace.txt HALO_TRACE_SECONDS=$TRACE_SECONDS
cd "$TREE"
start=$SECONDS
timeout $((${seconds%.*} + 60)) ./build/linux_x86_64/halo > "$root/log.txt" 2>&1 &
game=$!
# hide the HUD once the map is drawing (a command sent while it loads can stop the game)
( for i in $(seq 600); do grep -q "${HUD_AFTER:-its shaders and pipelines made}" "$root/log.txt" 2>/dev/null && break; sleep 0.1; done
  sleep 0.5
  exec 3<>/dev/tcp/127.0.0.1/$HALO_TELNET_CONSOLE_PORT && printf '(begin (show_hud 0) %s(sleep 1))\r\n' "${CONSOLE:-}" >&3 && sleep 1 && exec 3>&-
  echo "hud hidden at frame $(ls $root/frames | wc -l)" ) &
wait $game
echo "exit=$? took=$((SECONDS - start))s frames=$(ls $root/frames | wc -l)"
wait
