#!/usr/bin/env python3
"""Draws round 3 of OpenH1's logo concepts, H1 as original military
sci-fi, and the board that compares them:

    python website/logo/round3/build.py

Eight variations of one direction: H1 stencilled, stamped, embroidered and
backlit, five of them as field equipment and three as early-2000s console
hardware would render them. All lettering is a stencil alphabet drawn here
(STENCIL), not a font. For each concept it writes, into
website/logo/round3/<concept>/, the same files as round 2 (mark, lockup,
app icon and 16-pixel small version), and website/logo-board-3.html. Needs
round 2's website/logo/round2/build.py and its dependencies.
"""

import html
import importlib.util
import random
from pathlib import Path

HERE = Path(__file__).resolve().parent
WEBSITE = HERE.parent.parent
BOARD = WEBSITE / "logo-board-3.html"

_spec = importlib.util.spec_from_file_location("round2", HERE.parent / "round2/build.py")
r2 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(r2)
num, svg, uri, img, rr, placed, glow = r2.num, r2.svg, r2.uri, r2.img, r2.rr, r2.placed, r2.glow
mark_body, icon_standard, MONO_COLOURS = r2.mark_body, r2.icon_standard, r2.MONO_COLOURS


# ---------------------------------------------------------------- the stencil alphabet
# cap height 100; each glyph: (width, polygons). Every stroke that would meet
# another is cut loose by a bridge, as a stencil must be.

def mirror(points, width):
    return [(width - x, y) for x, y in points]


_H_LEFT = [(0, 0), (22, 0), (22, 40), (29, 40), (29, 60), (22, 60), (22, 100), (0, 100)]
_O_LEFT = lambda b: [(b, 0), (18, 0), (0, 18), (0, 82), (18, 100), (b, 100), (b, 78), (28, 78), (22, 72), (22, 28), (28, 22), (b, 22)]
STENCIL = {
    "H": (64, [_H_LEFT, mirror(_H_LEFT, 64)]),
    "1": (42, [[(0, 21), (15, 2), (15, 24), (0, 43)], [(20, 0), (42, 0), (42, 100), (20, 100)]]),
    "0": (64, [_O_LEFT(29), mirror(_O_LEFT(29), 64)]),
    "O": (72, [_O_LEFT(33), mirror(_O_LEFT(33), 72)]),
    "P": (62, [[(0, 0), (22, 0), (22, 100), (0, 100)],
               [(28, 0), (46, 0), (62, 16), (62, 44), (46, 60), (28, 60), (28, 42), (38, 42), (42, 38), (42, 22), (38, 18), (28, 18)]]),
    "E": (54, [[(0, 0), (22, 0), (22, 100), (0, 100)], [(28, 0), (54, 0), (54, 20), (28, 20)],
               [(28, 40), (48, 40), (48, 60), (28, 60)], [(28, 80), (54, 80), (54, 100), (28, 100)]]),
    "N": (67, [[(0, 0), (22, 0), (39, 66), (39, 100), (22, 34), (22, 100), (0, 100)], [(45, 0), (67, 0), (67, 100), (45, 100)]]),
    "-": (30, [[(0, 42), (30, 42), (30, 58), (0, 58)]]),
    "/": (40, [[(28, 0), (40, 0), (12, 100), (0, 100)]]),
    " ": (24, []),
}


def stencil(t: str, x: float, cap: float, baseline: float, spacing: float = 10.0) -> tuple:
    """t in the stencil alphabet, its caps cap high, from x: (path, width)"""
    s = cap / 100
    out = []
    cursor = 0.0
    for ch in t:
        width, polygons = STENCIL[ch]
        for poly in polygons:
            out.append("M" + "L".join(f"{num(x + (cursor + px) * s)} {num(baseline - cap + py * s)}" for px, py in poly) + "Z")
        cursor += width + spacing
    return "".join(out), (cursor - spacing) * s


def stencil_width(t: str, cap: float, spacing: float = 10.0) -> float:
    return stencil(t, 0, cap, cap, spacing)[1]


def centred(t: str, cap: float, cx: float, baseline: float) -> str:
    return stencil(t, cx - stencil_width(t, cap) / 2, cap, baseline)[0]


def words(colours, is_mono, x, cap, baseline):
    """the wordmark: OPENH1 in the stencil alphabet"""
    d1, w1 = stencil("OPEN", x, cap, baseline)
    d2, w2 = stencil("H1", x + w1 + cap * 0.1, cap, baseline)
    return (f'<path fill="{colours["ink"]}" d="{d1}"/><path fill="{colours["accent"]}" d="{d2}"/>'), w1 + cap * 0.1 + w2


def fav_h1(colour: str, top: float = 4, height: float = 8, dx: float = 0) -> str:
    """H1 drawn on the 16-pixel grid"""
    b, mid = top + height, top + height / 2
    return (f'<path fill="{colour}" d="M{num(2 + dx)} {num(top)}h2v{num(height)}h-2Z'
            f'M{num(6 + dx)} {num(top)}h2v{num(height)}h-2Z'
            f'M{num(4 + dx)} {num(mid - 1)}h2v2h-2Z'
            f'M{num(11 + dx)} {num(top)}h2v{num(height)}h-2Z'
            f'M{num(9 + dx)} {num(top + 1)}h2v2h-2Z"/>')


