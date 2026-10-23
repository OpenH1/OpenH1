#!/usr/bin/env bash
# Installs, or updates, OpenH1 from one of its packages, in the home of
# whoever runs it: the one installer of the Linux release's tree
# (tools/linux_package.py) and of the Steam Frame's package
# (tools/frame_package.py), each of which has it as its install.sh. Refer to
# "Steam Deck" and "Steam Frame" in port/linux/README.md
# (share/doc/openh1/README.linux.md in the tree; README.md in the Frame's).
#
#   install.sh        # in the unzipped package (opened from the file manager)
#   install.sh [--package <folder>] [--into <folder>] [--batch]
#
# --package: the package to install; by default the one this script is in.
# --into: the folder to install it in, instead of its own (below).
# --batch: no terminal window, no questions and no steamos-add-to-steam:
#   it prints what is left to do (tools/frame_install.sh, which runs it on
#   the Frame over ssh; the Frame's self-updater, through halo-vr.sh).
#
# What it installs, and where (the packages' own part, "the package" below):
# - The Linux release's tree (bin/openh1, share/openh1): in
#   ~/Applications/OpenH1, with a menu entry named OpenH1
#   (~/.local/share/applications/openh1.desktop).
# - The VR build's tree (share/openh1/vr), beside the other, so that a
#   computer keeps both: in ~/Applications/OpenH1-VR, named OpenH1 VR
#   (openh1-vr.desktop), with a Steam shortcut of its own.
# - The Steam Frame's package (halo, halo-vr.sh, SHA256SUMS): in
#   ~/apps/halo-vr, named OpenH1 VR (openh1-vr.desktop), its launcher
#   halo-vr.sh. Its SHA256SUMS is checked first; the replaced halo,
#   libSDL3.so.0 and halo-vr.sh are kept as <name>.prev (the last only), and
#   the files an earlier package had and this one has not are removed.
#
# For each, the same: opened from the file manager, it runs in a terminal
# window of its own (konsole), to show what it did. It refuses while the
# game runs (pgrep). Each file of the package that differs from the
# installed one is copied as <name>.new and compared with the package's;
# once all are, each takes its name at once (a rename), so the game is never
# half copied. It asks for the game's maps if there are none yet (kdialog),
# writes the menu entry, adds that to Steam (SteamOS's steamos-add-to-steam)
# if Steam has no shortcut to the game, and gives the shortcut its library
# artwork (Steam's grid folder; the package's art, never over a player's
# own). Where kdialog or steamos-add-to-steam is missing, it says what is
# left to do. It never touches the player's files: config.toml,
# halo-vr.env, maps, debug.txt, the logs, the self-updater's files and the
# saved games (~/.local/share/halo-linux); nor Steam's own files, which it
# only reads (Steam rewrites them while it runs).
#
# A package that already holds a player's files (config.toml or maps beside
# the game: a release unpacked and played before this script came with it)
# is installed where it is: only the menu entry, the maps and Steam's
# shortcut are seen to.
set -euo pipefail

usage() {
	sed -n '9,16p' "$0" | sed 's/^# \{0,1\}//' >&2
	exit 2
}

here=$(cd "$(dirname "$0")" && pwd)
package=$here
into=
batch=
arguments=("$@")
while [ $# -gt 0 ]; do
	case $1 in
	--package) [ $# -ge 2 ] || usage; package=$(cd "$2" && pwd); shift 2 ;;
	--into) [ $# -ge 2 ] || usage; into=$(mkdir -p "$2" && cd "$2" && pwd); shift 2 ;;
	--batch) batch=yes; shift ;;
	*) usage ;;
	esac
done

# what a folder holds: the Steam Frame's package, the VR build's tree, the
# other tree, or none of them
kind() {
	if [ -f "$1/halo-vr.sh" ] && { [ -f "$1/halo" ] || [ -f "$1/SHA256SUMS" ]; }; then echo frame
	elif [ -f "$1/bin/openh1" ] && [ -f "$1/share/openh1/vr" ]; then echo vr
	elif [ -f "$1/bin/openh1" ] && [ -d "$1/share/openh1" ]; then echo desktop
	fi
}
package_kind=$(kind "$package")
if [ -z "$package_kind" ]; then
	echo "$package is not a package of OpenH1 (no bin/openh1 and share/openh1, nor the Steam Frame's halo-vr.sh and SHA256SUMS)." >&2
	exit 1
