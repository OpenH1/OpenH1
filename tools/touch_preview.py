#!/usr/bin/env python3
"""Draws the Android touch controls' default layout over a picture, as the
app draws it, for reviewing a change to their look or layout without a
phone:

    python tools/touch_preview.py --out port/android/docs
    python tools/touch_preview.py --out /tmp/touch --background shot.png

The places and sizes are TouchLayout's own (tools/tests/TouchLayoutDump.java,
compiled with JDK 17+), the pictures the app's (res/drawable-nodpi, from
tools/touch_assets.py); the drawing follows TouchControls.onDraw, whose
constants are repeated here. Each display is drawn with the right trigger
and the stick held, to show the pressed look, in one display.hud_button_style
and with or without input.touch_captions. Without --background the picture
behind is a dark landscape drawn here (no game art). Needs Pillow and NumPy.
"""

import argparse
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
DRAWABLES = ROOT / "port/android/app/src/main/res/drawable-nodpi"
JAVA = ROOT / "port/android/app/src/main/java/com/halo/decomp"

# (name, width, height, dots per inch, display.hud_button_style, input.touch_captions)
DISPLAYS = [
    ("touch-controls-phone-classic", 2400, 1080, 420, "classic", False),
    ("touch-controls-phone-gems", 2400, 1080, 420, "gems", False),
    ("touch-controls-phone-classic-captions", 2400, 1080, 420, "classic", True),
    ("touch-controls-tablet-menu-gems", 2560, 1600, 320, "menu_gems", False),
]

# TouchControls's tables: each kind's controller button (-1 for none), its
# art (a face, a trigger or a stick; None for a Kenney plate), its icon on a
# plate, and the seldom used kinds; the default profile's bindings and the
# game controls' names
KIND_GAMEPAD = [0, 1, 2, 3, 7, 6, 14, 15, 5, 4, 12, 13, 8, 9, 10, 11, -1, 7]
KIND_ART = ["a", "b", "x", "y", "trigger_right", "trigger_left", "thumbstick_left", "thumbstick_right",
            "white", "black", None, None, None, None, None, None, None, "trigger_right"]
KIND_ICON = {10: ("pause", 0), 11: ("back", 0), 12: ("arrow", 270), 13: ("arrow", 90),
             14: ("arrow", 180), 15: ("arrow", 0)}
MINOR_KINDS = {8, 9, 11, 12, 13, 14, 15}
DEFAULT_BINDINGS = [0, 4, 2, 3, 1, 5, 6, 7, -1, -1, -1, -1, 8, 9, 10, 11]
GAME_CONTROLS = ["Jump", "Gren. type", "Reload", "Weapon", "Melee", "Light",
                 "Grenade", "Fire", "Pause", "Back", "Crouch", "Zoom"]
STYLES = {"classic": "classic", "gems": "gems", "menu_gems": "menu"}
STICK = 16
FIRE = 4

# TouchControls's drawing constants
BUTTON_ALPHA = 0.8
MINOR_BUTTON_ALPHA = 0.55
PLATE_ALPHA = 0.6
MINOR_PLATE_ALPHA = 0.4
PRESSED_PLATE_ALPHA = 0.85
STICK_PAD_ALPHA = 0.7
STICK_KNOB_ALPHA = 0.9
MINOR_ICON_ALPHA = 0.7
ART_SIZE = 1.0
ICON_SIZE = 0.5
MINOR_ICON_SIZE = 0.44
PRESSED_SIZE = 0.9
PRESSED_BRIGHTEN = 0x26
CAPTION_SIZE = 10
CAPTION_ALPHA = 0.6
STICK_KNOB_RADIUS = 24
TOOLBAR_RADIUS = 28
OPTIONS_RADIUS = 34