def specks(seed: int, box, count: int, size=(0.5, 1.6)) -> str:
    rnd = random.Random(seed)
    x0, y0, x1, y1 = box
    out = []
    for _ in range(count):
        w = rnd.uniform(*size)
        h = rnd.uniform(size[0], size[1] * 0.7)
        x = rnd.uniform(x0, x1 - w)
        y = rnd.uniform(y0, y1 - h)
        out.append(f"M{num(x)} {num(y)}h{num(w)}v{num(h)}h{num(-w)}Z")
    return "".join(out)


def brushed(gid: str, seed: int, w: float = 100, h: float = 100, lines: int = 70) -> str:
    """a group of fine horizontal lines, the grain of brushed metal"""
    rnd = random.Random(seed)
    out = []
    for _ in range(lines):
        y = rnd.uniform(0, h)
        x = rnd.uniform(-20, w * 0.6)
        length = rnd.uniform(w * 0.3, w * 1.1)
        out.append(f'<rect x="{num(x)}" y="{num(y)}" width="{num(length)}" height=".35" '
                   f'fill="{"#fff" if rnd.random() < 0.5 else "#000"}" opacity="{num(rnd.uniform(0.05, 0.16))}"/>')
    return f'<g id="{gid}">{"".join(out)}</g>'


CONCEPTS = []


# 1. Hull Marking: H1 stencilled on an armour panel, with seams, fasteners and wear
HULL_PANEL = "M14 4H86L96 14V86L86 96H14L4 86V14Z"
HULL_H1 = centred("H1", 40, 50, 62)
HULL_SERIAL = stencil("OPENH1-001", 14, 5, 90)[0]
HULL_DEFS = ('<linearGradient id="hull" gradientUnits="userSpaceOnUse" x1="0" y1="4" x2="0" y2="96">'
             '<stop offset="0" stop-color="#535c48"/><stop offset="1" stop-color="#32382b"/></linearGradient>')
RIVETS = [(12, 12), (88, 12), (12, 71), (88, 71)]
HULL_LAYERS = [
    ("panel", "fill", HULL_PANEL),
    ("seam", "cut", '<path fill="none" stroke="{c}" stroke-width="1.3" d="M4 77.5H96M62 77.5V96"/>'),
    ("lip", "skip", '<path fill="none" stroke="{c}" stroke-width=".6" d="M4 79H96M63.4 79V96"/>'),
    ("lip", "skip", f'<path fill="none" stroke="{{c}}" stroke-width=".7" opacity=".6" d="M14.4 5H85.6L95 14.4"/>'),
    ("seam", "cut", "".join(f"M{x - 1.9} {y}a1.9 1.9 0 1 0 3.8 0a1.9 1.9 0 1 0-3.8 0Z" for x, y in RIVETS)),
    ("lip", "skip", "".join(f"M{x - 0.8} {y - 0.7}a.7 .7 0 1 0 1.4 0a.7 .7 0 1 0-1.4 0Z" for x, y in RIVETS)),
    ("paint", "cut", HULL_H1),
    ("panel", "fill", specks(7, (27, 22, 73, 62), 90)),
    ("paint", "cut", HULL_SERIAL),
    ("accent", "cut", "M66 84.5H88V90H66Z"),
    ("seam", "skip", '<path fill="{c}" d="M70 84.5h3l-3 5.5h-3Z M77 84.5h3l-3 5.5h-3Z M84 84.5h3l-2.8 5.5h-3Z" opacity=".85"/>'),
]
HULL_PAL = {"panel": "url(#hull)", "seam": "#1f2419", "lip": "#7b8569", "paint": "#e7e0c9", "accent": "#ff6b1a",
            "ink": "#e7e0c9", "accent2": "#ff6b1a"}
CONCEPTS.append({
    "id": "hull", "name": "Hull Marking", "style": "Field: stencil on an armour panel",
    "ground": ("#14170f", "#ecebe3"),
    "pal": {"dark": dict(HULL_PAL, accent="#ff6b1a"), "light": dict(HULL_PAL, ink="#2b3122")},
    "defs": {"dark": HULL_DEFS, "light": HULL_DEFS},
    "layers": HULL_LAYERS,
    "icon": lambda c: ('<rect width="1024" height="1024" fill="#2b3024"/>'
                       '<path fill="none" stroke="#1f2419" stroke-width="10" d="M0 300H1024M0 760H1024M700 0V300"/>'
                       + placed(mark_body(c, "dark"), 7.4, 142, 142)),
    "wordmark": words, "cap": 20,
    "favicon": ('<path fill="#465040" d="M2.5 0H13.5L16 2.5V13.5L13.5 16H2.5L0 13.5V2.5Z"/>' + fav_h1("#e7e0c9", 3, 8)
                + '<path fill="#ff6b1a" d="M8 13h5v1.5h-5Z"/>'),
    "idea": "H1 sprayed through a stencil onto an armour panel: seams, four fasteners, chipped paint, a small "
            "serial and a signal-orange tab. It reads as a marking on real equipment rather than a logo.",
    "legal": "Our own stencil letters on a generic chamfered panel. No emblem, insignia, unit name or lettering from "
             "the series.",
    "xbox": "Not UNSC: no eagle, globe, wings or star, and none of its type. Not Xbox: olive and sand, no X, orb or green.",
})