fi
# (opened from the file manager: a terminal window, to show what it did)
if [ -z "$batch" ] && [ ! -t 1 ] && [ -z "${OPENH1_INSTALL_WINDOW:-}" ] && command -v konsole >/dev/null; then
	OPENH1_INSTALL_WINDOW=1 exec konsole --hold -e "$0" "${arguments[@]+"${arguments[@]}"}"
fi

# ---------- the package

# app: the game's name in the menu and in Steam; dir: where it goes;
# entry: its menu entry; program: what the entry and Steam start; process:
# the game's process (pgrep -x); game: the file an installed game has; icon:
# the entry's; art: the prefix of the library artwork's files
# (<art>capsule.png ...); keep: the files whose replaced copy is kept as
# <name>.prev; image: whether the game copies its maps from a disc image at
# its first start
case $package_kind in
frame)
	app="OpenH1 VR"
	dir=$HOME/apps/halo-vr
	entry=$HOME/.local/share/applications/openh1-vr.desktop
	program=halo-vr.sh
	process=halo
	game=halo
	icon=openh1.png
	art=steam-
	keep="halo libSDL3.so.0 halo-vr.sh"
	image=
	play="start SteamVR, then it, from the library"
	no_maps="No maps here: copy the maps folder of the game's Xbox disc to %s/maps (or name the folder that contains it in config.toml's paths.data)."
	;;
vr|desktop)
	if [ "$package_kind" = vr ]; then
		app="OpenH1 VR"
		dir=$HOME/Applications/OpenH1-VR
		entry=$HOME/.local/share/applications/openh1-vr.desktop
	else
		app=OpenH1
		dir=$HOME/Applications/OpenH1
		entry=$HOME/.local/share/applications/openh1.desktop
	fi
	program=bin/openh1
	process=openh1
	game=bin/openh1
	icon=share/icons/hicolor/256x256/apps/openh1.png
	art=share/openh1/steam/
	keep=
	image=yes
	play="play it from Game Mode's library"
	no_maps="No maps yet: at its first start, in Desktop Mode, the game asks for your disc image (.iso) and copies them. (Or copy the maps folder of the game's disc to %s/maps.)"
	;;
esac

