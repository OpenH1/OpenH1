"""The overview sheet and the headline sheet."""
import os

import build as B
import concepts as C
from shot import shot

R5, BUILD = B.R5, B.BUILD
PICK = "plate"

HEADLINES = [
    # (line 1, line 2 lit, who it speaks to)
    ("Not a remaster.", "The original.", "Purists. Says what it is, and what it isn't."),
    ("The original,", "recompiled.", "Purists and modders. The whole method in three words."),
    ("", "Bit for bit.", "Purists. The scorecard's promise, as a boast."),
    ("Bring 127", "friends.", "LAN veterans. 128 players, said with a grin."),
    ("Every tick", "intact.", "Purists. 30 ticks a second, exactly as shipped."),
    ("From", "the source.", "Everyone. The decompile and the original, in one phrase."),
    ("No emulator.", "No compromise.", "PC players. The two things it isn't."),
    ("Open it", "up.", "Modders. Plays on the name and the licence."),
    ("Back on", "the LAN.", "LAN veterans. The thing they miss."),
    ("30 ticks.", "Any frame rate.", "Purists and high-refresh players. The tech, plainly."),
    ("Decompiled. Recompiled.", "Yours.", "Modders and the open-source crowd."),
    ("Native.", "Finally.", "Mac, Linux, Android and Steam Frame players who waited."),
]
TOP4 = [0, 1, 2, 3]
KICKERS = {3: "Native port · 128 players, online and on the LAN"}


def headline_shots():
    pick = next(c for c in C.ALL if c.key == PICK)
    out = []
    for i in TOP4:
        a, b, _ = HEADLINES[i]
        png = B.site_shot(pick, tag=f"hl{i+1}", headline=(a, b), eyebrow=KICKERS.get(i))
        out.append((i, png))
    return out


def headline_sheet(shots):
    cards = "".join(f'''<div class="card"><h3 class="mono"><span class="g">Top {k+1}</span> · {HEADLINES[i][0]} {HEADLINES[i][1]}</h3>
        <img class="shot" src="{os.path.relpath(p, BUILD)}"><p class="note">{HEADLINES[i][2]}{' Kicker changed to match.' if i in KICKERS else ''}</p></div>'''
                    for k, (i, p) in enumerate(shots))
    rows = "".join(f'''<li class="{'top' if i in TOP4 else ''}"><span class="mono">{i+1:02d}</span>
        <b>{(a + ' ' + b).strip()}</b><span class="who">{w}</span>{'<span class="mono g">top 4</span>' if i in TOP4 else ''}</li>'''
                   for i, (a, b, w) in enumerate(HEADLINES))
    body = f'''<div class="page">
      <div class="mono"><span class="g">OpenH1 · hero headline</span> · 12 candidates, top 4 in the real page</div>
      <h1>Hero headline</h1>
      <p class="lead">Twelve new lines for the enthusiasts: purists, LAN veterans, modders and VR players. Short, sure of themselves, no borrowed taglines.
      The top four are set in the real hero, over the video poster, with the recommended logo (TAIL CODE) in the header.</p>
      <div class="sec grid2">{cards}</div>
      <div class="sec card"><h3 class="mono">All twelve</h3><ol class="hl">{rows}</ol></div>
    </div>'''
    css = B.CSS.replace("SITE", os.path.relpath(B.SITE, BUILD)) + """
      .hl { list-style: none; margin: 0; padding: 0; display: grid; grid-template-columns: 1fr 1fr; gap: 0 40px; }
      .hl li { display: grid; grid-template-columns: 34px 330px 1fr auto; gap: 14px; align-items: baseline; padding: 13px 0; border-top: 1px solid #1a2117; }
      .hl b { font: 900 21px/1.15 Archivo; font-stretch: 116%; text-transform: uppercase; letter-spacing: -0.01em; }
      .hl li.top b { color: #9dff3c; }
      .hl .who { color: #8e9a89; font-size: 13.5px; }
    """
    html = os.path.join(BUILD, "sheet-headlines.html")
    open(html, "w").write(f'<!doctype html><meta charset="utf-8"><style>{css}</style>{body}{B.SCRIPT}')
    png = os.path.join(R5, "headlines.png")
    shot(html, png, 1600, 2400, scale=2, budget=3000)
    B.trim(png)
    return png


