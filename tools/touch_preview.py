#!/usr/bin/env python3
"""Draws the Android touch controls' default layout over a picture, as the
app draws it, for reviewing a change to their look or layout without a
phone:

    python tools/touch_preview.py --out port/android/docs
    python tools/touch_preview.py --out /tmp/touch --background shot.png

The places and sizes are TouchLayout's own (tools/tests/TouchLayoutDump.java,
compiled with JDK 17+), the pictures the app's (res/drawable-nodpi, from
tools/touch_assets.py); the drawing follows TouchControls.onDraw, whose
constants are repeated here. Each display is drawn in one
display.hud_button_style, with the captions (input.touch_captions), and
either with the right trigger and the stick held (the pressed look) or idle
(faded, no finger on the screen), and in a player's state (TouchContext):
an action prompted, a magazine to reload, no grenades, a vehicle. Without --background the picture behind
is a dark landscape drawn here (no game art). Needs Pillow and NumPy.
"""

import argparse
import math
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
DRAWABLES = ROOT / "port/android/app/src/main/res/drawable-nodpi"
JAVA = ROOT / "port/android/app/src/main/java/com/halo/decomp"

# player 1's state (TouchContext's bits) and the time of the pulse drawn
ACTION, RELOAD, NO_GRENADES, VEHICLE = 1, 2, 4, 8
PULSE_PEAK = 312  # milliseconds into the reload pulse: its brightest (1.6 Hz)

