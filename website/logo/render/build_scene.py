"""Builds a photo-real Blender scene of the OpenH1 mark ("Hull Marking", variation C)
from its vector source, and saves it as scene.blend next to this script:

    blender -b -P website/logo/render/build_scene.py

Everything is made here, apart from two CC0 downloads (CREDITS.md):

* mark.svg is parsed for the panel outline, the seam, the four fasteners, the
  stencilled H and 1, the paint specks and the OPENH1-001 serial, and its exact
  colours.
* The stencil shapes are rasterised (anti-aliased, 8192 px across the panel) into
  mask textures in _build/, which the paint shader reads; they are packed into
  the .blend.
* The panel is a 10 mm steel plate (1 SVG unit = 3 mm, so about 28 cm across),
  split at the seam into the main plate and the serial strip over a backing
  plate, with rounded edges, four counterbores and button-head hex-socket screws.
* The paint is procedural: olive drab over primer over bare steel, worn through
  on edges (Cycles bevel-node edge mask plus noise), around the fasteners and in
  small impact chips; scratches, grime in the crevices (AO), satin roughness
  variation and a little surface waviness. The stencil paint sits on top with
  crisp edges (a tight-fitting stencil: barely any overspray) and stays solid:
  fresh paint, not faded or worn through.
* Cameras and light rigs for each shot; render.py picks one per shot.
"""

import math
import os
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector

HERE = Path(__file__).resolve().parent
SVG = HERE.parent / "round4" / "c-bright" / "mark.svg"
TAB_SVG = HERE.parent / "round4" / "a-tab" / "mark.svg"   # the hazard tab and its seam, from variation A
BUILD = HERE / "_build"
BLEND = HERE / "scene.blend"

S = 0.003          # metres per SVG unit: the 100-unit mark is a 30 cm plate
HULL = "#465048"   # the hull paint: a dark, cool grey-green
STENCIL_WHITE = "#f1f2f0"    # the H and the serial: white stencil paint
STENCIL_ORANGE = "#ff5a0a"   # the 1 and the caution stripes: military-tech orange
TEXTURES = HERE / "textures"
# scanned CC0 textures (CREDITS.md): Poly Haven's green_metal_rust, painted
# military-green steel, gives the paint its real surface; studio_small_08 is a
# photographed studio for the lighting and reflections
SCANS = {
    "diff": "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/2k/green_metal_rust/green_metal_rust_diff_2k.jpg",
    "rough": "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/2k/green_metal_rust/green_metal_rust_rough_2k.jpg",
    "nor_gl": "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/2k/green_metal_rust/green_metal_rust_nor_gl_2k.jpg",
    "ao": "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/2k/green_metal_rust/green_metal_rust_ao_2k.jpg",
}
HDRI_URL = "https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/studio_small_08_2k.hdr"
SCAN_TILE_MM = 420.0   # one tile of the scan covers 42 cm of panel
USE_SCAN = False       # the scanned paint: off (the owners found it unconvincing)
# ambientCG's PaintedMetal004 (CC0): real scratches and chips through paint to
# steel. Only its wear is used (where the paint is gone, its roughness and
# relief); the paint keeps our own hull colour
CHIPS_ZIP = "https://ambientcg.com/get?file=PaintedMetal004_2K-JPG.zip"
CHIPS_DIR = HERE / "textures" / "PaintedMetal004"
CHIPS_TILE_MM = 520.0
USE_CHIPS = False      # the chipped-paint scan: off too (the owners don't want scanned scratches)
HDRI_STRENGTH = 0.55


def fetch(url):
    """a texture in textures/, downloaded the first time"""
    import urllib.request
    path = TEXTURES / url.rsplit("/", 1)[1]
    if not path.exists():
        TEXTURES.mkdir(exist_ok=True)
        print("downloading", url)
        urllib.request.urlretrieve(url, path)
    return path


_scans = {}


def chips(kind):
    """a map of the chipped-paint scan (Color, Roughness, NormalGL, Metalness),
    downloaded and unpacked the first time"""
    import urllib.request
    import zipfile
    path = CHIPS_DIR / f"PaintedMetal004_2K-JPG_{kind}.jpg"
    if not path.exists():
        CHIPS_DIR.mkdir(parents=True, exist_ok=True)
        archive = CHIPS_DIR / "PaintedMetal004_2K-JPG.zip"
        print("downloading", CHIPS_ZIP)
        urllib.request.urlretrieve(CHIPS_ZIP, archive)
        with zipfile.ZipFile(archive) as z:
            z.extractall(CHIPS_DIR)
    key = "chips_" + kind
    if key not in _scans:
        img = bpy.data.images.load(str(path))
        if kind != "Color":
            img.colorspace_settings.name = "Non-Color"
        _scans[key] = img
    return _scans[key]


def scan(kind):
    """the scan's image, and for the colour map the mean of its luminance (to
    use it as relative detail over our own paint colour)"""
    if kind not in _scans:
        img = bpy.data.images.load(str(fetch(SCANS[kind])))
        if kind != "diff":
            img.colorspace_settings.name = "Non-Color"
        mean = None
        if kind in ("diff", "rough"):
            px = np.empty(len(img.pixels), dtype=np.float32)
            img.pixels.foreach_get(px)
            px = px.reshape(-1, 4)
            mean = float((px[:, 0] * 0.2126 + px[:, 1] * 0.7152 + px[:, 2] * 0.0722).mean())
        _scans[kind] = (img, mean)
    return _scans[kind]
T = 0.016          # plate thickness
SEAM_GAP = 0.5     # SVG units between the main plate and the serial strip
MASK_PX = 8192     # stencil mask resolution across the 100-unit square
CB_DEPTH = 0.0045  # counterbore depth
RECESS = 0.0018    # the face inside the raised frame sits this much lower
STRIP_PROUD = 0.0025  # the bolted-on serial strip and tab stand this far proud of the face
LEAN = 3.0         # degrees the plate leans back
LIGHT_SCALE = 1 / 16  # light powers below are relative; this sets the exposure


# --------------------------------------------------------------------------- SVG

def srgb_to_linear(hexcol):
    hexcol = hexcol.lstrip("#")
    out = []
    for i in range(3):
        c = int(hexcol[2 * i:2 * i + 2], 16) / 255
        out.append(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4)
    return tuple(out)


_TOKEN = re.compile(r"[MmLlHhVvZzAa]|-?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")


def _arc(p0, p1, r, large, sweep, steps=24):
    """points along a circular SVG arc from p0 to p1 (rx == ry, no rotation)"""
    p0, p1 = np.array(p0), np.array(p1)
    mid = (p0 + p1) / 2
    half = np.linalg.norm(p1 - p0) / 2
    r = max(r, half)
    h = math.sqrt(max(r * r - half * half, 0))
    d = (p1 - p0) / (2 * half)
    perp = np.array([-d[1], d[0]])
    sign = 1 if large != sweep else -1
    c = mid + sign * h * perp
    a0 = math.atan2(*(p0 - c)[::-1])
    a1 = math.atan2(*(p1 - c)[::-1])
    da = a1 - a0
    # SVG's y axis points down, so sweep=1 is increasing angle
    if sweep and da < 0:
        da += 2 * math.pi
    if not sweep and da > 0:
        da -= 2 * math.pi
    return [tuple(c + r * np.array([math.cos(a0 + da * t / steps), math.sin(a0 + da * t / steps)]))
            for t in range(1, steps + 1)]


def parse_path(d):
    """SVG path data -> list of closed polygons (lists of (x, y)); handles the
    commands the logo uses: M L H V Z and circular arcs, absolute or relative"""
    toks = _TOKEN.findall(d)
    polys, cur, pos, start = [], [], (0.0, 0.0), (0.0, 0.0)
    i, cmd = 0, None

    def num():
        nonlocal i
        v = float(toks[i])
        i += 1
        return v

    while i < len(toks):
        if toks[i].isalpha():
            cmd = toks[i]
            i += 1
            if cmd in "Zz":
                if cur:
                    polys.append(cur)
                cur, pos = [], start
                continue
        rel = cmd.islower()
        c = cmd.upper()
        if c == "M":
            if cur:
                polys.append(cur)
            x, y = num(), num()
            pos = (pos[0] + x, pos[1] + y) if rel else (x, y)
            start, cur = pos, [pos]
            cmd = "l" if rel else "L"
        elif c == "L":
            x, y = num(), num()
            pos = (pos[0] + x, pos[1] + y) if rel else (x, y)
            cur.append(pos)
        elif c == "H":
            x = num()
            pos = (pos[0] + x if rel else x, pos[1])
            cur.append(pos)
        elif c == "V":
            y = num()
            pos = (pos[0], pos[1] + y if rel else y)
            cur.append(pos)
        elif c == "A":
            rx, _ry, _rot, large, sweep, x, y = (num() for _ in range(7))
            end = (pos[0] + x, pos[1] + y) if rel else (x, y)
            cur.extend(_arc(pos, end, rx, int(large), int(sweep)))
            pos = end
        else:
            raise ValueError(f"unsupported path command {cmd}")
    if cur:
        polys.append(cur)
    return polys


