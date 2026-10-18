#!/usr/bin/env python3
"""Draws OpenH1's logo concepts and the board that compares them:

    python website/logo/build.py

Each concept's mark is drawn on a small square grid of units, the grid of
its 16-pixel favicon: the mark sits in the favicon's tile at one unit to the
pixel, so its straight edges land on whole pixels there and at every larger
size that is a multiple of 16. For each concept it writes, into
website/logo/<concept>/:

- mark.svg, mark-light.svg, mark-mono.svg: the mark alone, for dark
  backgrounds, light ones, and one colour (currentColor);
- lockup.svg, lockup-light.svg, lockup-mono.svg: the mark with the wordmark,
  "OpenH1" in Overpass (website/public/fonts) as outlines, so the files need
  no font;
- icon.svg: the app icon, the mark on the navy tile, a full-bleed square of
  1024 that the platforms mask to their own shapes (tools/app_icon.py);
- favicon.svg: the mark on a rounded tile, on the 16-pixel grid.

and website/logo-board.html, which shows them all. Needs fontTools and
uharfbuzz.
"""

import base64
import html
from pathlib import Path

import uharfbuzz as hb
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

HERE = Path(__file__).resolve().parent
WEBSITE = HERE.parent
FONTS = WEBSITE / "public/fonts"
BOARD = WEBSITE / "logo-board.html"

# the site's colours: white and the menus' blue on navy, amber (a visor's
# gold) for the one accent; darker blue and amber where the ground is light
DARK = {"p": "#ffffff", "b": "#2299ff", "a": "#ffb321", "soft": "#c9daf0"}
LIGHT = {"p": "#0b1a2e", "b": "#1170d0", "a": "#d98500", "soft": "#3d5a7e"}
MONO = {"p": "currentColor", "b": "currentColor", "a": "currentColor", "soft": "currentColor"}
TILE_TOP, TILE_BOTTOM = "#003366", "#000e23"


def num(value: float) -> str:
    text = f"{value:.2f}".rstrip("0").rstrip(".")
    return "0" if text == "-0" else text


# ---------------------------------------------------------------- the marks
# each: its grid's size n, and its shapes, (colour role, path in units)

CONCEPTS = [
    {
        "id": "ligature",
        "name": "Ligature",
        "n": 12,
        "shapes": [
            ("p", "M1 2H4V12H1Z"),
            # the right stem is the 1: taller, with its flag reaching into the H
            ("p", "M8 0H11V12H8V3L6 5V2Z"),
            ("a", "M4 6H8V8H4Z"),
        ],
        "idea": "An H and a 1 that share a stem: the 1 grows out of the H's right side, and the amber "
                "crossbar is the bridge the port builds from the original game to new machines. "
                "It can stand in for the “H1” in the wordmark.",
        "legal": "Pure letterforms of the project's own name, built from rectangles. No character, helmet, "
                 "ring or lettering from the game or its makers.",
    },
    {
        "id": "source",
        "name": "Source",
        "n": 14,
        "shapes": [
            ("b", "M0 1H4V3H2V11H4V13H0Z"),
            ("b", "M14 1H10V3H12V11H10V13H14Z"),
            ("p", "M7 3H9V11H7V5.5L5 7.5V5Z"),
        ],
        "idea": "The 1 between code brackets: a game rebuilt from its source, in the open, by its community.",
        "legal": "Square brackets are a universal programming symbol and the 1 is a plain numeral. "
                 "Nothing is taken from the game's art, logos or fonts.",
    },
    {
        "id": "readout",
        "name": "Readout",
        "n": 12,
        "shapes": [
            # the stem, cut by a slit, and the flag, cut off at the same angle
            ("a", "M5 0V3L2 5V2Z"),
            ("p", "M6 0H9V4.5L6 6.5Z"),
            ("p", "M6 7.5L9 5.5V12H6Z"),
        ],
        "idea": "A 1 cut into segments at one angle, like a digit on an instrument's readout: precise "
                "and technical, with the amber flag as the one lit segment.",
        "legal": "Segment displays are a generic convention of clocks and meters, not the game's HUD. "
                 "No glyph or interface element is copied.",
    },
    {
        "id": "lock-on",
        "name": "Lock-on",
        "n": 12,
        "shapes": [
            ("p", "M0 0H4V2H2V4H0Z"),
            ("p", "M12 0H8V2H10V4H12Z"),
            ("p", "M0 12H4V10H2V8H0Z"),
            ("p", "M12 12H8V10H10V8H12Z"),
            ("a", "M6 3.25L8.75 6L6 8.75L3.25 6Z"),
        ],
        "idea": "Four corner marks around an amber diamond: a focus frame that has locked on, "
                "open at the corners like the project.",
        "legal": "Corner marks and a diamond are generic geometry, used by cameras and editors alike. "
                 "There is no reticle from the game and no ring.",
    },
    {
        "id": "gateway",
        "name": "Gateway",
        "n": 14,
        "shapes": [
            ("b", "M1 14V6.5A6 6 0 0 1 13 6.5V14H11V6.5A4 4 0 0 0 3 6.5V14Z"),
            ("a", "M7 5H9V14H7V7.5L5 9.5V7Z"),
        ],
        "idea": "A doorway, open at the bottom, with the 1 stepping through it: one game, "
                "out to every platform.",
        "legal": "An arch is a common symbol for a portal or a door. It is not a ring, and nothing in it "
                 "comes from the series.",
    },
]


