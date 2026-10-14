#!/usr/bin/env bash
# SteamVR on Linux without a headset: builds the halotest driver
# (steamvr_driver/driver_halotest.cpp) and runs a command with SteamVR using it,
# with a configuration folder of its own, then stops SteamVR. Refer to README.md.
#   steamvr_driver.sh <folder> [--capture <dir> <every>] <command...>
#   steamvr_driver.sh --build        (only builds the driver; no SteamVR needed)
# <folder> holds SteamVR's configuration for the run (its settings, its paths
# and its logs, logs/vrserver.txt with the driver's), made if need be; the
# user's SteamVR settings are not read or changed, but SteamVR's launchers
# run from the user's Steam folder (linked in <folder>/home/.steam), where
# they may write logs and caches. --capture writes each layer of every
# <every>th frame to <dir>. The command runs with XR_RUNTIME_JSON naming
# SteamVR's runtime, and HOME and XDG_CONFIG_HOME the folder's. SteamVR:
# STEAMVR (default ~/.local/share/Steam/steamapps/common/SteamVR). The
# driver's build: HALOTEST_BUILD (default build/steamvr_driver).
#
# However the script ends (the command's end, Ctrl-C, SIGTERM, an error),
# it stops the command and the SteamVR processes started for it: those whose
# HOME is the folder's.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
build_only=
if [ "${1:-}" = --build ]; then
	build_only=1