# 2. Squad Patch: an embroidered morale patch with a merrowed border
PATCH = rr(6, 12, 88, 76, 14)
PATCH_DEFS = ('<pattern id="weave" width="2" height="2" patternUnits="userSpaceOnUse">'
              '<path d="M0 2L2 0" stroke="#000" stroke-width=".5" opacity=".18"/></pattern>'
              '<pattern id="satin" width="1.4" height="1.4" patternUnits="userSpaceOnUse" patternTransform="rotate(35)">'
              '<rect width="1.4" height="1.4" fill="#d9c592"/><rect width="1.4" height=".5" fill="#b9a372"/></pattern>'
              '<pattern id="satin2" width="1.4" height="1.4" patternUnits="userSpaceOnUse" patternTransform="rotate(35)">'
              '<rect width="1.4" height="1.4" fill="#f07a2c"/><rect width="1.4" height=".5" fill="#c95e1a"/></pattern>')
PATCH_H1 = centred("H1", 32, 50, 58)
PATCH_WORD = centred("OPENH1", 6.5, 50, 76.5)
PATCH_PAL = {"field": "#4b5232", "weave": "url(#weave)", "border": "#1c2013", "stitch": "#9b9572",
             "thread": "url(#satin)", "thread2": "url(#satin2)", "edge": "#262a19", "ink": "#d9c592", "accent": "#f07a2c"}
PATCH_LIGHT = dict(PATCH_PAL, ink="#3a4026", accent="#d9621a")
PATCH_LAYERS = [
    ("field", "fill", PATCH),
    ("weave", "skip", PATCH),
    ("border", "fill", f'<path fill="none" stroke="{{c}}" stroke-width="5.5" d="{rr(8.75, 14.75, 82.5, 70.5, 11.25)}"/>'),
    ("stitch", "cut", f'<path fill="none" stroke="{{c}}" stroke-width="1" stroke-dasharray="2.2 1.4" d="{rr(14, 20, 72, 60, 7)}"/>'),
    ("edge", "skip", f'<path fill="none" stroke="{{c}}" stroke-width="1.4" stroke-linejoin="round" d="{PATCH_H1}"/>'),
    ("thread", "cut", PATCH_H1),
    ("thread2", "cut", "M34 64.5H66V66.5H34Z"),
    ("thread2", "cut", PATCH_WORD),
]
CONCEPTS.append({
    "id": "patch", "name": "Squad Patch", "style": "Field: embroidered unit patch",
    "ground": ("#191c11", "#efeadb"),
    "pal": {"dark": PATCH_PAL, "light": PATCH_LIGHT},
    "defs": {"dark": PATCH_DEFS, "light": PATCH_DEFS},
    "layers": PATCH_LAYERS,
    "icon": lambda c: ('<defs><pattern id="cloth" width="12" height="12" patternUnits="userSpaceOnUse">'
                       '<path d="M0 12L12 0" stroke="#000" stroke-width="3" opacity=".2"/></pattern></defs>'
                       '<rect width="1024" height="1024" fill="#2c3120"/><rect width="1024" height="1024" fill="url(#cloth)"/>'
                       + placed(mark_body(c, "dark"), 9.2, 52, 52)),
    "wordmark": words, "cap": 20,
    "favicon": ('<rect x=".5" y="1.5" width="15" height="13" rx="3" fill="#4b5232" stroke="#1c2013"/>'
                + fav_h1("#d9c592", 4, 7) + '<path fill="#f07a2c" d="M5 12.2h6v1h-6Z"/>'),
    "idea": "H1 embroidered in satin thread on an olive morale patch, with a merrowed edge, a running stitch and "
            "the name in orange: the badge a squad would sew on. As an app icon it sits on cloth.",
    "legal": "A plain rounded patch with our own letters; no unit, rank or service insignia of any real or "
             "fictional force.",
    "xbox": "Not UNSC: no eagle, globe or wings, no rank devices from the series. Not Xbox: olive and tan thread, no X or orb.",
})


# 3. Serial Plate: a stamped equipment ID plate with fasteners
SERIAL_DEFS = ('<linearGradient id="alu" gradientUnits="userSpaceOnUse" x1="0" y1="16" x2="0" y2="84">'
               '<stop offset="0" stop-color="#eceff1"/><stop offset=".5" stop-color="#c9ced3"/><stop offset="1" stop-color="#a3aab1"/></linearGradient>'
               '<radialGradient id="screw" cx=".35" cy=".3" r=".8"><stop offset="0" stop-color="#f4f6f7"/><stop offset="1" stop-color="#6f767d"/></radialGradient>'
               f'<clipPath id="plate"><path d="{rr(4, 16, 92, 68, 5)}"/></clipPath>' + brushed("grain", 11))
SCREWS = [(10.5, 22.5), (89.5, 22.5), (10.5, 77.5), (89.5, 77.5)]


def barcode(seed: int, x: float, y: float, w: float, h: float) -> str:
    rnd = random.Random(seed)
    out, cx = [], x
    while cx < x + w:
        bw = rnd.choice([0.6, 0.6, 1.2, 1.8])
        if cx + bw > x + w:
            break
        out.append(f"M{num(cx)} {num(y)}h{num(bw)}v{num(h)}h{num(-bw)}Z")
        cx += bw + rnd.choice([0.6, 1.2])
    return "".join(out)