def shapes_svg(concept: dict, colours: dict, scale: float = 1.0, dx: float = 0.0, dy: float = 0.0) -> str:
    """the mark's shapes as <path>s, scaled and moved"""
    paths = "".join(f'<path fill="{colours[role]}" d="{d}"/>' for role, d in concept["shapes"])
    moves = []
    if dx or dy:
        moves.append(f"translate({num(dx)} {num(dy)})")
    if scale != 1.0:
        moves.append(f"scale({num(scale)})")
    return f'<g transform="{" ".join(moves)}">{paths}</g>' if moves else paths


def svg(width: float, height: float, body: str, title: str) -> str:
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {num(width)} {num(height)}">'
            f"<title>{html.escape(title)}</title>{body}</svg>\n")


def tile_gradient(gid: str = "tile") -> str:
    return (f'<defs><linearGradient id="{gid}" x1="0" y1="0" x2="0" y2="1">'
            f'<stop offset="0" stop-color="{TILE_TOP}"/><stop offset="1" stop-color="{TILE_BOTTOM}"/>'
            "</linearGradient></defs>")


# ---------------------------------------------------------------- the wordmark

class Wordmark:
    """"Open" in Overpass 750 and "H1" in Overpass 900, as outlines"""

    def __init__(self):
        self.fonts = {}

    def font(self, weight: int):
        if weight not in self.fonts:
            path = FONTS / f"Overpass-{weight}.ttf"
            face = hb.Face(hb.Blob.from_file_path(str(path)))
            tt = TTFont(path)
            self.fonts[weight] = (hb.Font(face), tt, tt.getGlyphSet(), tt.getGlyphOrder(),
                                  tt["head"].unitsPerEm, tt["OS/2"].sCapHeight)
        return self.fonts[weight]

    def run(self, text: str, weight: int, cap: float, x: float, baseline: float) -> tuple:
        """text with its caps cap high, starting at x: (path, advance)"""
        font, _, glyphs, order, upem, cap_height = self.font(weight)
        buf = hb.Buffer()
        buf.add_str(text)
        buf.guess_segment_properties()
        hb.shape(font, buf, {"kern": True, "liga": True})
        scale = cap / cap_height
        pen = SVGPathPen(glyphs, ntos=num)
        cursor = 0
        for info, pos in zip(buf.glyph_infos, buf.glyph_positions):
            glyphs[order[info.codepoint]].draw(TransformPen(pen, (
                scale, 0, 0, -scale, x + (cursor + pos.x_offset) * scale, baseline - pos.y_offset * scale)))
            cursor += pos.x_advance
        return pen.getCommands(), cursor * scale


