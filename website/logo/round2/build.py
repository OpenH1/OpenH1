#!/usr/bin/env python3
"""Draws round 2 of OpenH1's logo concepts and the board that compares them:

    python website/logo/round2/build.py

Ten concepts in very different styles, several of them in the look of the
early-2000s console era (black plastic with an inner light, chrome bevels,
glowing tubes, CRTs, big-box art), each original. Each mark is drawn in a
square of 100 units; its glow and bevel effects drop out of its favicon,
which is drawn by hand on the 16-pixel grid. For each concept it writes,
into website/logo/round2/<concept>/:

- mark.svg, mark-light.svg, mark-mono.svg: the mark for dark backgrounds,
  light ones, and one colour (currentColor, its inner shapes cut out);
- lockup.svg, lockup-light.svg, lockup-mono.svg: the mark with the wordmark,
  its letters as outlines (Overpass, website/public/fonts, or drawn here);
- icon.svg: the app icon, a full-bleed square of 1024 for the platforms'
  masks (tools/app_icon.py);
- favicon.svg: the small version, on the 16-pixel grid.

and website/logo-board-2.html, which shows them all. Needs fontTools and
uharfbuzz, and round 1's website/logo/build.py beside it.
"""

import html
import importlib.util
import math
from pathlib import Path

import uharfbuzz as hb
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

HERE = Path(__file__).resolve().parent
WEBSITE = HERE.parent.parent
BOARD = WEBSITE / "logo-board-2.html"

_spec = importlib.util.spec_from_file_location("round1", HERE.parent / "build.py")
round1 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(round1)
num, svg, uri, img, tab, pixels = round1.num, round1.svg, round1.uri, round1.img, round1.tab, round1.pixels


# ---------------------------------------------------------------- letters

class Font:
    def __init__(self, weight: int):
        path = WEBSITE / f"public/fonts/Overpass-{weight}.ttf"
        self.hb = hb.Font(hb.Face(hb.Blob.from_file_path(str(path))))
        tt = TTFont(path)
        self.glyphs = tt.getGlyphSet()
        self.order = tt.getGlyphOrder()
        self.cap = tt["OS/2"].sCapHeight


FONTS = {}


def font(weight: int) -> Font:
    if weight not in FONTS:
        FONTS[weight] = Font(weight)
    return FONTS[weight]


def text(t: str, weight: int, cap: float, x: float, baseline: float, tracking: float = 0.0,
         skew: float = 0.0) -> tuple:
    """t in Overpass, its caps cap high, from x: (path, width, glyphs), each
    glyph (character, left, right) of its outline"""
    f = font(weight)
    buf = hb.Buffer()
    buf.add_str(t)
    buf.guess_segment_properties()
    hb.shape(f.hb, buf, {"kern": True})
    s = cap / f.cap
    k = math.tan(math.radians(skew))
    pen = SVGPathPen(f.glyphs, ntos=num)
    cursor = 0.0
    boxes = []
    for i, (info, pos) in enumerate(zip(buf.glyph_infos, buf.glyph_positions)):
        name = f.order[info.codepoint]
        gx = x + (cursor + pos.x_offset) * s
        f.glyphs[name].draw(TransformPen(pen, (s, 0, s * k, -s, gx, baseline)))
        bounds = BoundsPen(f.glyphs)
        f.glyphs[name].draw(bounds)
        if bounds.bounds:
            boxes.append((t[i], gx + bounds.bounds[0] * s, gx + bounds.bounds[2] * s))
        cursor += pos.x_advance + (tracking * f.cap if i < len(t) - 1 else 0)
    return pen.getCommands(), cursor * s, boxes


def rr(x: float, y: float, w: float, h: float, r: float) -> str:
    """a rounded rectangle's path"""
    return (f"M{num(x + r)} {num(y)}H{num(x + w - r)}A{num(r)} {num(r)} 0 0 1 {num(x + w)} {num(y + r)}"
            f"V{num(y + h - r)}A{num(r)} {num(r)} 0 0 1 {num(x + w - r)} {num(y + h)}H{num(x + r)}"
            f"A{num(r)} {num(r)} 0 0 1 {num(x)} {num(y + h - r)}V{num(y + r)}A{num(r)} {num(r)} 0 0 1 {num(x + r)} {num(y)}Z")


# ---------------------------------------------------------------- drawing

MASKS = [0]


def element(template: str) -> str:
    return template if template.startswith("<") else f'<path fill="{{c}}" d="{template}"/>'


def body(layers: list, colours: dict, defs: str = "") -> str:
    out = f"<defs>{defs}</defs>" if defs else ""
    return out + "".join(element(t).replace("{c}", colours[role]) for role, _, t in layers)


def mono(layers: list) -> str:
    """the layers in one colour: 'fill' layers drawn, 'cut' layers cut out of
    what is below them, 'skip' layers (glows, highlights) left out"""
    current = ""
    for _, kind, t in layers:
        if kind == "fill":
            current += element(t).replace("{c}", "currentColor")
        elif kind == "cut":
            MASKS[0] += 1
            m = f"k{MASKS[0]}"
            current = (f'<mask id="{m}" maskUnits="userSpaceOnUse" x="-2000" y="-2000" width="6000" height="6000">'
                       f'<rect x="-2000" y="-2000" width="6000" height="6000" fill="#fff"/>'
                       f'{element(t).replace("{c}", "#000")}</mask><g mask="url(#{m})">{current}</g>')
    return current


def mark_body(c: dict, variant: str) -> str:
    if variant == "mono":
        return mono(c["layers"])
    return body(c["layers"], c["pal"][variant], c.get("defs", {}).get(variant, ""))


def placed(inner: str, scale: float, dx: float = 0.0, dy: float = 0.0) -> str:
    return f'<g transform="translate({num(dx)} {num(dy)}) scale({num(scale)})">{inner}</g>'


def icon_standard(c: dict, background: str, size: float = 640.0, dy: float = 0.0) -> str:
    """the dark mark on background, size of the icon's 1024"""
    off = (1024 - size) / 2
    return background + placed(mark_body(c, "dark"), size / 100, off, off + dy)


def lockup_standard(c: dict, variant: str) -> str:
    colours = MONO_COLOURS if variant == "mono" else c["pal"][variant]
    height = 48.0
    cap = c.get("cap", 22.0)
    gap = c.get("gap", 14.0)
    x = height + gap
    words, width = c["wordmark"](colours, variant == "mono", x, cap, height / 2 + cap / 2)
    return svg(x + width, height, placed(mark_body(c, variant), height / 100) + words, "OpenH1")


def lockup_words(c: dict, variant: str) -> str:
    """the wordmark alone, when it already holds the mark"""
    colours = MONO_COLOURS if variant == "mono" else c["pal"][variant]
    cap = 40.0
    words, width = c["wordmark"](colours, variant == "mono", 0.0, cap, cap)
    return svg(width, cap * 1.3, words, "OpenH1")


