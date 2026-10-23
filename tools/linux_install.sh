#!/usr/bin/env bash
# Installs, or updates, OpenH1 on a Linux desktop, the Steam Deck's first:
# the release's tree (tools/linux_package.py) whose install.sh this is, into
# ~/Applications/OpenH1. Refer to "Steam Deck" in port/linux/README.md
# (share/doc/openh1/README.linux.md in the tree).
#
#   install.sh        # in the unzipped OpenH1-Linux.zip (opened from the file manager)
#
# Opened from the file manager, it runs in a terminal window of its own
# (konsole), to show what it did. It copies the tree's files to
# ~/Applications/OpenH1, asks for the game's maps if there are none yet
# (kdialog), gives the game a menu entry named OpenH1
# (~/.local/share/applications/openh1.desktop), and adds that to Steam
# (SteamOS's steamos-add-to-steam) if Steam has no shortcut to it. Where
# kdialog or steamos-add-to-steam is missing, it says what is left to do.
#
# A tree that already holds a player's files (config.toml or maps beside
# bin/: a release unpacked and played before this script came with it) is
# installed where it is: only the menu entry, the maps and Steam's shortcut
# are seen to.
#
# It refuses while the game runs (pgrep -x openh1). Each file of the tree
# that differs from the installed one is copied as <name>.new and compared
# with the tree's; once all are, each takes its name at once (a rename), so
# the game is never half copied. The game updates itself in that folder
# (port/linux/src/updater.c), install.sh with it. config.toml, maps,
# debug.txt, the self-updater's files and the saved games
# (~/.local/share/halo-linux) are never touched; files an older tree had and
# this one has not are left, as the self-updater leaves them.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
if [ ! -f "$here/bin/openh1" ] || [ ! -d "$here/share/openh1" ]; then
	echo "$here is not OpenH1's release (no bin/openh1 and share/openh1 beside install.sh)." >&2
	exit 1
fi
# (opened from the file manager: a terminal window, to show what it did)
if [ ! -t 1 ] && [ -z "${OPENH1_INSTALL_WINDOW:-}" ] && command -v konsole >/dev/null; then
	OPENH1_INSTALL_WINDOW=1 exec konsole --hold -e "$0"
fi

# whether a folder has the player's files: config.toml, or maps (which the
# game finds without regard to case, install_layout.c)
has_maps() { [ -n "$(find "$1" -mindepth 1 -maxdepth 1 -iname maps -type d 2>/dev/null)" ]; }
in_use() { [ -f "$1/config.toml" ] || has_maps "$1"; }
data_set() { grep -Eq '^[[:space:]]*data[[:space:]]*=[[:space:]]*"[^"]+"' "$1/config.toml" 2>/dev/null; }

dir=$HOME/Applications/OpenH1
[ "$here" = "$dir" ] || ! in_use "$here" || dir=$here
if [ "$dir" != "$here" ] && [ -d "$dir" ] && [ -n "$(ls -A "$dir")" ] && [ ! -f "$dir/bin/openh1" ]; then
	echo "$dir is not empty and holds no OpenH1 (no bin/openh1): move it away, then install again." >&2
	exit 1
fi

running() {
	if command -v pgrep >/dev/null && pids=$(pgrep -x openh1); then
		echo "OpenH1 is running (process ${pids//$'\n'/ }): quit it, then install again." >&2
		exit 3
	fi
}

# ---------- the tree's files

if [ "$dir" = "$here" ]; then
	echo "OpenH1 is in $dir: it stays there."
	first=
