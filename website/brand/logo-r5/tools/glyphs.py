"""Text to SVG outlines, from the site's own OFL fonts (Archivo, Overpass Mono).

So every logo is plain vector paths: no font needed to show it, safe inside
an <img>, a favicon or an app icon.
"""
import functools
import os

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.boundsPen import BoundsPen
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

HERE = os.path.dirname(os.path.abspath(__file__))
# the site's public folder: in the repo (website/brand/logo-r5/tools -> website/public),
# or a copy of it beside these tools; OPENH1_SITE overrides both
SITE = os.environ.get("OPENH1_SITE") or next(
    p for p in (os.path.join(HERE, "..", "..", "..", "public"), os.path.join(HERE, "..", "site", "website", "public"))
    if os.path.exists(os.path.join(p, "index.html")))
FONTS = os.path.join(SITE, "fonts")
FILES = {"archivo": "Archivo-Latin.woff2", "mono": "OverpassMono-Latin.woff2", "inter": "Inter-Latin.woff2"}


@functools.lru_cache(None)
def font(family, **axes):
    t = TTFont(os.path.join(FONTS, FILES[family]))
    if axes:
        t = instancer.instantiateVariableFont(t, axes)
    return t


def _kern_table(t):
    """Pair kerning from GPOS 'kern' (PairPos formats 1 and 2)."""
    pairs, classes = {}, []
    gpos = t["GPOS"].table
    idx = set()
    for fr in gpos.FeatureList.FeatureRecord:
        if fr.FeatureTag == "kern":
            idx.update(fr.Feature.LookupListIndex)
    for i in sorted(idx):
        lk = gpos.LookupList.Lookup[i]
        subs = lk.SubTable
        for st in subs:
            if lk.LookupType == 9:
                st = st.ExtSubTable
            if getattr(st, "LookupType", 2) != 2 and lk.LookupType not in (2, 9):
                continue
            if not hasattr(st, "Format"):
                continue
            cov = st.Coverage.glyphs
            if st.Format == 1:
                for g, ps in zip(cov, st.PairSet):
                    for pvr in ps.PairValueRecord:
                        v = getattr(pvr.Value1, "XAdvance", 0) or 0
                        pairs.setdefault((g, pvr.SecondGlyph), v)
            elif st.Format == 2:
                classes.append((set(cov), st.ClassDef1.classDefs, st.ClassDef2.classDefs, st.Class1Record))
    return pairs, classes


@functools.lru_cache(None)
def _kern(family, axes_key):
    return _kern_table(font(family, **dict(axes_key)))


def kern(family, axes, a, b):
    pairs, classes = _kern(family, tuple(sorted(axes.items())))
    if (a, b) in pairs:
        return pairs[(a, b)]
    for cov, c1, c2, recs in classes:
        if a in cov:
            r = recs[c1.get(a, 0)].Class2Record[c2.get(b, 0)]
            v = getattr(r.Value1, "XAdvance", 0) or 0
            if v:
                return v
    return 0


def glyph_path(family, axes, ch, size, x=0.0, y=0.0):
    """One character's outline as SVG path data, baseline at y. Returns (d, advance)."""
    t = font(family, **axes)
    upm = t["head"].unitsPerEm
    name = t.getBestCmap()[ord(ch)]
    gs = t.getGlyphSet()
    s = size / upm
    pen = SVGPathPen(gs, ntos=lambda v: ("%.2f" % v).rstrip("0").rstrip("."))
    gs[name].draw(TransformPen(pen, (s, 0, 0, -s, x, y)))
    return pen.getCommands(), t["hmtx"][name][0] * s


def text_path(text, size, family="archivo", axes=None, tracking=0.0, x=0.0, y=0.0, adjust=None):
    """A run of text as one SVG path. tracking is in ems; adjust maps an index to an
    extra shift (in ems) after that character. Returns (d, width, per-char boxes)."""
    axes = axes or {}
    t = font(family, **axes)
    upm = t["head"].unitsPerEm
    cmap = t.getBestCmap()
    parts, boxes = [], []
    cx = x
    for i, ch in enumerate(text):
        d, adv = glyph_path(family, axes, ch, size, cx, y)
        parts.append(d)
        boxes.append((cx, adv))
        cx += adv + tracking * size
        if adjust and i in adjust:
            cx += adjust[i] * size
        if i + 1 < len(text):
            cx += kern(family, axes, cmap[ord(ch)], cmap[ord(text[i + 1])]) * size / upm
    width = cx - x - tracking * size
    return " ".join(parts), width, boxes


def ink_bounds(family, axes, ch, size):
    t = font(family, **axes)
    gs = t.getGlyphSet()
    bp = BoundsPen(gs)
    gs[t.getBestCmap()[ord(ch)]].draw(bp)
    s = size / t["head"].unitsPerEm
    xmin, ymin, xmax, ymax = bp.bounds
    return xmin * s, ymin * s, xmax * s, ymax * s


def metrics(family, axes, size):
    t = font(family, **axes)
    s = size / t["head"].unitsPerEm
    os2 = t["OS/2"]
    return {"cap": os2.sCapHeight * s, "x": os2.sxHeight * s}
