#!/bin/sh
# Starts the VR build on a Steam Frame: the target of its Steam shortcut
# (a non-Steam game), installed beside the game in ~/apps/halo-vr by the
# package's install.sh (tools/linux_install.sh), which replaces it at each
# update. Refer to
# README.md beside it, and to "Steam Frame" in port/linux/README.md.
#
# The environment, as Steam gives it to the game:
#
# - The game draws with Vulkan (Mesa's Turnip driver for the Frame's
#   Adreno), which Steam's OpenGL caps (MESA_GL_VERSION_OVERRIDE) do not
#   touch.
# - XR_RUNTIME_JSON: an OpenXR runtime's manifest, to use instead of the
#   active one ($XDG_CONFIG_HOME/openxr/1/active_runtime.json, SteamVR's
#   on the Frame). Unset: the active runtime.
# - HALO_*: the game's settings (port/linux/README.md, "Settings"), over
#   config.toml's. Put them in Steam's launch options for the shortcut
#   (Properties > Launch Options: HALO_VR=0 %command%, say), or in
#   halo-vr.env beside this script, which updates never touch.
#
# The game's data is the folder that contains maps/: here (the script's
# folder, the current folder of the game), unless config.toml's paths.data
# names another. Started from Steam (no terminal), the game's output (its
# log, and a crash's backtrace) goes to halo-vr.log here, the previous
# run's to halo-vr.log.prev; the game's own log is debug.txt in the data
# folder.
#
# The game updates itself through this script (port/linux/src/updater.c,
# "Updates" in port/linux/README.md): started by it (HALO_FRAME_LAUNCHER), the
# game downloads a newer release's package into update.partial/ while it
# plays, asks at the main menu, and, on UPDATE, writes update-ready and
# quits. This script then installs that package with its own install.sh
# (tools/linux_install.sh: its SHA256SUMS checked, each file put in place
# by a rename, the player's files untouched) and starts the new game
# (HALO_FRAME_UPDATED: it does not look again then). A download the player did
# not take is deleted at the next start.
set -eu
cd "$(dirname "$(readlink -f "$0")")"
if [ -f halo-vr.env ]; then
	set -a
	. ./halo-vr.env
	set +a
fi
if [ ! -t 2 ]; then
	[ -f halo-vr.log ] && mv -f halo-vr.log halo-vr.log.prev
	exec >halo-vr.log 2>&1
fi
HALO_FRAME_LAUNCHER=$(pwd)/halo-vr.sh
export HALO_FRAME_LAUNCHER
[ -f update-ready ] || rm -rf update.partial
status=0
./halo "$@" || status=$?
if [ -f update-ready ]; then
	rm -f update-ready
	echo "halo-vr.sh: installing the update"
	if [ -f update.partial/halo-vr-frame/install.sh ] &&
		bash update.partial/halo-vr-frame/install.sh --package "$(pwd)/update.partial/halo-vr-frame" \
			--into "$(pwd)" --batch; then
		echo "halo-vr.sh: updated; starting the new version"
	else
		echo "halo-vr.sh: the update was not installed; starting the game as it was"
	fi
	rm -rf update.partial
	HALO_FRAME_UPDATED=1 exec ./halo-vr.sh "$@"
fi
exit "$status"
