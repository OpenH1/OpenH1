"""Round 5: every concept shown where it will live.

  python build.py            # all concepts, sheets and overview
  python build.py plate      # one concept
"""
import html as H
import math
import os
import re
import sys
import urllib.parse

import concepts as C
from shot import shot

TOOLS = os.path.dirname(os.path.abspath(__file__))
R5 = os.path.abspath(os.path.join(TOOLS, ".."))
SITE = os.path.abspath(__import__("glyphs").SITE)
BUILD = os.path.join(R5, "build")
os.makedirs(BUILD, exist_ok=True)
INDEX = open(os.path.join(SITE, "index.html")).read()


# ------------------------------------------------------------------ shapes
def superellipse(cx, cy, a, n=5.0, steps=240):
    pts = []
    for i in range(steps):
        t = 2 * math.pi * i / steps
        c, s = math.cos(t), math.sin(t)
        x = cx + a * math.copysign(abs(c) ** (2 / n), c)
        y = cy + a * math.copysign(abs(s) ** (2 / n), s)
        pts.append(f"{x:.2f} {y:.2f}")
    return "M" + " L".join(pts) + " Z"


def data_uri(svg):
    return "data:image/svg+xml," + urllib.parse.quote(svg)


def favicon_svg(c, small=True):
    """The tab icon: the mark on a dark rounded tile, pure paths."""
    m = c.mark("color", small=small)
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">'
            f'<rect width="100" height="100" rx="20" fill="#0a0d09"/>'
            f'<g transform="translate(6 6) scale(.88)">{m}</g></svg>')


def icon_bg(gid):
    return (f'<radialGradient id="{gid}" cx="50%" cy="78%" r="75%">'
            f'<stop offset="0" stop-color="#1d3a0c"/><stop offset=".55" stop-color="#0b1408"/>'
            f'<stop offset="1" stop-color="#050705"/></radialGradient>')


def mac_icon(c, px):
    """macOS: the 1024 grid, an 824 squircle body, a soft shadow."""
    gid, cid = C.uid("bg"), C.uid("clip")
    body = superellipse(512, 512, 412)
    m = c.mark("color")
    k = 824 * 0.70 / 100
    off = 512 - 50 * k
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024" width="{px}" height="{px}">'
            f'<defs>{icon_bg(gid)}<clipPath id="{cid}"><path d="{body}"/></clipPath>'
            f'<filter id="{cid}s" x="-20%" y="-20%" width="140%" height="140%"><feDropShadow dx="0" dy="14" stdDeviation="16" flood-color="#000" flood-opacity=".5"/></filter></defs>'
            f'<path d="{body}" fill="#050705" filter="url(#{cid}s)"/>'
            f'<g clip-path="url(#{cid})"><rect width="1024" height="1024" fill="url(#{gid})"/>'
            f'<g transform="translate({off:.1f} {off:.1f}) scale({k:.3f})">{m}</g></g>'
            f'<path d="{body}" fill="none" stroke="#fff" stroke-opacity=".10" stroke-width="3"/></svg>')


def android_icon(c, px, shape):
    """Android adaptive: 108 layers, a 72 viewport, the launcher's mask; the mark inside the 66 safe zone."""
    gid, cid = C.uid("bg"), C.uid("clip")
    if shape == "circle":
        clip = '<circle cx="54" cy="54" r="36"/>'
    elif shape == "squircle":
        clip = f'<path d="{superellipse(54, 54, 36, n=4)}"/>'
    else:
        clip = '<rect x="18" y="18" width="72" height="72" rx="13"/>'
    m = c.mark("color")
    k = 46 / 100
    off = 54 - 50 * k
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="18 18 72 72" width="{px}" height="{px}">'
            f'<defs>{icon_bg(gid)}<clipPath id="{cid}">{clip}</clipPath></defs>'
            f'<g clip-path="url(#{cid})"><rect width="108" height="108" fill="url(#{gid})"/>'
            f'<g transform="translate({off:.2f} {off:.2f}) scale({k:.3f})">{m}</g></g></svg>')