WORDMARK = Wordmark()


def lockup(concept: dict, colours: dict) -> str:
    """the mark, and "OpenH1" with its caps centred on it"""
    height = 48.0
    cap = 26.0
    gap = 15.0
    baseline = height / 2 + cap / 2
    x = height + gap
    open_path, open_width = WORDMARK.run("Open", 750, cap, x, baseline)
    h1_path, h1_width = WORDMARK.run("H1", 900, cap, x + open_width + cap * 0.04, baseline)
    width = x + open_width + cap * 0.04 + h1_width
    body = (shapes_svg(concept, colours, height / concept["n"]) +
            f'<path fill="{colours["soft"]}" d="{open_path}"/><path fill="{colours["p"]}" d="{h1_path}"/>')
    return svg(width, height, body, "OpenH1")


def app_icon(concept: dict) -> str:
    size = 1024
    mark = 560.0
    scale = mark / concept["n"]
    body = (tile_gradient() + f'<rect width="{size}" height="{size}" fill="url(#tile)"/>' +
            shapes_svg(concept, DARK, scale, (size - mark) / 2, (size - mark) / 2))
    return svg(size, size, body, "OpenH1")


def favicon(concept: dict) -> str:
    pad = (16 - concept["n"]) / 2
    body = (tile_gradient() + '<rect width="16" height="16" rx="3.5" fill="url(#tile)"/>' +
            shapes_svg(concept, DARK, 1.0, pad, pad))
    return svg(16, 16, body, "OpenH1")


def files(concept: dict) -> dict:
    n = concept["n"]
    return {
        "mark.svg": svg(n, n, shapes_svg(concept, DARK), "OpenH1"),
        "mark-light.svg": svg(n, n, shapes_svg(concept, LIGHT), "OpenH1"),
        "mark-mono.svg": svg(n, n, shapes_svg(concept, MONO), "OpenH1"),
        "lockup.svg": lockup(concept, DARK),
        "lockup-light.svg": lockup(concept, LIGHT),
        "lockup-mono.svg": lockup(concept, MONO),
        "icon.svg": app_icon(concept),
        "favicon.svg": favicon(concept),
    }


# ---------------------------------------------------------------- the board

def uri(text: str, colour: str = "") -> str:
    if colour:
        text = text.replace("currentColor", colour)
    return "data:image/svg+xml;base64," + base64.b64encode(text.encode()).decode()


def img(src: str, cls: str = "", alt: str = "", size: str = "") -> str:
    attrs = f' class="{cls}"' if cls else ""
    if size:
        attrs += f' width="{size}" height="{size}"'
    return f'<img src="{src}" alt="{html.escape(alt)}"{attrs}>'


def tab(favicon_uri: str, theme: str, label: str = "OpenH1") -> str:
    return (f'<div class="tab {theme}"><img src="{favicon_uri}" width="16" height="16" alt="">'
            f"<span>{html.escape(label)}</span></div>")


def pixels(src: str, size: int, zoom: int) -> str:
    """the image drawn at size pixels, then enlarged without smoothing"""
    return f'<canvas class="px" data-src="{src}" data-size="{size}" width="{size * zoom}" height="{size * zoom}"></canvas>'


