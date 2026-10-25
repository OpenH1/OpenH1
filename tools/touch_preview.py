#!/usr/bin/env python3
"""Draws the Android touch controls' default layout over a picture, as the
app draws it, for reviewing a change to their look or layout without a
phone:

    python tools/touch_preview.py --out port/android/docs
    python tools/touch_preview.py --out /tmp/touch --background shot.png
    python tools/touch_preview.py --out /tmp/touch --size 2184x1968@360 --size 2520x1080@360

The places and sizes are TouchLayout's own (tools/tests/TouchLayoutDump.java,
compiled with JDK 17+), the pictures the app's (res/drawable-nodpi, from
tools/touch_assets.py); the drawing follows TouchControls.onDraw, whose
constants are repeated here. Each display is drawn in one
display.hud_button_style, with the captions (input.touch_captions), and
either with the right trigger and the stick held (the pressed look) or idle
(faded, no finger on the screen), and in a player's state (TouchContext):
an action prompted, a magazine to reload, no grenades, a vehicle. Without --background the picture behind
is a dark landscape drawn here (no game art). --size draws the default layout
at a display of your own, such as a folding phone's inner screen or a 21:9
cover screen, in place of the list. A display taller than it is wide is
portrait: the picture (--background: the game's own 4:3 or 16:9 frame, as
--picture-shape) goes where TouchControls places it, under the strip, with
--frame's look (display.frame_style) around it. Needs Pillow and NumPy.
"""

import argparse
import hashlib
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


def layout(width, height, dpi, shape="4:3"):
    with tempfile.TemporaryDirectory() as classes:
        subprocess.run(["javac", "--release", "17", "-d", classes, str(JAVA / "TouchLayout.java"),
                        str(ROOT / "tools/tests/TouchLayoutDump.java")], check=True)
        text = subprocess.run(["java", "-cp", classes, "com.halo.decomp.TouchLayoutDump",
                               str(width), str(height), str(dpi), shape],
                              check=True, capture_output=True, text=True).stdout
    lines = text.split("\n")
    scale, logical_width, logical_height, finger, toolbar_y, hide_x, top, bottom = (float(v) for v in lines[0].split())
    controls = []
    for line in lines[1:]:
        if line:
            kind, x, y, radius, size, shown = line.split()
            controls.append((int(kind), float(x), float(y), float(radius), float(size), shown == "true"))
    picture = (top, bottom) if top >= 0 else None
    return scale, logical_width, logical_height, finger, toolbar_y, hide_x, picture, controls


def draw_frame(image, s, look, width, height, strip, picture):
    """TouchControls.recordFrame: display.frame_style's look around the picture (top, bottom in logical units),
    clipped out of it"""
    green = (0x78, 0xcd, 0x3c)
    accent = HUD_COLOR if look == "halo" else green
    W, H = image.size
    rows = np.arange(H)[:, None]
    if look == "halo":
        shade = rows / max(1, H - 1)
        rgb = np.array([5, 9, 17]) + shade[..., None] * np.array([6, 10, 18])
        rgb = rgb * np.ones((1, W, 1))
        rgb[(np.floor(rows[:, 0] / s) % 3 == 0)] *= 1 - 46 / 255   # scan lines, a unit in three
    else:
        rgb = np.array([7, 9, 7]) * np.ones((H, W, 1))
    layer = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8)).convert("RGBA")
    draw = ImageDraw.Draw(layer)
    x, y = width / 2, picture[1] + (height - picture[1]) / 2
    if look == "halo":
        for radius, alpha in ((150, 0.10), (205, 0.13), (212, 0.07), (300, 0.055)):
            draw.ellipse([(x - radius) * s, (y - radius) * s, (x + radius) * s, (y + radius) * s],
                         outline=accent + (round(255 * alpha),), width=max(1, round(0.75 * s)))
        for tick in range(72):
            turn = tick * math.pi / 36
            outer = 228 if tick % 6 == 0 else 218
            draw.line([(x + 212 * math.cos(turn)) * s, (y + 212 * math.sin(turn)) * s,
                       (x + outer * math.cos(turn)) * s, (y + outer * math.sin(turn)) * s],
                      fill=accent + (31,), width=max(1, round(0.75 * s)))
    else:
        line = -height
        while line < width:
            draw.line([line * s, 0, (line + height) * s, height * s], fill=(12, 18, 11, 255), width=max(1, round(1.5 * s)))
            line += 6
        yy, xx = np.mgrid[0:H, 0:W]
        fade = np.clip(1 - np.hypot(xx / s - x, yy / s - y) / 230, 0, 1) * 0.27 * 255
        glow = Image.fromarray(np.dstack([np.full((H, W), c, np.uint8) for c in accent] + [fade.astype(np.uint8)]))
        layer.alpha_composite(glow)
        draw = ImageDraw.Draw(layer)
        for radius, alpha in ((120, 0.24), (126, 0.1)):
            draw.ellipse([(x - radius) * s, (y - radius) * s, (x + radius) * s, (y + radius) * s],
                         outline=accent + (round(255 * alpha),), width=max(1, round(1.5 * s)))
    strip_fill = Image.new("RGBA", image.size)
    ImageDraw.Draw(strip_fill).rectangle([0, 0, W, strip * s],
                                         fill=(6, 11, 20, 235) if look == "halo" else (10, 13, 10, 235))
    layer.alpha_composite(strip_fill)
    marks = Image.new("RGBA", image.size)
    draw = ImageDraw.Draw(marks)
    thin, thick = max(1, round(1.5 * s)), max(1, round(2 * s))
    draw.line([0, strip * s, W, strip * s], fill=accent + (150,), width=thin)
    top, bottom, gap = picture[0], picture[1], 4
    if look == "halo":
        draw.line([(x - 150) * s, strip * s, (x - 130) * s, (strip - 8) * s, (x + 130) * s, (strip - 8) * s,
                   (x + 150) * s, strip * s], fill=accent + (110,), width=thin)
        for corner in range(4):
            side = 1 if corner % 2 == 0 else -1
            corner_x = 6 if corner % 2 == 0 else width - 6
            line_y = top - gap if corner < 2 else bottom + gap
            out = -1 if corner < 2 else 1
            draw.line([corner_x * s, (line_y + out * 11) * s, corner_x * s, line_y * s,
                       (corner_x + side * 22) * s, line_y * s], fill=accent + (199,), width=thick)
        draw.line([0, (bottom + 0.75) * s, W, (bottom + 0.75) * s], fill=accent + (130,), width=thin)
    else:
        draw.rounded_rectangle([-gap * s, (top - gap) * s, (width + gap) * s, (bottom + gap) * s], radius=10 * s,
                               outline=accent + (171,), width=thick)
    layer.alpha_composite(marks)
    # (clipped out of the picture, as canvas.clipOutRect)
    alpha = np.array(layer.getchannel("A"))
    alpha[round(top * s):round(bottom * s), :] = 0
    layer.putalpha(Image.fromarray(alpha))
    inside = (0, round(top * s), W, round(bottom * s))
    before = hashlib.sha256(image.crop(inside).tobytes()).hexdigest()
    image.alpha_composite(layer)
    assert hashlib.sha256(image.crop(inside).tobytes()).hexdigest() == before, "the frame drew over the picture"


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