# the package's files, one to a line (never a player's, nor the
# self-updater's): the Frame's, as its SHA256SUMS names them; the tree's,
# those of its folders (lib/ only where the build brings its SDL)
package_files() {
	if [ "$package_kind" = frame ]; then
		sed 's/^[0-9a-f]\{64\}  //' "$package/SHA256SUMS"
	else
		local parts=(install.sh bin share)
		[ ! -d "$package/lib" ] || parts+=(lib)
		(cd "$package" && find "${parts[@]}" -type f ! -name '*.old' ! -name '*.new' | LC_ALL=C sort)
	fi
}
# whether a name may be a package's: not a player's file, nor the
# self-updater's (the Frame's names, which its SHA256SUMS gives, are plain
# names in its folder)
package_name() {
	if [ "$package_kind" = frame ]; then
		case $1 in
		''|.*|*/*|config.toml|halo-vr.env|maps|debug.txt|halo-vr.log*|update.partial|update-*|*.new|*.prev|*.old) return 1 ;;
		esac
	else
		case $1 in
		''|/*|.*|*/.*|*.new|*.old) return 1 ;;
		esac
	fi
}
# a file's mode, as the package's tools give them (whatever the unzip kept)
file_mode() {
	case $package_kind:$1 in
	frame:halo|frame:libSDL3.so.0|frame:halo-vr.sh|frame:install.sh) echo 755 ;;
	frame:*) echo 644 ;;
	*:install.sh|*:bin/*|*:lib/*.so*) echo 755 ;;
	*) echo 644 ;;
	esac
}

# whether a folder has the player's files: config.toml, or maps (which the
# game finds without regard to case, install_layout.c)
# (a maps folder of any case, or a link to one: the game follows it)
has_maps() {
	for maps in "$1"/[Mm][Aa][Pp][Ss]; do
		[ -d "$maps" ] && return 0
	done
	return 1
}
in_use() { [ -f "$1/config.toml" ] || has_maps "$1"; }
data_set() { grep -Eq '^[[:space:]]*data[[:space:]]*=[[:space:]]*"[^"]+"' "$1/config.toml" 2>/dev/null; }

if [ -n "$into" ]; then
	dir=$into
elif [ "$package" != "$dir" ] && in_use "$package"; then
	dir=$package
fi
if [ "$dir" != "$package" ] && [ -d "$dir" ] && [ -n "$(ls -A "$dir")" ] && [ ! -f "$dir/$game" ]; then
	echo "$dir is not empty and holds no OpenH1 (no $game): move it away, then install again." >&2
	exit 1
fi
if [ "$dir" != "$package" ] && [ -f "$dir/$game" ] && [ "$(kind "$dir")" != "$package_kind" ]; then
	echo "$dir holds the other build (the VR one, or the one without VR): move it away, then install again." >&2
	exit 1
fi

running() {
	if command -v pgrep >/dev/null && pids=$(pgrep -x "$process"); then
		echo "$app is running (process ${pids//$'\n'/ }): quit it, then install again." >&2
		exit 3
	fi
}
# (VERSION's commit and subject: the Frame's package has it)
version() { [ ! -f "$1/VERSION" ] || sed -n '1h;3{H;x;s/\n/ /;p;}' "$1/VERSION"; }

# ---------- the package's files

first=
if [ "$dir" = "$package" ]; then
	echo "$app is in $dir: it stays there."
else
	echo "Installing $app to $dir"
	[ ! -f "$package/VERSION" ] || echo "  ($(version "$package"))"
	running
	if [ "$package_kind" = frame ]; then
		if grep -Evq '^[0-9a-f]{64}  [A-Za-z0-9][A-Za-z0-9._+-]*$' "$package/SHA256SUMS"; then
			echo "$package/SHA256SUMS: a line that is not a file's SHA-256 and plain name: nothing was installed." >&2
			exit 4
		fi
		if ! (cd "$package" && sha256sum -c --quiet SHA256SUMS); then
			echo "$package differs from its SHA256SUMS: nothing was installed." >&2
			exit 4
		fi
	fi
	[ -e "$dir/$game" ] || first=yes
	was=$(version "$dir")
	mkdir -p "$dir"
	names=$(package_files)
	changed=()
	discard() { for name in "${changed[@]+"${changed[@]}"}"; do rm -f "$dir/$name.new"; done; }
	trap discard EXIT
	while IFS= read -r name; do
		[ -n "$name" ] || continue
		if ! package_name "$name"; then
			echo "$name: a name a package may not have: nothing was installed." >&2
			exit 4
		fi
		if [ -f "$dir/$name" ] && cmp -s "$package/$name" "$dir/$name"; then
			continue
		fi
		mkdir -p "$(dirname "$dir/$name")"
		changed+=("$name")
		if ! cp "$package/$name" "$dir/$name.new" || ! cmp -s "$package/$name" "$dir/$name.new"; then
			echo "$dir/$name.new differs from $package/$name: nothing was installed." >&2
			exit 4
		fi
		chmod "$(file_mode "$name")" "$dir/$name.new"
	done <<EOF
$names
EOF
	running
	for name in "${changed[@]+"${changed[@]}"}"; do
		if [ -e "$dir/$name" ]; then
			note=updated
			case " $keep " in *" $name "*)
				ln -f "$dir/$name" "$dir/$name.prev" 2>/dev/null || cp -p "$dir/$name" "$dir/$name.prev"
				note="updated, the previous kept as $name.prev" ;;
			esac
		else
			note=new
		fi
		mv -f "$dir/$name.new" "$dir/$name"
		[ -n "$first" ] || echo "  $name: $note"
	done
	trap - EXIT
	# (the Frame's: the files an earlier package had and this one has not,
	# then its SHA256SUMS, the list of what is installed)
	if [ "$package_kind" = frame ]; then
		if [ -f "$dir/SHA256SUMS" ]; then
			while IFS= read -r name; do
				package_name "$name" || continue
				printf '%s\n' "$names" | grep -qxF -- "$name" && continue
				[ -e "$dir/$name" ] || continue
				rm -f "$dir/$name"
				echo "  $name: removed (no longer in the package)"
			done <<EOF
$(sed 's/^[0-9a-f]\{64\}  //' "$dir/SHA256SUMS")
EOF
		fi
		cmp -s "$package/SHA256SUMS" "$dir/SHA256SUMS" 2>/dev/null || {
			cp "$package/SHA256SUMS" "$dir/SHA256SUMS.new"
			chmod 644 "$dir/SHA256SUMS.new"
			mv -f "$dir/SHA256SUMS.new" "$dir/SHA256SUMS"
		}
	fi
	if [ ${#changed[@]} -eq 0 ]; then
		echo "  Nothing changed: the same package is installed."
	elif [ -n "$first" ]; then
		echo "  ${#changed[@]} files copied."
	elif [ -f "$package/VERSION" ]; then
		echo "  Was: ${was:-a build installed by hand}"
		echo "  Now: $(version "$dir")"
	fi
	case " ${changed[*]+"${changed[*]}"} " in *" halo-vr.sh "*)
		[ ! -f "$dir/halo-vr.sh.prev" ] ||
			echo "  (Settings of your own for the launcher go in halo-vr.env, which updates keep: halo-vr.sh.prev is the replaced launcher.)" ;;
	esac
fi

# ---------- the game's data

# The game's first start copies the maps from a disc image itself (it asks,
# then shows a file picker: port/linux/src/sdl_platform.c), and takes an
# image in its folder without the picker; not on the Frame, whose game
# starts in the headset. Asked here once, in the desktop: a maps folder is
# copied as the game copies an image's (to maps.partial, then renamed
# maps); a disc image is linked into the game's folder, where that first
# start finds it.
title="OpenH1: your game's maps"
image_link=
if [ -n "$image" ]; then
	for link in "$dir"/*.iso "$dir"/*.xiso "$dir"/*.ISO "$dir"/*.XISO; do
		[ -L "$link" ] && image_link=$link
	done
fi
copy_maps() {
	[ ! -d "$1/maps" ] || set -- "$1/maps"
	if ls "$1"/*.map >/dev/null 2>&1; then
		echo "  maps: copying from $1"
		rm -rf "$dir/maps.partial"
		mkdir "$dir/maps.partial"
		cp "$1"/*.map "$dir/maps.partial/"
		[ ! -f "$1/loading.tga" ] || cp "$1/loading.tga" "$dir/maps.partial/"
		mv "$dir/maps.partial" "$dir/maps"
		echo "  maps: copied"
	elif [ -n "$image" ]; then
		echo "$1 has no maps (.map files): the game asks for its disc image at its first start." >&2
	else
		echo "$1 has no maps (.map files)." >&2
	fi
}
if [ -z "$batch" ] && ! has_maps "$dir" && ! data_set "$dir" && [ -z "$image_link" ] && command -v kdialog >/dev/null; then
	if [ -z "$image" ]; then
		# (the Frame's: the maps folder of the game's Xbox disc, or the
		# folder that contains it)
		if from=$(kdialog --title "$title" --getexistingdirectory "$HOME"); then
			copy_maps "$from"
		fi
	else
		answer=0
		kdialog --title "$title" --yes-label "A disc image" --no-label "A maps folder" --yesnocancel \
			"OpenH1 plays the maps of your own Halo: Combat Evolved for the Xbox.

Where are they: in a disc image of the game (.iso or .xiso), or in a maps folder copied from its disc?" || answer=$?
		if [ "$answer" = 0 ] &&
			chosen=$(kdialog --title "$title" --getopenfilename "$HOME" "*.iso *.xiso *.ISO *.XISO|Xbox disc images (.iso, .xiso)"); then
			if [ -f "$chosen" ]; then
				image_link=$dir/$(basename "$chosen")
				if [ -e "$image_link" ] && [ ! -L "$image_link" ]; then
					image_link=
					echo "$dir already has a file named $(basename "$chosen"): the game asks for its disc image at its first start." >&2
				else
					ln -sfn "$chosen" "$image_link"
					echo "  disc image: $chosen (the game copies its maps at its first start)"
				fi
			fi
		elif [ "$answer" = 1 ] && from=$(kdialog --title "$title" --getexistingdirectory "$HOME"); then
			copy_maps "$from"
		fi
	fi
fi
if [ -n "$image_link" ] && ! has_maps "$dir"; then
	echo "Start $app once in Desktop Mode (from the application menu, Games) and choose Yes when it asks: it copies the maps from $(readlink "$image_link") (about 2 GB). Then you can quit it, and play from Game Mode."
	if [ -z "$batch" ] && command -v kdialog >/dev/null && kdialog --title "$title" --yesno \
		"Start $app now, to copy the maps from the disc image? Choose Yes when it asks.

(Once: about 2 GB. When it has copied them, you can quit it, and play from Game Mode.)"; then
		(cd "$dir" && nohup "$dir/$program" >/dev/null 2>&1 &)
		echo "  $app started."
	fi
elif ! has_maps "$dir" && ! data_set "$dir"; then
	# shellcheck disable=SC2059
	printf "$no_maps\n" "$dir"
fi

# ---------- the menu entry

# the package's own (openh1.desktop, for a package's /usr: Exec=openh1,
# Icon=openh1), with this folder's program and icon; Steam names the
# shortcut made from it after it (in its list of programs, and from
# steamos-add-to-steam)
template=$dir/share/applications/openh1.desktop
[ "$package_kind" != frame ] || template=$dir/openh1.desktop
comment="Halo: Combat Evolved, ported from its decompilation"
[ "$package_kind" = desktop ] || comment="Halo: Combat Evolved in VR, ported from its decompilation"
escape() { printf '%s' "$1" | sed 's/[\\|&]/\\&/g'; }
mkdir -p "$(dirname "$entry")"
if [ -f "$template" ]; then
	sed -e "s|^Exec=.*|Exec=\"$(escape "$dir/$program")\"\\
Path=$(escape "$dir")|" \
		-e "s|^Icon=.*|Icon=$(escape "$dir/$icon")|" \
		-e "s|^Name=.*|Name=$app|" \
		-e "s|^Comment=.*|Comment=$comment|" \
		"$template" > "$entry.new"
else
	# (a package from before it carried the entry)
	cat > "$entry.new" <<EOF
[Desktop Entry]
Type=Application
Name=$app
Comment=$comment
Exec="$dir/$program"
Path=$dir
Icon=$dir/$icon
Terminal=false
Categories=Game;ActionGame;
EOF
fi
mv -f "$entry.new" "$entry"
echo "  menu entry: $entry"

# ---------- Steam's shortcut, and its artwork

# The shortcut only Steam may add (it rewrites its shortcuts.vdf while it
# runs): SteamOS's steamos-add-to-steam asks the running Steam to, which
# writes it at once. Its artwork goes in Steam's grid folder of each Steam
# user that has the shortcut, named after the shortcut's app ID (read from
# shortcuts.vdf: steam_artwork, below).
steam=$HOME/.local/share/Steam
has_shortcut() { grep -lqF "$dir/$program" "$steam"/userdata/*/config/shortcuts.vdf 2>/dev/null; }