def lockup_open_mark(c: dict, variant: str, box=(15.5, 84.5, 24.0, 78.0)) -> str:
    """"Open", then the mark as the H1: box is the mark's ink, left, right,
    top and bottom of its caps, in its units"""
    colours = MONO_COLOURS if variant == "mono" else c["pal"][variant]
    cap = 40.0
    d, width, _ = text("Open", 750, cap, 0, cap)
    left, right, top, bottom = box
    s = cap / (bottom - top)
    x = width + cap * 0.12 - left * s
    out = f'<path fill="{colours["ink"]}" d="{d}"/>' + placed(mark_body(c, variant), s, x, -top * s)
    return svg(x + right * s, cap * 1.3, out, "OpenH1")


class Mono(dict):
    def __missing__(self, key):
        return "currentColor"


MONO_COLOURS = Mono()


def words_two_tone(parts: list, weight=900, tracking=0.0, skew=0.0, upper=False):
    """a wordmark in Overpass: parts of (text, colour role, weight)"""
    def draw(colours, is_mono, x, cap, baseline):
        out, cursor = "", x
        for i, (t, role, w) in enumerate(parts):
            d, width, _ = text(t.upper() if upper else t, w or weight, cap, cursor, baseline, tracking, skew)
            out += f'<path fill="{colours[role]}" d="{d}"/>'
            cursor += width + (tracking * cap if i < len(parts) - 1 else 0)
        return out, cursor - x
    return draw


GLOW = ('<filter id="{id}" x="-50%" y="-50%" width="200%" height="200%">'
        '<feGaussianBlur stdDeviation="{r}"/></filter>')


def glow(gid: str, r: float) -> str:
    return GLOW.replace("{id}", gid).replace("{r}", num(r))


# ---------------------------------------------------------------- the concepts

CONCEPTS = []


# 1. Power Core: black plastic, a recess, and a glowing core shaped like a 1
ONE_CORE = "M49 26H61V74H49V42L39 50V38Z"
CORE_DEFS = (
    '<linearGradient id="shell" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#3a3e41"/>'
    '<stop offset=".5" stop-color="#17191b"/><stop offset="1" stop-color="#08090a"/></linearGradient>'
    '<linearGradient id="well" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#020303"/>'
    '<stop offset="1" stop-color="#121516"/></linearGradient>'
    '<radialGradient id="bloom" cx=".5" cy=".5" r=".5"><stop offset="0" stop-color="#d4ff3a" stop-opacity=".55"/>'
    '<stop offset=".6" stop-color="#7fe01a" stop-opacity=".12"/><stop offset="1" stop-color="#7fe01a" stop-opacity="0"/></radialGradient>'
    '<linearGradient id="gloss" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".22"/>'
    '<stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient>'
    + glow("coreglow", 3.2))
CORE_LAYERS = [
    ("shell", "fill", rr(6, 6, 88, 88, 24)),
    ("edge", "skip", f'<path fill="none" stroke="{{c}}" stroke-width="1.2" stroke-opacity=".35" d="{rr(7, 7, 86, 86, 23)}"/>'),
    ("well", "cut", rr(20, 18, 60, 64, 15)),
    ("bloom", "skip", rr(20, 18, 60, 64, 15)),
    ("lime", "skip", f'<path fill="{{c}}" filter="url(#coreglow)" d="{ONE_CORE}"/>'),
    ("lime", "fill", f'<path fill="{{c}}" stroke="{{c}}" stroke-width="2" stroke-linejoin="round" d="{ONE_CORE}"/>'),
    ("hot", "skip", "M52 30H58V71H52Z"),
    ("gloss", "skip", "M30 6H70C84 6 94 16 94 30V38C70 30 30 30 6 38V30C6 16 16 6 30 6Z"),
]
CORE_COLOURS = {"shell": "url(#shell)", "edge": "#ffffff", "well": "url(#well)", "bloom": "url(#bloom)",
                "lime": "#c8ff2e", "hot": "#f4ffd8", "gloss": "url(#gloss)", "metal": "#d6dadd", "soft": "#9aa1a6",
                "word": "#c8ff2e"}
CORE_LIGHT = dict(CORE_COLOURS, metal="#1a1c1e", soft="#5b6268", word="#5a8a00")


def core_icon(c):
    shell = '<rect width="1024" height="1024" fill="url(#shell)"/>'
    layers = [l for l in CORE_LAYERS if l[0] not in ("shell", "edge", "gloss")]
    inner = body(layers, CORE_COLOURS)
    gloss = '<path fill="url(#gloss)" d="M0 0H1024V330C700 250 324 250 0 330Z"/>'
    return f"<defs>{CORE_DEFS}</defs>{shell}{placed(inner, 13.4, -158, -158)}{gloss}"


def core_words(colours, is_mono, x, cap, baseline):
    d1, w1, _ = text("Open", 900, cap, x, baseline, 0.02)
    d2, w2, _ = text("H1", 900, cap, x + w1 + cap * 0.06, baseline, 0.02)
    halo = "" if is_mono or colours["word"] != "#c8ff2e" else f'<path fill="{colours["word"]}" filter="url(#wordglow)" opacity=".8" d="{d2}"/>'
    defs = "" if is_mono else f"<defs>{glow('wordglow', 1.6)}</defs>"
    return (defs + f'<path fill="{colours["metal"]}" d="{d1}"/>' + halo +
            f'<path fill="{colours["word"]}" d="{d2}"/>'), w1 + cap * 0.06 + w2


CONCEPTS.append({
    "id": "power-core", "name": "Power Core", "style": "2001 hardware: black plastic, inner light",
    "ground": ("#0b0c0d", "#e9ebec"),
    "pal": {"dark": CORE_COLOURS, "light": CORE_LIGHT},
    "defs": {"dark": CORE_DEFS, "light": CORE_DEFS},
    "layers": CORE_LAYERS,
    "icon": core_icon,
    "wordmark": core_words,
    "favicon": ('<rect width="16" height="16" rx="4" fill="#1c1f21"/><rect x="2.5" y="2.5" width="11" height="11" rx="2.5" fill="#050606"/>'
                '<path fill="#c8ff2e" d="M8 4H10V12H8V7L6 8.5V6.5Z"/>'),
    "idea": "A chunk of black console plastic with a recessed well and a lime core glowing inside it in the shape "
            "of a 1, like a power light seen through smoked plastic. The era's hardware, as a badge.",
    "legal": "It's a rounded slab with a numeral in it: no character, helmet, ring or anything from the game.",
    "xbox": "No X, no sphere or jewel shape. The lime is a glow accent, shifted to yellow-green, on a black brand.",
})


# 2. Tube: an H1 drawn in glowing tubes, the organic dashboard feel
TUBE_STROKES = ["M22 24V78", "M48 24V78", "M22 51H48", "M61 37C65 28.5 69.5 24 76 24V78"]


def tube_layers():
    layers = []
    for d in TUBE_STROKES:
        layers.append(("glow", "skip", f'<path fill="none" stroke="{{c}}" stroke-width="14" stroke-linecap="round" stroke-linejoin="round" filter="url(#tubeglow)" opacity=".7" d="{d}"/>'))
    for d in TUBE_STROKES:
        layers.append(("rim", "fill", f'<path fill="none" stroke="{{c}}" stroke-width="13" stroke-linecap="round" stroke-linejoin="round" d="{d}"/>'))
    for d in TUBE_STROKES:
        layers.append(("fluid", "skip", f'<path fill="none" stroke="{{c}}" stroke-width="8.5" stroke-linecap="round" stroke-linejoin="round" d="{d}"/>'))
    for d in TUBE_STROKES:
        layers.append(("core", "skip", f'<path fill="none" stroke="{{c}}" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round" stroke-opacity=".85" transform="translate(-1.6 -1.2)" d="{d}"/>'))
    return layers


