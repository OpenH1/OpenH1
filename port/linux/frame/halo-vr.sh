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
exec ./halo "$@"