else
	echo "Installing OpenH1 to $dir"
	running
	first=yes
	[ -e "$dir/bin/openh1" ] && first=
	mkdir -p "$dir"
	# (the tree's own files: never a player's, nor the self-updater's; lib/
	# only where the build brings its SDL)
	parts=(install.sh bin share)
	[ ! -d "$here/lib" ] || parts+=(lib)
	names=$(cd "$here" && find "${parts[@]}" -type f ! -name '*.old' ! -name '*.new' | LC_ALL=C sort)
	changed=()
	discard() { for name in "${changed[@]+"${changed[@]}"}"; do rm -f "$dir/$name.new"; done; }
	while IFS= read -r name; do
		[ -n "$name" ] || continue
		if [ -f "$dir/$name" ] && cmp -s "$here/$name" "$dir/$name"; then
			continue
		fi
		mkdir -p "$(dirname "$dir/$name")"
		changed+=("$name")
		if ! cp "$here/$name" "$dir/$name.new" || ! cmp -s "$here/$name" "$dir/$name.new"; then
			echo "$dir/$name.new differs from $here/$name: nothing was installed." >&2
			discard
			exit 4
		fi
		# (the modes tools/linux_package.py gives them, whatever the unzip kept)
		case $name in
		install.sh|bin/*|lib/*.so*) chmod 755 "$dir/$name.new" ;;
		*) chmod 644 "$dir/$name.new" ;;
		esac
	done <<EOF
$names
EOF
	trap discard EXIT
	running
	for name in "${changed[@]+"${changed[@]}"}"; do
		if [ -e "$dir/$name" ]; then note=updated; else note=new; fi
		mv -f "$dir/$name.new" "$dir/$name"
		[ -n "$first" ] || echo "  $name: $note"
	done
	trap - EXIT
	if [ ${#changed[@]} -eq 0 ]; then
		echo "  Nothing changed: the same release is installed."
	elif [ -n "$first" ]; then
		echo "  ${#changed[@]} files copied."
	fi
fi

# ---------- the game's data

# The game's first start copies the maps from a disc image itself (it asks,
# then shows a file picker: port/linux/src/sdl_platform.c), and takes an
# image in its folder without the picker. Asked here once, in the desktop:
# a maps folder is copied as the game copies an image's (to maps.partial,
# then renamed maps); a disc image is linked into the game's folder, where
# that first start finds it.
image_link=
for link in "$dir"/*.iso "$dir"/*.xiso "$dir"/*.ISO "$dir"/*.XISO; do
	[ -L "$link" ] && image_link=$link
done
if ! has_maps "$dir" && ! data_set "$dir" && [ -z "$image_link" ] && command -v kdialog >/dev/null; then
	title="OpenH1: your game's maps"
	answer=0
	kdialog --title "$title" --yes-label "A disc image" --no-label "A maps folder" --yesnocancel \
		"OpenH1 plays the maps of your own Halo: Combat Evolved for the Xbox.

Where are they: in a disc image of the game (.iso or .xiso), or in a maps folder copied from its disc?" || answer=$?
	if [ "$answer" = 0 ] &&
		image=$(kdialog --title "$title" --getopenfilename "$HOME" "*.iso *.xiso *.ISO *.XISO|Xbox disc images (.iso, .xiso)"); then
		if [ -f "$image" ]; then
			image_link=$dir/$(basename "$image")
			if [ -e "$image_link" ] && [ ! -L "$image_link" ]; then
				image_link=
				echo "$dir already has a file named $(basename "$image"): the game asks for its disc image at its first start." >&2
			else
				ln -sfn "$image" "$image_link"
				echo "  disc image: $image (the game copies its maps at its first start)"
			fi
		fi
	elif [ "$answer" = 1 ] && from=$(kdialog --title "$title" --getexistingdirectory "$HOME"); then
		[ ! -d "$from/maps" ] || from=$from/maps
		if ls "$from"/*.map >/dev/null 2>&1; then
			echo "  maps: copying from $from"
			rm -rf "$dir/maps.partial"
			mkdir "$dir/maps.partial"
			cp "$from"/*.map "$dir/maps.partial/"
			[ ! -f "$from/loading.tga" ] || cp "$from/loading.tga" "$dir/maps.partial/"
			mv "$dir/maps.partial" "$dir/maps"
			echo "  maps: copied"
		else
			echo "$from has no maps (.map files): the game asks for its disc image at its first start." >&2
		fi
	fi
fi
if [ -n "$image_link" ] && ! has_maps "$dir"; then
	echo "Start OpenH1 once in Desktop Mode (from the application menu, Games) and choose Yes when it asks: it copies the maps from $(readlink "$image_link") (about 2 GB). Then you can quit it, and play from Game Mode."
	if command -v kdialog >/dev/null && kdialog --title "OpenH1: your game's maps" --yesno \
		"Start OpenH1 now, to copy the maps from the disc image? Choose Yes when it asks.

(Once: about 2 GB. When it has copied them, you can quit it, and play from Game Mode.)"; then
		(cd "$dir" && nohup "$dir/bin/openh1" >/dev/null 2>&1 &)
		echo "  OpenH1 started."
	fi
elif ! has_maps "$dir" && ! data_set "$dir"; then
	echo "No maps yet: at its first start, in Desktop Mode, the game asks for your disc image (.iso) and copies them. (Or copy the maps folder of the game's disc to $dir/maps.)"
fi

# ---------- the menu entry

# the tree's own (share/applications/openh1.desktop, for a package's /usr:
# Exec=openh1, Icon=openh1), with this folder's program and icon; Steam
# names the shortcut made from it after it (in its list of programs, and
# from steamos-add-to-steam)
entry=$HOME/.local/share/applications/openh1.desktop
icon=$dir/share/icons/hicolor/256x256/apps/openh1.png
escape() { printf '%s' "$1" | sed 's/[\\|&]/\\&/g'; }
mkdir -p "$(dirname "$entry")"
sed -e "s|^Exec=.*|Exec=\"$(escape "$dir/bin/openh1")\"\\
Path=$(escape "$dir")|" \
	-e "s|^Icon=.*|Icon=$(escape "$icon")|" \
	"$dir/share/applications/openh1.desktop" > "$entry.new"
mv -f "$entry.new" "$entry"
echo "  menu entry: $entry"

# ---------- Steam's shortcut

# which only Steam may add (it rewrites its shortcuts.vdf while it runs):
# SteamOS's steamos-add-to-steam asks the running Steam to
if grep -lqF "$dir/bin/openh1" "$HOME"/.local/share/Steam/userdata/*/config/shortcuts.vdf 2>/dev/null; then
	echo "Steam has OpenH1 already: play it from Game Mode's library."
elif command -v steamos-add-to-steam >/dev/null && steamos-add-to-steam "$entry" >/dev/null 2>&1; then
	echo "Added to Steam as OpenH1: play it from Game Mode's library."
elif [ -d "$HOME/.local/share/Steam" ] || command -v steam >/dev/null; then
	[ -n "$first" ] && echo "Installed. Now add it to Steam, once:" || echo "Steam has no shortcut to OpenH1 yet. To add one:"
	cat <<EOF
  1. In Steam: Games > Add a Non-Steam Game to My Library.
  2. Tick OpenH1 in the list (or Browse..., select $dir/bin/openh1), then Add Selected Programs.
  3. Play OpenH1 from the library (on a Steam Deck, in Game Mode).
EOF
else
	echo "Start OpenH1 from the application menu (Games)."
fi
if [ "$dir" != "$here" ]; then
	echo "The game updates itself there. $here can be deleted."
fi