TUBE_DEFS = ('<linearGradient id="fluid" gradientUnits="userSpaceOnUse" x1="20" y1="22" x2="80" y2="80">'
             '<stop offset="0" stop-color="#5cffd8"/><stop offset="1" stop-color="#14b4ff"/></linearGradient>'
             + glow("tubeglow", 5))
TUBE_DARK = {"glow": "#18e3c8", "rim": "#06463f", "fluid": "url(#fluid)", "core": "#e9fffb",
             "ink": "#dffcf6", "accent": "#3ff0cf"}
TUBE_LIGHT = {"glow": "#7ff5e2", "rim": "#036358", "fluid": "url(#fluid)", "core": "#ffffff",
              "ink": "#063d37", "accent": "#00a58c"}


def tube_icon(c):
    bg = ('<defs><radialGradient id="pool" cx=".35" cy=".3" r=".9"><stop offset="0" stop-color="#0b4a45"/>'
          '<stop offset=".55" stop-color="#04201e"/><stop offset="1" stop-color="#010908"/></radialGradient>'
          '<radialGradient id="blob" cx=".5" cy=".5" r=".5"><stop offset="0" stop-color="#1de9c8" stop-opacity=".28"/>'
          '<stop offset="1" stop-color="#1de9c8" stop-opacity="0"/></radialGradient></defs>'
          '<rect width="1024" height="1024" fill="url(#pool)"/>'
          '<path fill="url(#blob)" d="M180 300C260 120 560 100 720 210C900 330 930 560 800 720C660 900 360 920 220 760C100 620 110 450 180 300Z"/>')
    return icon_standard(c, bg, 660)


CONCEPTS.append({
    "id": "tube", "name": "Tube", "style": "2001 dashboard: organic glowing tubes",
    "ground": ("#03110f", "#eef7f5"),
    "pal": {"dark": TUBE_DARK, "light": TUBE_LIGHT},
    "defs": {"dark": TUBE_DEFS, "light": TUBE_DEFS},
    "layers": tube_layers(),
    "icon": tube_icon,
    "lockup": lockup_open_mark,
    "wordmark": words_two_tone([("Open", "ink", 750), ("H1", "accent", 900)]),
    "favicon": ('<rect width="16" height="16" rx="3.5" fill="#04201e"/>'
                '<path fill="none" stroke="#3ff0cf" stroke-width="2" stroke-linecap="round" '
                'stroke-linejoin="round" d="M3.5 4.5V11.5M7.5 4.5V11.5M3.5 8H7.5M10 6.5C10.6 5.2 11.2 4.5 12.5 4.5V11.5"/>'),
    "idea": "H1 runs in liquid-filled tubes with a bright core and a soft bloom, the organic, "
            "lit-from-inside feel of the era's console menus, in teal rather than green.",
    "legal": "Monoline letters of the project's name. No interface element, ring or shape from the game or its menus.",
    "xbox": "Teal-to-cyan, not the console's green; letters, not an X; no orb or blob logo.",
})


# 3. Big Box: chrome letters with a deep extrusion, the 2001 game-box cover
def bigbox_letters(cap=50.0, x=15.0, baseline=73.0):
    d, width, _ = text("H1", 900, cap, x, baseline, 0.0, 12)
    return d, width


BB_D, BB_W = bigbox_letters()
BB_DX = (100 - BB_W) / 2 - 15 - 4
BB_DEFS = ('<linearGradient id="chrome" gradientUnits="userSpaceOnUse" x1="0" y1="23" x2="0" y2="73">'
           '<stop offset="0" stop-color="#ffffff"/><stop offset=".42" stop-color="#c5d0e0"/>'
           '<stop offset=".5" stop-color="#3f4b66"/><stop offset=".6" stop-color="#8d9bb8"/>'
           '<stop offset="1" stop-color="#f2f6fc"/></linearGradient>'
           '<linearGradient id="deep" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#1b2a8f"/>'
           '<stop offset="1" stop-color="#070c38"/></linearGradient>')


def bigbox_layers():
    move = f' transform="translate({num(BB_DX)} 0)"'
    layers = []
    for k in range(9, 0, -1):
        layers.append(("deep", "fill",
                       f'<path fill="{{c}}" transform="translate({num(BB_DX + k * 0.85)} {num(k * 0.85)})" d="{BB_D}"/>'))
    layers.append(("deep", "fill", f'<path fill="{{c}}"{move} d="{BB_D}"/>'))
    layers.append(("gap", "cut", f'<path fill="none" stroke="{{c}}" stroke-width="2.4"{move} d="{BB_D}"/>'))
    layers.append(("chrome", "fill", f'<path fill="{{c}}"{move} d="{BB_D}"/>'))
    layers.append(("lip", "skip", f'<path fill="none" stroke="{{c}}" stroke-width=".9" stroke-opacity=".9"{move} d="{BB_D}"/>'))
    layers.append(("spark", "skip", "M80 12L81.6 18.4L88 20L81.6 21.6L80 28L78.4 21.6L72 20L78.4 18.4Z"))
    return layers


BB_DARK = {"deep": "url(#deep)", "gap": "#000", "chrome": "url(#chrome)", "lip": "#ffffff", "spark": "#ffffff",
           "ink": "#e8eefa", "accent": "#ff8a1f"}
BB_LIGHT = dict(BB_DARK, spark="#ff8a1f", ink="#0d1650")


def bigbox_icon(c):
    bg = ('<defs><radialGradient id="stage" cx=".5" cy=".42" r=".75"><stop offset="0" stop-color="#3550ff"/>'
          '<stop offset=".5" stop-color="#14207a"/><stop offset="1" stop-color="#050828"/></radialGradient></defs>'
          '<rect width="1024" height="1024" fill="url(#stage)"/>'
          '<g fill="#fff" opacity=".09"><path d="M-40 760L1064 300V340L-40 800Z"/><path d="M-40 850L1064 390V400L-40 860Z"/></g>'
          '<rect y="840" width="1024" height="184" fill="#ff8a1f"/><rect y="840" width="1024" height="10" fill="#ffd08a"/>')
    return icon_standard(c, bg, 760, -60)


def bigbox_words(colours, is_mono, x, cap, baseline):
    d, width, _ = text("OpenH1", 900, cap, x, baseline, 0.01, 12)
    if is_mono:
        return f'<path fill="currentColor" d="{d}"/>', width + 3
    defs = ('<defs><linearGradient id="wchrome" gradientUnits="userSpaceOnUse" x1="0" y1="{0}" x2="0" y2="{1}">'
            '<stop offset="0" stop-color="#ffffff"/><stop offset=".45" stop-color="#c5d0e0"/><stop offset=".52" stop-color="#3f4b66"/>'
            '<stop offset=".62" stop-color="#8d9bb8"/><stop offset="1" stop-color="#f2f6fc"/></linearGradient></defs>').format(
        num(baseline - cap), num(baseline))
    deep = "".join(f'<path fill="#0d1650" transform="translate({num(k * 0.5)} {num(k * 0.5)})" d="{d}"/>' for k in range(5, 0, -1))
    return defs + deep + f'<path fill="url(#wchrome)" stroke="#ffffff" stroke-width=".5" d="{d}"/>', width + 3


