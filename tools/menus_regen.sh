#!/usr/bin/env bash
# Writes every menu file again (tools/ce_menus.py) from the inputs in one
# folder, outside the repository ($MENU_INPUTS, else ~/dev/menu-inputs):
#
#   tools/menus_regen.sh --fetch   # gets the inputs, once (git, python3)
#   tools/menus_regen.sh           # writes the menus; git status shows what changed
#
# The folder holds (port/assets/menus/README.md, "Writing them again"):
#   halopc-restored/  the PC version's tags, at HALOPC_RESTORED_COMMIT
#   invader/          Invader's tag definitions, at INVADER_COMMIT
#   redraws/          the hand-made redraws: port/assets/menus/svg copied in
#                     before each run (ce_menus.py writes that folder again),
#                     with any new redraw put here
#   placeholder.png   the picture of the frames with no redraw: the one the
#                     committed menus draw, cut from one of its frames
#   pictures/         the PC version's pictures of those frames
#                     (tools/ce_menu_pictures.py, from the tags)
#   venv/             Pillow, NumPy, SciPy and fontTools
# rsvg-convert comes from the system (librsvg2-bin, Homebrew's librsvg).
# Other arguments are passed to ce_menus.py. The tags are the game's data:
# never commit the folder.
set -euo pipefail

HALOPC_RESTORED=https://github.com/Aerocatia/halopc-restored.git
HALOPC_RESTORED_COMMIT=eb1c8f73ba387fb0844e9716f4e41072f09299e4
INVADER=https://github.com/SnowyMouse/invader.git
INVADER_COMMIT=20a23018f34cecb2fa33f046a6b2c29cd8341f34
# the largest of the placeholder frames, which shows the picture unscaled
PLACEHOLDER_FRAME=ce/shell/main_menu/settings_select/multiplayer_setup/playlist_edit/gametype_options__6.png

ROOT=$(cd "$(dirname "$0")/.." && pwd)
INPUTS=${MENU_INPUTS:-$HOME/dev/menu-inputs}
PYTHON=$INPUTS/venv/bin/python

fetch_repository() {  # <folder> <url> <commit>: a shallow clone of one commit
    if [ "$(git -C "$1" rev-parse HEAD 2>/dev/null)" = "$3" ]; then
        return
    fi
    if [ ! -d "$1/.git" ]; then
        git init -q "$1"
        git -C "$1" remote add origin "$2"
    fi
    git -C "$1" fetch -q --depth 1 origin "$3"
    git -C "$1" checkout -q --detach FETCH_HEAD
}

if [ "${1:-}" = "--fetch" ]; then
    shift
    mkdir -p "$INPUTS"
    fetch_repository "$INPUTS/halopc-restored" "$HALOPC_RESTORED" "$HALOPC_RESTORED_COMMIT"
    fetch_repository "$INPUTS/invader" "$INVADER" "$INVADER_COMMIT"
    if [ ! -x "$PYTHON" ]; then
        python3 -m venv "$INPUTS/venv"
        "$PYTHON" -m pip install -q pillow numpy scipy fonttools
    fi
    if [ ! -f "$INPUTS/placeholder.png" ]; then
        "$PYTHON" -I -c '
import sys
from PIL import Image
image = Image.open(sys.argv[1]).convert("RGBA")
image.crop(image.getchannel("A").getbbox()).save(sys.argv[2])' \
            "$ROOT/port/assets/menus/$PLACEHOLDER_FRAME" "$INPUTS/placeholder.png"
    fi
    "$PYTHON" "$ROOT/tools/ce_menu_pictures.py" --tags "$INPUTS/halopc-restored/tags" \
        --definitions "$INPUTS/invader/src/tag/hek/definition" --out "$INPUTS/pictures"
    [ $# -eq 0 ] && exit 0
fi

for input in halopc-restored/tags invader/src/tag/hek/definition placeholder.png pictures venv/bin/python; do
    if [ ! -e "$INPUTS/$input" ]; then
        echo "$INPUTS/$input is missing: run $0 --fetch" >&2
        exit 1
    fi
done
command -v rsvg-convert >/dev/null || { echo "rsvg-convert is missing (librsvg)" >&2; exit 1; }
mkdir -p "$INPUTS/redraws"
cp -R "$ROOT/port/assets/menus/svg/." "$INPUTS/redraws/"
exec "$PYTHON" -W ignore::DeprecationWarning "$ROOT/tools/ce_menus.py" \
    --tags "$INPUTS/halopc-restored/tags" \
    --definitions "$INPUTS/invader/src/tag/hek/definition" \
    --redraws "$INPUTS/redraws" \
    --placeholder "$INPUTS/placeholder.png" \
    --pictures "$INPUTS/pictures" "$@"
