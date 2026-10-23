#!/usr/bin/env bash
# Installs, or updates, the VR build on a Steam Frame from a computer with
# ssh to it: the package of tools/frame_package.py, into ~/apps/halo-vr/.
# Refer to "Steam Frame" in port/linux/README.md.
#
#   tools/frame_install.sh [--package <folder, .tar.gz or .zip>] <host>
#   tools/frame_install.sh [--package <folder, .tar.gz or .zip>] --local <folder>
#
# <host>: as ssh takes it (deck@steamframe.local, or a Host of ~/.ssh/config).
# --package: by default dist/halo-vr-frame of this repository; or a release's
#   OpenH1-Steam-Frame.zip.
# --local: install into <folder>/apps/halo-vr instead, <folder> standing in
#   for the Frame's home (to test this script).
#
# It copies the package, and this repository's installer
# (tools/linux_install.sh, which every package also has as its install.sh:
# the one installer of the Linux release and the Frame), to a folder of its
# own in the Frame's /tmp, and runs the installer there (--batch: no
# questions, and no steamos-add-to-steam, which wants the Frame's own
# session); then deletes the folder. The installer checks the package's
# SHA256SUMS, refuses while the game runs (pgrep -x halo), never starts it,
# puts each file in place at once (a rename), keeps the replaced halo,
# libSDL3.so.0 and halo-vr.sh as <name>.prev, never touches config.toml,
# halo-vr.env, the maps, the logs or the saved games, writes the menu entry
# OpenH1 VR, gives Steam's shortcut its library artwork once Steam has one,
# and says how to add the shortcut until it has.
set -euo pipefail

usage() {
	sed -n '6,13p' "$0" | sed 's/^# \{0,1\}//' >&2
	exit 2
}

repo=$(cd "$(dirname "$0")/.." && pwd)
installer=$repo/tools/linux_install.sh
package=$repo/dist/halo-vr-frame
host=
local_home=
while [ $# -gt 0 ]; do
	case $1 in
	--package) [ $# -ge 2 ] || usage; package=$2; shift 2 ;;
	--local) [ $# -ge 2 ] || usage; local_home=$(mkdir -p "$2" && cd "$2" && pwd); shift 2 ;;
	-h|--help) usage ;;
	-*) usage ;;
	*) [ -z "$host" ] || usage; host=$1; shift ;;
	esac
done
[ -n "$host" ] || [ -n "$local_home" ] || usage
[ -z "$host" ] || [ -z "$local_home" ] || usage

# (short: ssh limits its control socket's path to about 100 characters)
work=$(mktemp -d /tmp/halo-vr-install.XXXXXX)
remote=
on_frame() {
	ssh -o ControlMaster=auto -o ControlPath="$work/ssh" -o ControlPersist=60 -- "$host" "$1"
}
cleanup() {
	if [ -n "$host" ]; then
		[ -z "$remote" ] || on_frame "rm -rf '$remote'" 2>/dev/null || true
		ssh -o ControlPath="$work/ssh" -O exit "$host" 2>/dev/null || true
	fi
	rm -rf "$work"
}
trap cleanup EXIT

if [ -f "$package" ]; then
	mkdir "$work/package"
	case $package in
	*.zip)
		# (unzip keeps the modes the zip has; Python's zipfile, where there
		# is no unzip, does not: the installer sets them)
		if command -v unzip >/dev/null; then
			unzip -q "$package" -d "$work/package"
		else
			python3 -m zipfile -e "$package" "$work/package"
		fi ;;
	*) tar -xmzf "$package" -C "$work/package" ;;
	esac
	sums=$(find "$work/package" -maxdepth 2 -name SHA256SUMS | head -n 1)
	[ -n "$sums" ] || { echo "$package: not a package of tools/frame_package.py (no SHA256SUMS)" >&2; exit 1; }
	package=$(dirname "$sums")
fi
if [ ! -f "$package/SHA256SUMS" ] || [ ! -f "$package/halo-vr.sh" ]; then
	echo "$package: no package (no SHA256SUMS and halo-vr.sh); make one: python3 tools/frame_package.py" >&2
	exit 1
fi
package=$(cd "$package" && pwd)

if [ -n "$local_home" ]; then
	HOME=$local_home bash "$installer" --package "$package" --batch
	exit
fi
echo "Copying the package to $host"
remote=$(on_frame 'mktemp -d /tmp/openh1-install.XXXXXX')
case $remote in /tmp/openh1-install.*) ;; *) echo "$host: no folder made in /tmp" >&2; remote=; exit 1 ;; esac
tar -C "$(dirname "$package")" -cf - "$(basename "$package")" | on_frame "tar -xmf - -C '$remote'"
on_frame "cat > '$remote/install.sh'" < "$installer"
on_frame "bash '$remote/install.sh' --package '$remote/$(basename "$package")' --batch"
