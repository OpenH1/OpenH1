#!/usr/bin/env python3
"""Draws round 4: Hull Marking (round 3) with its 1 in signal orange, in
three variations, and the board that compares them:

    python website/logo/round4/build.py

A keeps the orange hazard tab; B removes it, so the 1 is the only orange;
C removes it and lifts the orange to a brighter tone, for contrast against
the olive plate at 16 pixels. In one colour the 1 is the H's colour, told
apart by its stencil bridge. For each variation it writes, into
website/logo/round4/<variation>/, the same files as rounds 2 and 3, and
website/logo-board-4.html. Needs round 3's website/logo/round3/build.py and
its dependencies.
"""

import html
import importlib.util
from pathlib import Path

HERE = Path(__file__).resolve().parent
WEBSITE = HERE.parent.parent
BOARD = WEBSITE / "logo-board-4.html"

_spec = importlib.util.spec_from_file_location("round3", HERE.parent / "round3/build.py")
r3 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(r3)
r2 = r3.r2
num, svg, uri, img, placed = r2.num, r2.svg, r2.uri, r2.img, r2.placed
stencil, specks = r3.stencil, r3.specks

SAND = "#e7e0c9"
OLIVE_INK = "#2b3122"
SIGNAL = "#ff6b1a"      # round 3's hazard tab
SIGNAL_BRIGHT = "#ff8f2e"

# the H and the 1, placed as round 3 placed H1
_CAP, _BASE = 40.0, 62.0
_X0 = 50 - r3.stencil_width("H1", _CAP) / 2
HULL_H = stencil("H", _X0, _CAP, _BASE)[0]
HULL_ONE = stencil("1", _X0 + (64 + 10) * _CAP / 100, _CAP, _BASE)[0]
RIVETS = r3.RIVETS


def hull_layers(tab: bool) -> list:
    seams = "M4 77.5H96M62 77.5V96" if tab else "M4 77.5H96"
    lips = "M4 79H96M63.4 79V96" if tab else "M4 79H96"
    layers = [
        ("panel", "fill", r3.HULL_PANEL),
        ("seam", "cut", f'<path fill="none" stroke="{{c}}" stroke-width="1.3" d="{seams}"/>'),
        ("lip", "skip", f'<path fill="none" stroke="{{c}}" stroke-width=".6" d="{lips}"/>'),
        ("lip", "skip", '<path fill="none" stroke="{c}" stroke-width=".7" opacity=".6" d="M14.4 5H85.6L95 14.4"/>'),
        ("seam", "cut", "".join(f"M{x - 1.9} {y}a1.9 1.9 0 1 0 3.8 0a1.9 1.9 0 1 0-3.8 0Z" for x, y in RIVETS)),
        ("lip", "skip", "".join(f"M{x - 0.8} {y - 0.7}a.7 .7 0 1 0 1.4 0a.7 .7 0 1 0-1.4 0Z" for x, y in RIVETS)),
        ("paint", "cut", HULL_H),
        ("one", "cut", HULL_ONE),
        ("panel", "fill", specks(7, (27, 22, 73, 62), 90)),
        ("paint", "cut", r3.HULL_SERIAL),
    ]
    if tab:
        layers += [
            ("one", "cut", "M66 84.5H88V90H66Z"),
            ("seam", "skip", '<path fill="{c}" d="M70 84.5h3l-3 5.5h-3Z M77 84.5h3l-3 5.5h-3Z M84 84.5h3l-2.8 5.5h-3Z" opacity=".85"/>'),
        ]
    return layers


def words(colours, is_mono, x, cap, baseline):
    """OPENH1 in the stencil alphabet, its 1 in the accent"""
    d1, w1 = stencil("OPENH", x, cap, baseline)
    d2, w2 = stencil("1", x + w1 + cap * 0.1, cap, baseline)
    return (f'<path fill="{colours["ink"]}" d="{d1}"/><path fill="{colours["accent"]}" d="{d2}"/>'), w1 + cap * 0.1 + w2


def favicon(orange: str, tab: bool) -> str:
    # a darker plate than round 3's, for the orange's sake: 3.7 to 1 for signal
    # orange, 4.6 for the brighter tone, 7.9 for the sand
    out = ('<path fill="#3a4232" d="M2.5 0H13.5L16 2.5V13.5L13.5 16H2.5L0 13.5V2.5Z"/>'
           # with the tab, the letters sit high to leave it room; without, centred
           f'<path fill="{SAND}" transform="translate(0 {0 if tab else 1})" d="M2 3h2v8h-2Z M6 3h2v8h-2Z M4 6h2v2h-2Z"/>'
           f'<path fill="{orange}" transform="translate(0 {0 if tab else 1})" d="M11 3h2v8h-2Z M9 4h2v2h-2Z"/>')
    if tab:
        out += f'<path fill="{orange}" d="M8 13h5v1.5h-5Z"/>'
    return out