def screw_shapes(points, r=3.0):
    return "".join(f"M{num(x - r)} {num(y)}a{num(r)} {num(r)} 0 1 0 {num(2 * r)} 0a{num(r)} {num(r)} 0 1 0 {num(-2 * r)} 0Z" for x, y in points)


def slots(points, r=3.0, angle=30):
    return "".join(f'<path stroke="{{c}}" stroke-width=".9" transform="rotate({angle + i * 37} {x} {y})" '
                   f'd="M{num(x - r * 0.75)} {y}H{num(x + r * 0.75)}"/>' for i, (x, y) in enumerate(points))


SERIAL_BIG_A, _wa = stencil("H1", 14.5, 20, 62)
SERIAL_BIG_B = stencil("-001", 14.5 + _wa + 2, 20, 62)[0]
SERIAL_LAYERS = [
    ("plate", "fill", rr(4, 16, 92, 68, 5)),
    ("grain", "skip", '<use href="#grain" clip-path="url(#plate)"/>'),
    ("rim", "skip", f'<path fill="none" stroke="{{c}}" stroke-width=".8" opacity=".7" d="{rr(5, 17, 90, 66, 4.2)}"/>'),
    ("screw", "cut", screw_shapes(SCREWS)),
    ("ink", "skip", slots(SCREWS)),
    ("ink", "cut", stencil("OPENH1", 17, 7, 33)[0]),
    ("ink", "cut", "M17 37H83V37.8H17Z"),
    ("ink", "cut", SERIAL_BIG_A),
    ("accent", "cut", SERIAL_BIG_B),
    ("ink", "cut", barcode(5, 17, 67, 44, 7)),
    ("ink", "cut", stencil("001", 68, 6, 74)[0]),
]
SERIAL_PAL = {"plate": "url(#alu)", "grain": "", "rim": "#ffffff", "screw": "url(#screw)", "ink": "#23272b", "accent": "#e8590c"}


def serial_lockup(c, variant):
    colours = MONO_COLOURS if variant == "mono" else c["pal"][variant]
    cap, h = 20.0, 40.0
    text = "OPENH1 / 001"
    tw = stencil_width(text, cap)
    w = tw + 40
    pts = [(8, 8), (w - 8, 8), (8, h - 8), (w - 8, h - 8)]
    d1, w1 = stencil("OPENH1", 20, cap, h / 2 + cap / 2)
    d2 = stencil(" / 001", 20 + w1 + 10 * cap / 100, cap, h / 2 + cap / 2)[0]
    if variant == "mono":
        layers = [("p", "fill", rr(0, 0, w, h, 5)), ("p", "cut", screw_shapes(pts, 2.6)), ("p", "cut", d1), ("p", "cut", d2)]
        return svg(w, h, r2.mono(layers), "OpenH1")
    defs = ('<defs>' + SERIAL_DEFS +
            '<linearGradient id="alu2" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#eceff1"/>'
            '<stop offset="1" stop-color="#a3aab1"/></linearGradient></defs>')
    body = (defs + f'<path fill="url(#alu2)" d="{rr(0, 0, w, h, 5)}"/>'
            f'<path fill="url(#screw)" d="{screw_shapes(pts, 2.6)}"/>'
            + slots(pts, 2.6).replace("{c}", "#23272b") +
            f'<path fill="#23272b" d="{d1}"/><path fill="#e8590c" d="{d2}"/>')
    return svg(w, h, body, "OpenH1")


CONCEPTS.append({
    "id": "serial", "name": "Serial Plate", "style": "Field: stamped equipment plate",
    "ground": ("#101214", "#e9ecee"),
    "pal": {"dark": SERIAL_PAL, "light": SERIAL_PAL},
    "defs": {"dark": SERIAL_DEFS, "light": SERIAL_DEFS},
    "layers": SERIAL_LAYERS,
    "icon": lambda c: icon_standard(c, '<rect width="1024" height="1024" fill="#1d2023"/>', 900),
    "lockup": serial_lockup,
    "favicon": ('<rect x=".5" y="2.5" width="15" height="11" rx="2" fill="#cfd4d8" stroke="#4d5359"/>'
                + fav_h1("#23272b", 4.5, 7)),
    "idea": "The ID plate riveted to a piece of kit: brushed aluminium, four screws, OPENH1 stamped on top, the unit "
            "number H1-001 in big stencil with the number in orange, and a data strip. The lockup is the plate itself.",
    "legal": "A generic equipment plate with our own name and number on it; no real or fictional maker, service or "
             "unit is named.",
    "xbox": "Not UNSC: no emblem, crest or service name. Not Xbox: bare aluminium and orange, no X, orb or green.",
})


# 4. Hazard Block: blocky stencil H1 over hazard striping
def hazard_stripes(x0, y0, x1, y1, w=6.0, gap=6.0) -> str:
    h = y1 - y0
    out, x = [], x0 - h
    while x < x1:
        out.append(f"M{num(x)} {num(y1)}L{num(x + h)} {num(y0)}h{num(w)}L{num(x + w)} {num(y1)}Z")
        x += w + gap
    return "".join(out)


