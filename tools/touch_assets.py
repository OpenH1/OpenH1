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
  style ("classic", "gems", "menu_gems"), the triggers and the sticks;
  each with the same thin light edge and soft drop shadow, so that it reads
  over any scene with nothing drawn behind it.
- The small controls, which have no redraw (the D-pad, start, back and
  the toolbar's hide, show and options), are line icons: the outlines of
  Kenney's Mobile Controls' glyphs (port/assets/touch/kenney-mobile-controls,
  CC0), in the HUD's colour, with the same soft shadow as the buttons.
- The captions' font, Overpass (port/assets/fonts, OFL), is copied into
  res/font/touch_caption.ttf, and its licence into
  res/raw/touch_caption_ofl.txt, which the APK carries with it.

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

from PIL import Image, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parent))
import button_assets  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
KENNEY = ROOT / "port/assets/touch/kenney-mobile-controls"
BUTTONS = button_assets.ASSETS
OUT = ROOT / "port/android/app/src/main/res/drawable-nodpi"

FONTS = ROOT / "port/assets/fonts"
FONT_OUT = ROOT / "port/android/app/src/main/res/font/touch_caption.ttf"
LICENCE_OUT = ROOT / "port/android/app/src/main/res/raw/touch_caption_ofl.txt"

# a line icon is drawn this many pixels across (a picture made smaller with
# mipmaps stays sharp), its line this many wide, in the HUD's colour
# (TouchControls.HUD_COLOR)
ICON = 192
LINE = 11
HUD_COLOR = (0x9c, 0xd3, 0xff)
# the redraws' texels drawn at this many pixels (a face, 26 texels high: 416)
SCALE = 16
# the supplied PNGs' art, at most this many pixels on a side
ART = 416
# every button's art gets the same thin light edge and soft dark shadow (in
# pixels of its longer side's ART), so that it reads on dark and light
# scenes alike (the classic black on a night sky) with nothing behind it
EDGE = 5
EDGE_ALPHA = 0.55
SHADOW_BLUR = 10
SHADOW_OFFSET = 4
SHADOW_ALPHA = 0.6

# (the resource's name, Kenney's SVG)
KENNEY_PICTURES = [
    ("touch_icon_pause", "icon_pause.svg"),
    ("touch_icon_back", "icon_menu.svg"),
    ("touch_icon_arrow", "icon_arrow.svg"),
    ("touch_icon_options", "icon_cog.svg"),
    ("touch_icon_hide", "icon_size_smaller.svg"),
    ("touch_icon_show", "icon_size_larger.svg"),
]

# each style's buttons (hud_hires_tags.c's hud_hires_button_draw): the
# resource's name and buttons.json's entry; "gems" has no black and white
# of its own, and draws menu_gems' (TouchControls.java)
FACES = [("classic", button, f"classic_{button}__0") for button in ("a", "b", "x", "y", "black", "white")] + \
    [("gems", button, f"gem_{button}__0") for button in ("a", "b", "x", "y")] + \
    [("menu", button, f"{button}_butn__0") for button in ("a", "b", "x", "y", "black", "white")]
CONTROLS = ["trigger_left", "trigger_right", "thumbstick_left", "thumbstick_right"]


def kenney(svg):
    """a glyph's outline, a line icon in the HUD's colour, its shadow under it"""
    inner = ICON - 2 * LINE
    with tempfile.TemporaryDirectory() as scratch:
        out = Path(scratch) / "picture.png"
        subprocess.run(["rsvg-convert", "-w", str(inner), "-h", str(inner), "-o", str(out), str(KENNEY / svg)],
                       check=True)
        # (solid: a glyph drawn in two parts has no seam between them)
        glyph = Image.open(out).getchannel("A").point(lambda v: 255 if v >= 96 else 0)
    alpha = Image.new("L", (ICON, ICON))
    alpha.paste(glyph, (LINE, LINE))
    half = LINE // 2
    outer = alpha.filter(ImageFilter.MaxFilter(2 * half + 1))
    inside = alpha.filter(ImageFilter.MinFilter(2 * half + 1))
    line = Image.frombytes("L", outer.size, bytes(max(0, a - b) for a, b in zip(outer.tobytes(), inside.tobytes())))
    icon = Image.new("RGBA", (ICON, ICON), HUD_COLOR + (0,))
    icon.putalpha(line.filter(ImageFilter.GaussianBlur(0.6)))
    return shadowed(icon)


