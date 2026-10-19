#!/usr/bin/env python3
"""Draws round 5: six panel concepts for the Hull Marking direction, each
with OPEN stencilled above H1, and the board that compares them:

    python website/logo/round5/build.py

Each panel is drawn straight on and flat-shaded like a technical
illustration, so it can be modelled: one light from the top left, raised
faces bevelled light on their upper-left edges and dark on their lower-right
ones, recessed faces the other way round, and flat drop shadows under the
parts that sit proud. The paint is fresh and solid: the H in white, the 1 in
orange, OPEN smaller above. Lettering is round 3's stencil alphabet. For
each concept it writes, into website/logo/round5/<concept>/, the same files
as rounds 2 to 4, and website/logo-board-5.html. Needs round 3's
website/logo/round3/build.py and its dependencies.
"""

import html
import importlib.util
import math
from pathlib import Path

HERE = Path(__file__).resolve().parent
WEBSITE = HERE.parent.parent
BOARD = WEBSITE / "logo-board-5.html"

_spec = importlib.util.spec_from_file_location("round3", HERE.parent / "round3/build.py")
r3 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(r3)
r2 = r3.r2
num, svg, uri, img, placed = r2.num, r2.svg, r2.uri, r2.img, r2.placed
stencil, stencil_width = r3.stencil, r3.stencil_width

WHITE = "#f1f2f0"
ORANGE = "#ff5a0a"
ORANGE_ON_LIGHT = "#d94800"   # 3.7 to 1 on a light ground, where #ff5a0a is 2.7
HULL = "#465048"
HULL_LIGHT = "#a9b4ab"        # the hull's light tone, for OPEN
LIGHT = "#8e9b91"             # a bevel facing the light
DARK = "#1e2521"              # a bevel facing away
SHADOW = "#161b18"
FLOOR = "#3d463f"             # a recessed floor
METAL = "#58645b"             # a part bolted on
INK = "#2a312c"               # letters on light grounds
SMALL_FACE = "#353d37"        # the 16-pixel plate: 3.6 to 1 for the orange, 10 for the white


class Colours(dict):
    """a palette whose unknown roles are colours already"""
    def __missing__(self, key):
        return key


# ---------------------------------------------------------------- geometry

LIGHT_DIR = (-0.54, -0.84)  # towards the light: up and to the left


def poly(points) -> str:
    return "M" + "L".join(f"{num(x)} {num(y)}" for x, y in points) + "Z"


def chamfer(x, y, w, h, c):
    return [(x + c, y), (x + w - c, y), (x + w, y + c), (x + w, y + h - c), (x + w - c, y + h), (x + c, y + h),
            (x, y + h - c), (x, y + c)]


def _area(points):
    return sum(x1 * y2 - x2 * y1 for (x1, y1), (x2, y2) in zip(points, points[1:] + points[:1])) / 2


def inset(points, d):
    """a convex polygon moved in by d"""
    sign = 1 if _area(points) > 0 else -1
    n = len(points)
    lines = []
    for i in range(n):
        (x1, y1), (x2, y2) = points[i], points[(i + 1) % n]
        ex, ey = x2 - x1, y2 - y1
        length = math.hypot(ex, ey)
        nx, ny = -ey / length * sign, ex / length * sign
        lines.append(((x1 + nx * d, y1 + ny * d), (ex, ey)))
    out = []
    for i in range(n):
        (p, r), (q, s) = lines[i - 1], lines[i]
        cross = r[0] * s[1] - r[1] * s[0]
        t = ((q[0] - p[0]) * s[1] - (q[1] - p[1]) * s[0]) / cross
        out.append((p[0] + t * r[0], p[1] + t * r[1]))
    return out


