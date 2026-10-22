#!/usr/bin/env python3
"""Draws the Android touch controls' default layout over a picture, as the
app draws it, for reviewing a change to their look or layout without a
phone:

    python tools/touch_preview.py --out port/android/docs
    python tools/touch_preview.py --out /tmp/touch --background shot.png

The places and sizes are TouchLayout's own (tools/tests/TouchLayoutDump.java,
compiled with JDK 17+), the pictures the app's (res/drawable-nodpi, from
tools/touch_assets.py); the drawing follows TouchControls.onDraw, whose
constants are repeated here. Each display is drawn with the fire button and
the stick held, to show the pressed look. Without --background the picture
behind is a dark landscape drawn here (no game art). Needs Pillow and NumPy.
"""

import argparse
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
DRAWABLES = ROOT / "port/android/app/src/main/res/drawable-nodpi"
JAVA = ROOT / "port/android/app/src/main/java/com/halo/decomp"

# (name, width, height, dots per inch, swap triggers)
DISPLAYS = [
    ("touch-controls-phone", 2400, 1080, 420, False),
    ("touch-controls-tablet", 2560, 1600, 320, False),
    ("touch-controls-phone-swap-triggers", 2400, 1080, 420, True),
]

# TouchControls's tables: each kind's controller button (-1 for none) and
# D-pad arrow, the default profile's bindings, each game control's icon and
# whether it is a seldom used one
KIND_GAMEPAD = [0, 1, 2, 3, 7, 6, 14, 15, 5, 4, 12, 13, 8, 9, 10, 11, -1, 7]
KIND_ARROW = {12: 270, 13: 90, 14: 180, 15: 0}
DEFAULT_BINDINGS = [0, 4, 2, 3, 1, 5, 6, 7, -1, -1, -1, -1, 8, 9, 10, 11]
CONTROL_ICONS = ["jump", "grenade_type", "reload", "weapon", "melee", "flashlight",
                 "grenade", "fire", "pause", "back", "crouch", "zoom"]
MINOR_CONTROLS = {1, 5, 9}
STICK = 16
FIRE = 4

# TouchControls's drawing constants
PLATE_ALPHA = 0.6
MINOR_PLATE_ALPHA = 0.4
PRESSED_PLATE_ALPHA = 0.85
STICK_PAD_ALPHA = 0.7
STICK_KNOB_ALPHA = 0.9
MINOR_ICON_ALPHA = 0.7
ICON_SIZE = 0.5
MINOR_ICON_SIZE = 0.44
PRESSED_ICON_SIZE = 0.9
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


class Painter:
    def __init__(self, image, scale):
        self.image = image
        self.scale = scale
        self.cache = {}

    def picture(self, name, x, y, radius, alpha, rotate=0):
        """TouchControls.picture: a square picture centred on a point (logical units)"""
        side = max(1, round(radius * 2 * self.scale))
        key = (name, side, rotate)
        if key not in self.cache:
            source = Image.open(DRAWABLES / ("touch_" + name + ".png")).convert("RGBA")
            drawn = source.resize((side, side), Image.LANCZOS)
            if rotate:
                drawn = drawn.rotate(-rotate, resample=Image.BICUBIC)
            self.cache[key] = drawn
        drawn = self.cache[key].copy()
        channels = np.array(drawn, dtype=np.float32)
        channels[..., 3] *= max(0.0, min(1.0, alpha))
        drawn = Image.fromarray(channels.astype(np.uint8))
        left = round(x * self.scale - side / 2)
        top = round(y * self.scale - side / 2)
        layer = Image.new("RGBA", self.image.size)
        layer.paste(drawn, (left, top))
        self.image.alpha_composite(layer)

    def plate(self, x, y, radius, active, alpha, opacity):
        self.picture("plate", x, y, radius, (PRESSED_PLATE_ALPHA if active else alpha) * opacity)
        if active:
            self.picture("press_ring", x, y, radius, max(opacity, 0.5))


def draw(name, width, height, dpi, swap, background, out):
    scale, logical_width, logical_height, finger, controls = layout(width, height, dpi)
    if background:
        image = Image.open(background).convert("RGBA").resize((width, height), Image.LANCZOS)
    else:
        image = landscape(width, height)
    bindings = list(DEFAULT_BINDINGS)
    if swap:
        bindings[6], bindings[7] = bindings[7], bindings[6]
    painter = Painter(image, scale)
    opacity = 1.0
    held = {FIRE, STICK}
    stick_x, stick_y = 0.55, -0.6
    for index, (kind, x, y, radius, size, shown) in enumerate(controls):
        if not shown:
            continue
        active = index in held
        if kind == STICK:
            knob = max(STICK_KNOB_RADIUS * size, radius * 0.375)
            painter.picture("stick_pad", x, y, radius, STICK_PAD_ALPHA * opacity)
            knob_x, knob_y = x + stick_x * radius, y + stick_y * radius
            painter.picture("stick_knob", knob_x, knob_y, knob, STICK_KNOB_ALPHA * opacity)
            if active:
                painter.picture("press_ring", knob_x, knob_y, knob, max(opacity, 0.5))
            continue
        game = bindings[KIND_GAMEPAD[kind]] if KIND_GAMEPAD[kind] >= 0 else -1
        minor = game in MINOR_CONTROLS if game >= 0 else kind in KIND_ARROW
        painter.plate(x, y, radius, active, MINOR_PLATE_ALPHA if minor else PLATE_ALPHA, opacity)
        icon_opacity = max(opacity, 0.5) * (MINOR_ICON_ALPHA if minor else 1)
        icon_size = radius * (MINOR_ICON_SIZE if minor else ICON_SIZE) * (PRESSED_ICON_SIZE if active else 1)
        if game >= 0:
            painter.picture("icon_" + CONTROL_ICONS[game], x, y, icon_size, icon_opacity)
        elif kind in KIND_ARROW:
            painter.picture("icon_arrow", x, y, icon_size, icon_opacity, KIND_ARROW[kind])
    # the toolbar: Hide (its word, not drawn here) and Options' gear
    toolbar_y = 38
    hide_radius = max(TOOLBAR_RADIUS, min(finger, 34))
    options_radius = max(OPTIONS_RADIUS, min(finger, 34))
    painter.plate(logical_width / 2, toolbar_y, hide_radius, False, PLATE_ALPHA, max(opacity, 0.5))
    painter.plate(logical_width - 42, toolbar_y, options_radius, False, PLATE_ALPHA, max(opacity, 0.5))
    painter.picture("icon_options", logical_width - 42, toolbar_y, options_radius * MINOR_ICON_SIZE, max(opacity, 0.5))
    _label_hide(image, logical_width / 2 * scale, toolbar_y * scale, scale)
    out.mkdir(parents=True, exist_ok=True)
    image.convert("RGB").save(out / (name + ".png"), optimize=True)
    print(out / (name + ".png"))


def _label_hide(image, x, y, scale):
    """the toolbar's "Hide", in the default font Pillow has"""
    from PIL import ImageDraw, ImageFont
    try:
        font = ImageFont.load_default(size=round(11 * scale))
    except TypeError:
        font = ImageFont.load_default()
    ImageDraw.Draw(image).text((x, y), "Hide", fill=(255, 255, 255, 255), font=font, anchor="mm")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--background", type=Path, help="a picture to draw the controls over")
    args = parser.parse_args()
    for name, width, height, dpi, swap in DISPLAYS:
        draw(name, width, height, dpi, swap, args.background, args.out)


if __name__ == "__main__":
    main()
