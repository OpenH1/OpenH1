#!/usr/bin/env python3
"""Makes the Android touch controls' pictures (TouchControls.java) from
their SVGs in port/assets/touch:

    python tools/touch_assets.py           # writes the PNGs
    python tools/touch_assets.py --check   # fails if a PNG is missing or not as written

Each picture is drawn once, large, into
port/android/app/src/main/res/drawable-nodpi/touch_*.png: the app scales it
to each control's size, with mipmaps, so one size serves every display. The
art is Kenney's Mobile Controls (port/assets/touch/kenney-mobile-controls,
CC0) and OpenH1's own icons in the same manner (port/assets/touch/openh1);
port/assets/touch/README.md says which picture is which control's. Run it
again when an SVG changes. Needs rsvg-convert (librsvg).
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KENNEY = ROOT / "port/assets/touch/kenney-mobile-controls"
OPENH1 = ROOT / "port/assets/touch/openh1"
OUT = ROOT / "port/android/app/src/main/res/drawable-nodpi"

# the plates are drawn up to about 420 pixels across (the stick at 200% on a
# tablet); a picture made smaller with mipmaps stays sharp
PLATE = 384
ICON = 192

# (the resource's name, the SVG, its size in pixels)
PICTURES = [
    ("touch_plate", KENNEY / "button_circle.svg", PLATE),
    ("touch_press_ring", KENNEY / "button_circle_highlight.svg", PLATE),
    ("touch_stick_pad", KENNEY / "joystick_circle_pad_d.svg", PLATE),
    ("touch_stick_knob", KENNEY / "joystick_circle_nub_a.svg", PLATE // 2),
    ("touch_icon_fire", KENNEY / "icon_fire.svg", ICON),
    ("touch_icon_jump", KENNEY / "icon_jump.svg", ICON),
    ("touch_icon_reload", KENNEY / "icon_arrow_rotate.svg", ICON),
    ("touch_icon_melee", KENNEY / "icon_sword.svg", ICON),
    ("touch_icon_zoom", KENNEY / "icon_crosshair.svg", ICON),
    ("touch_icon_pause", KENNEY / "icon_pause.svg", ICON),
    ("touch_icon_weapon", KENNEY / "icon_arrow_curved.svg", ICON),
    ("touch_icon_arrow", KENNEY / "icon_arrow.svg", ICON),
    ("touch_icon_back", KENNEY / "icon_menu.svg", ICON),
    ("touch_icon_options", KENNEY / "icon_cog.svg", ICON),
    ("touch_icon_grenade", OPENH1 / "icon_grenade.svg", ICON),
    ("touch_icon_grenade_type", OPENH1 / "icon_grenade_type.svg", ICON),
    ("touch_icon_crouch", OPENH1 / "icon_crouch.svg", ICON),
    ("touch_icon_flashlight", OPENH1 / "icon_flashlight.svg", ICON),
]


def render(svg, size, out):
    subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), "-o", str(out), str(svg)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="compare the PNGs with fresh renders")
    args = parser.parse_args()
    stale = []
    with tempfile.TemporaryDirectory() as scratch:
        for name, svg, size in PICTURES:
            target = OUT / (name + ".png")
            if args.check:
                fresh = Path(scratch) / (name + ".png")
                render(svg, size, fresh)
                if not target.exists() or target.read_bytes() != fresh.read_bytes():
                    stale.append(target.relative_to(ROOT))
            else:
                OUT.mkdir(parents=True, exist_ok=True)
                render(svg, size, target)
    if stale:
        print("Not as tools/touch_assets.py writes them:", *stale, sep="\n  ")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