def mark_tile(c, px, mode="color", bg="#0a0d09", small=None):
    small = px <= 40 if small is None else small
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" width="{px}" height="{px}">'
            f'<rect width="100" height="100" rx="20" fill="{bg}"/><g transform="translate(6 6) scale(.88)">{c.mark(mode, small=small)}</g></svg>')


# ------------------------------------------------------------------ the real page, swapped
OVERRIDE = """<style>
.reveal{opacity:1!important;transform:none!important;transition:none!important}
.wordmark{display:inline-flex;align-items:center}
.wordmark svg{display:block}
</style>"""


def site_page(c, out_html, headline=None, eyebrow=None):
    page = INDEX
    head = C.lockup_svg(c, "color", cap_px=c.header_cap)
    foot = C.lockup_svg(c, "color", cap_px=c.header_cap * 0.8)
    page = page.replace('<a class="wordmark" href="#main" aria-label="OpenH1, top of page">Open<b>H1</b></a>',
                        f'<a class="wordmark" href="#main" aria-label="OpenH1, top of page">{head}</a>', 1)
    page = page.replace('<a class="wordmark" href="#main">Open<b>H1</b></a>', f'<a class="wordmark" href="#main">{foot}</a>', 1)
    page = page.replace('<link rel="icon" href="favicon.png" sizes="32x32">',
                        f'<link rel="icon" href="{H.escape(data_uri(favicon_svg(c)))}">')
    # the poster only: the same still behind every take
    page = page.replace('data-webm="media/hero.webm" data-mp4="media/hero.mp4"', '')
    page = page.replace("</head>", OVERRIDE + "</head>", 1)
    if headline:
        a, b = headline
        page = re.sub(r'<h1 id="hero-title" class="reveal d1">.*?</h1>',
                      f'<h1 id="hero-title" class="reveal d1"><span>{a}</span><span class="lit">{b}</span></h1>', page, flags=re.S)
    if eyebrow:
        page = page.replace('Native port · Built from the decompiled Xbox source', eyebrow, 1)
    open(out_html, "w").write(page)


def site_shot(c, tag="site", **kw):
    html = os.path.join(SITE, f"_r5-{c.key}-{tag}.html")
    site_page(c, html, **kw)
    png = os.path.join(BUILD, f"{c.key}-{tag}.png")
    shot(html, png, 1440, 900, scale=2, budget=3000)
    os.remove(html)
    return png


