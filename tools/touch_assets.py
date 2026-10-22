#!/usr/bin/env python3
"""Makes the Android touch controls' pictures (TouchControls.java):

    python tools/touch_assets.py           # writes the PNGs
    python tools/touch_assets.py --check   # fails if a PNG is missing or not as written

Each picture is drawn once, large, into
port/android/app/src/main/res/drawable-nodpi/touch_*.png: the app scales it
to each control's size, with mipmaps, so one size serves every display.

- The controller's buttons are the HUD's redraws (port/assets/buttons, as
  tools/button_assets.py draws them for display.hud_button_style), drawn
  here larger and cropped to the art: A, B, X, Y, black and white in each
  style ("classic", "gems", "menu_gems"), the triggers and the sticks.
- The rest, which has no redraw, is Kenney's Mobile Controls
  (port/assets/touch/kenney-mobile-controls, CC0): the plates of the
  D-pad, start, back and the toolbar, the ring of a plate held, the move
  stick, and their icons.

port/assets/touch/README.md says which picture is which control's. Run it
again when an SVG or a redraw changes. Needs rsvg-convert, Pillow, NumPy
and SciPy (tools/button_assets.py's).
"""

import argparse
import io
import json
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import button_assets  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
KENNEY = ROOT / "port/assets/touch/kenney-mobile-controls"
BUTTONS = button_assets.ASSETS
OUT = ROOT / "port/android/app/src/main/res/drawable-nodpi"

# the plates are drawn up to about 420 pixels across (the stick at 200% on a
# tablet); a picture made smaller with mipmaps stays sharp
PLATE = 384
ICON = 192
# the redraws' texels drawn at this many pixels (a face, 26 texels high: 416)
SCALE = 16
# the supplied PNGs' art, at most this many pixels on a side
ART = 416

# (the resource's name, Kenney's SVG, its size in pixels)
KENNEY_PICTURES = [
    ("touch_plate", "button_circle.svg", PLATE),
    ("touch_press_ring", "button_circle_highlight.svg", PLATE),
    ("touch_stick_pad", "joystick_circle_pad_d.svg", PLATE),
    ("touch_stick_knob", "joystick_circle_nub_a.svg", PLATE // 2),
    ("touch_icon_pause", "icon_pause.svg", ICON),
    ("touch_icon_back", "icon_menu.svg", ICON),
    ("touch_icon_arrow", "icon_arrow.svg", ICON),
    ("touch_icon_options", "icon_cog.svg", ICON),
]

# each style's buttons (hud_hires_tags.c's hud_hires_button_draw): the
# resource's name and buttons.json's entry; "gems" has no black and white
# of its own, and draws menu_gems' (TouchControls.java)
FACES = [("classic", button, f"classic_{button}__0") for button in ("a", "b", "x", "y", "black", "white")] + \
    [("gems", button, f"gem_{button}__0") for button in ("a", "b", "x", "y")] + \
    [("menu", button, f"{button}_butn__0") for button in ("a", "b", "x", "y", "black", "white")]
CONTROLS = ["trigger_left", "trigger_right", "thumbstick_left", "thumbstick_right"]


def kenney(svg, size):
    with tempfile.TemporaryDirectory() as scratch:
        out = Path(scratch) / "picture.png"
        subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), "-o", str(out), str(KENNEY / svg)],
                       check=True)
        return Image.open(out).copy()


def cropped(image):
    return image.crop(image.getchannel("A").getbbox())


def face(entry):
    """a style's button, as button_assets.py's build draws it, SCALE times its texels"""
    if "source_png" in entry:
        image = Image.open(BUTTONS / entry["source_png"]).convert("RGBA")
        image = cropped(image)
        image.thumbnail((ART, ART), Image.Resampling.LANCZOS)
        return image
    if "classic" in entry:
        text = button_assets.classic(entry["svg"], entry["classic"], entry.get("letter", (0, 0)))
    else:
        text = button_assets.placed(BUTTONS / "svg" / entry["svg"], entry["width"], entry["height"],
                                    entry["place"], entry.get("letter", (0, 0)))
    return cropped(Image.fromarray(button_assets.render(text, SCALE)))


def pictures():
    for name, svg, size in KENNEY_PICTURES:
        yield name, kenney(svg, size)
    entries = {entry["name"]: entry for entry in json.loads(button_assets.LIST.read_text())["assets"]}
    for style, button, entry in FACES:
        yield f"touch_button_{style}_{button}", face(entries[entry])
    for control in CONTROLS:
        yield f"touch_{control}", face(entries[control + "__0"])


def encoded(image):
    data = io.BytesIO()
    image.save(data, "PNG", optimize=True)
    return data.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="compare the PNGs with fresh renders")
    args = parser.parse_args()
    stale = []
    written = set()
    for name, image in pictures():
        target = OUT / (name + ".png")
        written.add(target.name)
        data = encoded(image)
        if args.check:
            if not target.exists() or target.read_bytes() != data:
                stale.append(target.relative_to(ROOT))
        else:
            OUT.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
    # (pictures no longer drawn)
    for old in OUT.glob("touch_*.png"):
        if old.name not in written:
            if args.check:
                stale.append(old.relative_to(ROOT))
            else:
                old.unlink()
    if stale:
        print("Not as tools/touch_assets.py writes them:", *stale, sep="\n  ")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