HAZ_DEFS = f'<clipPath id="band"><rect x="4" y="73" width="92" height="15"/></clipPath>'
CONCEPTS.append({
    "id": "hazard", "name": "Hazard Block", "style": "Field: blocky stencil and hazard stripes",
    "ground": ("#121214", "#f1efe8"),
    "pal": {"dark": {"tile": "#ffc21a", "ink": "#16171a", "accent": "#ffc21a", "word": "#ffc21a"},
            "light": {"tile": "#ffc21a", "ink": "#16171a", "accent": "#d99a00", "word": "#16171a"}},
    "defs": {"dark": HAZ_DEFS, "light": HAZ_DEFS},
    "layers": [
        ("tile", "fill", rr(4, 4, 92, 92, 7)),
        ("ink", "cut", centred("H1", 46, 50, 62)),
        ("ink", "cut", f'<path fill="{{c}}" clip-path="url(#band)" d="{hazard_stripes(4, 73, 96, 88)}"/>'),
        ("ink", "cut", "M4 69H96V70.6H4Z"),
    ],
    "icon": lambda c: ('<rect width="1024" height="1024" fill="#ffc21a"/>'
                       + placed(f'<path fill="#16171a" d="{centred("H1", 46, 50, 60)}"/>', 10.24)
                       + f'<path fill="#16171a" d="{hazard_stripes(0, 760, 1024, 1024, 72, 72)}"/>'
                       + '<rect y="716" width="1024" height="18" fill="#16171a"/>'),
    "wordmark": lambda colours, m, x, cap, b: words({"ink": colours["word"], "accent": colours["accent"] if not m else "currentColor"}, m, x, cap, b),
    "cap": 20,
    "favicon": ('<rect width="16" height="16" rx="2.5" fill="#ffc21a"/>' + fav_h1("#16171a", 2.5, 7)
                + '<path fill="#16171a" d="M0 11.5H16V13.5Q16 16 13.5 16H2.5Q0 16 0 13.5Z"/>'
                + '<path fill="#ffc21a" d="M2.5 16l2-4.5h1.6l-2 4.5Z M7.5 16l2-4.5h1.6l-2 4.5Z M12.5 16l2-4.5h1.5V12.5l-1.6 3.5Z"/>'),
    "idea": "H1 cut from heavy stencil blocks in black on safety yellow, above a band of hazard striping: the marking "
            "on a blast door or a loading bay. Blunt, loud and readable from across a hangar or a tab bar.",
    "legal": "Hazard striping is a universal safety convention, and the letters are our own stencil blocks.",
    "xbox": "Not UNSC: no emblem, insignia or series lettering. Not Xbox: yellow and black, no X, orb or green.",
})


# 5. ID Tag: an embossed steel identity tag
TAG_DEFS = ('<linearGradient id="steel" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#eef1f3"/>'
            '<stop offset=".55" stop-color="#b9c0c6"/><stop offset="1" stop-color="#8b939b"/></linearGradient>')
TAG = rr(4, 20, 92, 60, 18)
TAG_TEXT = (stencil("H1", 22, 28, 60)[0], stencil("OPENH1", 61, 5.6, 40)[0], stencil("H1-001", 61, 5.6, 50)[0],
            stencil("0001", 61, 5.6, 60)[0])
TAG_ALL = "".join(TAG_TEXT)
CONCEPTS.append({
    "id": "id-tag", "name": "ID Tag", "style": "Field: embossed identity tag",
    "ground": ("#111315", "#eceef0"),
    "pal": {"dark": {"tag": "url(#steel)", "rim": "#ffffff", "slot": "#15181b", "shade": "#3e454c", "hi": "#ffffff",
                     "face": "#8d969f", "ink": "#c3c9cf", "accent": "#ffffff"},
            "light": {"tag": "url(#steel)", "rim": "#ffffff", "slot": "#15181b", "shade": "#3e454c", "hi": "#ffffff",
                      "face": "#8d969f", "ink": "#5f676f", "accent": "#2a2f34"}},
    "defs": {"dark": TAG_DEFS, "light": TAG_DEFS},
    "layers": [
        ("tag", "fill", TAG),
        ("rim", "skip", f'<path fill="none" stroke="{{c}}" stroke-width="1" opacity=".55" d="{rr(7.5, 23.5, 85, 53, 14.5)}"/>'),
        ("shade", "skip", f'<path fill="none" stroke="{{c}}" stroke-width=".8" opacity=".5" d="{rr(8.5, 24.5, 83, 51, 13.5)}"/>'),
        ("slot", "cut", rr(11, 42, 5, 16, 2.5)),
        ("shade", "skip", f'<path fill="{{c}}" transform="translate(.9 1.1)" d="{TAG_ALL}"/>'),
        ("hi", "skip", f'<path fill="{{c}}" transform="translate(-.5 -.6)" d="{TAG_ALL}"/>'),
        ("face", "cut", TAG_ALL),
    ],
    "icon": lambda c: icon_standard(c, '<defs><radialGradient id="felt" cx=".5" cy=".4" r=".8"><stop offset="0" stop-color="#2d3238"/>'
                                       '<stop offset="1" stop-color="#121417"/></radialGradient></defs><rect width="1024" height="1024" fill="url(#felt)"/>', 900),
    "wordmark": words, "cap": 20,
    "favicon": ('<rect x=".5" y="3" width="15" height="10" rx="3.5" fill="#c3c9cf" stroke="#4d555c"/>' + fav_h1("#2a2f34", 5, 6)),
    "idea": "An identity tag pressed from steel: a raised H1, the name and two numbers embossed beside it, a rolled "
            "edge and a slot for the chain. Quiet and personal, the mark every member of the squad carries.",
    "legal": "A plain rounded tag with our own name and numbers; no real or fictional service, person or record.",
    "xbox": "Not UNSC: no emblem or service details from the series. Not Xbox: bare steel, no X, orb or green.",
})


