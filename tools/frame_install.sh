#!/usr/bin/env bash
# Installs, or updates, the VR build on a Steam Frame, from a computer with
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
#   for the Frame's home (to test this script, or to install on the Frame
#   from a terminal on it).
#
# It refuses while the game runs (pgrep -x halo), and never starts it. The
# package goes to a folder of its own beside the game, where its SHA256SUMS
# is checked; then each file that differs becomes <name>.new, checked again,
# and, once all are, each takes its name at once (a rename): the game is
# never half copied. The replaced halo, libSDL3.so.0 and halo-vr.sh are
# kept as <name>.prev (only the last). brokers.txt is replaced, as the
# game's updates do. config.toml, halo-vr.env, the maps, the logs and the
# saved games are never touched. It prints what changed, and, until Steam
# has a shortcut to the launcher, how to add one (it does not edit Steam's
# files: Steam rewrites them while it runs, which on the Frame is always).
set -euo pipefail

usage() {
	sed -n '6,14p' "$0" | sed 's/^# \{0,1\}//' >&2
	exit 2
}

repo=$(cd "$(dirname "$0")/.." && pwd)
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
cleanup() {
	[ -z "$host" ] || ssh -o ControlPath="$work/ssh" -O exit "$host" 2>/dev/null || true
	rm -rf "$work"
}
trap cleanup EXIT

if [ -f "$package" ]; then
	mkdir "$work/package"
	case $package in
	*.zip)
		# (unzip keeps the modes the zip has; Python's zipfile, where there
		# is no unzip, does not: the programs' are set below)
		if command -v unzip >/dev/null; then
			unzip -q "$package" -d "$work/package"
		else
			python3 -m zipfile -e "$package" "$work/package"
		fi ;;
	*) tar -xzf "$package" -C "$work/package" ;;
	esac
	sums=$(find "$work/package" -maxdepth 2 -name SHA256SUMS | head -n 1)
	[ -n "$sums" ] || { echo "$package: not a package of tools/frame_package.py (no SHA256SUMS)" >&2; exit 1; }
	package=$(dirname "$sums")
	for name in halo libSDL3.so.0 halo-vr.sh; do
		[ ! -f "$package/$name" ] || chmod 755 "$package/$name"
	done
fi
if [ ! -f "$package/SHA256SUMS" ]; then
	echo "$package: no package (no SHA256SUMS); make one: python3 tools/frame_package.py" >&2
	exit 1
fi
# (only plain names, never the player's files: the Frame's side checks again)
if grep -Evq '^[0-9a-f]{64}  [A-Za-z0-9][A-Za-z0-9._+-]*$' "$package/SHA256SUMS" ||
	cut -c 67- "$package/SHA256SUMS" | grep -Eqx 'config\.toml|halo-vr\.env|maps|debug\.txt|halo-vr\.log.*|.*\.(new|prev)'; then
	echo "$package/SHA256SUMS: a name a package may not have" >&2
	exit 1
fi
names=$(cut -c 67- "$package/SHA256SUMS")

# a command on the Frame (one ssh connection for all of them)
on_frame() {
	if [ -n "$local_home" ]; then
		HOME=$local_home sh -c "$1"
	else
		ssh -o ControlMaster=auto -o ControlPath="$work/ssh" -o ControlPersist=60 -- "$host" "$1"
	fi
}

dir=apps/halo-vr
if [ -n "$host" ]; then where="$host:~/$dir"; else where=$local_home/$dir; fi
echo "Installing $(sed -n 1p "$package/VERSION" 2>/dev/null || echo "a package") to $where"

refuse_running='if pids=$(pgrep -x halo); then echo "Halo is running (process $(echo $pids)): quit it, then install again." >&2; exit 3; fi'
# shellcheck disable=SC2086
tar -C "$package" -cf - SHA256SUMS $names |
	on_frame "set -eu; $refuse_running; mkdir -p \"\$HOME/$dir\"; rm -rf \"\$HOME/$dir/.install\"; mkdir \"\$HOME/$dir/.install\"; tar -xf - -C \"\$HOME/$dir/.install\""