def concept_section(index: int, concept: dict, out: dict) -> str:
    name = concept["name"]
    fav = uri(out["favicon.svg"])
    icon = uri(out["icon.svg"])
    mark_dark = uri(out["mark.svg"])
    mark_light = uri(out["mark-light.svg"])
    return f"""
<section class="concept" id="{concept['id']}">
  <header>
    <span class="num">0{index}</span>
    <div>
      <h2>{name}</h2>
      <p class="idea">{html.escape(concept['idea'])}</p>
      <p class="legal"><b>Why it's safe.</b> {html.escape(concept['legal'])}</p>
    </div>
  </header>

  <div class="row marks">
    <figure class="panel dark">{img(mark_dark, 'mark', name)}<figcaption>Mark, on dark</figcaption></figure>
    <figure class="panel light">{img(mark_light, 'mark', name)}<figcaption>Mark, on light</figcaption></figure>
    <figure class="panel white">{img(uri(out['mark-mono.svg'], '#000000'), 'mark', name)}<figcaption>One colour, black</figcaption></figure>
    <figure class="panel navy">{img(uri(out['mark-mono.svg'], '#ffffff'), 'mark', name)}<figcaption>One colour, white</figcaption></figure>
  </div>

  <div class="row lockups">
    <figure class="panel dark">{img(uri(out['lockup.svg']), 'lockup', 'OpenH1')}<figcaption>Lockup, on dark</figcaption></figure>
    <figure class="panel light">{img(uri(out['lockup-light.svg']), 'lockup', 'OpenH1')}<figcaption>Lockup, on light</figcaption></figure>
    <figure class="panel white">{img(uri(out['lockup-mono.svg'], '#000000'), 'lockup', 'OpenH1')}<figcaption>Lockup, one colour</figcaption></figure>
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
      <figcaption>Favicon at its true 16 and 32 pixels</figcaption>
    </figure>
  </div>
</section>"""


def small_strip(outputs: list) -> str:
    cells = []
    for concept, out in outputs:
        fav = uri(out["favicon.svg"])
        cells.append(f"""
    <div class="strip-cell">
      <h3>{concept['name']}</h3>
      {tab(fav, 'light', concept['name'])}
      {tab(fav, 'dark', concept['name'])}
      <div class="bare">
        <span class="on-light">{img(uri(out['mark-light.svg']), '', '', '16')}</span>
        <span class="on-dark">{img(uri(out['mark.svg']), '', '', '16')}</span>
        <span class="on-light">{img(uri(out['mark-mono.svg'], '#000000'), '', '', '16')}</span>
      </div>
      {pixels(fav, 16, 8)}
    </div>""")
    return "".join(cells)