# 6. Backlit: gunmetal console hardware, the stencil lit lime from behind
BL_H1 = centred("H1", 40, 50, 64)
BL_DEFS = ('<linearGradient id="gun" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#686f77"/>'
           '<stop offset=".45" stop-color="#3a3f45"/><stop offset="1" stop-color="#1b1e22"/></linearGradient>'
           '<linearGradient id="recess" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#1c1f23"/>'
           '<stop offset="1" stop-color="#30353b"/></linearGradient>'
           '<linearGradient id="lit" gradientUnits="userSpaceOnUse" x1="0" y1="24" x2="0" y2="64"><stop offset="0" stop-color="#f3ffc2"/>'
           '<stop offset=".5" stop-color="#d0ff3a"/><stop offset="1" stop-color="#9fe000"/></linearGradient>'
           f'<clipPath id="body"><path d="{rr(4, 4, 92, 92, 20)}"/></clipPath>' + glow("bl", 3.4) + glow("led", 1.2)
           + brushed("grain2", 3))
BL_LAYERS = [
    ("gun", "fill", rr(4, 4, 92, 92, 20)),
    ("grain", "skip", '<use href="#grain2" clip-path="url(#body)"/>'),
    ("hi", "skip", f'<path fill="none" stroke="{{c}}" stroke-width="1" opacity=".35" d="{rr(5, 5, 90, 90, 19)}"/>'),
    ("recess", "skip", rr(12, 12, 76, 76, 13)),
    ("dark", "skip", f'<path fill="none" stroke="{{c}}" stroke-width="1.2" opacity=".6" d="{rr(12.6, 12.6, 74.8, 74.8, 12.4)}"/>'),
    ("lime", "skip", f'<path fill="{{c}}" filter="url(#bl)" opacity=".85" d="{BL_H1}"/>'),
    ("lit", "cut", BL_H1),
    ("shadow", "skip", f'<path fill="none" stroke="{{c}}" stroke-width="1.2" opacity=".45" transform="translate(.5 .7)" d="{BL_H1}"/>'),
    ("lime", "skip", '<rect x="66" y="76.5" width="12" height="3.2" rx="1.6" fill="{c}" filter="url(#led)"/>'),
    ("pip", "cut", rr(66, 76.5, 12, 3.2, 1.6)),
]
BL_PAL = {"gun": "url(#gun)", "grain": "", "hi": "#ffffff", "recess": "url(#recess)", "dark": "#0a0b0c", "lime": "#c8ff2e",
          "lit": "url(#lit)", "shadow": "#1b2a00", "pip": "#e9ffa8", "ink": "#c4c9ce", "accent": "#c8ff2e"}
CONCEPTS.append({
    "id": "backlit", "name": "Backlit", "style": "2001 hardware: gunmetal, lit through the stencil",
    "ground": ("#0c0d0e", "#e7e9eb"),
    "pal": {"dark": BL_PAL, "light": dict(BL_PAL, ink="#2a2e33", accent="#5a8a00")},
    "defs": {"dark": BL_DEFS, "light": BL_DEFS},
    "layers": BL_LAYERS,
    "icon": lambda c: ('<defs>' + BL_DEFS + '</defs><rect width="1024" height="1024" fill="url(#gun)"/>'
                       '<use href="#grain2" transform="scale(10.24)"/>'
                       + placed(r2.body([l for l in BL_LAYERS if l[0] not in ("gun", "grain", "hi")], BL_PAL), 11.6, -68, -68)),
    "wordmark": words, "cap": 20,
    "favicon": '<rect width="16" height="16" rx="3.5" fill="#3a3f45"/>' + fav_h1("#c8ff2e", 4, 8),
    "idea": "The stencil cut through a gunmetal console panel, with lime light shining through the letters and a "
            "power pip glowing below, as if the hardware has just switched on. Military marking, 2001 hardware.",
    "legal": "A rounded metal panel and our own stencil letters lit from behind; nothing from the game's equipment, "
             "interface or type.",
    "xbox": "Not UNSC: no emblem or insignia. Not Xbox: no X, no orb or jewel; the lime is a glow accent shifted to "
            "yellow-green, on gunmetal.",
})


# 7. Jelly: a translucent teal panel in black plastic, the stencil frosted into it
JL_H1 = centred("H1", 36, 50, 66)
JL_DEFS = ('<linearGradient id="shell2" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#34383b"/>'
           '<stop offset="1" stop-color="#0a0b0c"/></linearGradient>'
           '<linearGradient id="gel" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#44f5d6"/>'
           '<stop offset=".55" stop-color="#0d9a98"/><stop offset="1" stop-color="#05505a"/></linearGradient>'
           '<radialGradient id="core" cx=".5" cy=".6" r=".5"><stop offset="0" stop-color="#d8fff6" stop-opacity=".55"/>'
           '<stop offset="1" stop-color="#d8fff6" stop-opacity="0"/></radialGradient>'
           '<linearGradient id="sheen" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".55"/>'
           '<stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient>' + glow("jl", 2.6))