def mix(a: str, b: str, t: float) -> str:
    ca = [int(a[i:i + 2], 16) for i in (1, 3, 5)]
    cb = [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return "#" + "".join(f"{round(x + (y - x) * t):02x}" for x, y in zip(ca, cb))


def bevel(points, d, raised=True, light=LIGHT, dark=DARK):
    """the sloped faces between a polygon and its inset, each shaded by how it
    faces the light: layers"""
    sign = 1 if _area(points) > 0 else -1
    inner = inset(points, d)
    out = []
    n = len(points)
    for i in range(n):
        (x1, y1), (x2, y2) = points[i], points[(i + 1) % n]
        ex, ey = x2 - x1, y2 - y1
        length = math.hypot(ex, ey)
        ox, oy = ey / length * sign, -ex / length * sign      # outward
        t = ox * LIGHT_DIR[0] + oy * LIGHT_DIR[1]
        if not raised:
            t = -t
        colour = mix(dark, light, (t + 1) / 2)
        out.append((colour, "skip", poly([points[i], points[(i + 1) % n], inner[(i + 1) % n], inner[i]])))
    return out


def raised(points, d, face, shadow=None, light=LIGHT, dark=DARK, kind="fill"):
    """a part that sits proud: its flat shadow, its bevels and its face"""
    layers = []
    if shadow:
        layers.append((SHADOW, "skip", f'<path fill="{{c}}" opacity=".75" transform="translate({num(shadow[0])} {num(shadow[1])})" d="{poly(points)}"/>'))
    layers.append((face, kind, poly(points)))
    layers += bevel(points, d, True, light, dark)
    return layers


def pocket(points, d, floor=FLOOR, outline=True):
    """a recess: its walls, shaded the other way round, and its floor"""
    layers = [(DARK, "skip", poly(points))]
    layers += bevel(points, d, False)
    layers.append((floor, "skip", poly(inset(points, d))))
    if outline:
        layers.append(("none", "cut", f'<path fill="none" stroke="{{c}}" stroke-width="1.2" d="{poly(points)}"/>'))
    return layers


def bolt(x, y, r=2.5):
    """a hex bolt head, raised, with its shadow"""
    hexagon = [(x + r * math.cos(math.radians(30 + 60 * k)), y + r * math.sin(math.radians(30 + 60 * k))) for k in range(6)]
    layers = [(SHADOW, "skip", f'<path fill="{{c}}" opacity=".7" transform="translate(.7 .9)" d="{poly(hexagon)}"/>'),
              ("#5d695f", "skip", poly(hexagon))]
    layers += bevel(hexagon, r * 0.34, True, "#a7b3a9", "#252c27")
    layers.append(("none", "cut", poly(hexagon)))
    return layers


def lettering(cx, open_cap, open_base, h1_cap, h1_base, open_colour=HULL_LIGHT):
    """OPEN above H1, the H white and the 1 orange: layers"""
    x0 = cx - stencil_width("H1", h1_cap) / 2
    return [
        (open_colour, "cut", r3.centred("OPEN", open_cap, cx, open_base)),
        (WHITE, "cut", stencil("H", x0, h1_cap, h1_base)[0]),
        (ORANGE, "cut", stencil("1", x0 + 0.74 * h1_cap, h1_cap, h1_base)[0]),
    ]


def rule(x0, x1, y, colour=HULL_LIGHT, h=1.0):
    return [(colour, "cut", f"M{num(x0)} {num(y)}H{num(x1)}V{num(y + h)}H{num(x0)}Z")]


def outline_cut(points, w=1.2):
    return [("none", "cut", f'<path fill="none" stroke="{{c}}" stroke-width="{w}" d="{poly(points)}"/>')]


# ---------------------------------------------------------------- the small versions

def small_h1(top=4, height=8, dx=0.0):
    b, mid = top + height, top + height / 2
    return (f'<path fill="{WHITE}" d="M{num(2 + dx)} {num(top)}h2v{num(height)}h-2ZM{num(6 + dx)} {num(top)}h2v{num(height)}h-2Z'
            f'M{num(4 + dx)} {num(mid - 1)}h2v2h-2Z"/>'
            f'<path fill="{ORANGE}" d="M{num(11 + dx)} {num(top)}h2v{num(height)}h-2ZM{num(9 + dx)} {num(top + 1)}h2v2h-2Z"/>')


def small(silhouette: str, extras: str = "", top=4, height=8, dx=0.0, face=SMALL_FACE) -> str:
    return (f'<path fill="{face}" d="{silhouette}"/>'
            f'<path fill="none" stroke="{LIGHT}" stroke-opacity=".55" stroke-width=".8" d="{silhouette}" '
            f'clip-path="url(#tl)"/>' + extras + small_h1(top, height, dx))


SMALL_DEFS = '<defs><clipPath id="tl"><path d="M0 0H16L0 16Z"/></clipPath></defs>'


# ---------------------------------------------------------------- the concepts

def icon_bg(seed=0):
    return ('<defs><linearGradient id="bay" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#2a312d"/>'
            '<stop offset="1" stop-color="#151a17"/></linearGradient></defs><rect width="1024" height="1024" fill="url(#bay)"/>'
            '<path fill="none" stroke="#0f1311" stroke-width="8" d="M0 128H1024M0 896H1024"/>')


def concept(cid, name, style, layers, small_svg, idea, defs="", open_white=False):
    dark = Colours(ink=WHITE, accent=ORANGE, open=WHITE if open_white else HULL_LIGHT)
    light = Colours(ink=INK, accent=ORANGE_ON_LIGHT, open=INK)
    return {
        "id": cid, "name": name, "style": style,
        "ground": ("#111513", "#ecedea"),
        "pal": {"dark": dark, "light": light},
        "defs": {"dark": defs, "light": defs},
        "layers": layers,
        "icon": lambda c: icon_bg() + placed(r2.mark_body(c, "dark"), 8.8, 72, 72),
        "wordmark": words, "cap": 20,
        "favicon": SMALL_DEFS + small_svg,
        "idea": idea,
        "legal": "An original panel drawn here, with our own stencil letters. No emblem, insignia, unit name or "
                 "lettering from the series.",
        "xbox": "Not UNSC: no eagle, globe, wings or star, and none of its type. Not Xbox: grey-green, white and "
                "orange, no X, orb or green.",
    }


def words(colours, is_mono, x, cap, baseline):
    d1, w1 = stencil("OPEN", x, cap, baseline)
    xh = x + w1 + cap * 0.1
    d2, w2 = stencil("H", xh, cap, baseline)
    d3, w3 = stencil("1", xh + w2 + cap * 0.1, cap, baseline)
    width = w1 + cap * 0.1 + w2 + cap * 0.1 + w3
    return (f'<path fill="{colours["open"]}" d="{d1}"/><path fill="{colours["ink"]}" d="{d2}"/>'
            f'<path fill="{colours["accent"]}" d="{d3}"/>'), width


CONCEPTS = []

# 1. Access Hatch: a raised frame, a recessed door with hinges and a handle recess
frame = chamfer(4, 4, 92, 92, 12)
door = chamfer(17, 15, 66, 70, 6)
hatch = raised(frame, 3, HULL)
hatch += pocket(door, 2.4, HULL, outline=True)
for y in (26, 64):
    hatch += raised(chamfer(12, y, 9, 11, 1.5), 1.4, METAL, shadow=(1, 1.4))
    hatch += [("none", "cut", f'<path fill="none" stroke="{{c}}" stroke-width="1" d="{poly(chamfer(12, y, 9, 11, 1.5))}"/>')]
hatch += pocket(chamfer(38, 74, 24, 7, 2), 1.8, "#2c342f")
for x, y in ((30, 9.5), (70, 9.5), (30, 90.5), (70, 90.5), (89.5, 50)):
    hatch += bolt(x, y)
hatch += lettering(50, 8, 29, 30, 66)
hatch += rule(38, 62, 32.5)
CONCEPTS.append(concept(
    "hatch", "Access Hatch", "A door in a raised frame",
    hatch,
    small("M2 0H14L16 2V14L14 16H2L0 14V2Z", f'<path fill="{DARK}" d="M6 13h4v1h-4Z"/>', 3, 8),
    "An access hatch: a raised frame with five bolts, the door set into it on two hinges, a recessed handle at the "
    "bottom, and OPEN over H1 painted on the door, divided by a thin rule. The literal reading: a door marked open."))

# 2. Side Armour: an angular plate, a raised border, a recessed field, offset bolt rows
armour_outer = [(4, 10), (76, 10), (96, 30), (96, 90), (16, 90), (4, 78)]
armour = raised(armour_outer, 2.6, HULL)
armour += pocket(inset(armour_outer, 10), 2.6, "#414b43")
for x in (16, 32, 48, 64):
    armour += bolt(x, 15)
for x in (26, 42, 58, 74, 88):
    armour += bolt(x, 85)
armour += lettering(49, 8, 34, 32, 72, WHITE)
CONCEPTS.append(concept(
    "armour", "Side Armour", "Vehicle armour plate",
    armour,
    small("M0 1H12L16 5V15H3L0 12Z", "", 4, 8),
    "A vehicle's side armour: an asymmetric plate with its corners cut, a thick raised border carrying two offset "
    "rows of bolts, and the field recessed inside it, with OPEN in white over H1.", open_white=True))

# 3. Crate End: an equipment crate's end panel, corner guards, caution stripes
CRATE_DEFS = '<clipPath id="cband"><rect x="27" y="72" width="46" height="11"/></clipPath>'
crate = raised(chamfer(5, 5, 90, 90, 3), 2.2, HULL)
corners = [[(5, 5), (33, 5), (5, 33)], [(95, 5), (95, 33), (67, 5)], [(5, 95), (5, 67), (33, 95)], [(95, 95), (67, 95), (95, 67)]]
for tri, (bx, by) in zip(corners, ((13, 13), (87, 13), (13, 87), (87, 87))):
    crate += raised(tri, 2.2, METAL, shadow=(1.4, 1.9))
    crate += outline_cut(tri, 1)
    crate += bolt(bx, by, 2.3)
crate += [("#151716", "cut", "M27 72H73V83H27Z"),
          (ORANGE, "skip", f'<path fill="{{c}}" clip-path="url(#cband)" d="{r3.hazard_stripes(27, 72, 73, 83, 4.5, 4.5)}"/>')]
crate += lettering(50, 8, 28, 30, 64)
CONCEPTS.append(concept(
    "crate", "Crate End", "Equipment crate, corner guards",
    crate,
    small("M0 0H16V16H0Z", f'<path fill="{METAL}" d="M0 0h4l-4 4ZM16 0v4l-4-4ZM0 16v-4l4 4ZM16 16h-4l4-4Z"/>'
          f'<path fill="#151716" d="M4 12h8v2h-8Z"/><path fill="{ORANGE}" d="M5 14l1-2h1l-1 2ZM8 14l1-2h1l-1 2ZM11 14l1-2h1l-1 2Z"/>', 2, 8),
    "The end panel of an equipment crate: bolted triangular corner guards that stand proud of the panel, a band of "
    "black and orange caution stripes, and OPEN over H1 stencilled between them, as if on the lid of the kit.",
    CRATE_DEFS))

# 4. Faceted Plate: an angular plate with deep chamfered facets
facet_outer = [(16, 4), (84, 4), (96, 24), (96, 76), (84, 96), (16, 96), (4, 76), (4, 24)]
faceted = [(HULL, "fill", poly(facet_outer))]
faceted += bevel(facet_outer, 11, True, "#7f8c82", "#232a25")
faceted += [(HULL, "skip", poly(inset(facet_outer, 11)))]
faceted += outline_cut(inset(facet_outer, 11), 1.2)
faceted += [("#5f6b62", "skip", f'<path fill="none" stroke="{{c}}" stroke-width=".6" d="{poly(inset(facet_outer, 11))}"/>')]
for x, y in ((9.5, 50), (90.5, 50)):
    faceted += bolt(x, y, 2.4)
faceted += lettering(50, 8, 32, 30, 70)
faceted += rule(37, 63, 35.5)
CONCEPTS.append(concept(
    "faceted", "Faceted Plate", "Angular sci-fi plate",
    faceted,
    small("M3 0H13L16 3V13L13 16H3L0 13V3Z", f'<path fill="none" stroke="{DARK}" stroke-width="1" d="M3.6 1.5H12.4L14.5 3.6V12.4L12.4 14.5H3.6L1.5 12.4V3.6Z"/>', 4, 8),
    "An angular plate pressed into a flat top face with deep chamfered facets all round, lit from the top left so "
    "the facets read as solid form; a bolt on each side facet, and OPEN over H1 on the top face."))

# 5. Stacked Plates: a badge plate bolted onto a bigger base plate
base = chamfer(4, 4, 92, 92, 10)
badge = chamfer(14, 33, 72, 55, 6)
stack = raised(base, 2.6, HULL)
stack += raised(badge, 2.2, METAL, shadow=(2.2, 3))
stack += outline_cut(badge, 1.4)
for x, y in ((20.5, 39.5), (79.5, 39.5), (20.5, 81.5), (79.5, 81.5)):
    stack += bolt(x, y, 2.1)
stack += [(HULL_LIGHT, "cut", r3.centred("OPEN", 9, 50, 25.5))]
x0 = 50 - stencil_width("H1", 28) / 2
stack += [(WHITE, "cut", stencil("H", x0, 28, 74)[0]), (ORANGE, "cut", stencil("1", x0 + 0.74 * 28, 28, 74)[0])]
CONCEPTS.append(concept(
    "stack", "Stacked Plates", "A badge plate bolted onto a base plate",
    stack,
    small("M2 0H14L16 2V14L14 16H2L0 14V2Z", f'<path fill="{METAL}" d="M1 4H15V15H1Z"/>', 6, 8),
    "Two layers: OPEN stencilled on a base plate, and below it a smaller badge plate bolted on with four bolts, "
    "standing proud and casting a hard shadow, with H1 on it. The badge's edge is the rule between the words."))

# 6. Vent Panel: a plate with a recessed grille of louvres
vent = raised(chamfer(6, 4, 88, 92, 9), 2.6, HULL)
for x, y in ((15, 13), (85, 13), (15, 87), (85, 87)):
    vent += bolt(x, y, 2.4)
vent += pocket([(22, 67), (78, 67), (78, 91), (22, 91)], 1.6, "#3a433c")
for k in range(4):
    y = 69.5 + k * 5.2
    vent += pocket(chamfer(25, y, 50, 3.2, 1), 1.0, "#0f1311", outline=False)
    vent += [("none", "cut", f"M25 {num(y)}H75V{num(y + 3.2)}H25Z")]
vent += lettering(50, 8, 25, 30, 60)
CONCEPTS.append(concept(
    "vent", "Vent Panel", "Plate with a recessed grille",
    vent,
    small("M2 0H14L16 2V14L14 16H2L0 14V2Z", f'<path fill="#0f1311" d="M4 12h8v1h-8ZM4 14h8v1h-8Z"/>', 2, 8),
    "A service panel with four corner bolts and a recessed grille of four louvres cut into its lower third; OPEN "
    "over H1 above it. The vents give it a machine's purpose and a clear bottom to the composition."))


# ---------------------------------------------------------------- the board

PICKS = [
    ("stack", "Stacked Plates",
     "The clearest structure of the six: OPEN on the base, H1 on a plate bolted over it, so the two words get their own "
     "layers and the badge's edge does the work of the rule. It has the strongest depth story for the 3D model (a base, "
     "a plate standing proud, a hard shadow, four bolts), and it reduces cleanly at 16 pixels to H1 on a lighter plate."),
    ("hatch", "Access Hatch",
     "The best idea: a door, marked open. It reads as a real piece of hull straight away, has the richest set of "
     "raised and recessed parts to model (frame, door, hinges, handle recess, bolts), and the thin rule between OPEN "
     "and H1 keeps the lettering calm. At 16 pixels it is an H1 on a plate with a handle slot."),
]


def board(outputs):
    sections = "".join(r2.section(i + 1, c, o).replace("Why it's not the Xbox logo.", "Why it's not UNSC or Xbox.")
                       for i, (c, o) in enumerate(outputs))
    compare = "".join(
        f'<figure class="panel" style="background:#111513"><div class="cmp">{img(uri(o["mark.svg"]), "", c["name"], "150")}'
        f'<span>{img(uri(o["favicon.svg"]), "", "", "32")}{img(uri(o["favicon.svg"]), "", "", "16")}</span></div>'
        f'<figcaption>{html.escape(c["name"])}</figcaption></figure>' for c, o in outputs)
    cards = []
    by_id = {c["id"]: o for c, o in outputs}
    for rank, (cid, name, why) in enumerate(PICKS, 1):
        o = by_id[cid]
        cards.append(f"""
    <div class="pick-card">
      <span class="rank">No. {rank}</span>
      <h3>{html.escape(name)}</h3>
      <div class="show">{img(uri(o['icon.svg']), 'mac', '', '56')}{img(uri(o['lockup.svg']), 'lockup', 'OpenH1')}</div>
      <p>{html.escape(why)}</p>
    </div>""")
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>OpenH1 logo board, round 5</title>
<style>{r2.STYLE}
.strip-grid {{ grid-template-columns: repeat(3, 1fr); }}
.compare {{ grid-template-columns: repeat(3, 1fr); margin-top: 20px; }}
.cmp {{ display: flex; align-items: center; gap: 18px; }}
.cmp span {{ display: flex; flex-direction: column; align-items: center; gap: 10px; }}
.key {{ display: flex; gap: 18px; flex-wrap: wrap; color: var(--muted); font-size: 14px; margin-top: 10px; }}
.key i {{ display: inline-block; width: 14px; height: 14px; border-radius: 3px; vertical-align: -2px; margin-right: 6px; }}</style>
</head>
<body>
<div class="wrap">
  <h1>OpenH1 logo board, round 5</h1>
  <p class="lede">Six panels for the Hull Marking direction, each a standalone icon seen straight on, with OPEN
  stencilled above H1 in fresh paint: the H white, the 1 military orange, on a dark grey-green hull.</p>
  <p class="note">Drawn flat-shaded like a technical illustration so they can be modelled: one light from the top left;
  raised parts have light upper-left bevels, dark lower-right bevels and a hard shadow; recesses are the other way
  round, with a dark top wall and a lit bottom wall. Small versions use a slightly darker plate (#353d37) so the orange 1
  keeps contrast at 16 pixels. Files are in website/logo/round5/, made by website/logo/round5/build.py.</p>
  <div class="key"><span><i style="background:{LIGHT}"></i>bevel facing the light</span><span><i style="background:{DARK}"></i>bevel facing away</span>
  <span><i style="background:{FLOOR}"></i>recessed floor</span><span><i style="background:{METAL}"></i>part bolted on</span><span><i style="background:{SHADOW}"></i>hard shadow</span></div>
  <div class="row compare">{compare}</div>
  {sections}
  <section class="strip">
    <h2>At small size</h2>
    <p class="note">Each panel's small version at its true 16 pixels in a light and a dark browser tab, on white and on dark,
    the one-colour mark at 16 pixels, and the small version enlarged eight times. OPEN drops out below 32 pixels; the
    silhouette and H1 carry the icon.</p>
    <div class="strip-grid">{r2.strip(outputs)}</div>
  </section>
  <section class="pick">
    <h2>Top two</h2>
    <p>Close behind: Faceted Plate has the boldest silhouette and the simplest model; Side Armour is the most
    vehicle-like. Crate End is the only one carrying the caution stripes, which suit it but make it the busiest small.</p>
    <div class="picks" style="grid-template-columns:repeat(2,1fr)">{''.join(cards)}</div>
  </section>
</div>
<script>{r2.round1.SCRIPT}</script>
</body>
</html>
"""


def main() -> None:
    outputs = []
    for c in CONCEPTS:
        out = r2.files(c)
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