STYLE = """
@font-face { font-family: "Overpass"; font-weight: 750; src: url("public/fonts/Overpass-750.ttf") format("truetype"); }
@font-face { font-family: "Overpass"; font-weight: 900; src: url("public/fonts/Overpass-900.ttf") format("truetype"); }
:root {
  color-scheme: dark;
  --ground: #02060c; --navy: #002244; --navy-deep: #000e23; --panel: #061428;
  --edge: #2299ff; --edge-dim: #1d4f86; --amber: #ffb321;
  --text: #ffffff; --soft: #c9daf0; --muted: #8ea6c4;
  --light: #f3f6fa; --light-ink: #0b1a2e;
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--ground); color: var(--text); font: 750 16px/1.55 "Overpass", system-ui, sans-serif; -webkit-font-smoothing: antialiased; }
.wrap { max-width: 1200px; margin: 0 auto; padding: 40px 16px 80px; }
h1 { font-weight: 900; font-size: 40px; line-height: 1.1; margin: 0 0 8px; }
.lede { color: var(--soft); max-width: 760px; margin: 0 0 8px; }
.note { color: var(--muted); font-size: 14px; max-width: 760px; margin: 0; }
.concept { margin-top: 56px; padding-top: 32px; border-top: 1px solid var(--edge-dim); }
.concept header { display: flex; gap: 20px; align-items: flex-start; margin-bottom: 20px; }
.num { font-weight: 900; font-size: 15px; color: var(--edge); letter-spacing: 0.08em; padding-top: 10px; }
h2 { font-weight: 900; font-size: 30px; margin: 0 0 4px; }
.idea { margin: 0 0 6px; color: var(--soft); max-width: 820px; }
.legal { margin: 0; color: var(--muted); font-size: 14px; max-width: 820px; }
.legal b { color: var(--amber); font-weight: 900; }
.row { display: grid; gap: 12px; margin-bottom: 12px; }
.marks { grid-template-columns: repeat(4, 1fr); }
.lockups { grid-template-columns: repeat(3, 1fr); }
.icons { grid-template-columns: 2fr 1fr; }
.panel { margin: 0; border-radius: 12px; padding: 24px 20px 14px; display: flex; flex-direction: column; align-items: center; justify-content: space-between; gap: 14px; min-height: 180px; border: 1px solid var(--edge-dim); }
.panel figcaption { font-size: 12px; letter-spacing: 0.06em; text-transform: uppercase; color: var(--muted); text-align: center; }
.dark { background: var(--ground); }
.navy { background: var(--navy); }
.light { background: var(--light); border-color: #d5deea; }
.white { background: #ffffff; border-color: #d5deea; }
.light figcaption, .white figcaption { color: #5a6f8c; }
.desk { background: linear-gradient(160deg, #24364f, #0d1828); }
.fav { background: var(--panel); }
img.mark { width: 112px; height: 112px; margin-top: 12px; }
img.lockup { height: 48px; max-width: 100%; margin-top: 30px; }
.icon-set { display: flex; gap: 22px; align-items: center; flex-wrap: wrap; justify-content: center; }
.icon-set.small { gap: 16px; }
img.mac { border-radius: 22.5%; box-shadow: 0 6px 14px rgba(0,0,0,0.45); }
img.circle { border-radius: 50%; }
img.squircle { border-radius: 32%; }
img.square { border-radius: 0; }
.tabs { display: flex; flex-direction: column; gap: 8px; width: 100%; }
.tab { display: flex; align-items: center; gap: 8px; height: 32px; padding: 0 12px; border-radius: 8px 8px 0 0; font: 400 13px/1 system-ui, sans-serif; }
.tab.light { background: #ffffff; color: #1f1f1f; border: 1px solid #d5deea; }
.tab.dark { background: #35363a; color: #e8eaed; }
.fav-sizes { display: flex; gap: 18px; align-items: flex-end; }
.fav-sizes span { display: flex; flex-direction: column; align-items: center; gap: 6px; }
.fav-sizes i { font-style: normal; font-size: 11px; color: var(--muted); }
canvas.px { image-rendering: pixelated; border-radius: 4px; }
.strip { margin-top: 64px; padding-top: 32px; border-top: 1px solid var(--edge-dim); }
.strip-grid { display: grid; grid-template-columns: repeat(5, 1fr); gap: 12px; margin-top: 16px; }
.strip-cell { background: var(--panel); border: 1px solid var(--edge-dim); border-radius: 12px; padding: 14px; display: flex; flex-direction: column; gap: 8px; align-items: stretch; }
.strip-cell h3 { margin: 0; font-size: 15px; font-weight: 900; }
.strip-cell canvas { align-self: center; margin-top: 6px; }
.bare { display: flex; gap: 0; }
.bare span { flex: 1; display: flex; justify-content: center; padding: 8px 0; }
.on-light { background: #ffffff; } .on-dark { background: var(--ground); }
.bare span:first-child { border-radius: 6px 0 0 6px; } .bare span:last-child { border-radius: 0 6px 6px 0; }
.pick { margin-top: 64px; padding: 28px; border: 1px solid var(--edge); border-radius: 14px; background: linear-gradient(160deg, #04203f, var(--navy-deep)); }
.pick h2 { margin-bottom: 10px; }
.pick p { color: var(--soft); max-width: 860px; margin: 0 0 10px; }
.pick .hero { display: flex; gap: 28px; align-items: center; flex-wrap: wrap; margin: 18px 0 20px; }
.pick .hero img.lockup { margin: 0; height: 64px; }
.pick .hero .lightbox { background: var(--light); padding: 18px 22px; border-radius: 10px; }
.pick ol { color: var(--soft); max-width: 860px; padding-left: 20px; margin: 0; }
.pick li { margin-bottom: 6px; }
.pick b { color: var(--text); font-weight: 900; }
@media (max-width: 900px) {
  .marks { grid-template-columns: repeat(2, 1fr); }
  .lockups, .icons { grid-template-columns: 1fr; }
  .strip-grid { grid-template-columns: repeat(2, 1fr); }
}
"""

