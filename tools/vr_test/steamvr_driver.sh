#!/usr/bin/env bash
# SteamVR on Linux without a headset: builds the halotest driver
# (steamvr_driver/driver_halotest.cpp) and runs a command with SteamVR using it,
# in a configuration folder of its own, then stops SteamVR. Refer to README.md.
#   steamvr_driver.sh <folder> [--capture <dir> <every>] <command...>
# <folder> holds the configuration (never the user's: SteamVR's settings,
# its paths and its logs, logs/vrserver.txt with the driver's), made if need
# be. --capture writes each layer of every <every>th frame to <dir>. The
# command runs with XR_RUNTIME_JSON naming SteamVR's runtime, and HOME and
# XDG_CONFIG_HOME the folder's. SteamVR: STEAMVR (default
# ~/.local/share/Steam/steamapps/common/SteamVR).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
folder=$(mkdir -p "$1" && cd "$1" && pwd); shift
capture_dir= capture_every=0
if [ "${1:-}" = --capture ]; then
	capture_dir=$(mkdir -p "$2" && cd "$2" && pwd); capture_every=$3; shift 3
fi
steamvr=${STEAMVR:-$HOME/.local/share/Steam/steamapps/common/SteamVR}
[ -f "$steamvr/steamxr_linux64.json" ] || { echo "no SteamVR at $steamvr (STEAMVR)"; exit 1; }
if pgrep -x vrserver > /dev/null; then
	echo "SteamVR is running: stop it first (this stops SteamVR when the command ends)"; exit 1
fi

# ---------- the driver, with OpenVR's driver header and Vulkan's headers
# (downloaded once, checked against their SHA-256)
build=$repo/build/steamvr_driver
third_party=$build/third_party
openvr_tag=v2.15.6
openvr_sha256=1036efe998d63e82d1d3db2b32a2f58df4a8eeaf5280f50aaf28220ff60a40ab
vulkan_tag=v1.4.309
vulkan_sha256=437925ada160d86ed763d29dcb9318c1bb0d024d7deaf77bc7c170b8eb6b6f10
mkdir -p "$third_party"
if [ ! -f "$third_party/openvr/openvr_driver.h" ]; then
	mkdir -p "$third_party/openvr"
	curl -fsSL -o "$third_party/openvr/openvr_driver.h.part" \
		"https://raw.githubusercontent.com/ValveSoftware/openvr/$openvr_tag/headers/openvr_driver.h"
	echo "$openvr_sha256  $third_party/openvr/openvr_driver.h.part" | sha256sum -c --quiet
	mv "$third_party/openvr/openvr_driver.h.part" "$third_party/openvr/openvr_driver.h"
fi
if [ ! -f "$third_party/vulkan/include/vulkan/vulkan.h" ]; then
	curl -fsSL -o "$third_party/vulkan.tar.gz" \
		"https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/$vulkan_tag.tar.gz"
	echo "$vulkan_sha256  $third_party/vulkan.tar.gz" | sha256sum -c --quiet
	rm -rf "$third_party/vulkan"; mkdir -p "$third_party/vulkan"
	tar xzf "$third_party/vulkan.tar.gz" -C "$third_party/vulkan" --strip-components=1
	rm "$third_party/vulkan.tar.gz"
fi
driver=$build/halotest
mkdir -p "$driver/bin/linux64"
cp "$here/steamvr_driver/driver.vrdrivermanifest" "$driver/"
if [ ! -f "$driver/bin/linux64/driver_halotest.so" ] ||
	[ "$here/steamvr_driver/driver_halotest.cpp" -nt "$driver/bin/linux64/driver_halotest.so" ]; then
	${CXX:-g++} -std=c++17 -O2 -Wall -shared -fPIC -fvisibility=hidden -I "$third_party/openvr" \
		-I "$third_party/vulkan/include" "$here/steamvr_driver/driver_halotest.cpp" \
		-o "$driver/bin/linux64/driver_halotest.so" -l:libvulkan.so.1
fi

# ---------- the configuration: SteamVR's paths (openvrpaths.vrpath, in
# XDG_CONFIG_HOME/openvr) naming the folder's settings and logs and the
# driver; the settings forcing the driver; and a HOME whose ~/.steam has
# what SteamVR's launchers look for there
mkdir -p "$folder/xdg/openvr" "$folder/config" "$folder/logs" "$folder/home/.steam"
cat > "$folder/xdg/openvr/openvrpaths.vrpath" <<JSON
{ "config": ["$folder/config"], "external_drivers": ["$driver"], "jsonid": "vrpathreg", "log": ["$folder/logs"], "runtime": ["$steamvr"], "version": 1 }
JSON
cat > "$folder/config/steamvr.vrsettings" <<'JSON'
{
   "driver_null" : { "enable" : false },
   "driver_vrlink" : { "enable" : false },
   "driver_lighthouse" : { "enable" : false },
   "driver_halotest" : { "enable" : true },
   "steamvr" : { "activateMultipleDrivers" : false, "enableHomeApp" : false, "forcedDriver" : "halotest", "requireHmd" : true }
}
JSON
for link in root sdk32 sdk64 bin32 bin64; do
	[ -e "$HOME/.steam/$link" ] && ln -sfn "$(readlink -f "$HOME/.steam/$link")" "$folder/home/.steam/$link"
done

# ---------- the command, then SteamVR stopped
status=0
env HOME="$folder/home" XDG_CONFIG_HOME="$folder/xdg" XR_RUNTIME_JSON="$steamvr/steamxr_linux64.json" \
	STEAMVR_DISABLE_CRASH_REPORTING=1 HALOTEST_CAPTURE_DIRECTORY="$capture_dir" HALOTEST_CAPTURE_EVERY="$capture_every" \
	"$@" || status=$?
pkill -f "$steamvr/bin/" 2> /dev/null || true
pkill -x vrwebhelper 2> /dev/null || true
sleep 3
pkill -9 -f "$steamvr/bin/" 2> /dev/null || true
pkill -9 -x vrwebhelper 2> /dev/null || true
exit $status