def layout(width, height, dpi):
    with tempfile.TemporaryDirectory() as classes:
        subprocess.run(["javac", "--release", "17", "-d", classes, str(JAVA / "TouchLayout.java"),
                        str(ROOT / "tools/tests/TouchLayoutDump.java")], check=True)
        text = subprocess.run(["java", "-cp", classes, "com.halo.decomp.TouchLayoutDump",
                               str(width), str(height), str(dpi)],
                              check=True, capture_output=True, text=True).stdout
    lines = text.split("\n")
    scale, logical_width, logical_height, finger = (float(v) for v in lines[0].split())
    controls = []
    for line in lines[1:]:
        if line:
            kind, x, y, radius, size, shown = line.split()
            controls.append((int(kind), float(x), float(y), float(radius), float(size), shown == "true"))
    return scale, logical_width, logical_height, finger, controls


def landscape(width, height):
    """a dark, hazy valley at dusk: no game's art"""
    y = np.linspace(0, 1, height)[:, None]
    x = np.linspace(0, 1, width)[None, :]
    sky = np.stack([18 + 30 * y, 26 + 38 * y, 38 + 40 * y], -1) * np.ones((1, width, 1))
    ridge = 0.52 + 0.06 * np.sin(x * 7.0 + 1.3) + 0.03 * np.sin(x * 19.0) + 0.015 * np.sin(x * 53.0)
    near = 0.70 + 0.05 * np.sin(x * 4.0 + 0.4) + 0.02 * np.sin(x * 23.0 + 2.0)
    picture = sky.copy()
    hills = (y > ridge)[..., None]
    picture = np.where(hills, np.stack([22 + 10 * y, 34 + 14 * y, 30 + 8 * y], -1) * np.ones((1, width, 1)), picture)
    ground = (y > near)[..., None]
    picture = np.where(ground, np.stack([30 + 20 * y, 38 + 22 * y, 26 + 10 * y], -1) * np.ones((1, width, 1)), picture)
    glow = np.exp(-((x - 0.62) ** 2 * 18 + (y - 0.45) ** 2 * 30))[..., None] * np.array([40, 34, 20])
    return Image.fromarray(np.clip(picture + glow, 0, 255).astype(np.uint8)).convert("RGBA")


def font(size):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


class Painter:
    def __init__(self, image, scale):
        self.image = image
        self.scale = scale
        self.cache = {}

    def composite(self, drawn, x, y, alpha):
        channels = np.array(drawn, dtype=np.float32)
        channels[..., 3] *= max(0.0, min(1.0, alpha))
        drawn = Image.fromarray(channels.astype(np.uint8))
        layer = Image.new("RGBA", self.image.size)
        layer.paste(drawn, (round(x * self.scale - drawn.width / 2), round(y * self.scale - drawn.height / 2)))
        self.image.alpha_composite(layer)

    def picture(self, name, x, y, radius, alpha, rotate=0, fit=False, brighten=0):
        """TouchControls.picture (square) or fitted (its longer side 2 * radius)"""
        source = Image.open(DRAWABLES / ("touch_" + name + ".png")).convert("RGBA")
        longest = radius * 2 * self.scale
        if fit:
            factor = longest / max(source.size)
            size = (max(1, round(source.width * factor)), max(1, round(source.height * factor)))
        else:
            size = (max(1, round(longest)),) * 2
        drawn = source.resize(size, Image.LANCZOS)
        if brighten:
            channels = np.array(drawn, dtype=np.int32)
            channels[..., :3] = np.minimum(channels[..., :3] + brighten, 255)
            drawn = Image.fromarray(channels.astype(np.uint8))
        if rotate:
            drawn = drawn.rotate(-rotate, resample=Image.BICUBIC)
        self.composite(drawn, x, y, alpha)

    def plate(self, x, y, radius, active, alpha, opacity):
        self.picture("plate", x, y, radius, (PRESSED_PLATE_ALPHA if active else alpha) * opacity)
        if active:
            self.picture("press_ring", x, y, radius, max(opacity, 0.5))

    def text(self, words, x, y, size, alpha, shadow=False):
        layer = Image.new("RGBA", self.image.size)
        draw = ImageDraw.Draw(layer)
        face = font(round(size * self.scale))
        if shadow:
            for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                draw.text((x * self.scale + dx * 2, y * self.scale + dy * 2), words, fill=(0, 0, 0, 160),
                          font=face, anchor="ms")
        draw.text((x * self.scale, y * self.scale), words, fill=(255, 255, 255, round(255 * alpha)),
                  font=face, anchor="ms" if shadow else "mm")
        self.image.alpha_composite(layer)