def variation(vid: str, name: str, orange: str, light_orange: str, tab: bool, idea: str) -> dict:
    pal = {"panel": "url(#hull)", "seam": "#1f2419", "lip": "#7b8569", "paint": SAND, "one": orange,
           "ink": SAND, "accent": orange}
    concept = {
        "id": vid, "name": name, "style": f"Hull Marking, the 1 in {'signal' if orange == SIGNAL else 'bright signal'} orange"
                                          f"{', tab kept' if tab else ', no tab'}",
        "ground": ("#14170f", "#ecebe3"),
        "pal": {"dark": pal, "light": dict(pal, ink=OLIVE_INK, accent=light_orange)},
        "defs": {"dark": r3.HULL_DEFS, "light": r3.HULL_DEFS},
        "layers": hull_layers(tab),
        "wordmark": words, "cap": 20,
        "favicon": favicon(orange, tab),
        "idea": idea,
        "legal": "Unchanged from round 3: our own stencil letters on a generic chamfered panel; no emblem, insignia, "
                 "unit name or lettering from the series.",
        "xbox": "Not UNSC: no eagle, globe, wings or star, none of its type. Not Xbox: olive, sand and orange, no X, "
                "orb or green.",
    }
    concept["icon"] = lambda c: ('<rect width="1024" height="1024" fill="#2b3024"/>'
                                 '<path fill="none" stroke="#1f2419" stroke-width="10" d="M0 300H1024M0 760H1024M700 0V300"/>'
                                 + placed(r2.mark_body(c, "dark"), 7.4, 142, 142))
    return concept


VARIATIONS = [
    variation("a-tab", "A: tab kept", SIGNAL, "#c4500a", True,
              "Round 3's panel with the 1 in signal orange and the orange hazard tab kept beside the serial. "
              "The tab echoes the 1, but it is a second orange shape competing for the same attention."),
    variation("b-no-tab", "B: no tab", SIGNAL, "#c4500a", False,
              "The 1 in signal orange and the hazard tab removed, along with the seam that boxed it in: the 1 is the "
              "only orange on the panel, so the eye goes H, then 1, then the small sand serial."),
    variation("c-bright", "C: brighter, no tab", SIGNAL_BRIGHT, "#c4500a", False,
              "As B, with the orange lifted to a brighter tone. Against the small version's olive plate it reaches about "
              "4.6 to 1 instead of 3.7, which is what lets the 1 hold its own beside the sand H at 16 pixels."),
]

RECOMMENDATION = """
  <section class="pick">
    <h2>Recommendation: C, brighter orange, no tab</h2>
    <p><b>Drop the tab.</b> With the 1 in orange, the tab becomes a second orange shape of almost the same size in the
    bottom corner. It pulls the eye away from the 1 and makes the orange read as decoration rather than as the one
    signal on the panel. Without it (and without the seam that boxed it in) the bottom strip is a quiet sand serial,
    and the 1 owns the colour. At 16 pixels the tab was a stray orange bar under the letters; without it the small
    version is just H and 1.</p>
    <p><b>Lift the orange.</b> The round-3 orange (#ff6b1a) reads well large, but at 16 pixels its 2-pixel stem sinks a
    little next to the sand H. The small version's plate is now a shade darker for all three (#3a4232), which takes
    signal orange to about 3.7 to 1; the brighter tone (#ff8f2e) reaches about 4.6 to 1 (the sand H is 7.9), so the 1
    holds its own in a browser tab while still reading as signal orange rather than amber. On light backgrounds the
    lockup uses a deeper orange (#c4500a, about 3.9 to 1 on the light ground) for the 1, so the wordmark keeps its
    contrast there too.</p>
    <p><b>One colour.</b> In single colour the 1 takes the H's colour; the stencil bridge that cuts its flag loose is
    what still marks it out, so the mark needs no second tone to work in print, embroidery or engraving.</p>
  </section>"""


def board(outputs: list) -> str:
    sections = "".join(r2.section(i + 1, c, o).replace("Why it's not the Xbox logo.", "Why it's not UNSC or Xbox.")
                       for i, (c, o) in enumerate(outputs))
    compare = "".join(
        f'<figure class="panel" style="background:#14170f">'
        f'<div class="cmp">{img(uri(o["mark.svg"]), "", c["name"], "128")}{img(uri(o["favicon.svg"]), "", "", "32")}'
        f'{img(uri(o["favicon.svg"]), "", "", "16")}{r2.pixels(uri(o["favicon.svg"]), 16, 5)}</div>'
        f'<figcaption>{html.escape(c["name"])}</figcaption></figure>' for c, o in outputs)
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>OpenH1 logo board, round 4</title>
<style>{r2.STYLE}
.strip-grid {{ grid-template-columns: repeat(3, 1fr); }}
.compare {{ grid-template-columns: repeat(3, 1fr); margin-top: 20px; }}
.cmp {{ display: flex; align-items: flex-end; gap: 16px; }}
.pick b {{ color: var(--text); }}</style>
</head>
<body>
<div class="wrap">
  <h1>OpenH1 logo board, round 4</h1>
  <p class="lede">Hull Marking, revised: the 1 in signal orange, the H in sand. Three variations side by side: the
  hazard tab kept, the tab removed, and the tab removed with a brighter orange for small sizes.</p>
  <p class="note">Files are in website/logo/round4/, one folder per variation, made by website/logo/round4/build.py.
  Earlier rounds are in logo-board.html, logo-board-2.html and logo-board-3.html.</p>
  <div class="row compare">{compare}</div>
  {sections}
  <section class="strip">
    <h2>At small size</h2>
    <p class="note">Each variation's small version at its true 16 pixels in a light and a dark browser tab, on white and on
    dark, the one-colour mark at 16 pixels, and the small version enlarged eight times.</p>
    <div class="strip-grid">{r2.strip(outputs)}</div>
  </section>
  {RECOMMENDATION}
</div>
<script>{r2.round1.SCRIPT}</script>
</body>
</html>
"""


def main() -> None:
    outputs = []
    for c in VARIATIONS:
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