JL_LAYERS = [
    ("shell", "fill", rr(4, 4, 92, 92, 24)),
    ("hi", "skip", f'<path fill="none" stroke="{{c}}" stroke-width="1" opacity=".3" d="{rr(5, 5, 90, 90, 23)}"/>'),
    ("gel", "cut", rr(13, 13, 74, 74, 15)),
    ("core", "skip", rr(13, 13, 74, 74, 15)),
    ("deep", "skip", f'<path fill="none" stroke="{{c}}" stroke-width="2.2" opacity=".55" d="{rr(14.1, 14.1, 71.8, 71.8, 14)}"/>'),
    ("glow", "skip", f'<path fill="{{c}}" filter="url(#jl)" opacity=".9" d="{JL_H1}"/>'),
    ("frost", "fill", JL_H1),
    ("sheen", "skip", "M28 13H72C80.3 13 87 19.7 87 28V36C68 30 32 30 13 36V28C13 19.7 19.7 13 28 13Z"),
]
JL_PAL = {"shell": "url(#shell2)", "hi": "#ffffff", "gel": "url(#gel)", "core": "url(#core)", "deep": "#03343a",
          "glow": "#5cffe0", "frost": "#effffb", "sheen": "url(#sheen)", "ink": "#d6fbf3", "accent": "#3ff0cf"}
CONCEPTS.append({
    "id": "jelly", "name": "Jelly Panel", "style": "2001 hardware: translucent jelly in black plastic",
    "ground": ("#06100f", "#eaf3f1"),
    "pal": {"dark": JL_PAL, "light": dict(JL_PAL, ink="#0d2a27", accent="#00927c")},
    "defs": {"dark": JL_DEFS, "light": JL_DEFS},
    "layers": JL_LAYERS,
    "icon": lambda c: ('<defs>' + JL_DEFS + '</defs><rect width="1024" height="1024" fill="url(#shell2)"/>'
                       + placed(r2.body([l for l in JL_LAYERS if l[0] not in ("shell", "hi")], JL_PAL), 12.2, -98, -98)),
    "wordmark": words, "cap": 20,
    "favicon": ('<rect width="16" height="16" rx="4" fill="#141617"/><rect x="1.5" y="1.5" width="13" height="13" rx="3" fill="#0fa8a2"/>'
                + fav_h1("#f2fffc", 4, 8)),
    "idea": "A window of teal jelly set in chunky black plastic, glowing from inside, with the stencil frosted into "
            "it and a gloss across the top: the translucent hardware of 2001 wearing a squad marking.",
    "legal": "A rounded window in a rounded case with our own stencil letters; no device, shape or type from the game "
             "or any console.",
    "xbox": "Not UNSC: no emblem or insignia. Not Xbox: a teal rounded square, not a green X or orb.",
})


# 8. Chrome Insignia: a bevelled chrome badge with an embossed stencil and a teal line
BADGE = "M18 8H82L94 20V56L50 93L6 56V20Z"
INNER = 'transform="translate(50 50) scale(.84) translate(-50 -50)"'
LINE = 'transform="translate(50 50) scale(.75) translate(-50 -50)"'
IN_H1 = centred("H1", 28, 50, 57)
CHEVRON = "M36 71L50 64L64 71V75.5L50 68.5L36 75.5Z"
IN_DEFS = ('<linearGradient id="rim" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#ffffff"/>'
           '<stop offset=".35" stop-color="#aab4c0"/><stop offset=".5" stop-color="#4c5664"/>'
           '<stop offset=".65" stop-color="#c9d1db"/><stop offset="1" stop-color="#6d7785"/></linearGradient>'
           '<linearGradient id="chrome3" gradientUnits="userSpaceOnUse" x1="0" y1="29" x2="0" y2="76">'
           '<stop offset="0" stop-color="#ffffff"/><stop offset=".45" stop-color="#c3ccd8"/><stop offset=".5" stop-color="#46505e"/>'
           '<stop offset=".62" stop-color="#9aa6b6"/><stop offset="1" stop-color="#eef2f7"/></linearGradient>'
           f'<clipPath id="field"><path {INNER} d="{BADGE}"/></clipPath>' + glow("teal", 1.4))
IN_LAYERS = [
    ("rim", "fill", BADGE),
    ("gap", "cut", f'<path fill="none" stroke="{{c}}" stroke-width="1.4" {INNER} d="{BADGE}"/>'),
    ("left", "skip", f'<path fill="{{c}}" {INNER} d="{BADGE}"/>'),
    ("right", "skip", '<path fill="{c}" clip-path="url(#field)" d="M50 0H100V100H50Z"/>'),
    ("teal", "skip", f'<path fill="none" stroke="{{c}}" stroke-width="1.6" filter="url(#teal)" {LINE} d="{BADGE}"/>'),
    ("teal", "cut", f'<path fill="none" stroke="{{c}}" stroke-width=".9" {LINE} d="{BADGE}"/>'),
    ("drop", "skip", f'<path fill="{{c}}" transform="translate(.9 1.3)" d="{IN_H1}{CHEVRON}"/>'),
    ("chrome", "cut", IN_H1),
    ("chrome", "cut", CHEVRON),
    ("lip", "skip", f'<path fill="none" stroke="{{c}}" stroke-width=".4" d="{IN_H1}{CHEVRON}"/>'),
]
IN_PAL = {"rim": "url(#rim)", "gap": "#0c0e10", "left": "#3a424b", "right": "#222830", "teal": "#2bf0d0",
          "drop": "#07090b", "chrome": "url(#chrome3)", "lip": "#ffffff", "ink": "#d5dce4", "accent": "#2bf0d0"}