# (name, width, height, dots per inch, display.hud_button_style, idle, state)
DISPLAYS = [
    ("touch-controls-phone-classic", 2400, 1080, 420, "classic", False, 0),
    ("touch-controls-phone-gems", 2400, 1080, 420, "gems", False, 0),
    ("touch-controls-phone-classic-idle", 2400, 1080, 420, "classic", True, 0),
    ("touch-controls-tablet-menu-gems", 2560, 1600, 320, "menu_gems", False, 0),
    ("touch-controls-phone-action-no-grenades", 2400, 1080, 420, "classic", True, ACTION | NO_GRENADES),
    ("touch-controls-phone-reload", 2400, 1080, 420, "classic", True, RELOAD),
    ("touch-controls-phone-vehicle", 2400, 1080, 420, "classic", True, VEHICLE),
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
MINOR_KINDS = {6, 7, 8, 9, 11, 12, 13, 14, 15}
DEFAULT_BINDINGS = [0, 4, 2, 3, 1, 5, 6, 7, -1, -1, -1, -1, 8, 9, 10, 11]
CAPTIONS = ["JUMP", "GREN TYPE", "RELOAD", "SWAP", "MELEE", "LIGHT",
            "GRENADE", "FIRE", "PAUSE", "BACK", "CROUCH", "ZOOM"]
STYLES = {"classic": "classic", "gems": "gems", "menu_gems": "menu"}
STICK = 16
FIRE = 4

# TouchControls's drawing constants
BUTTON_ALPHA = 0.8
MINOR_BUTTON_ALPHA = 0.55
ICON_ALPHA = 0.85
MINOR_ICON_ALPHA = 0.6
STICK_RING_ALPHA = 0.35
STICK_KNOB_ALPHA = 0.55
ART_SIZE = 1.0
ICON_SIZE = 0.62
MINOR_ICON_SIZE = 0.56
PRESSED_SIZE = 0.9
PRESSED_BRIGHTEN = 0x26
HUD_COLOR = (0x9c, 0xd3, 0xff)
CAPTION_SIZE = 9
CAPTION_ALPHA = 0.85
IDLE_OPACITY = 0.7
STICK_KNOB_RADIUS = 24
TOOLBAR_RADIUS = 28
OPTIONS_RADIUS = 34
GLOW_MARGIN = 0.25
GLOW_GROW = 0.14
DIMMED = 0.35
ACTION_GLOW = 1.0
PULSE_HZ = 1.6
PULSE_SWELL = 0.06
CONTROL_JUMP, CONTROL_ACTION, CONTROL_GRENADE, CONTROL_CROUCH = 0, 2, 6, 10
CAPTION_FONT = ROOT / "port/android/app/src/main/res/font/touch_caption.ttf"


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
    return ImageFont.truetype(str(CAPTION_FONT), size)


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

    def glow(self, name, x, y, radius, alpha):
        """TouchControls.glow: the art's shape blurred, in the HUD's colour, past it by GLOW_MARGIN"""
        source = Image.open(DRAWABLES / ("touch_" + name + ".png")).convert("RGBA")
        margin = round(max(source.size) * GLOW_MARGIN)
        shape = Image.new("L", (source.width + 2 * margin, source.height + 2 * margin))
        grown = source.getchannel("A").resize((round(source.width * (1 + GLOW_GROW)),
                                               round(source.height * (1 + GLOW_GROW))), Image.LANCZOS)
        shape.paste(grown, (margin - round(source.width * GLOW_GROW / 2), margin - round(source.height * GLOW_GROW / 2)))
        glow = Image.new("RGBA", shape.size, HUD_COLOR + (0,))
        glow.putalpha(shape.filter(ImageFilter.GaussianBlur(margin * 0.25)))
        factor = radius * 2 * self.scale / max(glow.size)
        glow = glow.resize((max(1, round(glow.width * factor)), max(1, round(glow.height * factor))), Image.LANCZOS)
        self.composite(glow, x, y, alpha)

    def caption(self, words, x, y, alpha):
        """TouchControls.caption: Overpass, the HUD's colour, a soft shadow"""
        size = CAPTION_SIZE * self.scale
        face = font(round(size))
        spacing = 0.06 * size
        widths = [face.getlength(c) + spacing for c in words]
        left = x * self.scale - (sum(widths) - spacing) / 2
        shadow = Image.new("RGBA", self.image.size)
        layer = Image.new("RGBA", self.image.size)
        for target, fill, dy in ((shadow, (0, 0, 0, round(204 * alpha)), self.scale),
                                 (layer, HUD_COLOR + (round(255 * CAPTION_ALPHA * alpha),), 0)):
            draw = ImageDraw.Draw(target)
            pen = left
            for c, w in zip(words, widths):
                draw.text((pen, y * self.scale + dy), c, fill=fill, font=face, anchor="ls")
                pen += w
        self.image.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(1.5 * self.scale)))
        self.image.alpha_composite(layer)

    def stick(self, x, y, radius, knob_x, knob_y, knob, active, opacity):
        s = self.scale
        layer = Image.new("RGBA", self.image.size)
        draw = ImageDraw.Draw(layer)
        draw.ellipse([(x - radius + 1) * s, (y - radius + 1) * s, (x + radius - 1) * s, (y + radius - 1) * s],
                     outline=HUD_COLOR + (round(255 * STICK_RING_ALPHA * opacity),), width=round(2 * s))
        draw.ellipse([(knob_x - knob) * s, (knob_y - knob) * s, (knob_x + knob) * s, (knob_y + knob) * s],
                     fill=HUD_COLOR + (round(255 * (0.8 if active else STICK_KNOB_ALPHA) * opacity),))
        shadow = Image.new("RGBA", self.image.size, (0, 0, 0, 0))
        shadow.putalpha(layer.getchannel("A").point(lambda v: v // 2).filter(ImageFilter.GaussianBlur(3 * s)))
        self.image.alpha_composite(shadow, (0, round(s)))
        self.image.alpha_composite(layer)


def pulse(millis):
    return 0.5 - 0.5 * math.cos(millis / 1000 * PULSE_HZ * 2 * math.pi)


def draw(name, width, height, dpi, style, idle, state, background, out):
    scale, logical_width, logical_height, finger, controls = layout(width, height, dpi)
    if background:
        image = Image.open(background).convert("RGBA").resize((width, height), Image.LANCZOS)
    else:
        image = landscape(width, height)
    painter = Painter(image, scale)
    opacity = IDLE_OPACITY if idle else 1.0
    held = set() if idle else {FIRE, STICK}
    stick_x, stick_y = (0, 0) if idle else (0.55, -0.6)
    for index, (kind, x, y, radius, size, shown) in enumerate(controls):
        if not shown:
            continue
        active = index in held
        sink = PRESSED_SIZE if active else 1
        minor = kind in MINOR_KINDS
        brighten = PRESSED_BRIGHTEN if active else 0
        if kind == STICK:
            knob = max(STICK_KNOB_RADIUS * size, radius * 0.375)
            painter.stick(x, y, radius, x + stick_x * radius, y + stick_y * radius, knob, active, opacity)
            continue
        art = KIND_ART[kind]
        game = DEFAULT_BINDINGS[KIND_GAMEPAD[kind]] if KIND_GAMEPAD[kind] >= 0 else -1
        if state & VEHICLE and game in (CONTROL_JUMP, CONTROL_CROUCH):
            continue
        if art:
            if art in ("a", "b", "x", "y", "black", "white"):
                folder = STYLES[style]
                if folder == "gems" and art in ("black", "white"):
                    folder = "menu"
                art = f"button_{folder}_{art}"
            glow = 0
            swell = 1
            if game == CONTROL_ACTION and state & RELOAD:
                glow = 0.2 + 0.8 * pulse(PULSE_PEAK)
                swell = 1 + PULSE_SWELL * pulse(PULSE_PEAK)
            elif game == CONTROL_ACTION and state & ACTION:
                glow = ACTION_GLOW
            dim = DIMMED if game == CONTROL_GRENADE and state & NO_GRENADES else 1
            size_drawn = radius * ART_SIZE * sink * swell
            if glow:
                painter.glow(art, x, y, size_drawn * (1 + 2 * GLOW_MARGIN), glow * max(opacity, 0.5))
            painter.picture(art, x, y, size_drawn,
                            (1 if active else MINOR_BUTTON_ALPHA if minor else BUTTON_ALPHA) * opacity * dim, fit=True,
                            brighten=PRESSED_BRIGHTEN if active or glow > 0.6 else 0)
            if game >= 0:
                source = Image.open(DRAWABLES / ("touch_" + art + ".png"))
                bottom = y + radius * ART_SIZE * source.height / max(source.size)
                words = "EXIT" if state & VEHICLE and game == CONTROL_ACTION else CAPTIONS[game]
                painter.caption(words, x, bottom + CAPTION_SIZE + 1, max(opacity, 0.5) * dim)
            continue
        icon, rotate = KIND_ICON[kind]
        painter.picture("icon_" + icon, x, y, radius * (MINOR_ICON_SIZE if minor else ICON_SIZE) * sink,
                        (1 if active else MINOR_ICON_ALPHA if minor else ICON_ALPHA) * max(opacity, 0.5),
                        rotate, fit=True, brighten=brighten)
    # the toolbar: hide and options, line icons
    toolbar_y = 38
    hide_radius = max(TOOLBAR_RADIUS, min(finger, 34))
    options_radius = max(OPTIONS_RADIUS, min(finger, 34))
    toolbar = max(opacity, 0.5) * ICON_ALPHA
    painter.picture("icon_hide", logical_width / 2, toolbar_y, hide_radius * MINOR_ICON_SIZE, toolbar, fit=True)
    painter.picture("icon_options", logical_width - 42, toolbar_y, options_radius * MINOR_ICON_SIZE, toolbar, fit=True)
    out.mkdir(parents=True, exist_ok=True)
    image.convert("RGB").save(out / (name + ".png"), optimize=True)
    print(out / (name + ".png"))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--background", type=Path, help="a picture to draw the controls over")
    args = parser.parse_args()
    for name, width, height, dpi, style, idle, state in DISPLAYS:
        draw(name, width, height, dpi, style, idle, state, args.background, args.out)


if __name__ == "__main__":
    main()