def shadowed(image):
    """an icon with the buttons' soft shadow (scaled to its size), on room enough for it"""
    factor = image.width / ART
    blur = max(1, round(SHADOW_BLUR * factor * 2))
    offset = max(1, round(SHADOW_OFFSET * factor * 2))
    pad = 2 * blur + offset
    alpha = Image.new("L", (image.width + 2 * pad, image.height + 2 * pad))
    alpha.paste(image.getchannel("A"), (pad, pad))
    shadow = alpha.filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.GaussianBlur(blur))
    out = Image.new("RGBA", alpha.size)
    dark = Image.new("RGBA", alpha.size, (0, 0, 0, 0))
    dark.putalpha(shadow.point(lambda v: round(v * SHADOW_ALPHA)))
    out.alpha_composite(dark, (0, offset))
    out.alpha_composite(image, (pad, pad))
    return out


def without_words(image):
    """a stick's art without the word over it (LEFT, RIGHT): the rows below the
    first band of empty rows under the word"""
    alpha = image.getchannel("A")
    rows = [any(alpha.crop((0, y, image.width, y + 1)).getdata()) for y in range(image.height)]
    top = rows.index(True)
    gap = rows.index(False, top)
    start = rows.index(True, gap)
    return cropped(image.crop((0, start, image.width, image.height)))


def cropped(image):
    return image.crop(image.getchannel("A").getbbox())


def face(entry, words=False):
    """a style's button, as button_assets.py's build draws it, SCALE times its texels"""
    if "source_png" in entry:
        image = Image.open(BUTTONS / entry["source_png"]).convert("RGBA")
        image = cropped(image)
        if words:
            image = without_words(image)
        image.thumbnail((ART, ART), Image.Resampling.LANCZOS)
        return image
    if "classic" in entry:
        text = button_assets.classic(entry["svg"], entry["classic"], entry.get("letter", (0, 0)))
    else:
        text = button_assets.placed(BUTTONS / "svg" / entry["svg"], entry["width"], entry["height"],
                                    entry["place"], entry.get("letter", (0, 0)))
    return cropped(Image.fromarray(button_assets.render(text, SCALE)))


def outlined(image):
    """the art with its light edge and drop shadow, on room enough for both"""
    pad = EDGE + SHADOW_BLUR * 2 + SHADOW_OFFSET
    alpha = Image.new("L", (image.width + 2 * pad, image.height + 2 * pad))
    alpha.paste(image.getchannel("A"), (pad, pad))
    edge = alpha.filter(ImageFilter.MaxFilter(2 * EDGE + 1)).filter(ImageFilter.GaussianBlur(1))
    shadow = edge.filter(ImageFilter.GaussianBlur(SHADOW_BLUR))
    out = Image.new("RGBA", alpha.size)
    dark = Image.new("RGBA", alpha.size, (0, 0, 0, 0))
    dark.putalpha(shadow.point(lambda v: round(v * SHADOW_ALPHA)))
    out.alpha_composite(dark, (0, SHADOW_OFFSET))
    light = Image.new("RGBA", alpha.size, (255, 255, 255, 0))
    light.putalpha(edge.point(lambda v: round(v * EDGE_ALPHA)))
    out.alpha_composite(light)
    out.alpha_composite(image, (pad, pad))
    return out


def pictures():
    for name, svg in KENNEY_PICTURES:
        yield name, kenney(svg)
    entries = {entry["name"]: entry for entry in json.loads(button_assets.LIST.read_text())["assets"]}
    for style, button, entry in FACES:
        yield f"touch_button_{style}_{button}", outlined(face(entries[entry]))
    for control in CONTROLS:
        yield f"touch_{control}", outlined(face(entries[control + "__0"], control.startswith("thumbstick")))


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
    # (the font, and its licence, which travels with it in the APK)
    for source, target in ((FONTS / "Overpass-750.ttf", FONT_OUT), (FONTS / "Overpass-OFL.txt", LICENCE_OUT)):
        data = source.read_bytes()
        if args.check:
            if not target.exists() or target.read_bytes() != data:
                stale.append(target.relative_to(ROOT))
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
    if stale:
        print("Not as tools/touch_assets.py writes them:", *stale, sep="\n  ")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