def read_tab(path=TAB_SVG):
    """variation A's hazard tab: the orange bar, its dark stripes, and the x of
    the seam that boxes it in"""
    ns = {"s": "http://www.w3.org/2000/svg"}
    paths = [(p.get("fill"), p.get("stroke"), p.get("d")) for p in ET.parse(path).getroot().findall("s:path", ns)]
    tab = {}
    seam_d = next(d for f, s, d in paths if f == "none" and s == "#1f2419")
    for poly in parse_path(seam_d.replace("M", "Z M").lstrip("Z ")):
        if len(poly) >= 2 and abs(poly[0][0] - poly[-1][0]) < 1e-6:
            tab["seam_x"] = poly[0][0]
    for f, s, d in paths:
        if not f or not f.startswith("#") or "a" in d:
            continue
        polys = parse_path(d)
        ys = [pt[1] for p in polys for pt in p]
        if min(ys) < 80:
            continue
        if f.lower() == "#1f2419":
            tab["stripes"] = polys
        elif f.lower() != "#e7e0c9":
            tab["bar"] = polys
    return tab


def read_mark(path=SVG):
    ns = {"s": "http://www.w3.org/2000/svg"}
    root = ET.parse(path).getroot()
    stops = [s.get("stop-color") for s in root.iter("{http://www.w3.org/2000/svg}stop")]
    paths = root.findall("s:path", ns)
    mark = {"hull_top": stops[0], "hull_bottom": stops[-1]}
    fills = [(p.get("fill"), p.get("stroke"), p.get("d")) for p in paths]
    mark["outline"] = parse_path(fills[0][2])[0]

    def bbox(polys):
        a = np.array([pt for p in polys for pt in p])
        return a.min(0), a.max(0)

    seam_d = next(d for f, s, d in fills if f == "none" and s == "#1f2419")
    mark["seam_y"] = float(_TOKEN.findall(seam_d)[2])
    rivet_d = next(d for f, s, d in fills if f == "#1f2419" and "a" in d)
    rivets = []
    for poly in parse_path(rivet_d):
        a = np.array(poly)
        rivets.append(((a[:, 0].min() + a[:, 0].max()) / 2, (a[:, 1].min() + a[:, 1].max()) / 2,
                       (a[:, 0].max() - a[:, 0].min()) / 2))
    mark["rivets"] = rivets
    sand_paths = [(f, d) for f, s, d in fills if f and f.startswith("#") and f.lower() not in ("#1f2419", "#7b8569")]
    for f, d in sand_paths:
        polys = parse_path(d)
        lo, hi = bbox(polys)
        if f.lower() == "#ff8f2e" or (f.lower() != sand_paths[0][0].lower()):
            mark["one"], mark["orange"] = polys, f
        elif hi[1] < mark["seam_y"]:
            mark["h"], mark["sand"] = polys, f
        else:
            mark["serial"] = polys
    mark["specks"] = parse_path([d for f, s, d in fills if f == "url(#hull)"][1])
    return mark


# ------------------------------------------------------------------ rasteriser

def raster_poly(poly, n, ss=4):
    """anti-aliased coverage of one polygon on an n x n grid over [0, 100]^2;
    returns (row0, col0, tile)"""
    q = np.asarray(poly, float) * (n / 100.0)
    e = np.hstack([q, np.roll(q, -1, axis=0)])
    e = e[e[:, 1] != e[:, 3]]
    ymin, ymax = np.minimum(e[:, 1], e[:, 3]), np.maximum(e[:, 1], e[:, 3])
    r0, r1 = max(int(math.floor(ymin.min())), 0), min(int(math.ceil(ymax.max())), n)
    c0 = max(int(math.floor(q[:, 0].min())) - 1, 0)
    c1 = min(int(math.ceil(q[:, 0].max())) + 2, n)
    w = c1 - c0
    tile = np.zeros((r1 - r0, w), np.float32)
    for row in range(r0, r1):
        acc = np.zeros(w + 2)
        for k in range(ss):
            yc = row + (k + 0.5) / ss
            act = (ymin <= yc) & (ymax > yc)
            if not act.any():
                continue
            a = e[act]
            xs = np.sort(a[:, 0] + (yc - a[:, 1]) * (a[:, 2] - a[:, 0]) / (a[:, 3] - a[:, 1])) - c0
            xs = np.clip(xs, 0, w)
            for x, sgn in ((xs[1::2], 1.0), (xs[0::2], -1.0)):
                fl = np.floor(x).astype(int)
                fr = x - fl
                np.add.at(acc, fl, sgn * (fr - 1))
                np.add.at(acc, fl + 1, -sgn * fr)
        tile[row - r0] = np.cumsum(acc)[:w] / ss
    return r0, c0, np.clip(tile, 0, 1)


def raster(polys, n):
    out = np.zeros((n, n), np.float32)
    for p in polys:
        r0, c0, t = raster_poly(p, n)
        sl = out[r0:r0 + t.shape[0], c0:c0 + t.shape[1]]
        np.maximum(sl, t, out=sl)
    return out


def gauss_blur(img, sigma):
    rad = int(math.ceil(sigma * 3))
    k = np.exp(-0.5 * (np.arange(-rad, rad + 1) / sigma) ** 2)
    k /= k.sum()
    out = img
    for axis in (0, 1):
        pad = [(0, 0), (0, 0)]
        pad[axis] = (rad, rad)
        p = np.pad(out, pad, mode="edge")
        acc = np.zeros_like(out)
        for j, kv in enumerate(k):
            acc += kv * (p[j:j + out.shape[0], :] if axis == 0 else p[:, j:j + out.shape[1]])
        out = acc
    return out


def write_png(path, arr):
    import OpenImageIO as oiio
    a = (np.clip(arr, 0, 1) * 255 + 0.5).astype(np.uint8)
    h, w = a.shape[:2]
    ch = 1 if a.ndim == 2 else a.shape[2]
    out = oiio.ImageOutput.create(str(path))
    out.open(str(path), oiio.ImageSpec(w, h, ch, "uint8"))
    out.write_image(a)
    out.close()