CONCEPTS.append({
    "id": "big-box", "name": "Big Box", "style": "2001 box art: chrome, bevel, extrusion",
    "ground": ("#070b30", "#eef0f7"),
    "pal": {"dark": BB_DARK, "light": BB_LIGHT},
    "defs": {"dark": BB_DEFS, "light": BB_DEFS},
    "layers": bigbox_layers(),
    "icon": bigbox_icon,
    "lockup": lockup_words,
    "wordmark": bigbox_words,
    "cap": 22,
    "favicon": ('<rect width="16" height="16" rx="3.5" fill="#121c78"/>'
                '<path fill="#050828" d="M3 5H5V8H7V5H9V13H7V10H5V13H3ZM12 5H14V13H12V8L10.5 9V7Z"/>'
                '<path fill="#ffffff" d="M2 4H4V7H6V4H8V8H2Z M11 4H13V8H11V7L9.5 8V6Z"/>'
                '<path fill="#9fb0cf" d="M2 8H4V12H2ZM6 8H8V12H6ZM4 8H6V9H4ZM11 8H13V12H11Z"/>'),
    "idea": "Slanted chrome letters with a deep blue extrusion and one lens sparkle, straight off a 2001 "
            "game box, with the shelf-edge orange band on the app icon. Loud on purpose; the small version is flat.",
    "legal": "The letters are Overpass, slanted, not the game's logo type, and the box art is our own composition. "
             "No characters, no ring, no publisher marks.",
    "xbox": "Blue and orange chrome, not green; no X, no orb, no console branding band.",
})


# 4. CRT: amber phosphor pixels and a cursor
CRT_ART = [
    ".##..##...##.....",
    ".##..##..###.....",
    ".##..##.####.....",
    ".######...##.....",
    ".######...##.....",
    ".##..##...##.....",
    ".##..##...##.....",
    ".##..##...##.####",
]
CRT_FAV = [
    "..##..##...##...",
    "..##..##..###...",
    "..##..##.####...",
    "..######...##...",
    "..######...##...",
    "..##..##...##...",
    "..##..##...##...",
    "..##..##...##...",
]


def crt_rects(cell: float, x0: float, y0: float, gap: float = 0.0, cursor: bool = False, art=None) -> str:
    out = []
    for r, row in enumerate(art or CRT_ART):
        c = 0
        while c < len(row):
            if row[c] == "#" and (c >= 13) == cursor:
                start = c
                while c < len(row) and row[c] == "#" and (c >= 13) == cursor:
                    c += 1
                out.append(f"M{num(x0 + start * cell)} {num(y0 + r * cell)}h{num((c - start) * cell)}v{num(cell - gap)}h{num(-(c - start) * cell)}Z")
            else:
                c += 1
    return "".join(out)


CRT_CELL = 100 / 17
CRT_X = (100 - 16 * CRT_CELL) / 2 - CRT_CELL
CRT_Y = (100 - 8 * CRT_CELL) / 2
CRT_LETTERS = crt_rects(CRT_CELL, CRT_X, CRT_Y)
CRT_CURSOR = crt_rects(CRT_CELL, CRT_X, CRT_Y, cursor=True)
CRT_DEFS = glow("phos", 2.4)
CRT_DARK = {"phos": "#ffb000", "cursor": "#fff3d6", "ink": "#ffcf6b", "accent": "#ffb000"}
CRT_LIGHT = {"phos": "#3a2600", "cursor": "#e07b00", "ink": "#3a2600", "accent": "#e07b00"}