# ------------------------------------------------------------------ the sheet
CSS = """
@font-face { font-family: "Archivo"; font-weight: 100 900; font-stretch: 62% 125%; src: url("SITE/fonts/Archivo-Latin.woff2") format("woff2"); }
@font-face { font-family: "Inter"; font-weight: 400 800; src: url("SITE/fonts/Inter-Latin.woff2") format("woff2"); }
@font-face { font-family: "Overpass Mono"; font-weight: 400 700; src: url("SITE/fonts/OverpassMono-Latin.woff2") format("woff2"); }
* { box-sizing: border-box; }
body { margin: 0; background: #070907; color: #eef4ea; font: 400 15px/1.5 Inter, system-ui; width: 1600px; }
.page { padding: 56px 40px 60px; }
.mono { font: 600 11px/1.4 "Overpass Mono", monospace; letter-spacing: .16em; text-transform: uppercase; color: #8e9a89; }
.g { color: #9dff3c; }
h1 { margin: 6px 0 6px; font: 900 64px/0.95 Archivo; font-stretch: 118%; letter-spacing: -0.02em; text-transform: uppercase; }
.lead { max-width: 1060px; color: #c6cfc1; font-size: 18px; margin: 14px 0 0; }
.meta { display: flex; gap: 28px; margin-top: 16px; }
.card { background: #0b0e0a; border: 1px solid #1a2117; border-radius: 12px; padding: 22px 24px 24px; }
.card h3 { margin: 0 0 16px; }
.sec { margin-top: 26px; }
.shot { width: 100%; display: block; border-radius: 8px; border: 1px solid #1a2117; }
.crop { width: 100%; height: 0; padding-top: 16.25%; background-size: 260% auto; background-position: 0 0; border-radius: 8px; border: 1px solid #1a2117; margin-top: 14px; image-rendering: auto; }
.grid2 { display: grid; grid-template-columns: 1fr 1fr; gap: 20px; }
.tabs { display: grid; gap: 14px; }
.bar { height: 42px; border-radius: 10px 10px 0 0; display: flex; align-items: flex-end; padding: 0 10px; gap: 2px; }
.bar.dark { background: #1f2320; }
.bar.light { background: #dde1e5; }
.tab { height: 34px; width: 240px; border-radius: 9px 9px 0 0; display: flex; align-items: center; gap: 9px; padding: 0 12px; font: 500 12.5px/1 Inter; white-space: nowrap; overflow: hidden; }
.dark .tab.on { background: #353a36; color: #e8ece6; }
.dark .tab.off { color: #9aa197; }
.light .tab.on { background: #fff; color: #202124; }
.light .tab.off { color: #5f6368; }
.tab canvas { flex: none; }
.tab .x { margin-left: auto; opacity: .55; }
.url { height: 12px; border-radius: 0 0 10px 10px; }
.dark + .url { background: #353a36; } .light + .url { background: #fff; }
.zoom { display: flex; gap: 22px; align-items: flex-end; margin-top: 10px; }
.zoom figure { margin: 0; text-align: center; }
.zoom canvas.big { image-rendering: pixelated; border-radius: 4px; background: #000; }
.zoom figcaption { margin-top: 8px; }
.icons { display: flex; gap: 22px; align-items: flex-end; flex-wrap: wrap; }
.icons figure { margin: 0; text-align: center; }
.icons figcaption { margin-top: 8px; }
.load { position: relative; width: 100%; aspect-ratio: 16/9; border-radius: 8px; overflow: hidden;
  background: radial-gradient(ellipse 70% 60% at 50% 42%, #133170 0%, #0a1d47 45%, #050f29 80%, #030a1c 100%); }
.load::after { content: ""; position: absolute; inset: 0; background: repeating-linear-gradient(0deg, rgba(0,0,0,.22) 0 1px, transparent 1px 3px); }
.load .corner { position: absolute; right: 4.2%; bottom: 7.5%; z-index: 1; }
.load .ld { position: absolute; left: 4.2%; bottom: 7.5%; z-index: 1; width: 34%; }
.load .ld .t { font: 600 10px/1 "Overpass Mono"; letter-spacing: .2em; color: #b9c8e8; }
.load .ld .p { height: 3px; margin-top: 9px; background: rgba(185,200,232,.18); border-radius: 2px; overflow: hidden; }
.load .ld .p i { display: block; height: 100%; width: 62%; background: #b9c8e8; }
.load .chap { position: absolute; left: 0; right: 0; top: 40%; text-align: center; font: 600 12px/1.6 "Overpass Mono"; letter-spacing: .4em; color: rgba(205,218,245,.75); z-index: 1; }
.swatches { display: grid; grid-template-columns: repeat(3, 1fr); gap: 12px; }
.sw { border-radius: 8px; height: 120px; display: grid; place-items: center; padding: 0 14px; }
.sizes { display: flex; align-items: flex-end; gap: 26px; flex-wrap: wrap; margin-top: 18px; padding: 20px; background: #040504; border-radius: 8px; }
.sizes figure { margin: 0; display: grid; gap: 8px; justify-items: start; }
.note { color: #8e9a89; font-size: 13.5px; margin: 12px 0 0; }
"""