# Steam's library artwork for the shortcut, from the package's files: in
# each Steam user's config/grid, <id>p.png (the capsule, 600 x 900),
# <id>.png (the wide capsule, 920 x 430), <id>_hero.png (1920 x 620),
# <id>_logo.png and <id>_icon.png, <id> being the shortcut's app ID: its
# appid in shortcuts.vdf (binary VDF, read only), or, in a shortcuts.vdf
# without one, Steam's own for it (the CRC-32 of its Exe and AppName, the
# top bit set). A file there that is neither this art nor art this
# script wrote before (.steam-art in the game's folder lists those, with
# their SHA-256) is the player's own, and kept. Prints what it did.
steam_artwork() {
	command -v python3 >/dev/null || { echo "  (No python3: Steam's library shows no artwork for $app.)"; return 0; }
	python3 -I - "$steam" "$dir/$program" "$dir/$art" "$dir/.steam-art" <<'PYTHON'
import hashlib, os, struct, sys, zlib
from pathlib import Path

steam, program, art, record_path = Path(sys.argv[1]), sys.argv[2], sys.argv[3], Path(sys.argv[4])
# the package's file for each of a grid's names (<id> + suffix)
ART = {"p.png": "capsule.png", ".png": "wide.png", "_hero.png": "hero.png", "_logo.png": "logo.png",
       "_icon.png": "icon.png"}


def read_map(data, at):
    """a map of binary VDF from at (after its name), and where it ends"""
    entries = {}
    while True:
        kind = data[at]
        at += 1
        if kind == 8:
            return entries, at
        end = data.index(b"\0", at)
        key = data[at:end].decode("utf-8", "replace").lower()
        at = end + 1
        if kind == 0:
            value, at = read_map(data, at)
        elif kind == 1:
            end = data.index(b"\0", at)
            value, at = data[at:end], end + 1
        elif kind in (2, 4, 6):  # int32, float32, a colour or pointer
            value, at = struct.unpack_from("<I", data, at)[0], at + 4
        elif kind == 7:  # uint64
            value, at = struct.unpack_from("<Q", data, at)[0], at + 8
        else:
            raise ValueError(f"a field of type {kind}")
        entries[key] = value


def shortcut_ids(path):
    """the app IDs of the shortcuts in shortcuts.vdf at path whose program is ours"""
    data = path.read_bytes()
    if not data.startswith(b"\0shortcuts\0"):
        return []
    shortcuts, _ = read_map(data, len(b"\0shortcuts\0"))
    found = []
    for shortcut in shortcuts.values():
        if not isinstance(shortcut, dict) or not isinstance(shortcut.get("exe"), bytes):
            continue
        exe = shortcut["exe"]
        if exe.decode("utf-8", "replace").strip().strip('"') != program:
            continue
        appid = shortcut.get("appid")
        if not isinstance(appid, int):
            appid = (zlib.crc32(exe + shortcut.get("appname", b"")) | 0x80000000) & 0xFFFFFFFF
        found.append(appid & 0xFFFFFFFF)
    return found


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


record = {}
if record_path.is_file():
    for line in record_path.read_text(encoding="utf-8", errors="replace").splitlines():
        digest, _, name = line.partition("  ")
        if name:
            record[name] = digest
written, kept, users = 0, [], 0
for shortcuts in sorted(steam.glob("userdata/*/config/shortcuts.vdf")):
    try:
        ids = shortcut_ids(shortcuts)
    except (ValueError, IndexError, struct.error, OSError) as problem:
        print(f"  ({shortcuts}: not read: {problem})")
        continue
    if not ids:
        continue
    users += 1
    grid = shortcuts.parent / "grid"
    for appid in ids:
        for suffix, name in ART.items():
            source = Path(art + name)
            if not source.is_file():
                continue
            target = grid / f"{appid}{suffix}"
            new = source.read_bytes()
            if target.is_file():
                current = target.read_bytes()
                if current == new:
                    record[str(target)] = hashlib.sha256(new).hexdigest()
                    continue
                if record.get(str(target)) != hashlib.sha256(current).hexdigest():
                    kept.append(target.name)
                    continue
            grid.mkdir(parents=True, exist_ok=True)
            partial = target.with_name(target.name + ".new")
            partial.write_bytes(new)
            os.replace(partial, target)
            record[str(target)] = hashlib.sha256(new).hexdigest()
            written += 1
if record:
    partial = record_path.with_name(record_path.name + ".new")
    partial.write_text("".join(f"{digest}  {name}\n" for name, digest in sorted(record.items())), encoding="utf-8")
    os.replace(partial, record_path)
if written:
    print(f"  Steam library artwork: {written} files (if Steam shows a blank tile, restart Steam)")
if kept:
    print(f"  Steam library artwork of your own kept: {', '.join(sorted(set(kept)))}")
PYTHON
}