else
	[ $# -ge 2 ] || { echo "usage: $0 <folder> [--capture <dir> <every>] <command...> | --build"; exit 2; }
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
fi

# ---------- the driver, with OpenVR's driver header and Vulkan's headers:
# each file downloaded from a pinned commit (not a tag, which can move, nor
# a generated archive, whose bytes can change) and checked against its
# SHA-256 every run, so that a file a download or a copy left partial is
# fetched again
build=${HALOTEST_BUILD:-$repo/build/steamvr_driver}
third_party=$build/third_party
openvr_commit=0924064316de3effbcd1acf1e309182a2deb1c05   # v2.15.6
vulkan_commit=952f776f6573aafbb62ea717d871cd1d6816c387   # v1.4.309
headers="
openvr/openvr_driver.h 1036efe998d63e82d1d3db2b32a2f58df4a8eeaf5280f50aaf28220ff60a40ab https://raw.githubusercontent.com/ValveSoftware/openvr/$openvr_commit/headers/openvr_driver.h
vulkan/include/vulkan/vulkan.h c3c0635de74ae3dd38268d6a4cb767b36e07135e68fa6f64b777e2d4d2db7d1a https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/$vulkan_commit/include/vulkan/vulkan.h
vulkan/include/vulkan/vulkan_core.h 39f358bb99f7be5206524d119de4a5b5ab5c515c329c2857ff395d37e6387b4c https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/$vulkan_commit/include/vulkan/vulkan_core.h
vulkan/include/vulkan/vk_platform.h 2fa47dd60cd94eb6fd8d7615953878f5664eb57fdff3b1b9b856c1d8f1496737 https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/$vulkan_commit/include/vulkan/vk_platform.h
vulkan/include/vk_video/vulkan_video_codecs_common.h 7afc98e84d3bea72e40a65161463abbd4be2ff1e1853eca3f8c331afb7329a8b https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/$vulkan_commit/include/vk_video/vulkan_video_codecs_common.h
vulkan/include/vk_video/vulkan_video_codec_h264std.h 05859b58e094a8dcc88cb3766a1fcbfd62bcaab20c93d2c3e5cec853628f277c https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/$vulkan_commit/include/vk_video/vulkan_video_codec_h264std.h
vulkan/include/vk_video/vulkan_video_codec_h264std_decode.h d2be01f84f773644a24b7634f49e63ff8c6f0e807119b1bf609176585031f93a https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/$vulkan_commit/include/vk_video/vulkan_video_codec_h264std_decode.h
vulkan/include/vk_video/vulkan_video_codec_h264std_encode.h c68a1fa4ebadd1b50992a61fb93310a6686d5ea932b91b850d3ad9bc8dec488e https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/$vulkan_commit/include/vk_video/vulkan_video_codec_h264std_encode.h
vulkan/include/vk_video/vulkan_video_codec_h265std.h a5d0b2e7b47843db610d2769c7b0202c90cd7db82108b60fb2631d1c7976a395 https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/$vulkan_commit/include/vk_video/vulkan_video_codec_h265std.h
vulkan/include/vk_video/vulkan_video_codec_h265std_decode.h 944646ced5bf9f49b79144c18369f0b932900c375fd81c262a3c30e9c60de8c6 https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/$vulkan_commit/include/vk_video/vulkan_video_codec_h265std_decode.h
vulkan/include/vk_video/vulkan_video_codec_h265std_encode.h aa8c75beac0962abf752a5ec741a42b1a108ee28fb6b3abcdc995e57e20c6c14 https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/$vulkan_commit/include/vk_video/vulkan_video_codec_h265std_encode.h
vulkan/include/vk_video/vulkan_video_codec_av1std.h 76c408eb8dce6e1f068594eb3239e75a05e3395f2ce10fef512599bd916fe879 https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/$vulkan_commit/include/vk_video/vulkan_video_codec_av1std.h
vulkan/include/vk_video/vulkan_video_codec_av1std_decode.h 2b912389d39d6627ee8af01e052e9557d213b27c0b79a0e25f5366cfe1942f1a https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/$vulkan_commit/include/vk_video/vulkan_video_codec_av1std_decode.h
vulkan/include/vk_video/vulkan_video_codec_av1std_encode.h 93ad363de123988cd7b27acc7930f7f93497bc2e5d74f8420ee8ba18e8746e0a https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/$vulkan_commit/include/vk_video/vulkan_video_codec_av1std_encode.h
"
fetch_headers() {
	local name sha256 url file
	while read -r name sha256 url; do
		[ -n "$name" ] || continue
		file=$third_party/$name
		if [ -f "$file" ] && echo "$sha256  $file" | sha256sum -c --quiet --status 2> /dev/null; then
			continue
		fi
		mkdir -p "$(dirname "$file")"
		curl -fsSL -o "$file.part" "$url"
		if ! echo "$sha256  $file.part" | sha256sum -c --quiet --status; then
			rm -f "$file.part"
			echo "steamvr_driver.sh: $name downloaded is not the one pinned (SHA-256)"; return 1
		fi
		mv "$file.part" "$file"
	done <<< "$headers"
}
fetch_headers

# (built again when the source, the headers' checksums or the compiler
# change, not only when the source is newer)
driver=$build/halotest
library=$driver/bin/linux64/driver_halotest.so
compile=(${CXX:-g++} -std=c++17 -O2 -Wall -shared -fPIC -fvisibility=hidden -I "$third_party/openvr"
	-I "$third_party/vulkan/include" "$here/steamvr_driver/driver_halotest.cpp" -o "$library" -l:libvulkan.so.1)
key=$( { sha256sum < "$here/steamvr_driver/driver_halotest.cpp"; echo "$headers"; echo "${compile[*]}"; } | sha256sum | cut -d' ' -f1)
mkdir -p "$driver/bin/linux64"
cp "$here/steamvr_driver/driver.vrdrivermanifest" "$driver/"
if [ ! -f "$library" ] || [ "$(cat "$library.key" 2> /dev/null)" != "$key" ]; then
	rm -f "$library.key"
	"${compile[@]}"
	echo "$key" > "$library.key"
fi
if [ -n "$build_only" ]; then
	echo "$library"; exit 0
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
	if [ -e "$HOME/.steam/$link" ]; then
		ln -sfn "$(readlink -f "$HOME/.steam/$link")" "$folder/home/.steam/$link"
	fi
done

# ---------- the command, then SteamVR stopped

# the processes SteamVR started for this run: SteamVR's, by their path or
# their name, whose environment has the folder's HOME (never the user's own
# SteamVR, nor another run's)
home="$folder/home"
steamvr_processes() {
	local pid
	for pid in $( { pgrep -f -- "$(printf '%s' "$steamvr/" | sed 's/[][\.*^$+?(){}|]/\\&/g')" || true
		pgrep -x 'vrserver|vrcompositor|vrmonitor|vrwebhelper|vrdashboard|vrstartup|vrstartup.sh|vrpathreg' || true
		} | sort -un); do
		[ "$pid" != $$ ] || continue
		if grep -qxF "HOME=$home" < <(tr '\0' '\n' < "/proc/$pid/environ" 2> /dev/null); then
			echo "$pid"
		fi
	done
	return 0
}
command_pid=
cleanup() {
	local status=$? pids
	trap - EXIT INT TERM HUP
	if [ -n "$command_pid" ] && kill -0 "$command_pid" 2> /dev/null; then
		kill -TERM "$command_pid" 2> /dev/null || true
		sleep 2
		kill -KILL "$command_pid" 2> /dev/null || true
	fi
	pids=$(steamvr_processes)
	if [ -n "$pids" ]; then
		kill -TERM $pids 2> /dev/null || true
		sleep 3
		pids=$(steamvr_processes)
		[ -z "$pids" ] || kill -KILL $pids 2> /dev/null || true
	fi
	exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

status=0
env HOME="$home" XDG_CONFIG_HOME="$folder/xdg" XR_RUNTIME_JSON="$steamvr/steamxr_linux64.json" \
	STEAMVR_DISABLE_CRASH_REPORTING=1 HALOTEST_CAPTURE_DIRECTORY="$capture_dir" HALOTEST_CAPTURE_EVERY="$capture_every" \
	"$@" &
command_pid=$!
wait "$command_pid" || status=$?
command_pid=
exit $status