VERDICT = {
    "plate": ("Recommended", "The one to take forward. Confident at every size, the best favicon of the six, and it sits on the page like a product badge. The plate also makes a clean patch, sticker and loading-screen tag."),
    "leak": ("Runner-up", "The most original idea and the best story. Slightly busy at 16 px; keep it for merch, stencils and the loading screen, or as the alternative if she wants more character."),
    "power": ("Strong", "The clearest boot-glow link, and the most Valve-like. The power symbol is common, so it is the least ownable."),
    "rising": ("Not recommended", "Ownable on paper, but the ligature reads as an odd H rather than H1 at header size."),
    "contact": ("Worth a look", "Calm and grown-up; the tracker is a good nod. Quieter than the brief's ModRetro swagger, and the disc reads as a dot at 16 px."),
    "bracket": ("Good secondary", "Belongs to the site, but the brackets are the site's style, not a mark. Better as a frame for social cards than as the logo."),
}


def overview(site_pngs):
    cards = []
    for n, c in enumerate(C.ALL, 1):
        rel = os.path.relpath(site_pngs[c.key], BUILD)
        v, why = VERDICT[c.key]
        cls = " pick" if c.key == PICK else ""
        cards.append(f'''<div class="card{cls}">
          <div class="mono"><span class="g">{n:02d}</span> · {v}</div>
          <h2>{c.name}</h2>
          <div class="hcrop" style="background-image:url('{rel}')"></div>
          <div class="lk">{C.lockup_svg(c, "color", cap_px=30)}</div>
          <div class="row">
            <figure>{B.fav_canvas(c, 16, 16, f"o16-{c.key}")}<figcaption class="mono">16</figcaption></figure>
            <figure><canvas class="big" width="64" height="64" data-from="o16-{c.key}" style="width:64px;height:64px;image-rendering:pixelated"></canvas><figcaption class="mono">16 ×4</figcaption></figure>
            <figure>{B.fav_canvas(c, 32, 32)}<figcaption class="mono">32</figcaption></figure>
            <figure>{B.mac_icon(c, 92)}<figcaption class="mono">macOS</figcaption></figure>
            <figure>{B.android_icon(c, 70, "circle")}<figcaption class="mono">Android</figcaption></figure>
            <figure class="onec">{C.lockup_svg(c, "#040504", cap_px=11)}<figcaption class="mono">one colour</figcaption></figure>
          </div>
          <p class="line">{c.line}</p>
          <p class="why"><b>{v}.</b> {why}</p>
        </div>''')
    body = f'''<div class="page">
      <div class="mono"><span class="g">OpenH1 · logo round 5</span> · six concepts, each shown where it will live · October 2026</div>
      <h1>Six logos, in place</h1>
      <p class="lead">Wordmark first, built from the site's own Archivo and Overpass Mono (both OFL) and its Boot Glow green. Every logo is plain vector paths.
      Each strip at the top of a card is the real page's header with that logo swapped in; each concept also has its own sheet.</p>
      <div class="ov">{"".join(cards)}</div>
    </div>'''
    css = B.CSS.replace("SITE", os.path.relpath(B.SITE, BUILD)) + """
      .ov { display: grid; grid-template-columns: 1fr 1fr; gap: 20px; margin-top: 30px; }
      .ov .card { padding: 22px 24px 20px; }
      .ov .card.pick { border-color: #9dff3c; box-shadow: 0 0 0 1px #9dff3c, 0 0 60px -20px rgba(157,255,60,.5); }
      .ov h2 { margin: 6px 0 14px; font: 900 34px/1 Archivo; font-stretch: 118%; letter-spacing: -0.01em; }
      .hcrop { height: 92px; border-radius: 8px; border: 1px solid #1a2117; background-size: 320% auto; background-position: 4% 0; }
      .ov .lk { height: 110px; display: grid; place-items: center start; padding: 0 22px; margin-top: 12px; background: #040504; border-radius: 8px; }
      .ov .row { display: flex; gap: 20px; align-items: flex-end; margin-top: 14px; }
      .ov figure { margin: 0; text-align: center; }
      .ov figcaption { margin-top: 6px; }
      .ov .onec { background: #eef4ea; padding: 10px 12px 6px; border-radius: 6px; }
      .ov .onec figcaption { color: #4a5446; }
      .line { color: #c6cfc1; font-size: 14px; margin: 14px 0 0; }
      .why { color: #8e9a89; font-size: 14px; margin: 8px 0 0; }
      .why b { color: #eef4ea; }
      .pick .why b { color: #9dff3c; }
    """
    html = os.path.join(BUILD, "sheet-overview.html")
    open(html, "w").write(f'<!doctype html><meta charset="utf-8"><style>{css}</style>{body}{B.SCRIPT}')
    png = os.path.join(R5, "overview.png")
    shot(html, png, 1600, 2600, scale=2, budget=3000)
    B.trim(png)
    return png


if __name__ == "__main__":
    import sys
    sites = {c.key: os.path.join(BUILD, f"{c.key}-site.png") for c in C.ALL}
    overview(sites)
    if "overview" not in sys.argv:
        headline_sheet(headline_shots())