def draw(name, width, height, dpi, style, idle, state, background, out, frame="plain", shape="4:3"):
    scale, logical_width, logical_height, finger, toolbar_y, hide_x, picture, controls = layout(width, height, dpi,
                                                                                               shape)
    if picture:
        # portrait: the game's picture where TouchControls.placePicture puts it, black around it
        image = Image.new("RGBA", (width, height), (0, 0, 0, 255))
        top, bottom = round(picture[0] * scale), round(picture[1] * scale)
        shot = Image.open(background).convert("RGBA") if background else landscape(width, bottom - top)
        image.paste(shot.resize((width, bottom - top), Image.LANCZOS), (0, top))
        if frame != "plain":
            strip = toolbar_y + 36
            draw_frame(image, scale, frame, logical_width, logical_height, strip, picture)
    elif background:
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
    hide_radius = max(TOOLBAR_RADIUS, min(finger, 34))
    options_radius = max(OPTIONS_RADIUS, min(finger, 34))
    toolbar = max(opacity, 0.5) * ICON_ALPHA
    painter.picture("icon_hide", hide_x, toolbar_y, hide_radius * MINOR_ICON_SIZE, toolbar, fit=True)
    painter.picture("icon_options", logical_width - 42, toolbar_y, options_radius * MINOR_ICON_SIZE, toolbar, fit=True)
    out.mkdir(parents=True, exist_ok=True)
    image.convert("RGB").save(out / (name + ".png"), optimize=True)
    print(out / (name + ".png"))


def parse_size(text):
    """a display given as WIDTHxHEIGHT@DPI, such as 2184x1968@360"""
    pixels, _, dpi = text.partition("@")
    width, _, height = pixels.partition("x")
    if not all(part.isdecimal() and int(part) > 0 for part in (width, height, dpi)):
        raise argparse.ArgumentTypeError(f"{text!r}: WIDTHxHEIGHT@DPI expected, such as 2184x1968@360")
    return int(width), int(height), int(dpi)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--background", type=Path, help="a picture to draw the controls over")
    parser.add_argument("--size", type=parse_size, action="append", metavar="WxH@DPI",
                        help="draw the default layout at this display (pixels and dots per inch) "
                             "instead of the list; repeatable")
    parser.add_argument("--frame", choices=["plain", "halo", "xbox"], default="plain",
                        help="portrait: display.frame_style's look around the picture")
    parser.add_argument("--picture-shape", choices=["4:3", "16:9"], default="4:3",
                        help="portrait: display.picture_shape")
    args = parser.parse_args()
    if args.size:
        for width, height, dpi in args.size:
            suffix = "" if height <= width else "-" + args.frame + ("-169" if args.picture_shape == "16:9" else "")
            draw("touch-controls-" + str(width) + "x" + str(height) + suffix, width, height, dpi, "classic", False, 0,
                 args.background, args.out, args.frame, args.picture_shape)
        return
    for name, width, height, dpi, style, idle, state in DISPLAYS:
        draw(name, width, height, dpi, style, idle, state, args.background, args.out)


if __name__ == "__main__":
    main()