SCRIPT = """
<script>
// favicons drawn the way a browser draws them: the SVG rasterised at the real pixel size
document.querySelectorAll('canvas[data-svg]').forEach((cv) => {
  const img = new Image();
  img.onload = () => {
    const ctx = cv.getContext('2d');
    ctx.drawImage(img, 0, 0, cv.width, cv.height);
    document.querySelectorAll('canvas[data-from="' + cv.id + '"]').forEach((big) => {
      const b = big.getContext('2d'); b.imageSmoothingEnabled = false;
      b.drawImage(cv, 0, 0, big.width, big.height);
    });
  };
  img.src = cv.dataset.svg;
});
</script>"""


def fav_canvas(c, px, css, cid=None):
    uri = H.escape(data_uri(favicon_svg(c, small=True)))
    i = f' id="{cid}"' if cid else ""
    return f'<canvas{i} width="{px}" height="{px}" style="width:{css}px;height:{css}px" data-svg="{uri}"></canvas>'


def tabs_card(c):
    k = c.key
    rows = []
    for theme in ("dark", "light"):
        rows.append(f'''<div><div class="bar {theme}">
          <div class="tab on">{fav_canvas(c, 32, 16)}<span>OpenH1: the 2001 original, native</span><span class="x">✕</span></div>
          <div class="tab off"><svg width="16" height="16" viewBox="0 0 16 16"><circle cx="8" cy="8" r="7" fill="none" stroke="currentColor" stroke-width="1.4"/></svg><span>New Tab</span></div>
        </div><div class="url"></div></div>''')
    zoom = f'''<div class="zoom">
      <figure>{fav_canvas(c, 16, 16, f"f16-{k}")}<figcaption class="mono">16 px</figcaption></figure>
      <figure><canvas class="big" width="96" height="96" data-from="f16-{k}" style="width:96px;height:96px"></canvas><figcaption class="mono">16 px, ×6</figcaption></figure>
      <figure>{fav_canvas(c, 32, 32, f"f32-{k}")}<figcaption class="mono">32 px</figcaption></figure>
      <figure><canvas class="big" width="96" height="96" data-from="f32-{k}" style="width:96px;height:96px"></canvas><figcaption class="mono">32 px, ×3</figcaption></figure>
    </div>'''
    return f'''<div class="card"><h3 class="mono">Browser tab <span class="g">· favicon</span></h3>
      <div class="tabs">{"".join(rows)}</div>{zoom}
      <p class="note">Tabs show the 32 px icon at 16 points, as on a Retina screen. The zooms are the real 16 and 32 px pixels.</p></div>'''


def icons_card(c):
    return f'''<div class="card"><h3 class="mono">App icon <span class="g">· macOS and Android</span></h3>
      <div class="icons">
        <figure>{mac_icon(c, 196)}<figcaption class="mono">macOS · 1024 grid</figcaption></figure>
        <figure>{mac_icon(c, 64)}<figcaption class="mono">Dock 64</figcaption></figure>
        <figure>{mac_icon(c, 32)}<figcaption class="mono">32</figcaption></figure>
      </div>
      <div class="icons" style="margin-top:22px">
        <figure>{android_icon(c, 104, "circle")}<figcaption class="mono">Android · circle</figcaption></figure>
        <figure>{android_icon(c, 104, "squircle")}<figcaption class="mono">squircle</figcaption></figure>
        <figure>{android_icon(c, 104, "rounded")}<figcaption class="mono">rounded square</figcaption></figure>
        <figure>{android_icon(c, 48, "circle")}<figcaption class="mono">48 dp</figcaption></figure>
      </div></div>'''


def load_card(c):
    return f'''<div class="card"><h3 class="mono">In game <span class="g">· loading screen corner</span></h3>
      <div class="load">
        <div class="ld"><div class="t">Loading</div><div class="p"><i></i></div></div>
        <div class="corner">{C.lockup_svg(c, "color", cap_px=13)}</div></div>
      <p class="note">Over the game's dark blue loading frame, bottom right, at about the size it would sit at 1080p scaled to this frame.</p></div>'''