CONCEPTS.append({
    "id": "insignia", "name": "Chrome Insignia", "style": "2001 hardware: brushed chrome badge",
    "ground": ("#0a0c0f", "#e8ebef"),
    "pal": {"dark": IN_PAL, "light": dict(IN_PAL, ink="#1e242b", accent="#00927c")},
    "defs": {"dark": IN_DEFS, "light": IN_DEFS},
    "layers": IN_LAYERS,
    "icon": lambda c: icon_standard(c, '<defs><radialGradient id="bay" cx=".5" cy=".35" r=".8"><stop offset="0" stop-color="#21323a"/>'
                                       '<stop offset="1" stop-color="#07090b"/></radialGradient></defs><rect width="1024" height="1024" fill="url(#bay)"/>', 860, 10),
    "wordmark": words, "cap": 20,
    "favicon": ('<path fill="#aeb7c2" d="M3.5 .5H12.5L15 3V9.5L8 15.5L1 9.5V3Z"/><path fill="#262c33" d="M4.2 2H11.8L13.5 3.7V9L8 13.8L2.5 9V3.7Z"/>'
                + fav_h1("#eef2f7", 3.5, 6, 0)),
    "idea": "A faceted badge in bevelled chrome around a gunmetal field, with an embossed chrome stencil H1, a single "
            "rank-style chevron and a teal light line running inside the rim: insignia as 2001 hardware would cast it.",
    "legal": "Our own angular badge outline with generic chevron and stencil letters; no wings, star, eagle, globe or "
             "any insignia from the series.",
    "xbox": "Not UNSC: no eagle, globe or wings-and-star. Not Xbox: chrome and teal, an angular badge, no X or orb.",
})


# ---------------------------------------------------------------- the board

PICKS = [
    ("backlit", "Backlit",
     "It is the brief in one object: a real military stencil, rendered as 2001 console hardware with the light "
     "coming through the cut-outs. It is the strongest app icon of the round, the 16-pixel version is a clean lime "
     "H1 on gunmetal, and it stays clearly its own mark, with no emblem and no X or orb."),
    ("hull", "Hull Marking",
     "The most believable military sci-fi mark here: it looks like paint on equipment, not a logo pretending to be "
     "one. The seams, fasteners and wear carry the world, the stencil alphabet is ours, and it simplifies cleanly "
     "to a sand H1 on an olive plate at 16 pixels."),
]


def section(i, c, out):
    return r2.section(i, c, out).replace("Why it's not the Xbox logo.", "Why it's not UNSC or Xbox.")


def picks(outputs):
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
    <h2>Top two</h2>
    <p>Close behind: Serial Plate is the most practical lockup (the plate is the wordmark), and Chrome Insignia is the
    most "2001 console" of the badges. Squad Patch and ID Tag are lovely merchandise but busier as a front-page mark.</p>
    <div class="picks" style="grid-template-columns:repeat(2,1fr)">{''.join(cards)}</div>
  </section>"""


def board(outputs):
    sections = "".join(section(i + 1, c, o) for i, (c, o) in enumerate(outputs))
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>OpenH1 logo board, round 3</title>
<style>{r2.STYLE}
.strip-grid {{ grid-template-columns: repeat(4, 1fr); }}</style>
</head>
<body>
<div class="wrap">
  <h1>OpenH1 logo board, round 3</h1>
  <p class="lede">One direction, eight ways: H1 as plausible military sci-fi. Five variants are field equipment
  (a hull marking, a patch, a serial plate, hazard blocks, an ID tag); three render the same marks the way 2001
  console hardware would (backlit gunmetal, translucent jelly, chrome).</p>
  <p class="note">All lettering is a stencil alphabet drawn for this project, not a font: every stroke that would meet
  another is cut loose by a bridge. No eagle, globe, wings or star, no series lettering, and no X, orb or ring.
  Files are in website/logo/round3/, made by website/logo/round3/build.py. Rounds 1 and 2 are in logo-board.html
  and logo-board-2.html.</p>
  {sections}
  <section class="strip">
    <h2>At small size</h2>
    <p class="note">Each variant's small version at its true 16 pixels in a light and a dark browser tab, on white and on
    dark, the one-colour mark at 16 pixels, and the small version enlarged eight times.</p>
    <div class="strip-grid">{r2.strip(outputs)}</div>
  </section>
  {picks(outputs)}
</div>
<script>{r2.round1.SCRIPT}</script>
</body>
</html>
"""


def files(c: dict) -> dict:
    return r2.files(c)


def main() -> None:
    outputs = []
    for c in CONCEPTS:
        c.setdefault("cap", 20)
        out = files(c)
        folder = HERE / c["id"]
        folder.mkdir(exist_ok=True)
        for name, content in out.items():
            (folder / name).write_text(content, encoding="utf-8")
        outputs.append((c, out))
        print(folder.relative_to(WEBSITE.parent))
    BOARD.write_text(board(outputs), encoding="utf-8")
    print(BOARD.relative_to(WEBSITE.parent))


if __name__ == "__main__":
    main()