if has_shortcut; then
	echo "Steam has $app already: ${play}."
	steam_artwork
elif [ -z "$batch" ] && command -v steamos-add-to-steam >/dev/null && steamos-add-to-steam "$entry" >/dev/null 2>&1; then
	echo "Added to Steam as $app: ${play}."
	# (Steam writes the shortcut as it adds it; it is waited for, briefly)
	for _ in 1 2 3 4 5 6 7 8 9 10; do
		has_shortcut && break
		sleep 1
	done
	if has_shortcut; then
		steam_artwork
	else
		echo "  (Steam has not written the shortcut yet: install again later to give it its library artwork.)"
	fi
elif [ "$package_kind" = frame ] || [ -d "$steam" ] || command -v steam >/dev/null; then
	[ -n "$first" ] && echo "Installed. Now add it to Steam, once:" || echo "Steam has no shortcut to $app yet. To add one:"
	if [ "$package_kind" = frame ]; then
		echo "  1. On the Frame, in Steam's desktop mode: Games > Add a Non-Steam Game to My Library."
	else
		echo "  1. In Steam: Games > Add a Non-Steam Game to My Library."
	fi
	echo "  2. Tick $app in the list (or Browse..., select $dir/$program), then Add Selected Programs."
	if [ "$package_kind" = frame ]; then
		echo "  3. Start SteamVR, then $app from the library."
	else
		echo "  3. Play $app from the library (on a Steam Deck, in Game Mode)."
	fi
	echo "  (Then install again, to give it its library artwork.)"
else
	echo "Start $app from the application menu (Games)."
fi
if [ "$dir" != "$package" ] && [ -z "$batch" ]; then
	echo "The game updates itself there. $package can be deleted."
fi