SCRIPT = """
document.querySelectorAll("canvas.px").forEach(function (big) {
  var image = new Image();
  image.onload = function () {
    var size = +big.dataset.size, small = document.createElement("canvas");
    small.width = small.height = size;
    small.getContext("2d").drawImage(image, 0, 0, size, size);
    var context = big.getContext("2d");
    context.imageSmoothingEnabled = false;
    context.drawImage(small, 0, 0, big.width, big.height);
  };
  image.src = big.dataset.src;
});
"""

RECOMMENDATION = """
<section class="pick">
  <h2>Recommendation: Ligature</h2>
  <div class="hero">
    {lockup_dark}
    <span class="lightbox">{lockup_light}</span>
    {icon}
  </div>
  <p><b>Ligature is the one to take forward.</b> It is the only concept that spells the project's name
  rather than illustrating an idea about it, which makes it the most ownable and the hardest to mistake for
  anything else. It is three straight-edged shapes on the 16-pixel grid, so its stems and crossbar land on
  whole pixels in a browser tab, and it reads the same in one colour, on light and on dark. The amber crossbar carries the
  site's accent without needing a visor or a helmet to get there, and the mark can replace the “H1”
  in the wordmark for a tighter lockup later.</p>
  <p><b>Runner-up: Source.</b> The clearest “open source” story and very legible small, but brackets
  around a character are a common pattern for developer tools, so it is less distinctive. Readout and
  Gateway are good at large sizes but lose detail at 16 pixels; Lock-on is safe but says little about the
  project.</p>
</section>
"""


def board(outputs: list) -> str:
    sections = "".join(concept_section(i + 1, c, o) for i, (c, o) in enumerate(outputs))
    chosen = dict((c["id"], o) for c, o in outputs)["ligature"]
    pick = RECOMMENDATION.format(
        lockup_dark=img(uri(chosen["lockup.svg"]), "lockup", "OpenH1"),
        lockup_light=img(uri(chosen["lockup-light.svg"]), "lockup", "OpenH1"),
        icon=img(uri(chosen["icon.svg"]), "mac", "App icon", "96"))
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>OpenH1 logo board</title>
<style>{STYLE}</style>
</head>
<body>
<div class="wrap">
  <h1>OpenH1 logo board</h1>
  <p class="lede">Five original marks for OpenH1, each shown as a mark, a wordmark lockup, an app icon in the
  platforms' masks, and a favicon at its true size. Every mark is drawn on the grid of its 16-pixel favicon,
  so its straight edges land on whole pixels.</p>
  <p class="note">The wordmark is Overpass, the site's typeface, drawn as outlines. The colours are the site's:
  navy and white, the menus' blue, and amber as the one accent. Files are in website/logo/, one folder per concept,
  made by website/logo/build.py.</p>
  {sections}
  <section class="strip">
    <h2>At small size</h2>
    <p class="note">Each concept's favicon at its true 16 pixels in a light and a dark browser tab; the bare mark at
    16 pixels on white, on dark and in one colour; and the favicon enlarged eight times to show its pixels.</p>
    <div class="strip-grid">{small_strip(outputs)}</div>
  </section>
  {pick}
</div>
<script>{SCRIPT}</script>
</body>
</html>
"""


def main() -> None:
    outputs = []
    for concept in CONCEPTS:
        out = files(concept)
        folder = HERE / concept["id"]
        folder.mkdir(exist_ok=True)
        for name, text in out.items():
            (folder / name).write_text(text, encoding="utf-8")
        outputs.append((concept, out))
        print(f"{folder.relative_to(WEBSITE.parent)}: {', '.join(out)}")
    BOARD.write_text(board(outputs), encoding="utf-8")
    print(BOARD.relative_to(WEBSITE.parent))


if __name__ == "__main__":
    main()