# a 5x7 bitmap face for the wordmark (lowercase descends two rows)
BITMAP = {
    "O": [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "p": [".....", ".....", "####.", "#...#", "#...#", "#...#", "####.", "#....", "#...."],
    "e": [".....", ".....", ".###.", "#...#", "#####", "#....", ".####"],
    "n": [".....", ".....", "####.", "#...#", "#...#", "#...#", "#...#"],
    "H": ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "1": ["..#..", ".##..", "#.#..", "..#..", "..#..", "..#..", "#####"],
}


def crt_words(colours, is_mono, x, cap, baseline):
    cell = cap / 7
    top = baseline - cap
    paths = {"ink": "", "accent": ""}
    cursor = x
    for ch in "OpenH1":
        role = "accent" if ch in "H1" else "ink"
        for r, row in enumerate(BITMAP[ch]):
            for c, v in enumerate(row):
                if v == "#":
                    paths[role] += f"M{num(cursor + c * cell)} {num(top + r * cell)}h{num(cell)}v{num(cell)}h{num(-cell)}Z"
        cursor += 6 * cell
    blink = f"M{num(cursor)} {num(top + 6 * cell)}h{num(4 * cell)}v{num(cell)}h{num(-4 * cell)}Z"
    out = f'<path fill="{colours["ink"]}" d="{paths["ink"]}"/><path fill="{colours["accent"]}" d="{paths["accent"]}"/>'
    out += f'<path fill="{colours["cursor"]}" d="{blink}"/>'
    return out, cursor + 4 * cell - x


def crt_icon(c):
    cell = 1024 / 19
    x0 = (1024 - 16 * cell) / 2 - cell
    y0 = (1024 - 8 * cell) / 2
    lines = "".join(f'<rect y="{num(y)}" width="1024" height="3" fill="#000" opacity=".35"/>' for y in range(2, 1024, 8))
    return ('<defs><radialGradient id="tube" cx=".5" cy=".45" r=".7"><stop offset="0" stop-color="#2a1a00"/>'
            '<stop offset="1" stop-color="#050300"/></radialGradient>' + glow("big", 22) + '</defs>'
            '<rect width="1024" height="1024" fill="url(#tube)"/>'
            f'<path fill="#ffb000" filter="url(#big)" opacity=".75" d="{crt_rects(cell, x0, y0)}"/>'
            f'<path fill="#ffb000" d="{crt_rects(cell, x0, y0, gap=10)}"/>'
            f'<path fill="#fff3d6" d="{crt_rects(cell, x0, y0, gap=10, cursor=True)}"/>' + lines)


CONCEPTS.append({
    "id": "crt", "name": "CRT", "style": "Pixel mark on an amber phosphor screen",
    "ground": ("#0d0800", "#f6efe1"),
    "pal": {"dark": CRT_DARK, "light": CRT_LIGHT},
    "defs": {"dark": CRT_DEFS, "light": ""},
    "layers": [
        ("phos", "skip", f'<path fill="{{c}}" filter="url(#phos)" opacity=".6" d="{CRT_LETTERS}"/>'),
        ("phos", "fill", CRT_LETTERS),
        ("cursor", "fill", CRT_CURSOR),
    ],
    "icon": crt_icon,
    "lockup": lockup_words,
    "wordmark": crt_words,
    "favicon": ('<rect width="16" height="16" rx="3" fill="#140c00"/>'
                f'<path fill="#ffb000" d="{crt_rects(1, 0, 4, art=CRT_FAV)}"/>'),
    "idea": "H1 and a blinking cursor in fat pixels on an amber monitor: the decompilation, old screens and "
            "LAN-party CRTs. The favicon is the pixel art itself, so it's exact at 16 pixels.",
    "legal": "Our own pixel drawing of the name; no game sprite, font or screen is reproduced.",
    "xbox": "Amber phosphor, not green; pixels and a cursor, no X and no orb.",
})


# 5. Stencil: a clipped field plate with stencilled H1
STENCIL_PLATE = "M14 4H86L96 14V86L86 96H14L4 86V14Z"
STENCIL_KEY = '<path fill="none" stroke="{c}" stroke-width="2.5" d="M17 10.5H83L89.5 17V83L83 89.5H17L10.5 83V17Z"/>'
STENCIL_H = "M15 27H29V44H34V56H29V73H15Z M43 27H57V73H43V56H38V44H43Z"
STENCIL_ONE_FLAG = "M68 27V41L58 48V34Z"
STENCIL_ONE = "M70 27H84V73H70Z"
STENCIL_DARK = {"plate": "#d8c79c", "ink": "#262b1b", "accent": "#ff6b1a", "word": "#e6dcc0", "word2": "#ff7a2a"}
STENCIL_LIGHT = {"plate": "#4a5530", "ink": "#efe7cf", "accent": "#ff6b1a", "word": "#2b311d", "word2": "#e85d0c"}

STENCIL_CUTS = {  # stencil bridges: (left edge as a share of the letter's width, from, to as shares of the cap)
    "O": [(0.47, -0.1, 1.1)], "P": [(0.40, -0.1, 0.62)], "E": [(0.40, -0.1, 1.1)],
    "N": [(0.33, -0.1, 0.45), (0.60, 0.55, 1.1)], "H": [(0.30, 0.3, 0.7), (0.63, 0.3, 0.7)], "1": [(0.38, -0.1, 0.42)],
}


def stencil_words(colours, is_mono, x, cap, baseline):
    d, width, boxes = text("OPENH1", 900, cap, x, baseline, 0.06)
    cuts = ""
    for ch, left, right in boxes:
        for share, a, b in STENCIL_CUTS.get(ch, []):
            cx = left + (right - left) * share
            cuts += f"M{num(cx)} {num(baseline - cap * b)}h{num(cap * 0.07)}v{num(cap * (b - a))}h{num(-cap * 0.07)}Z"
    MASKS[0] += 1
    m = f"s{MASKS[0]}"
    split = boxes[4][1] - cap * 0.03
    mask = (f'<mask id="{m}" maskUnits="userSpaceOnUse" x="0" y="0" width="2000" height="200">'
            f'<rect width="2000" height="200" fill="#fff"/><path fill="#000" d="{cuts}"/></mask>')
    clip_a = f'<clipPath id="{m}a"><rect width="{num(split)}" height="200"/></clipPath>'
    clip_b = f'<clipPath id="{m}b"><rect x="{num(split)}" width="2000" height="200"/></clipPath>'
    return (f"<defs>{mask}{clip_a}{clip_b}</defs><g mask=\"url(#{m})\">"
            f'<path clip-path="url(#{m}a)" fill="{colours["word"]}" d="{d}"/>'
            f'<path clip-path="url(#{m}b)" fill="{colours["word2"]}" d="{d}"/></g>'), width


CONCEPTS.append({
    "id": "stencil", "name": "Field Stencil", "style": "Military stencil, field plate",
    "ground": ("#191c12", "#efeadb"),
    "pal": {"dark": STENCIL_DARK, "light": STENCIL_LIGHT},
    "layers": [
        ("plate", "fill", STENCIL_PLATE),
        ("ink", "cut", STENCIL_KEY),
        ("ink", "cut", STENCIL_H),
        ("ink", "cut", STENCIL_ONE),
        ("accent", "cut", STENCIL_ONE_FLAG),
    ],
    "icon": lambda c: icon_standard(c, '<rect width="1024" height="1024" fill="#384124"/>'
                                       '<rect width="1024" height="1024" fill="none" stroke="#2b321b" stroke-width="40"/>', 700),
    "wordmark": stencil_words,
    "cap": 21,
    "favicon": ('<path fill="#d8c79c" d="M2.5 0H13.5L16 2.5V13.5L13.5 16H2.5L0 13.5V2.5Z"/>'
                '<path fill="#262b1b" d="M2 4H4V12H2ZM6 4H8V12H6ZM4 7H6V9H4ZM11 4H13V12H11Z"/>'
                '<path fill="#ff6b1a" d="M9 4H11V6H9Z"/>'),
    "idea": "H1 stencilled on a clipped field plate: the stencil bridges break the crossbar and cut the 1's flag loose, "
            "and the flag is painted signal orange. A squad marking for a project anyone can join.",
    "legal": "Generic stencil lettering on a plain plate. No eagle, globe, rank insignia, unit badge or any "
             "emblem from the series.",
    "xbox": "Sand, olive and orange, stencilled letters: nothing of the console's colours, shapes or type.",
})


# 6. Slab: a heavy custom wordmark, OPENH1 drawn from scratch
SLAB = {  # cap height 100; (width, path)
    "O": (96, rr(0, 0, 96, 100, 40) + " " + rr(33, 31, 30, 38, 15)),
    "P": (92, "M0 0H58A34 34 0 0 1 58 68H32V100H0Z M32 26H55A8 8 0 0 1 55 42H32Z"),
    "E": (82, "M0 0H82V26H32V37H72V63H32V74H82V100H0Z"),
    "N": (94, "M0 0H32L62 50V0H94V100H62L32 50V100H0Z"),
    "H": (94, "M0 0H32V36H62V0H94V100H62V64H32V100H0Z"),
    "1": (66, "M30 0H66V100H32V42L8 50V18Z"),
}


def slab_word(word: str, x: float, cap: float, baseline: float, spacing: float = 9.0) -> tuple:
    s = cap / 100
    paths = []
    cursor = 0.0
    for ch in word:
        w, d = SLAB[ch]
        paths.append(f'<path transform="translate({num(x + cursor * s)} {num(baseline - cap)}) scale({num(s)})" d="{d}" fill-rule="evenodd"/>')
        cursor += w + spacing
    return paths, (cursor - spacing) * s


SLAB_DARK = {"tile": "#ff3d2e", "letter": "#f6f0e6", "ink": "#f6f0e6", "accent": "#ff3d2e"}
SLAB_LIGHT = {"tile": "#ff3d2e", "letter": "#f6f0e6", "ink": "#151515", "accent": "#ff3d2e"}
_slab_w = slab_word("H1", 0, 44, 0)[1]
SLAB_MARK = "".join(p.replace("<path ", '<path fill="{c}" ') for p in slab_word("H1", (100 - _slab_w) / 2, 44, 72)[0])


def slab_lockup(c, variant):
    colours = MONO_COLOURS if variant == "mono" else c["pal"][variant]
    cap = 40.0
    paths, width = slab_word("OPENH1", 0, cap, cap)
    out = "".join(p.replace("<path ", f'<path fill="{colours["ink"] if i < 4 else colours["accent"]}" ') for i, p in enumerate(paths))
    return svg(width, cap, out, "OpenH1")


def slab_icon(c):
    paths, width = slab_word("H1", 0, 460, 0)
    dx = (1024 - width) / 2
    inner = "".join(p.replace("<path ", '<path fill="#f6f0e6" ') for p in paths)
    return f'<rect width="1024" height="1024" fill="#ff3d2e"/><g transform="translate({num(dx)} {num(512 + 230)})">{inner}</g>'


CONCEPTS.append({
    "id": "slab", "name": "Slab", "style": "Custom heavy wordmark",
    "ground": ("#151515", "#f6f0e6"),
    "pal": {"dark": SLAB_DARK, "light": SLAB_LIGHT},
    "layers": [
        ("tile", "fill", rr(2, 2, 96, 96, 22)),
        ("letter", "cut", SLAB_MARK),
    ],
    "icon": slab_icon,
    "lockup": slab_lockup,
    "favicon": ('<rect width="16" height="16" rx="3.5" fill="#ff3d2e"/>'
                '<path fill="#f6f0e6" d="M1.5 4H4.5V7H6.5V4H9.5V12H6.5V9H4.5V12H1.5Z M11.5 4H14.5V12H11.5V8L10 8.5V5.5Z"/>'),
    "idea": "OPENH1 drawn from scratch as one heavy block of letters: big rounded O and P, slab E, N and H, "
            "a 1 with a sharp flag. The name is the logo; the H1 in a red tile is the icon.",
    "legal": "Custom letters drawn here for this name, unrelated to any game or console lettering.",
    "xbox": "Red and bone, upright custom slab letters: no X, no orb, no green.",
})


# 7. Split: a square opened along a cut shaped like a 1
SPLIT_LEFT = "M10 13H50L24 39V53L50 27V93H10Z"
SPLIT_RIGHT = "M60 7H90V87H60Z"
CONCEPTS.append({
    "id": "split", "name": "Split", "style": "Abstract geometric",
    "ground": ("#1d1b2e", "#f7efe2"),
    "pal": {"dark": {"left": "#ff5a47", "right": "#ff5a47", "ink": "#f7efe2", "accent": "#ff5a47"},
            "light": {"left": "#1d1b2e", "right": "#1d1b2e", "ink": "#1d1b2e", "accent": "#ff5a47"}},
    "layers": [("left", "fill", SPLIT_LEFT), ("right", "fill", SPLIT_RIGHT)],
    "icon": lambda c: icon_standard(c, '<rect width="1024" height="1024" fill="#1d1b2e"/>', 620),
    "wordmark": words_two_tone([("Open", "ink", 900), ("H1", "accent", 900)]),
    "favicon": ('<rect width="16" height="16" rx="3.5" fill="#1d1b2e"/>'
                '<path fill="#ff5a47" d="M2 3H7L4 6V8L7 5V14H2Z M9 2H14V13H9Z"/>'),
    "idea": "A solid square split open: the cut between its halves is a 1, and the right half has slid up as if "
            "the block were being opened. Open, and one, in two shapes.",
    "legal": "Two plain polygons; the 1 exists only as the gap between them. Nothing figurative at all.",
    "xbox": "Coral and cream squares with a straight cut: no X, no curves, no orb, no green.",
})


# 8. Crest: a varsity monogram on an original shield
CREST_SHIELD = "M14 8H44L50 13L56 8H86V46C86 70 70 84 50 94C30 84 14 70 14 46Z"
CREST_H = "M18 30H31V33.5H28V43H36V33.5H33V30H46V33.5H43V56.5H46V60H33V56.5H36V47H28V56.5H31V60H18V56.5H21V33.5H18Z"
CREST_ONE = "M67 30H74V56.5H78V60H63V56.5H67V36L61 39V35.5Z"
CREST_INSET = 'transform="translate(50 50) scale(.87) translate(-50 -50)"'
CREST_DEFS = f'<clipPath id="inner"><path {CREST_INSET} d="{CREST_SHIELD}"/></clipPath>'
CREST_PAL = {"shield": "#8e1b2e", "gold": "#e2b552", "bone": "#f6ecd8", "ink": "#f6ecd8"}
CREST_LIGHT = dict(CREST_PAL, ink="#5e1020")
CONCEPTS.append({
    "id": "crest", "name": "Crest", "style": "Monogram on an original shield",
    "ground": ("#170a0d", "#f6efe4"),
    "pal": {"dark": CREST_PAL, "light": CREST_LIGHT},
    "defs": {"dark": CREST_DEFS, "light": CREST_DEFS},
    "layers": [
        ("shield", "fill", CREST_SHIELD),
        ("bone", "skip", '<path fill="{c}" clip-path="url(#inner)" d="M50 0H100V100H50Z"/>'),
        ("gold", "cut", f'<path fill="none" stroke="{{c}}" stroke-width="2.3" {CREST_INSET} d="{CREST_SHIELD}"/>'),
        ("bone", "cut", CREST_H),
        ("shield", "cut", CREST_ONE),
    ],
    "icon": lambda c: icon_standard(c, '<rect width="1024" height="1024" fill="#2a0d14"/>', 760),
    "wordmark": words_two_tone([("OPENH1", "ink", 750)], tracking=0.22),
    "cap": 18,
    "favicon": ('<path fill="#8e1b2e" d="M2 1H14V8C14 11.5 11.5 13.6 8 15.5C4.5 13.6 2 11.5 2 8Z"/>'
                '<path fill="#f6ecd8" d="M3.5 3.5H5.5V6H7V3.5H9V10.5H7V8H5.5V10.5H3.5ZM11 3.5H13V10.5H11V6.5L9.8 7V5Z"/>'),
    "idea": "A notched shield parted down the middle, burgundy and bone, with a varsity H and 1 counterchanged "
            "across it inside a gold keyline: a team crest for a community project.",
    "legal": "A heraldic shield of our own outline with collegiate letters. No eagle, globe or insignia from the "
             "series, and no ring.",
    "xbox": "Burgundy, gold and bone heraldry: none of the console's shapes, colours or type.",
})


# 9. Launch: a trajectory arcing in to become the 1's flag
LAUNCH_ONE = "M18 54C28 31 46 18 84 13V90H65V37C47 39 32 45 18 54Z"
LAUNCH_DEFS = ('<linearGradient id="heat" gradientUnits="userSpaceOnUse" x1="10" y1="80" x2="84" y2="20">'
               '<stop offset="0" stop-color="#7b3cff"/><stop offset=".55" stop-color="#ff2e88"/>'
               '<stop offset="1" stop-color="#ff9a1f"/></linearGradient>')
LAUNCH_DARK = {"heat": "url(#heat)", "trail": "#a24dff", "ink": "#fbf4ff", "accent": "#ff7a3d"}
LAUNCH_LIGHT = dict(LAUNCH_DARK, ink="#1a0b2e", accent="#f0306f")
CONCEPTS.append({
    "id": "launch", "name": "Launch", "style": "Kinetic: a trajectory",
    "ground": ("#140a24", "#fbf7ff"),
    "pal": {"dark": LAUNCH_DARK, "light": LAUNCH_LIGHT},
    "defs": {"dark": LAUNCH_DEFS, "light": LAUNCH_DEFS},
    "layers": [
        ("trail", "fill", '<path fill="none" stroke="{c}" stroke-width="4.5" stroke-linecap="round" stroke-dasharray="0.1 8" d="M5 86C7 77 10 69 14 62"/>'),
        ("heat", "fill", LAUNCH_ONE),
    ],
    "icon": lambda c: icon_standard(c, '<defs><radialGradient id="night" cx=".7" cy=".25" r=".9"><stop offset="0" stop-color="#3b1666"/>'
                                       '<stop offset="1" stop-color="#0d0618"/></radialGradient></defs><rect width="1024" height="1024" fill="url(#night)"/>', 640),
    "wordmark": words_two_tone([("Open", "ink", 900), ("H1", "accent", 900)], skew=10),
    "favicon": ('<rect width="16" height="16" rx="3.5" fill="#1d0d36"/>'
                '<defs><linearGradient id="h" x1="0" y1="1" x2="1" y2="0"><stop offset="0" stop-color="#b04dff"/>'
                '<stop offset=".5" stop-color="#ff2e88"/><stop offset="1" stop-color="#ff9a1f"/></linearGradient></defs>'
                '<path fill="url(#h)" d="M2.5 9C4.5 5 7.5 3 13.5 2.5V14H10.5V6C7.5 6.3 5 7.3 2.5 9Z"/>'),
    "idea": "A 1 whose flag is a trajectory: a dotted arc rises from the lower left, turns into a swoosh and "
            "lands as the thick stem. Motion and arrival: the game, launched somewhere new.",
    "legal": "An open arc that becomes a numeral. It never closes into a circle or ring, and has no figure in it.",
    "xbox": "Violet-to-orange heat, an open arc: no X, no sphere, no green.",
})


# 10. Buddy: a character built from type, two 1s for eyes
BUDDY_EYES = "M34 30H42V60H34V43L27 48V39Z M62 30H70V60H62V43L55 48V39Z"
BUDDY_MOUTH = rr(56, 70, 18, 7, 3.5)
BUDDY_PAL = {"head": "#ffd23f", "ink": "#17161c", "word": "#fff4d0", "accent": "#ffd23f"}
BUDDY_LIGHT = {"head": "#ffd23f", "ink": "#17161c", "word": "#17161c", "accent": "#e8a800"}


def buddy_lockup(c, variant):
    colours = MONO_COLOURS if variant == "mono" else c["pal"][variant]
    cap = 30.0
    size = cap * 1.06
    face = placed(mark_body(c, variant), size / 100, 0, -cap * 0.03)
    d1, w1, _ = text("pen", 900, cap, size + cap * 0.06, cap)
    d2, w2, _ = text("H1", 900, cap, size + cap * 0.06 + w1 + cap * 0.04, cap)
    width = size + cap * 0.06 + w1 + cap * 0.04 + w2
    out = face + f'<path fill="{colours["word"]}" d="{d1}"/><path fill="{colours["accent"]}" d="{d2}"/>'
    return svg(width, cap * 1.3, out, "OpenH1")


CONCEPTS.append({
    "id": "buddy", "name": "Buddy", "style": "Character built from type",
    "ground": ("#17161c", "#fff6d6"),
    "pal": {"dark": BUDDY_PAL, "light": BUDDY_LIGHT},
    "layers": [("head", "fill", rr(6, 8, 88, 86, 30)), ("ink", "cut", BUDDY_EYES), ("ink", "cut", BUDDY_MOUTH)],
    "icon": lambda c: ('<rect width="1024" height="1024" fill="#ffd23f"/>' +
                       placed(f'<path fill="#17161c" d="{BUDDY_EYES}"/><path fill="#17161c" d="{BUDDY_MOUTH}"/>', 10.24, 0, -20)),
    "lockup": buddy_lockup,
    "favicon": ('<rect width="16" height="16" rx="5" fill="#ffd23f"/>'
                '<path fill="#17161c" d="M5 4H7V9H5V6.5L4 7V5.5Z M10 4H12V9H10V6.5L9 7V5.5Z M9 11H12.5V12.5H9Z"/>'),
    "idea": "A face made of type: two 1s for eyes, their flags as eyebrows, and a sideways dash for a mouth. "
            "Cheeky and determined. It stands in for the O of OpenH1, so the wordmark smiles.",
    "legal": "A rounded square and three glyphs. Not a character from any game, and no helmet or visor; the "
             "face is bare.",
    "xbox": "Sunny yellow and ink, a face of numerals: no X, no orb, no green.",
})


# ---------------------------------------------------------------- files

def files(c: dict) -> dict:
    out = {}
    for name, variant in (("mark.svg", "dark"), ("mark-light.svg", "light"), ("mark-mono.svg", "mono")):
        out[name] = svg(100, 100, mark_body(c, variant), "OpenH1")
    lock = c.get("lockup", lockup_standard)
    for name, variant in (("lockup.svg", "dark"), ("lockup-light.svg", "light"), ("lockup-mono.svg", "mono")):
        out[name] = lock(c, variant)
    out["icon.svg"] = svg(1024, 1024, c["icon"](c), "OpenH1")
    out["favicon.svg"] = svg(16, 16, c["favicon"], "OpenH1")
    return out


# ---------------------------------------------------------------- the board

def section(index: int, c: dict, out: dict) -> str:
    dark, light = c["ground"]
    fav = uri(out["favicon.svg"])
    icon = uri(out["icon.svg"])
    name = c["name"]
    return f"""
<section class="concept" id="{c['id']}">
  <header>
    <span class="num">{index:02d}</span>
    <div>
      <p class="kind">{html.escape(c['style'])}</p>
      <h2>{html.escape(name)}</h2>
      <p class="idea">{html.escape(c['idea'])}</p>
      <p class="legal"><b>Why it's safe.</b> {html.escape(c['legal'])}</p>
      <p class="legal"><b>Why it's not the Xbox logo.</b> {html.escape(c['xbox'])}</p>
    </div>
  </header>
  <div class="row marks">
    <figure class="panel" style="background:{dark}">{img(uri(out['mark.svg']), 'mark', name)}<figcaption>Mark, on dark</figcaption></figure>
    <figure class="panel lite" style="background:{light}">{img(uri(out['mark-light.svg']), 'mark', name)}<figcaption>Mark, on light</figcaption></figure>
    <figure class="panel lite" style="background:#fff">{img(uri(out['mark-mono.svg'], '#000000'), 'mark', name)}<figcaption>One colour, black</figcaption></figure>
    <figure class="panel" style="background:#111">{img(uri(out['mark-mono.svg'], '#ffffff'), 'mark', name)}<figcaption>One colour, white</figcaption></figure>
  </div>
  <div class="row lockups">
    <figure class="panel" style="background:{dark}">{img(uri(out['lockup.svg']), 'lockup', 'OpenH1')}<figcaption>Lockup, on dark</figcaption></figure>
    <figure class="panel lite" style="background:{light}">{img(uri(out['lockup-light.svg']), 'lockup', 'OpenH1')}<figcaption>Lockup, on light</figcaption></figure>
    <figure class="panel lite" style="background:#fff">{img(uri(out['lockup-mono.svg'], '#000000'), 'lockup', 'OpenH1')}<figcaption>Lockup, one colour</figcaption></figure>
  </div>
  <div class="row icons">
    <figure class="panel desk">
      <div class="icon-set">
        {img(icon, 'mac', 'macOS icon', '128')}
        {img(icon, 'circle', 'Android icon, round mask', '128')}
        {img(icon, 'squircle', 'Android icon, rounded mask', '128')}
        {img(icon, 'square', 'Windows and Linux icon', '128')}
      </div>
      <div class="icon-set small">
        {img(icon, 'mac', '', '64')}{img(icon, 'mac', '', '48')}{img(icon, 'mac', '', '32')}
        {img(icon, 'square', '', '48')}{img(icon, 'square', '', '24')}
      </div>
      <figcaption>App icon: macOS, Android round and rounded masks, Windows and Linux square; smaller below</figcaption>
    </figure>
    <figure class="panel fav">
      <div class="tabs">{tab(fav, 'light')}{tab(fav, 'dark')}</div>
      <div class="fav-sizes">
        <span>{img(fav, '', '16 pixel favicon', '16')}<i>16</i></span>
        <span>{img(fav, '', '32 pixel favicon', '32')}<i>32</i></span>
        <span>{pixels(fav, 16, 6)}<i>16, enlarged</i></span>
      </div>
      <figcaption>Small version at its true 16 and 32 pixels</figcaption>
    </figure>
  </div>
</section>"""


def strip(outputs: list) -> str:
    cells = []
    for c, out in outputs:
        fav = uri(out["favicon.svg"])
        cells.append(f"""
    <div class="strip-cell">
      <h3>{html.escape(c['name'])}</h3>
      {tab(fav, 'light', c['name'])}
      {tab(fav, 'dark', c['name'])}
      <div class="bare">
        <span class="on-light">{img(fav, '', '', '16')}</span>
        <span class="on-dark">{img(fav, '', '', '16')}</span>
        <span class="on-light">{img(uri(out['mark-mono.svg'], '#000000'), '', '', '16')}</span>
      </div>
      {pixels(fav, 16, 8)}
    </div>""")
    return "".join(cells)


STYLE = round1.STYLE + """
.kind { margin: 0 0 2px; font-size: 12px; letter-spacing: 0.1em; text-transform: uppercase; color: var(--edge); }
.panel.lite figcaption { color: #5a6f8c; }
.panel.lite { border-color: #d5deea; }
.strip-grid { grid-template-columns: repeat(5, 1fr); }
.picks { display: grid; grid-template-columns: repeat(3, 1fr); gap: 14px; margin-top: 18px; }
.pick-card { background: rgba(0,0,0,0.25); border: 1px solid var(--edge-dim); border-radius: 12px; padding: 18px; }
.pick-card .rank { font-weight: 900; color: var(--amber); font-size: 13px; letter-spacing: 0.08em; }
.pick-card h3 { margin: 2px 0 10px; font-size: 22px; font-weight: 900; }
.pick-card .show { display: flex; gap: 12px; align-items: center; margin-bottom: 12px; }
.pick-card .show img.lockup { height: 34px; margin: 0; max-width: 70%; }
.pick-card p { margin: 0; color: var(--soft); font-size: 15px; }
@media (max-width: 900px) { .picks { grid-template-columns: 1fr; } }
"""

PICKS = [
    ("power-core", "Power Core",
     "The most 2001-console piece on the board without borrowing anything: black plastic, a lit core, gloss. "
     "It is a complete object, so it makes the strongest app icon here, and the small version is a clean lime 1 on "
     "black that reads instantly in a tab."),
    ("crt", "CRT",
     "The best story for this project, the decompilation and old screens, with the warmest era feel: LAN "
     "parties and amber monitors. Its pixel favicon is exact at 16 pixels, and the bitmap wordmark with its "
     "cursor gives the site a voice no other port has."),
    ("slab", "Slab",
     "The boldest brand: the name itself is the logo, drawn from scratch, so it is fully ownable and needs no "
     "symbol to explain. Red and bone stand out from every other game project, and the H1 tile is a strong, "
     "simple icon at any size."),
]


def picks(outputs: list) -> str:
    by_id = {c["id"]: o for c, o in outputs}
    cards = []
    for rank, (cid, name, why) in enumerate(PICKS, 1):
        o = by_id[cid]
        cards.append(f"""
    <div class="pick-card">
      <span class="rank">No. {rank}</span>
      <h3>{html.escape(name)}</h3>
      <div class="show">{img(uri(o['icon.svg']), 'mac', '', '56')}{img(uri(o['lockup.svg']), 'lockup', 'OpenH1')}</div>
      <p>{html.escape(why)}</p>
    </div>""")
    return f"""
  <section class="pick">
    <h2>Top three</h2>
    <p>Honourable mentions: Split is the most timeless and abstract (two shapes, the 1 is only the gap), Crest and
    Launch are polished and distinctive, and Field Stencil has the most character at full size. Big Box is the
    most fun but too loud for a front-page mark; it would make a great release banner or sticker.</p>
    <div class="picks">{''.join(cards)}</div>
  </section>"""


def board(outputs: list) -> str:
    sections = "".join(section(i + 1, c, o) for i, (c, o) in enumerate(outputs))
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>OpenH1 logo board, round 2</title>
<style>{STYLE}</style>
</head>
<body>
<div class="wrap">
  <h1>OpenH1 logo board, round 2</h1>
  <p class="lede">Ten new concepts in very different styles, several of them in the look of the early-2000s
  console era: black plastic with a light inside, glowing tubes, chrome box art, CRTs. Each is original,
  evokes rather than copies, and has a small version drawn by hand for 16 pixels, where the glow and bevel
  effects drop out.</p>
  <p class="note">Each concept is shown in its own palette, plus one-colour versions. Files are in
  website/logo/round2/, one folder per concept, made by website/logo/round2/build.py. Round 1 is in
  logo-board.html.</p>
  {sections}
  <section class="strip">
    <h2>At small size</h2>
    <p class="note">Each concept's small version at its true 16 pixels in a light and a dark browser tab, on white
    and on dark, the one-colour mark at 16 pixels, and the small version enlarged eight times.</p>
    <div class="strip-grid">{strip(outputs)}</div>
  </section>
  {picks(outputs)}
</div>
<script>{round1.SCRIPT}</script>
</body>
</html>
"""


def main() -> None:
    outputs = []
    for c in CONCEPTS:
        out = files(c)
        folder = HERE / c["id"]
        folder.mkdir(exist_ok=True)
        for name, content in out.items():
            (folder / name).write_text(content, encoding="utf-8")
        outputs.append((c, out))
        print(f"{folder.relative_to(WEBSITE.parent)}")
    BOARD.write_text(board(outputs), encoding="utf-8")
    print(BOARD.relative_to(WEBSITE.parent))


if __name__ == "__main__":
    main()