def mono_card(c):
    sw = [("#eef4ea", "#0b0e0a"), ("#040504", "#eef4ea"), ("#040504", "#9dff3c")]
    swatches = "".join(f'<div class="sw" style="background:{bg}">{C.lockup_svg(c, fg, cap_px=19)}</div>' for bg, fg in sw)
    marks = "".join(f'<div class="sw" style="background:{bg};height:92px">{mark_tile(c, 60, fg, bg=bg, small=False)}</div>' for bg, fg in sw)
    sizes = "".join(f'<figure>{C.lockup_svg(c, "color", cap_px=cp)}<span class="mono">cap {cp} px</span></figure>' for cp in (24, 14, 9, 6))
    ms = '<div style="flex-basis:100%;height:0"></div>' + "".join(f'<figure>{mark_tile(c, px)}<span class="mono">mark {px}</span></figure>' for px in (48, 24, 16))
    return f'''<div class="card"><h3 class="mono">One colour <span class="g">· and small</span></h3>
      <div class="swatches">{swatches}</div>
      <div class="swatches" style="margin-top:12px">{marks}</div>
      <div class="sizes">{sizes}{ms}</div></div>'''


def concept_sheet(c, n, site_png):
    rel = os.path.relpath(site_png, BUILD)
    body = f'''<div class="page">
      <div class="mono"><span class="g">OpenH1 · logo round 5</span> · concept {n:02d} of {len(C.ALL)}</div>
      <h1>{n:02d} {c.name}</h1>
      <div class="mono" style="color:#c6cfc1;letter-spacing:.12em">{c.tagline}</div>
      <p class="lead">{c.line}</p>
      <div class="meta mono"><span>Type: <span style="color:#c6cfc1">{c.type_note}</span></span><span>Colours: <span style="color:#c6cfc1">the site's #9DFF3C on #040504, text #EEF4EA</span></span></div>
      <div class="sec card"><h3 class="mono">On the site <span class="g">· the real page, logo and favicon swapped in, 1440 × 900</span></h3>
        <img class="shot" src="{rel}">
        <div class="crop" style="background-image:url('{rel}')"></div>
        <p class="note">Above: the header at 2×, top left.</p></div>
      <div class="sec grid2">{tabs_card(c)}{icons_card(c)}</div>
      <div class="sec grid2">{load_card(c)}{mono_card(c)}</div>
    </div>'''
    page = f'<!doctype html><meta charset="utf-8"><style>{CSS.replace("SITE", os.path.relpath(SITE, BUILD))}</style>{body}{SCRIPT}'
    html = os.path.join(BUILD, f"sheet-{c.key}.html")
    open(html, "w").write(page)
    return html


def trim(png, bg=(7, 9, 7), pad=60):
    """Cut the empty page bottom off a sheet."""
    from PIL import Image
    im = Image.open(png).convert("RGB")
    w, h = im.size
    px = im.load()
    y = h - 1
    while y > 0:
        row = [px[x, y] for x in range(0, w, 8)]
        if any(abs(r - bg[0]) + abs(g_ - bg[1]) + abs(b - bg[2]) > 12 for r, g_, b in row):
            break
        y -= 1
    im.crop((0, 0, w, min(h, y + pad))).save(png, optimize=True)


def main(keys):
    out = os.path.join(R5)
    made = []
    for n, c in enumerate(C.ALL, 1):
        if keys and c.key not in keys:
            continue
        png = site_shot(c)
        html = concept_sheet(c, n, png)
        sheet = os.path.join(out, f"concept-{n:02d}-{c.key}.png")
        shot(html, sheet, 1600, 3000, scale=2, budget=3000)
        trim(sheet)
        made.append(sheet)
    return made


if __name__ == "__main__":
    main(sys.argv[1:])