def make_masks(mark):
    """stencil.png: R = H, G = 1, B = serial, A = specks, slightly softened (the shader
    thresholds it at 0.5 with noise for a ragged sprayed edge); haze.png: the
    same, blurred wide and small, for overspray, with A = the H and 1 blurred
    wider still, which tells the shader how near a letter's edge it is"""
    BUILD.mkdir(exist_ok=True)
    sharp, haze, tabp = BUILD / "stencil.png", BUILD / "haze.png", BUILD / "tab.png"
    if all(f.exists() and f.stat().st_mtime > max(SVG.stat().st_mtime, TAB_SVG.stat().st_mtime,
                                                  Path(__file__).stat().st_mtime) for f in (sharp, haze, tabp)):
        return sharp, haze, tabp
    # tab.png, at half the resolution: R = the orange bar, G = its black stripes,
    # B = both blurred, for overspray and for wear near their edges
    nt = MASK_PX // 2
    bar = raster(mark["tab"]["bar"], nt)
    stripes = raster(mark["tab"]["stripes"], nt) * (bar > 0).astype(np.float32)
    write_png(tabp, np.dstack([gauss_blur(bar, 0.5), gauss_blur(stripes, 0.5),
                               gauss_blur(np.maximum(bar, stripes), 12.0)]))
    n = MASK_PX
    print("rasterising stencil masks ...", flush=True)
    specks = raster(mark["specks"], n)
    chans, hazes = [], []
    for key in ("h", "one", "serial"):
        m = raster(mark[key], n)
        hz = m.reshape(n // 4, 4, n // 4, 4).mean(axis=(1, 3))
        hazes.append(gauss_blur(hz, 7.0))
        chans.append(gauss_blur(m, 0.7))
    letters = np.maximum(chans[0], chans[1])
    hazes.append(gauss_blur(letters.reshape(n // 4, 4, n // 4, 4).mean(axis=(1, 3)), 26.0))
    # A = the paint specks, blurred wider: the shader breaks their square edges up into chips
    chans.append(gauss_blur(specks, 18.0))
    write_png(sharp, np.dstack(chans))
    write_png(haze, np.dstack(hazes))
    return sharp, haze, tabp


# -------------------------------------------------------------------- geometry

def svg_to_xy(x, y):
    return ((x - 50) * S, (50 - y) * S)


def link(obj, coll=None):
    (coll or bpy.context.scene.collection).objects.link(obj)
    return obj


def prism(name, outline_svg, z0, z1):
    bm = bmesh.new()
    pts = [svg_to_xy(*p) for p in outline_svg]
    # make the outline counter-clockwise seen from +Z
    area = sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(pts, pts[1:] + pts[:1]))
    if area < 0:
        pts.reverse()
    top = [bm.verts.new((x, y, z1)) for x, y in pts]
    bot = [bm.verts.new((x, y, z0)) for x, y in pts]
    bm.faces.new(top)
    bm.faces.new(list(reversed(bot)))
    k = len(pts)
    for i in range(k):
        j = (i + 1) % k
        bm.faces.new((bot[i], bot[j], top[j], top[i]))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    return bpy.data.objects.new(name, me)


def cylinder(name, r, z0, z1, verts=96, x=0.0, y=0.0):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=verts, radius1=r, radius2=r, depth=z1 - z0)
    bmesh.ops.translate(bm, verts=bm.verts, vec=(x, y, (z0 + z1) / 2))
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    return bpy.data.objects.new(name, me)


def apply_modifiers(obj):
    dg = bpy.context.evaluated_depsgraph_get()
    ev = obj.evaluated_get(dg)
    me = bpy.data.meshes.new_from_object(ev, preserve_all_data_layers=True, depsgraph=dg)
    old = obj.data
    obj.modifiers.clear()
    obj.data = me
    bpy.data.meshes.remove(old)


def boolean_diff(obj, cutters):
    for c in cutters:
        link(c)
        m = obj.modifiers.new("cut", "BOOLEAN")
        m.operation, m.solver, m.object = "DIFFERENCE", "EXACT", c
    apply_modifiers(obj)
    obj.data.materials.clear()      # the cutters leave empty material slots behind
    for p in obj.data.polygons:
        p.material_index = 0
    for c in cutters:
        bpy.data.objects.remove(c)


def smooth_bevel(obj, width, segments=4):
    for p in obj.data.polygons:
        p.use_smooth = True
    m = obj.modifiers.new("bevel", "BEVEL")
    m.width, m.segments, m.limit_method = width, segments, "ANGLE"
    m.angle_limit = math.radians(30)
    m.harden_normals = True
    m.miter_outer = "MITER_ARC"
    m.use_clamp_overlap = True


def screw(name, x, y):
    """ISO 7380-like button-head hex-socket screw, M5: 9.5 mm head, 2.75 mm tall"""
    R, H = 0.00475, 0.00275
    prof = [(0.0, H)]
    cap_r, cap_h, skirt = 0.0046, 0.0020, 0.00075
    rs = (cap_r ** 2 + cap_h ** 2) / (2 * cap_h)
    for t in np.linspace(0, 1, 14)[1:]:
        rr = cap_r * t
        prof.append((rr, skirt + cap_h - (rs - math.sqrt(rs * rs - rr * rr))))
    prof += [(R - 0.00005, skirt - 0.0001), (R, skirt - 0.00025), (R, 0.0), (0.0, 0.0)]
    bm = bmesh.new()
    vs = [bm.verts.new((r, 0, z)) for r, z in prof]
    es = [bm.edges.new((a, b)) for a, b in zip(vs, vs[1:])]
    bmesh.ops.spin(bm, geom=vs + es, cent=(0, 0, 0), axis=(0, 0, 1), angle=2 * math.pi, steps=72,
                   use_merge=True)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-7)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    # hex socket, 3 mm across flats, 1.9 mm deep
    sock = cylinder(name + "_socket", 0.0015 / math.cos(math.pi / 6), H - 0.0019, H + 0.002, verts=6)
    sock.rotation_euler.z = math.radians(17)
    boolean_diff(ob, [sock])
    for p in ob.data.polygons:
        p.use_smooth = True
    ob.data.set_sharp_from_angle(angle=math.radians(40))
    ob.location = (x, y, -CB_DEPTH)
    return ob


def inset_polygon(poly, d):
    """a convex polygon (SVG units, y down, either winding) moved inward by d:
    each edge offset along its inward normal, neighbours intersected"""
    n = len(poly)
    cx = sum(p[0] for p in poly) / n
    cy = sum(p[1] for p in poly) / n
    lines = []
    for i in range(n):
        (ax, ay), (bx, by) = poly[i], poly[(i + 1) % n]
        ex, ey = bx - ax, by - ay
        ln = math.hypot(ex, ey)
        nx, ny = -ey / ln, ex / ln
        if (cx - ax) * nx + (cy - ay) * ny < 0:
            nx, ny = -nx, -ny
        lines.append(((ax + nx * d, ay + ny * d), (ex, ey)))
    out = []
    for i in range(n):
        (p1, d1), (p2, d2) = lines[i - 1], lines[i]
        den = d1[0] * d2[1] - d1[1] * d2[0]
        t = ((p2[0] - p1[0]) * d2[1] - (p2[1] - p1[1]) * d2[0]) / den
        out.append((p1[0] + d1[0] * t, p1[1] + d1[1] * t))
    return out


def build_panel(mark, mats):
    seam = mark["seam_y"]
    outline = mark["outline"]
    # outline is the chamfered octagon M14 4H86L96 14V86L86 96H14L4 86V14Z
    ys = sorted({p[1] for p in outline})
    top_ch, bot_ch = ys[1], ys[-2]       # 14 and 86
    xs = sorted({p[0] for p in outline})
    x0, x1, xi0, xi1 = xs[0], xs[-1], xs[1], xs[-2]
    y0, y1 = ys[0], ys[-1]
    g = SEAM_GAP / 2
    main_out = [(xi0, y0), (xi1, y0), (x1, top_ch), (x1, seam - g), (x0, seam - g), (x0, top_ch)]
    sx = mark["tab"]["seam_x"]
    strip_out = [(x0, seam + g), (sx - g, seam + g), (sx - g, y1), (xi0, y1), (x0, bot_ch)]
    tab_out = [(sx + g, seam + g), (x1, seam + g), (x1, bot_ch), (xi1, y1), (sx + g, y1)]

    panel = link(prism("Panel", main_out, -T, 0.0))
    cutters = [cylinder(f"cb{i}", r * S, -CB_DEPTH, 0.01, x=svg_to_xy(x, y)[0], y=svg_to_xy(x, y)[1])
               for i, (x, y, r) in enumerate(mark["rivets"])]
    cutters += [cylinder(f"hole{i}", 0.0028, -T - 0.01, 0.0, x=svg_to_xy(x, y)[0], y=svg_to_xy(x, y)[1], verts=48)
                for i, (x, y, r) in enumerate(mark["rivets"])]
    # the field inside a raised frame, set RECESS lower: the frame carries the
    # bolts (its width clears their counterbores)
    m = max(min(x - x0, x1 - x, y - y0, seam - g - y) + r for (x, y, r) in mark["rivets"]) + 1.5
    field = inset_polygon(main_out, m)
    cutters.append(prism("field", field, -RECESS, 0.01))
    boolean_diff(panel, cutters)
    smooth_bevel(panel, 0.0024)
    panel.data.materials.append(mats["paint"])

    strip = link(prism("SerialStrip", strip_out, -T, STRIP_PROUD))
    smooth_bevel(strip, 0.0022)
    strip.data.materials.append(mats["paint"])
    strip.parent = panel
    tabp = link(prism("TabPanel", tab_out, -T, STRIP_PROUD))
    smooth_bevel(tabp, 0.0022)
    tabp.data.materials.append(mats["paint"])
    tabp.parent = panel

    inset = 2.5
    back_out = [(xi0 + inset * 0.6, y0 + inset), (xi1 - inset * 0.6, y0 + inset), (x1 - inset, top_ch + inset * 0.6),
                (x1 - inset, bot_ch - inset * 0.6), (xi1 - inset * 0.6, y1 - inset), (xi0 + inset * 0.6, y1 - inset),
                (x0 + inset, bot_ch - inset * 0.6), (x0 + inset, top_ch + inset * 0.6)]
    back = link(prism("Backing", back_out, -T - 0.004, -T))
    back.data.materials.append(mats["backing"])
    back.parent = panel

    for i, (x, y, r) in enumerate(mark["rivets"]):
        sc = link(screw(f"Screw{i}", *svg_to_xy(x, y)))
        sc.data.materials.append(mats["screw"])
        sc.parent = panel
    return panel


# ------------------------------------------------------------------- materials

class Nodes:
    """terse shader-graph building: every helper returns an output socket"""

    def __init__(self, mat):
        self.nt = mat.node_tree
        self.nt.nodes.clear()

    def new(self, kind, **props):
        n = self.nt.nodes.new(kind)
        for k, v in props.items():
            setattr(n, k, v)
        return n

    def feed(self, sock, val):
        if isinstance(val, bpy.types.NodeSocket):
            self.nt.links.new(val, sock)
        elif val is not None:
            sock.default_value = val

    def math(self, op, a, b=None, c=None, clamp=False):
        n = self.new("ShaderNodeMath", operation=op, use_clamp=clamp)
        for i, v in enumerate((a, b, c)):
            self.feed(n.inputs[i], v)
        return n.outputs[0]

    def add(self, *vals):
        out = vals[0]
        for v in vals[1:]:
            out = self.math("ADD", out, v)
        return out

    def mul(self, a, b, clamp=False):
        return self.math("MULTIPLY", a, b, clamp=clamp)

    def sub(self, a, b):
        return self.math("SUBTRACT", a, b)

    def inv(self, a):
        return self.math("SUBTRACT", 1.0, a, clamp=True)

    def ramp(self, v, lo, hi, a=0.0, b=1.0, smooth=True):
        n = self.new("ShaderNodeMapRange", interpolation_type="SMOOTHSTEP" if smooth else "LINEAR", clamp=True)
        for name, val in (("Value", v), ("From Min", lo), ("From Max", hi), ("To Min", a), ("To Max", b)):
            self.feed(n.inputs[name], val)
        return n.outputs["Result"]

    def vmath(self, op, a, b=None, scale=None):
        n = self.new("ShaderNodeVectorMath", operation=op)
        self.feed(n.inputs[0], a)
        if b is not None:
            self.feed(n.inputs[1], b)
        if scale is not None:
            self.feed(n.inputs["Scale"], scale)
        return n.outputs["Value"] if op in ("DOT_PRODUCT", "DISTANCE", "LENGTH") else n.outputs["Vector"]

    def xyz(self, v):
        n = self.new("ShaderNodeSeparateXYZ")
        self.feed(n.inputs[0], v)
        return n.outputs

    def vec(self, x, y, z):
        n = self.new("ShaderNodeCombineXYZ")
        for i, v in enumerate((x, y, z)):
            self.feed(n.inputs[i], v)
        return n.outputs[0]

    def noise(self, vec, scale, detail=4.0, rough=0.5, kind="FBM", distortion=0.0):
        n = self.new("ShaderNodeTexNoise", noise_dimensions="3D", noise_type=kind)
        self.feed(n.inputs["Vector"], vec)
        self.feed(n.inputs["Scale"], scale)
        self.feed(n.inputs["Detail"], detail)
        self.feed(n.inputs["Roughness"], rough)
        self.feed(n.inputs["Distortion"], distortion)
        return n.outputs["Fac"]

    def noise_col(self, vec, scale, detail=2.0):
        n = self.new("ShaderNodeTexNoise", noise_dimensions="3D")
        self.feed(n.inputs["Vector"], vec)
        self.feed(n.inputs["Scale"], scale)
        self.feed(n.inputs["Detail"], detail)
        return n.outputs["Color"]

    def voronoi(self, vec, scale, feature="F1", rand=1.0):
        n = self.new("ShaderNodeTexVoronoi", feature=feature, voronoi_dimensions="3D")
        self.feed(n.inputs["Vector"], vec)
        self.feed(n.inputs["Scale"], scale)
        self.feed(n.inputs["Randomness"], rand)
        return n.outputs

    def rgb(self, c):
        n = self.new("ShaderNodeRGB")
        n.outputs[0].default_value = (*c, 1.0)
        return n.outputs[0]

    def mix(self, fac, a, b):
        """colour mix"""
        n = self.new("ShaderNodeMix", data_type="RGBA", clamp_factor=True)
        ins = [s for s in n.inputs if s.enabled]
        self.feed(ins[0], fac)
        self.feed(ins[1], a if not isinstance(a, tuple) else (*a, 1.0))
        self.feed(ins[2], b if not isinstance(b, tuple) else (*b, 1.0))
        return next(s for s in n.outputs if s.enabled)

    def mixf(self, fac, a, b):
        return self.add(a, self.mul(fac, self.sub(b, a)))

    def scale_col(self, c, f):
        n = self.new("ShaderNodeMix", data_type="RGBA", blend_type="MULTIPLY", clamp_factor=True)
        ins = [s for s in n.inputs if s.enabled]
        ins[0].default_value = 1.0
        self.feed(ins[1], c)
        g = self.new("ShaderNodeCombineColor")
        for i in range(3):
            self.feed(g.inputs[i], f)
        self.feed(ins[2], g.outputs[0])
        return next(s for s in n.outputs if s.enabled)


def paint_material(name, mark, panel_ref, stencil_img, haze_img, tab_img, kind="panel"):
    """kind: 'panel' (plates, with the stencil), 'screw' (fastener heads)"""
    mat = bpy.data.materials.new(name)
    try:
        mat.use_nodes = True
    except Exception:
        pass
    N = Nodes(mat)
    out = N.new("ShaderNodeOutputMaterial")
    bsdf = N.new("ShaderNodeBsdfPrincipled")
    N.nt.links.new(bsdf.outputs[0], out.inputs["Surface"])

    tc = N.new("ShaderNodeTexCoord")
    tc.object = panel_ref
    P = N.vmath("SCALE", tc.outputs["Object"], scale=1000.0)          # millimetres, panel space
    px, py, pz = N.xyz(P)
    geo = N.new("ShaderNodeNewGeometry")
    nobj = N.new("ShaderNodeVectorTransform", vector_type="NORMAL", convert_from="WORLD", convert_to="OBJECT")
    N.feed(nobj.inputs[0], geo.outputs["Normal"])
    front = N.ramp(N.xyz(nobj.outputs[0])[2], 0.75, 0.95)

    # ---- edge mask from the bevel node: where the rounded normal departs from the real one
    bev = N.new("ShaderNodeBevel", samples=8)
    bev.inputs["Radius"].default_value = 0.006
    edge_raw = N.vmath("DISTANCE", bev.outputs["Normal"], geo.outputs["Normal"])
    edge = N.ramp(edge_raw, 0.01, 0.32, smooth=False)

    # ---- noise fields
    warp = N.vmath("ADD", P, N.vmath("SCALE", N.vmath("SUBTRACT", N.noise_col(P, 0.08, 3), (0.5, 0.5, 0.5)),
                                     scale=6.0))
    big = N.noise(warp, 0.035, 6, 0.6)          # ~3 cm blotches
    mid = N.noise(warp, 0.16, 10, 0.68)         # ~6 mm, rough
    fine = N.noise(P, 1.3, 6, 0.7)              # sub-millimetre
    mottle = N.noise(P, 0.012, 4, 0.55)         # paint shade drift

    # ---- the scanned painted steel, tiled flat over the panel's face
    suv = N.vec(N.math("DIVIDE", px, SCAN_TILE_MM), N.math("DIVIDE", py, SCAN_TILE_MM), 0.0)
    scan_nodes = {}
    for k in ("diff", "rough", "nor_gl", "ao"):
        img, mean = scan(k)
        t = N.new("ShaderNodeTexImage", image=img, interpolation="Cubic", extension="REPEAT")
        N.feed(t.inputs[0], suv)
        scan_nodes[k] = (t, mean)
    lum = N.new("ShaderNodeRGBToBW")
    N.feed(lum.inputs[0], scan_nodes["diff"][0].outputs["Color"])
    # the scan's light and shade, relative to its own mean (1.0 = average)
    scan_detail = N.math("DIVIDE", lum.outputs[0], max(scan_nodes["diff"][1], 1e-4))
    scan_rough = N.math("DIVIDE", N.xyz(scan_nodes["rough"][0].outputs["Color"])[0], max(scan_nodes["rough"][1], 1e-4))
    scan_ao = N.xyz(scan_nodes["ao"][0].outputs["Color"])[0]
    # the chipped-paint scan: where its paint is gone (its metalness), and
    # its own roughness and relief
    cuv = N.vec(N.math("DIVIDE", px, CHIPS_TILE_MM), N.math("DIVIDE", py, CHIPS_TILE_MM), 0.0)
    chip_nodes = {}
    for k in ("Color", "Roughness", "NormalGL", "Metalness"):
        t = N.new("ShaderNodeTexImage", image=chips(k), interpolation="Cubic", extension="REPEAT")
        N.feed(t.inputs[0], cuv)
        chip_nodes[k] = t
    chip_metal = N.ramp(N.xyz(chip_nodes["Metalness"].outputs["Color"])[0], 0.35, 0.65)
    chip_rough = N.xyz(chip_nodes["Roughness"].outputs["Color"])[0]
    chip_steel = N.new("ShaderNodeRGBToBW")
    N.feed(chip_steel.inputs[0], chip_nodes["Color"].outputs["Color"])

    # distance to the nearest fastener centre, in mm
    dmin = None
    for (x, y, r) in mark["rivets"]:
        cx, cy = svg_to_xy(x, y)
        d = N.vmath("DISTANCE", N.vec(px, py, 0.0), (cx * 1000, cy * 1000, 0.0))
        dmin = d if dmin is None else N.math("MINIMUM", dmin, d)

    # ---- where wear and dirt gather, in mm of panel space
    seam_mm = (50 - mark["seam_y"]) * S * 1000
    near_seam = N.ramp(N.math("ABSOLUTE", N.sub(py, seam_mm)), 10.0, 1.5)
    ax, ay = N.math("ABSOLUTE", px), N.math("ABSOLUTE", py)
    grip = N.mul(N.ramp(ax, 100.0, 136.0), N.ramp(ay, 85.0, 15.0))           # hands on the sides
    set_down = N.ramp(py, -112.0, -138.0)                                       # set down on its bottom edge
    corners = N.ramp(N.vmath("DISTANCE", N.vec(ax, ay, 0.0), (126.0, 126.0, 0.0)), 34.0, 8.0)
    handling = N.math("MINIMUM", N.add(grip, set_down, corners), 1.0)
    cluster = N.ramp(N.noise(N.vec(px, py, 17.0), 0.022, 4, 0.55), 0.42, 0.66)   # wear comes in clumps
    if kind == "panel":
        near_bolt = N.mul(N.ramp(dmin, 13.0, 5.6), front)
    else:
        near_bolt = N.mul(N.ramp(dmin, 4.75, 1.6), 0.3)
    zone = N.math("MINIMUM", N.add(N.mul(edge, 0.7), N.mul(near_bolt, 0.6), N.mul(near_seam, 0.45),
                                   N.mul(handling, 0.35)), 1.2)

    # ---- chip field: paint worn through where the zones say, in clumps, plus rare knocks
    chip = N.add(N.mul(zone, N.add(0.18, N.mul(cluster, 0.95))), N.mul(N.sub(mid, 0.5), 1.25),
                 N.mul(N.sub(fine, 0.5), 0.42), N.mul(N.sub(big, 0.5), 0.25))
    if kind == "screw":
        rr = N.ramp(dmin, 0.0, 4.75, 0.0, 1.0, smooth=False)       # 0 at the socket, 1 at the rim
        tool = N.add(N.ramp(rr, 0.6, 0.32), N.mul(N.ramp(pz, -1.2, -0.45), 0.3))
        chip = N.add(chip, N.mul(tool, 0.75))
    vor = N.voronoi(warp, 0.04)
    spot = N.mul(N.ramp(vor["Distance"], 0.16, 0.0), N.ramp(N.xyz(vor["Color"])[0], 0.9, 0.95))
    chip = N.add(chip, N.mul(spot, 0.85))

    T0 = 0.70
    gone = N.ramp(chip, T0, T0 + 0.02)                               # olive paint removed
    steel = N.ramp(N.add(chip, N.mul(edge, 0.08)), T0 + 0.05, T0 + 0.08)   # primer gone too: bare steel
    worn_thin = N.ramp(chip, T0 - 0.2, T0)                           # burnished, thinner paint near a chip

    # ---- scratches: thin contour lines of stretched noise, two directions, sparse
    scratch = 0.0
    for ang, sx, gate_seed, strength in ((24.0, 0.012, 3.1, 1.0), (-58.0, 0.02, 7.7, 0.8), (71.0, 0.03, 12.4, 0.5)):
        a = math.radians(ang)
        u = N.add(N.mul(px, math.cos(a)), N.mul(py, math.sin(a)))
        v = N.add(N.mul(px, -math.sin(a)), N.mul(py, math.cos(a)))
        n = N.noise(N.vec(N.mul(u, sx), N.mul(v, 0.55), gate_seed), 1.0, 2, 0.5)
        line = N.ramp(N.math("ABSOLUTE", N.sub(n, 0.5)), 0.006, 0.0)
        gate = N.ramp(N.noise(N.vec(px, py, gate_seed * 10), 0.02, 3, 0.5), 0.56, 0.68)
        s = N.mul(N.mul(line, gate), strength)
        scratch = s if scratch == 0.0 else N.math("MAXIMUM", scratch, s)
    deep = N.mul(scratch, N.ramp(N.noise(P, 0.05, 2), 0.55, 0.65))

    # ---- colours
    # (a heavy military vehicle's hull: a darker, cooler grey-green than the
    # mark's olive, semi-gloss, worn to steel where it is handled)
    olive = srgb_to_linear(HULL)
    olive_dry = tuple(c * f for c, f in zip(olive, (1.1, 1.1, 1.06)))
    paint = N.mix(N.ramp(mottle, 0.35, 0.7), N.rgb(olive), N.rgb(olive_dry))
    paint = N.scale_col(paint, N.add(0.82, N.mul(fine, 0.12), N.mul(big, 0.26)))
    # faded, sun-bleached patches and an occasional touched-up panel area
    fade = N.ramp(N.noise(warp, 0.008, 3, 0.5), 0.52, 0.72)
    paint = N.mix(N.mul(fade, 0.5), paint, N.rgb(tuple(c * f for c, f in zip(olive, (1.45, 1.38, 1.2)))))
    touch = N.ramp(N.noise(N.vec(px, py, 41.0), 0.009, 2, 0.5), 0.66, 0.74)
    paint = N.mix(N.mul(touch, 0.35), paint, N.rgb(tuple(c * f for c, f in zip(olive, (0.85, 0.92, 0.9)))))
    polish = N.ramp(N.noise(warp, 0.02, 4, 0.6), 0.6, 0.78)
    paint = N.mix(N.mul(worn_thin, 0.45), paint, N.scale_col(paint, 1.55))   # edges rubbed lighter
    # the scan's real paint surface over ours: its streaks, stains and blemishes,
    # as relative light and shade, and its ambient occlusion
    if USE_SCAN:
        paint = N.scale_col(paint, N.add(0.1, N.mul(N.math("MINIMUM", scan_detail, 2.4), 0.9)))
        paint = N.scale_col(paint, N.add(0.55, N.mul(scan_ao, 0.45)))
    col = paint

    stencil_a = None
    if kind == "panel":
        uv = N.vec(N.add(N.math("DIVIDE", px, 100 * S * 1000), 0.5), N.add(N.math("DIVIDE", py, 100 * S * 1000), 0.5), 0.0)
        st = N.new("ShaderNodeTexImage", image=stencil_img, interpolation="Cubic", extension="CLIP")
        hz = N.new("ShaderNodeTexImage", image=haze_img, interpolation="Linear", extension="CLIP")
        N.feed(st.inputs[0], uv)
        N.feed(hz.inputs[0], uv)
        sm, hm = N.xyz(st.outputs["Color"]), N.xyz(hz.outputs["Color"])
        rag = N.add(N.mul(N.sub(N.noise(P, 2.6, 4, 0.6), 0.5), 0.05), N.mul(N.sub(mid, 0.5), 0.03))
        # stencil wear: like the plate's, it gathers near the letters' edges and
        # bridges and in the plate's wear zones, in clumps, with whole areas intact;
        # the SVG's paint specks are kept only where that wear is
        near_letter_edge = N.ramp(hz.outputs["Alpha"], 0.97, 0.5)
        cl2 = N.ramp(N.noise(N.vec(px, py, 29.0), 0.03, 4, 0.55), 0.4, 0.66)
        swear = N.add(N.mul(near_letter_edge, N.mul(cl2, 0.75)), N.mul(zone, 0.5), N.mul(cluster, 0.25),
                      N.mul(N.sub(mid, 0.5), 1.0), N.mul(N.sub(fine, 0.5), 0.35))
        sgone = N.ramp(swear, 0.69, 0.72)
        sgone_serial = N.ramp(swear, 0.8, 0.83)          # the small serial keeps legible
        scuff = N.mul(N.ramp(swear, 0.5, 0.66), 0.45)                   # thinned, not gone
        specks = N.ramp(N.add(st.outputs["Alpha"], N.mul(N.sub(N.noise(P, 1.4, 8, 0.72), 0.5), 0.9)), 0.36, 0.42)
        specks = N.mul(specks, N.ramp(N.add(N.mul(cl2, 0.6), N.mul(near_letter_edge, 0.5), N.mul(cluster, 0.3)),
                                      0.45, 0.6))
        intact = N.mul(N.inv(scratch), front)
        keep = N.mul(N.mul(N.mul(N.inv(scuff), N.inv(N.math("MAXIMUM", gone, sgone))), intact), N.inv(specks))
        keep_s = N.mul(N.mul(N.inv(N.mul(scuff, 0.6)), N.inv(N.math("MAXIMUM", gone, sgone_serial))), intact)
        # (white and military-tech orange, not the board's sand and peach)
        sand, orange = srgb_to_linear(STENCIL_WHITE), srgb_to_linear(STENCIL_ORANGE)
        # (the letters stay solid: fresh, crisp stencil paint, worn only where
        # the plate's own paint has gone to steel at the very edges)
        solid = N.mul(N.inv(gone), front)
        a_h = N.mul(N.ramp(N.add(sm[0], rag), 0.48, 0.52), solid)
        a_o = N.mul(N.ramp(N.add(sm[1], rag), 0.48, 0.52), solid)
        a_s = N.mul(N.ramp(N.add(sm[2], N.mul(rag, 0.6)), 0.47, 0.53), solid)
        speck = N.ramp(fine, 0.45, 0.75)
        col = N.mix(N.mul(N.mul(hm[0], 0.06), speck), col, N.rgb(sand))
        col = N.mix(N.mul(N.mul(hm[1], 0.08), speck), col, N.rgb(orange))
        col = N.mix(N.mul(N.mul(hm[2], 0.03), speck), col, N.rgb(sand))
        stencil_tint = N.add(N.mul(fine, 0.12), 0.9)
        col = N.mix(a_h, col, N.scale_col(N.rgb(sand), stencil_tint))
        col = N.mix(a_o, col, N.scale_col(N.rgb(orange), stencil_tint))
        col = N.mix(a_s, col, N.scale_col(N.rgb(sand), stencil_tint))
        # the hazard tab: orange bar, black stripes over it, worn like the rest
        tb = N.new("ShaderNodeTexImage", image=tab_img, interpolation="Cubic", extension="CLIP")
        N.feed(tb.inputs[0], uv)
        tm = N.xyz(tb.outputs["Color"])
        near_tab_edge = N.mul(N.ramp(tm[2], 0.97, 0.5), N.ramp(tm[2], 0.05, 0.3))
        twear = N.add(swear, N.mul(near_tab_edge, N.mul(cl2, 0.6)))
        keep_t = N.mul(N.mul(N.inv(N.mul(N.ramp(twear, 0.5, 0.66), 0.45)), N.inv(N.math("MAXIMUM", gone, N.ramp(twear, 0.71, 0.74)))),
                       intact)
        a_t = N.mul(N.ramp(N.add(tm[0], N.mul(rag, 0.8)), 0.48, 0.52), solid)
        a_k = N.mul(N.ramp(N.add(tm[1], N.mul(rag, 0.8)), 0.48, 0.52), solid)
        col = N.mix(N.mul(N.mul(tm[2], 0.05), speck), col, N.rgb(orange))
        col = N.mix(a_t, col, N.scale_col(N.rgb(orange), stencil_tint))
        col = N.mix(a_k, col, N.scale_col(N.rgb((0.016, 0.016, 0.015)), N.add(0.8, N.mul(fine, 0.4))))
        stencil_a = N.math("MAXIMUM", N.math("MAXIMUM", N.math("MAXIMUM", a_h, a_o), a_s), a_t)

    # the scan's chips and scratches through the hull paint to bare steel
    # (not through the stencils, which stay solid)
    chip_through = chip_metal if stencil_a is None else N.mul(chip_metal, N.inv(stencil_a))
    if not USE_CHIPS:
        chip_through = 0.0
    col = N.mix(chip_through, col, N.scale_col(N.rgb((0.62, 0.61, 0.58)), N.add(0.7, N.mul(chip_steel.outputs[0], 0.6))))
    primer = (0.055, 0.058, 0.052)
    steel_col = N.scale_col(N.rgb((0.64, 0.63, 0.6)), N.add(0.85, N.mul(fine, 0.25)))
    col = N.mix(N.mul(scratch, 0.55), col, N.scale_col(col, 1.9))                 # scratch lightens
    col = N.mix(gone, col, N.rgb(primer))
    metal = N.math("MAXIMUM", N.math("MAXIMUM", steel, N.mul(deep, 0.9)), chip_through)
    col = N.mix(metal, col, steel_col)

    # grime in crevices and a faint film of dirt
    ao = N.new("ShaderNodeAmbientOcclusion", samples=12, only_local=False)
    ao.inputs["Distance"].default_value = 0.004
    crev = N.ramp(ao.outputs["AO"], 0.95, 0.35)
    grime = N.add(N.mul(crev, N.add(0.55, N.mul(mid, 0.45))), N.mul(N.ramp(big, 0.55, 0.85), 0.18))
    # dirt settles towards the bottom and runs down in faint streaks
    low = N.ramp(py, -60.0, -150.0)
    streak = N.mul(N.ramp(N.noise(N.vec(N.mul(px, 0.35), N.mul(py, 0.012), 3.3), 1.0, 4, 0.6), 0.56, 0.72),
                   N.ramp(big, 0.35, 0.65))
    # dirt packed into the seam and round the bolt heads, running down below them
    seam_grime = N.mul(N.ramp(N.math("ABSOLUTE", N.sub(py, seam_mm)), 7.0, 1.0), N.add(0.45, N.mul(mid, 0.5)))
    bolt_grime = N.mul(N.ramp(dmin, 9.0, 5.0), N.add(0.35, N.mul(mid, 0.6)))
    runs = 0.0
    wob = N.mul(N.sub(N.noise(N.vec(N.mul(py, 0.04), 7.0, 0.0), 1.0, 2), 0.5), 5.0)
    for (x, y, r) in mark["rivets"]:
        cx, cy = (v * 1000 for v in svg_to_xy(x, y))
        dx = N.math("ABSOLUTE", N.add(N.sub(px, cx), wob))
        run = N.mul(N.mul(N.ramp(dx, 4.5, 0.8), N.ramp(py, cy - 4.0, cy - 9.0)), N.ramp(py, cy - 9.0, cy - 60.0, 1.0, 0.0))
        runs = run if runs == 0.0 else N.math("MAXIMUM", runs, run)
    seam_runs = N.mul(N.mul(N.ramp(N.noise(N.vec(N.mul(px, 0.3), N.mul(py, 0.02), 8.8), 1.0, 3, 0.6), 0.55, 0.68),
                            N.ramp(py, seam_mm - 1.0, seam_mm - 4.0)), N.ramp(py, seam_mm - 4.0, seam_mm - 40.0, 1.0, 0.0))
    grime = N.add(grime, N.mul(low, N.add(0.12, N.mul(mid, 0.25))), N.mul(streak, 0.25), N.mul(seam_grime, 0.8),
                  N.mul(bolt_grime, 0.6), N.mul(runs, N.add(0.2, N.mul(mid, 0.4))), N.mul(seam_runs, 0.35))
    grime = N.math("MINIMUM", grime, 1.0)
    col = N.mix(N.mul(grime, 0.75), col, N.rgb((0.022, 0.02, 0.016)))
    dust = N.add(N.mul(N.ramp(N.noise(warp, 0.03, 5, 0.6), 0.5, 0.8), 0.22), N.mul(N.ramp(fine, 0.72, 0.8), 0.35))
    col = N.mix(N.mul(dust, N.inv(metal)), col, N.rgb((0.15, 0.14, 0.12)))

    # ---- roughness
    rough_paint = N.add(0.46, N.mul(N.sub(big, 0.5), 0.4), N.mul(N.sub(mid, 0.5), 0.16), N.mul(N.sub(fine, 0.5), 0.08),
                        N.mul(polish, -0.22), N.mul(touch, -0.12), N.mul(handling, -0.16))
    # (the scan's roughness, around our own level)
    if USE_SCAN:
        rough_paint = N.mixf(0.6, rough_paint, N.math("MINIMUM", N.mul(rough_paint, scan_rough), 1.0))
    rough = N.mixf(N.mul(worn_thin, 0.6), rough_paint, 0.38)
    if stencil_a is not None:
        rough = N.mixf(stencil_a, rough, N.add(0.5, N.mul(fine, 0.1)))
    rough = N.mixf(N.mul(scratch, 0.5), rough, 0.32)
    rough = N.mixf(gone, rough, 0.62)
    rough = N.mixf(metal, rough, N.add(0.28, N.mul(mid, 0.18)))
    if USE_CHIPS:
        rough = N.mixf(N.mul(chip_through, 0.8), rough, N.add(0.2, N.mul(chip_rough, 0.4)))
        rough = N.mixf(0.35, rough, N.add(0.25, N.mul(chip_rough, 0.6)))
    rough = N.mixf(N.mul(grime, 0.7), rough, 0.85)
    rough = N.mixf(dust, rough, 0.9)

    # ---- height (mm): paint layers, stencil film, scratches, orange peel, waviness
    h = N.add(N.mul(N.inv(gone), 0.12), N.mul(N.inv(steel), 0.04), N.mul(N.sub(N.noise(P, 2.2, 3), 0.5), 0.004),
              N.mul(N.sub(N.noise(P, 0.02, 3), 0.5), 0.18), N.mul(deep, -0.02), N.mul(scratch, -0.008))
    if stencil_a is not None:
        h = N.add(h, N.mul(stencil_a, 0.06))
    bump = N.new("ShaderNodeBump")
    bump.inputs["Distance"].default_value = 0.001
    bump.inputs["Strength"].default_value = 1.0
    N.feed(bump.inputs["Height"], h)
    # the scan's surface relief under our own height detail
    nmap = N.new("ShaderNodeNormalMap")
    nmap.inputs["Strength"].default_value = 1.0
    N.feed(nmap.inputs["Color"], scan_nodes["nor_gl"][0].outputs["Color"])
    if USE_SCAN:
        N.nt.links.new(nmap.outputs["Normal"], bump.inputs["Normal"])
    elif USE_CHIPS:
        cmap = N.new("ShaderNodeNormalMap")
        cmap.inputs["Strength"].default_value = 0.8
        N.feed(cmap.inputs["Color"], chip_nodes["NormalGL"].outputs["Color"])
        N.nt.links.new(cmap.outputs["Normal"], bump.inputs["Normal"])

    N.feed(bsdf.inputs["Base Color"], col)
    debug = os.environ.get("H1_DEBUG")       # e.g. H1_DEBUG=edge shows a mask flat
    if debug:
        em = N.new("ShaderNodeEmission")
        N.feed(em.inputs["Color"], N.scale_col(N.rgb((1, 1, 1)), locals()[debug]))
        N.nt.links.new(em.outputs[0], out.inputs["Surface"])
    N.feed(bsdf.inputs["Metallic"], metal)
    N.feed(bsdf.inputs["Roughness"], rough)
    N.feed(bsdf.inputs["Normal"], bump.outputs["Normal"])
    return mat


def simple_material(name, color, rough, metallic=0.0, noise_amt=0.0):
    mat = bpy.data.materials.new(name)
    try:
        mat.use_nodes = True
    except Exception:
        pass
    N = Nodes(mat)
    out = N.new("ShaderNodeOutputMaterial")
    bsdf = N.new("ShaderNodeBsdfPrincipled")
    N.nt.links.new(bsdf.outputs[0], out.inputs["Surface"])
    c = N.rgb(color)
    if noise_amt:
        tc = N.new("ShaderNodeTexCoord")
        n = N.noise(tc.outputs["Object"], 3.0, 6, 0.6)
        c = N.scale_col(c, N.add(1 - noise_amt, N.mul(n, 2 * noise_amt)))
        N.feed(bsdf.inputs["Roughness"], N.add(rough - 0.08, N.mul(n, 0.16)))
    else:
        bsdf.inputs["Roughness"].default_value = rough
    N.feed(bsdf.inputs["Base Color"], c)
    bsdf.inputs["Metallic"].default_value = metallic
    return mat


# ------------------------------------------------------------- studio and rigs

def make_world():
    w = bpy.data.worlds.new("Studio")
    try:
        w.use_nodes = True
    except Exception:
        pass
    N = Nodes(w)
    out = N.new("ShaderNodeOutputWorld")
    bg = N.new("ShaderNodeBackground")
    N.nt.links.new(bg.outputs[0], out.inputs[0])
    # a photographed studio (studio_small_08) for real light and reflections;
    # the camera never sees it (render.py makes camera rays see the backdrop or alpha)
    env = N.new("ShaderNodeTexEnvironment")
    env.image = bpy.data.images.load(str(fetch(HDRI_URL)))
    N.nt.links.new(env.outputs["Color"], bg.inputs["Color"])
    lp = N.new("ShaderNodeLightPath")
    strength = N.mixf(lp.outputs["Is Camera Ray"], HDRI_STRENGTH, 0.0)
    N.feed(bg.inputs["Strength"], strength)
    w["dome_node"] = bg.name
    return w


def sweep_backdrop(coll, mat):
    """a curved floor-to-wall sweep, 4 m wide"""
    bm = bmesh.new()
    prof = []
    for t in np.linspace(-2.0, 0.9, 30):
        prof.append((t, 0.0))
    rad = 1.0
    for a in np.linspace(0, math.pi / 2, 24)[1:]:
        prof.append((0.9 + rad * math.sin(a), rad * (1 - math.cos(a))))
    for zz in np.linspace(1.1, 4.0, 10):
        prof.append((0.9 + rad, zz))
    rows = []
    for xw in (-2.5, 2.5):
        rows.append([bm.verts.new((xw, y, z)) for y, z in prof])
    for i in range(len(prof) - 1):
        bm.faces.new((rows[0][i], rows[0][i + 1], rows[1][i + 1], rows[1][i]))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    me = bpy.data.meshes.new("Backdrop")
    bm.to_mesh(me)
    bm.free()
    for p in me.polygons:
        p.use_smooth = True
    ob = bpy.data.objects.new("Backdrop", me)
    me.materials.append(mat)
    link(ob, coll)
    return ob


def area_light(name, coll, loc, target, power, size, size_y=None, color=(1, 1, 1), spread=180.0):
    ld = bpy.data.lights.new(name, "AREA")
    ld.energy = power * LIGHT_SCALE
    ld.color = color
    if size_y:
        ld.shape, ld.size, ld.size_y = "RECTANGLE", size, size_y
    else:
        ld.shape, ld.size = "SQUARE", size
    ld.spread = math.radians(spread)
    ob = bpy.data.objects.new(name, ld)
    ob.location = loc
    d = Vector(target) - Vector(loc)
    ob.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
    ob.visible_camera = False
    link(ob, coll)
    return ob


def camera(name, coll, loc, target, lens, sensor=36.0):
    cd = bpy.data.cameras.new(name)
    cd.lens, cd.sensor_width = lens, sensor
    cd.clip_start = 0.01
    ob = bpy.data.objects.new(name, cd)
    ob.location = loc
    d = Vector(target) - Vector(loc)
    ob.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
    link(ob, coll)
    return ob


def new_coll(name):
    c = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(c)
    return c


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    mark = read_mark()
    print("mark:", {k: (len(v) if isinstance(v, list) else v) for k, v in mark.items()})
    mark["tab"] = read_tab()
    sharp_png, haze_png, tab_png = make_masks(mark)
    st_img = bpy.data.images.load(str(sharp_png))
    hz_img = bpy.data.images.load(str(haze_png))
    tab_img = bpy.data.images.load(str(tab_png))
    for im in (st_img, hz_img, tab_img):
        im.colorspace_settings.name = "Non-Color"
        im.alpha_mode = "CHANNEL_PACKED"
        im.pack()

    # an empty that carries the panel's texture space, so every part shares it
    ref = bpy.data.objects.new("PanelSpace", None)
    link(ref)
    mats = {
        "paint": paint_material("HullPaint", mark, ref, st_img, hz_img, tab_img, "panel"),
        "screw": paint_material("FastenerPaint", mark, ref, st_img, hz_img, tab_img, "screw"),
        "backing": simple_material("BackingSteel", (0.03, 0.032, 0.03), 0.6, 0.3, noise_amt=0.2),
    }
    panel = build_panel(mark, mats)
    ref.parent = panel

    # stand the panel on the floor, leaning back a little on a hidden stand, front towards -Y
    panel.rotation_euler = (math.radians(90 - LEAN), 0, 0)
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    parts = [panel] + list(panel.children)
    zmin = min((o.matrix_world @ v.co).z for o in parts if o.type == "MESH" for v in o.evaluated_get(dg).data.vertices)
    panel.location.z = -zmin
    bpy.context.view_layer.update()

    studio = new_coll("Studio")
    floor_mat = simple_material("BackdropMat", (0.03, 0.031, 0.033), 0.75, noise_amt=0.15)
    sweep_backdrop(studio, floor_mat)

    scene.world = make_world()
    centre = panel.matrix_world @ Vector((0, 0, -T / 2))
    m3 = panel.matrix_world.to_3x3()
    normal = (m3 @ Vector((0, 0, 1))).normalized()
    up = (m3 @ Vector((0, 1, 0))).normalized()
    right = (m3 @ Vector((1, 0, 0))).normalized()
    c = tuple(centre)

    def on_panel(sx, sy, out):
        """a point in panel-aligned coordinates (metres): sx right, sy up, out along the normal"""
        return tuple(centre + right * sx + up * sy + normal * out)

    # hero: head-on and centred, the camera a touch above centre so the floor and
    # the contact shadow show; a soft key high on the left grazes the paint, a
    # weak fill on the right, strip lights behind outline the bevels, and a pool
    # of light on the backdrop behind the plate gives it depth
    hero = new_coll("Rig_hero")
    lens = 105.0
    frame_h = 0.282 / 0.66
    d = frame_h * lens / 24.0
    cam_pos = centre + (normal * math.cos(math.radians(4)) + Vector((0, 0, 1)) * math.sin(math.radians(4))) * d
    camera("Cam_hero", hero, tuple(cam_pos), tuple(centre + Vector((0, 0, -0.004))), lens)
    area_light("Key", hero, on_panel(-1.0, 0.6, 0.16), on_panel(-0.04, 0.04, 0), 150, 0.6, spread=35)
    area_light("FrontSoft", hero, on_panel(-0.2, 0.35, 1.6), c, 40, 1.2, spread=60)
    area_light("Fill", hero, on_panel(1.1, -0.15, 1.3), c, 22, 1.5, spread=90)
    area_light("TopRim", hero, on_panel(0.0, 0.62, -0.18), on_panel(0, 0.13, 0), 110, 1.0, 0.12, spread=40)
    area_light("RimL", hero, on_panel(-0.62, 0.05, -0.22), on_panel(-0.14, 0.0, 0), 90, 0.12, 0.9, spread=40)
    area_light("RimR", hero, on_panel(0.62, 0.05, -0.22), on_panel(0.14, 0.0, 0), 90, 0.12, 0.9,
               color=(1.0, 0.95, 0.88), spread=40)
    bd = bpy.data.lights.new("BackdropPool", "SPOT")
    bd.energy, bd.spot_size, bd.spot_blend, bd.shadow_soft_size = 3000 * LIGHT_SCALE, math.radians(60), 1.0, 0.4
    bd.color = (0.85, 0.9, 1.0)
    bdo = bpy.data.objects.new("BackdropPool", bd)
    bdo.location = on_panel(0, 0.02, -0.25)
    wall = Vector((centre.x, 1.9, 0.75))
    bdo.rotation_euler = (wall - Vector(bdo.location)).to_track_quat("-Z", "Y").to_euler()
    bdo.visible_camera = False
    link(bdo, hero)

    # front: straight on, even light, for the transparent renders
    front = new_coll("Rig_front")
    lens = 200.0
    span = 0.282 / 0.80
    camera("Cam_front", front, on_panel(0, 0, span * lens / 36.0), c, lens)
    area_light("FrontKey", front, on_panel(-0.9, 0.9, 1.3), c, 220, 1.4, spread=80)
    area_light("FrontFill", front, on_panel(0.9, -0.2, 1.4), c, 90, 1.4, spread=80)
    area_light("FrontTop", front, on_panel(0.0, 1.0, 0.25), c, 60, 1.4, 0.4, spread=60)
    area_light("FrontLow", front, on_panel(0.0, -1.0, 0.6), c, 22, 1.4, 0.4, spread=60)

    # icon: as front, the panel filling about 85 %
    icon = new_coll("Rig_icon")
    span_i = 0.282 / 0.85
    camera("Cam_icon", icon, on_panel(0, 0, span_i * lens / 36.0), c, lens)

    # detail: a near-frontal macro of the lower-left fastener, the H's foot, the
    # seam and the start of the serial, with low raking light from the left
    detail = new_coll("Rig_detail")
    fx, fy = svg_to_xy(*mark["rivets"][2][:2])
    tgt = on_panel(fx + 0.030, fy - 0.016, 0)
    cdir = (normal * 0.93 + right * -0.30 + up * 0.18).normalized()
    camera("Cam_detail", detail, tuple(Vector(tgt) + cdir * 0.42), tgt, 100)
    focus = bpy.data.objects.new("Focus_detail", None)
    focus.location = on_panel(fx + 0.012, fy, 0)
    link(focus, detail)
    area_light("DetailKey", detail, on_panel(-0.75, 0.35, 0.28), tgt, 140, 0.5, spread=60)
    area_light("DetailFill", detail, on_panel(0.8, -0.1, 1.0), tgt, 22, 1.2, spread=90)
    area_light("DetailTop", detail, on_panel(0.1, 0.6, -0.12), tgt, 60, 0.9, 0.12, spread=40)

    # the hero's key, fill and rims light only the plate, so the floor stays dark;
    # the floor gets its own soft overhead light, which also casts the contact shadow
    lit = bpy.data.collections.new("PlateOnly")
    for o in parts:
        lit.objects.link(o)
    for name in ("Key", "FrontSoft", "Fill", "TopRim", "RimL", "RimR"):
        bpy.data.objects[name].light_linking.receiver_collection = lit
    fl = area_light("FloorSoft", hero, (centre.x, centre.y - 0.1, 1.2), (centre.x, centre.y - 0.05, 0), 60, 1.0, spread=60)
    fl.light_linking.receiver_collection = studio

    for coll in (hero, front, icon, detail):
        coll.hide_render = True

    # render defaults; render.py sets the rest per shot
    scene.render.engine = "CYCLES"
    scene.cycles.device = "GPU"
    scene.cycles.use_denoising = True
    scene.cycles.use_adaptive_sampling = True
    scene.cycles.adaptive_threshold = 0.008
    scene.cycles.samples = 256
    scene.cycles.max_bounces = 8
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "8"
    try:
        scene.view_settings.view_transform = "Khronos PBR Neutral"
    except TypeError:
        scene.view_settings.view_transform = "AgX"
    scene["panel_centre"] = list(centre)
    bpy.ops.wm.save_as_mainfile(filepath=str(BLEND), compress=True)
    print("saved", BLEND)


if __name__ == "__main__":
    main()