on_frame "sh -s -- '$dir'" <<'FRAME'
set -eu
dir=$HOME/$1
install=$dir/.install
cd "$dir"
trap 'rm -rf "$install"' EXIT
sha() { sha256sum < "$1" | cut -d ' ' -f 1; }
running() {
	if pids=$(pgrep -x halo); then
		echo "Halo is running (process $(echo $pids)): quit it, then install again." >&2
		for name in $(cut -c 67- "$install/SHA256SUMS"); do rm -f "$name.new"; done
		exit 3
	fi
}

running
if ! (cd "$install" && sha256sum -c --quiet SHA256SUMS); then
	echo "The copy differs from the package (its SHA256SUMS): nothing was installed." >&2
	exit 4
fi
first=yes
[ -e halo ] && first=
was=$(sed -n '1p;3p' VERSION 2>/dev/null | paste -s -d ' ' -) || true

# each file that differs, as <name>.new, checked
changed=
while read -r sum name; do
	case $name in
	''|.*|*/*|config.toml|halo-vr.env|maps|debug.txt|halo-vr.log*|*.new|*.prev)
		echo "$name: a name a package may not have: nothing was installed." >&2
		exit 4 ;;
	esac
	[ -f "$name" ] && [ "$(sha "$name")" = "$sum" ] && continue
	mv -f "$install/$name" "$name.new"
	if [ "$(sha "$name.new")" != "$sum" ]; then
		echo "$name.new differs from the package: nothing was installed." >&2
		for other in $changed "$name"; do rm -f "$other.new"; done
		exit 4
	fi
	changed="$changed $name"
done < "$install/SHA256SUMS"

# then each takes its name (the old program files kept as <name>.prev)
running
for name in $changed; do
	if [ -f "$name" ]; then
		note=updated
		case $name in
		halo|libSDL3.so.0|halo-vr.sh)
			ln -f "$name" "$name.prev" 2>/dev/null || cp -p "$name" "$name.prev"
			note="updated, the previous kept as $name.prev" ;;
		esac
	else
		note=new
	fi
	mv -f "$name.new" "$name"
	echo "  $name: $note"
done
# the files an earlier package had and this one has not
if [ -f SHA256SUMS ]; then
	for name in $(cut -c 67- SHA256SUMS); do
		case $name in ''|.*|*/*|config.toml|halo-vr.env|maps|debug.txt|halo-vr.log*|*.new|*.prev) continue ;; esac
		grep -q "  $name\$" "$install/SHA256SUMS" && continue
		rm -f "$name"
		echo "  $name: removed (no longer in the package)"
	done
fi
mv -f "$install/SHA256SUMS" SHA256SUMS
if [ -z "$changed" ]; then
	echo "  Nothing changed: the same package is installed."
else
	[ -n "$first" ] || echo "  Was: ${was:-a build installed by hand}"
	echo "  Now: $(sed -n '1p;3p' VERSION | paste -s -d ' ' -)"
fi
case " $changed " in *" halo-vr.sh "*)
	[ -f halo-vr.sh.prev ] && echo "  (Settings of your own for the launcher go in halo-vr.env, which updates keep: halo-vr.sh.prev is the replaced launcher.)" ;;
esac

# the game's data
if [ ! -d maps ] && ! grep -Eq '^[[:space:]]*data[[:space:]]*=[[:space:]]*"[^"]+"' config.toml 2>/dev/null; then
	echo "No maps here: copy the maps folder of the game's Xbox disc to $dir/maps (or name the folder that contains it in config.toml's paths.data)."
fi

# Steam's shortcut, which only Steam may add (it rewrites its shortcuts.vdf)
if ! grep -lqF "$dir/halo-vr.sh" "$HOME"/.local/share/Steam/userdata/*/config/shortcuts.vdf 2>/dev/null; then
	[ -n "$first" ] && echo "Installed. Now add it to Steam, once:" || echo "Steam has no shortcut to the launcher yet. To add one:"
	cat <<EOF
  1. On the Frame, in Steam's desktop mode: Games > Add a Non-Steam Game to My Library.
  2. Browse..., select $dir/halo-vr.sh, then Add Selected Programs.
  3. Start SteamVR, then Halo VR (halo-vr.sh) from the library.
EOF
fi
FRAME