def draw(name, width, height, dpi, style, captions, background, out):
    scale, logical_width, logical_height, finger, controls = layout(width, height, dpi)
    if background:
        image = Image.open(background).convert("RGBA").resize((width, height), Image.LANCZOS)
    else:
        image = landscape(width, height)
    painter = Painter(image, scale)
    opacity = 1.0
    held = {FIRE, STICK}
    stick_x, stick_y = 0.55, -0.6
    for index, (kind, x, y, radius, size, shown) in enumerate(controls):
        if not shown:
            continue
        active = index in held
        sink = PRESSED_SIZE if active else 1
        minor = kind in MINOR_KINDS
        if kind == STICK:
            knob = max(STICK_KNOB_RADIUS * size, radius * 0.375)
            painter.picture("stick_pad", x, y, radius, STICK_PAD_ALPHA * opacity)
            knob_x, knob_y = x + stick_x * radius, y + stick_y * radius
            painter.picture("stick_knob", knob_x, knob_y, knob, STICK_KNOB_ALPHA * opacity)
            if active:
                painter.picture("press_ring", knob_x, knob_y, knob, max(opacity, 0.5))
            continue
        art = KIND_ART[kind]
        if art:
            if art in ("a", "b", "x", "y", "black", "white"):
                folder = STYLES[style]
                if folder == "gems" and art in ("black", "white"):
                    folder = "menu"
                art = f"button_{folder}_{art}"
            painter.picture(art, x, y, radius * ART_SIZE * sink,
                            (1 if active else MINOR_BUTTON_ALPHA if minor else BUTTON_ALPHA) * opacity, fit=True,
                            brighten=PRESSED_BRIGHTEN if active else 0)
            game = DEFAULT_BINDINGS[KIND_GAMEPAD[kind]]
            if captions and game >= 0:
                painter.text(GAME_CONTROLS[game], x, y + radius + CAPTION_SIZE + 1, CAPTION_SIZE,
                             CAPTION_ALPHA * max(opacity, 0.5), shadow=True)
            continue
        painter.plate(x, y, radius, active, MINOR_PLATE_ALPHA if minor else PLATE_ALPHA, opacity)
        icon, rotate = KIND_ICON[kind]
        painter.picture("icon_" + icon, x, y, radius * (MINOR_ICON_SIZE if minor else ICON_SIZE) * sink,
                        max(opacity, 0.5) * (MINOR_ICON_ALPHA if minor else 1), rotate)
    # the toolbar: Hide and Options' gear
    toolbar_y = 38
    hide_radius = max(TOOLBAR_RADIUS, min(finger, 34))
    options_radius = max(OPTIONS_RADIUS, min(finger, 34))
    painter.plate(logical_width / 2, toolbar_y, hide_radius, False, PLATE_ALPHA, max(opacity, 0.5))
    painter.plate(logical_width - 42, toolbar_y, options_radius, False, PLATE_ALPHA, max(opacity, 0.5))
    painter.picture("icon_options", logical_width - 42, toolbar_y, options_radius * MINOR_ICON_SIZE, max(opacity, 0.5))
    painter.text("Hide", logical_width / 2, toolbar_y, 11, 1)
    out.mkdir(parents=True, exist_ok=True)
    image.convert("RGB").save(out / (name + ".png"), optimize=True)
    print(out / (name + ".png"))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--background", type=Path, help="a picture to draw the controls over")
    args = parser.parse_args()
    for name, width, height, dpi, style, captions in DISPLAYS:
        draw(name, width, height, dpi, style, captions, args.background, args.out)


if __name__ == "__main__":
    main()
