"""OpenH1 logo round 5: six wordmark-first concepts, all pure vector paths.

Each concept gives:
  lockup(mode)  -> (inner svg, (x, y, w, h) viewBox) of the full logo, baseline at y=0,
                   cap height CAP; mode is "color" or a single hex colour
  mark(mode, small=False) -> inner svg in a 0..100 square (no background)
Units: type set at 100 units/em, so Archivo's cap height is 68.6.
"""
import itertools
import math

import glyphs as g

TEXT = "#eef4ea"
GREEN = "#9dff3c"
HOT = "#eaffd6"
BG = "#040504"
CAP = 68.6

_ids = itertools.count(1)


def uid(p):
    return f"{p}{next(_ids)}"


def glow_filter(fid, strength=1.0, region=(-400, -400, 2400, 1200)):
    """A tight bloom plus a wide soft one, the Boot Glow look."""
    x, y, w, h = region
    s = strength
    return (f'<filter id="{fid}" filterUnits="userSpaceOnUse" x="{x}" y="{y}" width="{w}" height="{h}" color-interpolation-filters="sRGB">'
            f'<feGaussianBlur in="SourceGraphic" stdDeviation="{2.2*s:.2f}" result="a"/>'
            f'<feGaussianBlur in="SourceGraphic" stdDeviation="{9*s:.2f}" result="b"/>'
            f'<feComponentTransfer in="b" result="b2"><feFuncA type="linear" slope="0.55"/></feComponentTransfer>'
            f'<feMerge><feMergeNode in="b2"/><feMergeNode in="a"/><feMergeNode in="SourceGraphic"/></feMerge></filter>')


def colours(mode):
    if mode == "color":
        return TEXT, GREEN, True
    return mode, mode, False


def runs(text, axes, family="archivo", size=100, tracking=0.0, x=0.0, adjust=None):
    """Per-character paths: [(ch, d, x, adv)], and the run's width."""
    t = g.font(family, **axes)
    upm = t["head"].unitsPerEm
    cmap = t.getBestCmap()
    out, cx = [], x
    for i, ch in enumerate(text):
        d, adv = g.glyph_path(family, axes, ch, size, cx, 0)
        out.append((ch, d, cx, adv))
        cx += adv + tracking * size
        if adjust and i in adjust:
            cx += adjust[i] * size
        if i + 1 < len(text):
            cx += g.kern(family, axes, cmap[ord(ch)], cmap[ord(text[i + 1])]) * size / upm
    return out, cx - x - tracking * size


def ink(ch, axes, family="archivo", size=100):
    return g.ink_bounds(family, axes, ch, size)


def stem_left_of_one(axes, size=100):
    """Left edge of the 1's stem, between its foot and its flag."""
    from fontTools.pens.recordingPen import RecordingPen
    t = g.font("archivo", **axes)
    gs = t.getGlyphSet()
    rp = RecordingPen()
    gs[t.getBestCmap()[ord("1")]].draw(rp)
    xs = [pt[0] for op, args in rp.value for pt in args if 160 < pt[1] < 470 and pt[0] > 100]
    return min(xs) * size / t["head"].unitsPerEm


def poly(pts):
    return "M" + " L".join(f"{x:.2f} {y:.2f}" for x, y in pts) + " Z"


def rect(x, y, w, h):
    return poly([(x, y), (x + w, y), (x + w, y + h), (x, y + h)])


# --------------------------------------------------------------------- 1 COUNTER
class Counter:
    key, name = "counter", "COUNTER"
    tagline = "The 1 is a HUD digit."
    line = ("Wide heavy caps, OPENH set solid and the 1 drawn as a lit segment digit, the way the "
            "rifle's ammo counter draws its numbers. Reads as a premium wordmark first; the nerd detail is the digit.")
    type_note = "Archivo Expanded 900 (OFL), custom segment 1"
    AX = {"wdth": 125, "wght": 900}
    header_cap = 17

    def one(self, x0, h=CAP, s=22.0):
        """The segment 1: a sloped flag segment and two stem segments, pointed at the gap."""
        gap = s * 0.24
        fl, ft = s * 0.92, s * 0.74
        sx = x0 + fl + gap          # stem left
        mid = -h / 2
        top, bot = -h, 0
        p = s / 2
        upper = poly([(sx, top), (sx + s, top), (sx + s, mid - gap / 2 - p), (sx + s / 2, mid - gap / 2),
                      (sx, mid - gap / 2 - p)])
        lower = poly([(sx, mid + gap / 2 + p), (sx + s / 2, mid + gap / 2), (sx + s, mid + gap / 2 + p),
                      (sx + s, bot), (sx, bot)])
        fx = sx - gap
        flag = poly([(fx, top), (fx, top + ft), (fx - fl, top + ft + fl * 0.5), (fx - fl, top + fl * 0.5)])
        return [flag, upper, lower], sx + s

    def lockup(self, mode):
        txt, acc, glow = colours(mode)
        rs, w = runs("OPENH", self.AX)
        h_right = rs[-1][2] + ink("H", self.AX)[2]
        parts, right = self.one(h_right + 13.5)
        body = f'<path d="{" ".join(r[1] for r in rs)}" fill="{txt}"/>'
        if glow:
            fid = uid("g")
            body = (f'<defs>{glow_filter(fid, 0.9)}</defs>' + body +
                    f'<g filter="url(#{fid})"><path d="{" ".join(parts)}" fill="{acc}"/></g>')
        else:
            body += f'<path d="{" ".join(parts)}" fill="{acc}"/>'
        x0 = ink("O", self.AX)[0]
        return body, (x0, -CAP, right - x0, CAP)

    def mark(self, mode, small=False):
        txt, acc, glow = colours(mode)
        s = 22.0 * (1.3 if small else 1.0)
        h = 80 if small else 76
        parts, right = self.one(0, h=h, s=s)
        w = right
        tx, ty = 50 - w / 2, 50 + h / 2
        body = f'<path transform="translate({tx:.2f} {ty:.2f})" d="{" ".join(parts)}" fill="{acc}"/>'
        if glow and not small:
            fid = uid("g")
            return f'<defs>{glow_filter(fid, 0.8, (-50, -50, 200, 200))}</defs><g filter="url(#{fid})">{body}</g>'
        return body


# --------------------------------------------------------------------- 2 POWER ON
class Power:
    key, name = "power", "POWER ON"
    tagline = "The O is the power button."
    line = ("Clean, Valve-plain mixed case. The O is the power symbol, the 0-and-1 every console "
            "has on its front, glowing like a machine that has just been switched on. OpenH1 starts with "
            "an O and ends with a 1: off, then on.")
    type_note = "Archivo 700 (OFL), drawn power-symbol O"
    AX = {"wdth": 100, "wght": 700}
    header_cap = 19

    def symbol(self, cx, cy, r_out, t, half=42, bar_bottom=0.08):
        rm = r_out - t / 2
        a0, a1 = math.radians(-90 + half), math.radians(270 - half)
        x0, y0 = cx + rm * math.cos(a0), cy + rm * math.sin(a0)
        x1, y1 = cx + rm * math.cos(a1), cy + rm * math.sin(a1)
        arc = f"M{x0:.2f} {y0:.2f} A{rm:.2f} {rm:.2f} 0 1 1 {x1:.2f} {y1:.2f}"
        bar = f"M{cx:.2f} {cy - r_out - 0.5:.2f} L{cx:.2f} {cy + r_out * bar_bottom:.2f}"
        return arc, bar

    def lockup(self, mode):
        txt, acc, glow = colours(mode)
        rs, w = runs("OpenH1", self.AX)
        ox0, oy0, ox1, oy1 = ink("O", self.AX)
        cx, cy = (ox0 + ox1) / 2, -(oy0 + oy1) / 2
        r = (oy1 - oy0) / 2 + 0.2
        t = 14.2
        arc, bar = self.symbol(cx, cy, r, t)
        rest = " ".join(x[1] for x in rs[1:])
        sym = (f'<path d="{arc}" fill="none" stroke="{acc}" stroke-width="{t}"/>'
               f'<path d="{bar}" fill="none" stroke="{acc}" stroke-width="{t}"/>')
        body = f'<path d="{rest}" fill="{txt}"/>'
        if glow:
            fid = uid("g")
            body = f'<defs>{glow_filter(fid, 1.0)}</defs><g filter="url(#{fid})">{sym}</g>' + body
        else:
            body = sym + body
        right = rs[-1][2] + ink("1", self.AX)[2]
        return body, (ox0, -CAP - 1.5, right - ox0, CAP + 2.7)

    def mark(self, mode, small=False):
        txt, acc, glow = colours(mode)
        t = 13 if small else 10.5
        arc, bar = self.symbol(50, 52 if small else 53, 40 if small else 37, t, half=44 if small else 42)
        sym = (f'<path d="{arc}" fill="none" stroke="{acc}" stroke-width="{t}"/>'
               f'<path d="{bar}" fill="none" stroke="{acc}" stroke-width="{t}"/>')
        if glow and not small:
            fid = uid("g")
            return f'<defs>{glow_filter(fid, 0.8, (-50, -50, 200, 200))}</defs><g filter="url(#{fid})">{sym}</g>'
        return sym


# --------------------------------------------------------------------- 3 CONTACT
class Contact:
    key, name = "contact", "CONTACT"
    tagline = "One contact, one o'clock."
    line = ("A motion-tracker disc with a single blip at one o'clock, beside the name set in the "
            "monospaced family of the port's own HUD font. Instrument-panel calm, Teenage Engineering more than energy drink.")
    type_note = "Overpass Mono 700 (OFL), drawn tracker disc"
    AX = {"wght": 700}
    header_cap = 15

    def disc(self, mode, small=False, glow=True, fill_bg=False):
        txt, acc, gl = colours(mode)
        gl = gl and glow
        R = 50
        ring = 10 if small else 6.5
        ang = math.radians(30 - 90)
        br = 27 if small else 29
        bx, by = 50 + br * math.cos(ang), 50 + br * math.sin(ang)
        out = []
        if fill_bg:
            out.append(f'<circle cx="50" cy="50" r="{R - ring/2:.2f}" fill="#0b1608"/>')
        if not small:
            gid = uid("sw")
            # the sweep: a wedge from 12 to just past the blip, fading back
            a0, a1 = math.radians(-90 - 55), math.radians(-90 + 34)
            rr = R - ring
            p0 = (50 + rr * math.cos(a0), 50 + rr * math.sin(a0))
            p1 = (50 + rr * math.cos(a1), 50 + rr * math.sin(a1))
            out.append(f'<defs><linearGradient id="{gid}" gradientUnits="userSpaceOnUse" x1="{p0[0]:.1f}" y1="{p0[1]:.1f}" x2="{p1[0]:.1f}" y2="{p1[1]:.1f}">'
                       f'<stop offset="0" stop-color="{acc}" stop-opacity="0"/><stop offset="1" stop-color="{acc}" stop-opacity="{0.42 if gl else 0}"/></linearGradient></defs>')
            if gl:
                out.append(f'<path d="M50 50 L{p0[0]:.2f} {p0[1]:.2f} A{rr} {rr} 0 0 1 {p1[0]:.2f} {p1[1]:.2f} Z" fill="url(#{gid})"/>')
            op = 0.38 if gl else 1
            sw = 2.2 if gl else 3
            out.append(f'<circle cx="50" cy="50" r="23" fill="none" stroke="{acc}" stroke-width="{sw}" opacity="{op}"/>')
            out.append(f'<path d="M50 {ring} V{100-ring} M{ring} 50 H{100-ring}" stroke="{acc}" stroke-width="{sw*0.8:.2f}" opacity="{op*0.7 if gl else 1:.2f}"/>')
        out.append(f'<circle cx="50" cy="50" r="{R - ring/2:.2f}" fill="none" stroke="{acc}" stroke-width="{ring}"/>')
        blip = f'<circle cx="{bx:.2f}" cy="{by:.2f}" r="{12 if small else 7.5}" fill="{HOT if gl else acc}"/>'
        if gl:
            fid = uid("g")
            out.append(f'<defs>{glow_filter(fid, 0.9, (-50, -50, 200, 200))}</defs><g filter="url(#{fid})">'
                       f'<circle cx="{bx:.2f}" cy="{by:.2f}" r="{7.5}" fill="{acc}"/>{blip}</g>')
        else:
            out.append(blip)
        return "".join(out)

    def lockup(self, mode):
        txt, acc, glow = colours(mode)
        rs, w = runs("OPENH1", self.AX, family="mono", tracking=0.04)
        capm = g.metrics("mono", self.AX, 100)["cap"]
        # disc as tall as 1.5 caps, centred on the caps
        D = capm * 1.5
        sc = D / 100
        gap = capm * 0.62
        disc = f'<g transform="translate(0 {-capm/2 - D/2:.2f}) scale({sc:.4f})">{self.disc(mode)}</g>'
        tx = D + gap
        text = f'<path transform="translate({tx:.2f} 0)" d="{" ".join(r[1] for r in rs)}" fill="{txt}"/>'
        right = tx + rs[-1][2] + ink("1", self.AX, "mono")[2]
        self.cap_units = capm
        return disc + text, (0, -capm / 2 - D / 2, right, D)

    def mark(self, mode, small=False):
        return self.disc(mode, small=small, glow=not small)


# --------------------------------------------------------------------- 4 BRACKET
class Bracket:
    key, name = "bracket", "LOCK-ON"
    tagline = "The name, locked in the HUD frame."
    line = ("The site's own corner brackets close round the name like a target lock. It belongs to the page it sits on: the same frame, the same green, "
            "the same wide caps, set lighter and spaced like an instrument label.")
    type_note = "Archivo Expanded 600 (OFL), drawn brackets"
    AX = {"wdth": 125, "wght": 600}
    header_cap = 13

    def lockup(self, mode):
        txt, acc, glow = colours(mode)
        rs, w = runs("OPENH1", self.AX, tracking=0.13)
        x0 = ink("O", self.AX)[0]
        x1 = rs[-1][2] + ink("1", self.AX)[2]
        px, py = CAP * 0.95, CAP * 0.55
        L, T = x0 - px, -CAP - py
        R, B = x1 + px, py
        arm, sw = CAP * 0.50, CAP * 0.075
        o = sw / 2
        br = (f"M{L+o:.2f} {T+arm:.2f} V{T+o:.2f} H{L+arm:.2f} "
              f"M{R-arm:.2f} {T+o:.2f} H{R-o:.2f} V{T+arm:.2f} "
              f"M{R-o:.2f} {B-arm:.2f} V{B-o:.2f} H{R-arm:.2f} "
              f"M{L+arm:.2f} {B-o:.2f} H{L+o:.2f} V{B-arm:.2f}")
        dot = ""
        text = f'<path d="{" ".join(r[1] for r in rs)}" fill="{txt}"/>'
        brk = f'<path d="{br}" fill="none" stroke="{acc}" stroke-width="{sw:.2f}" stroke-linecap="butt" stroke-linejoin="miter"/>'
        if glow:
            fid = uid("g")
            body = f'<defs>{glow_filter(fid, 0.7)}</defs><g filter="url(#{fid})">{brk}{dot}</g>{text}'
        else:
            body = brk + dot + text
        return body, (L, T, R - L, B - T)

    def mark(self, mode, small=False):
        txt, acc, glow = colours(mode)
        one_col = txt
        sw = 11 if small else 7
        arm = 30 if small else 27
        m = 8 if small else 10
        o = sw / 2
        L, T, R, B = m, m, 100 - m, 100 - m
        br = (f"M{L+o} {T+arm} V{T+o} H{L+arm} M{R-arm} {T+o} H{R-o} V{T+arm} "
              f"M{R-o} {B-arm} V{B-o} H{R-arm} M{L+arm} {B-o} H{L+o} V{B-arm}")
        ax = {"wdth": 125, "wght": 900 if small else 800}
        size = 70 if small else 62
        d, adv = g.glyph_path("archivo", ax, "1", size)
        bx0, by0, bx1, by1 = g.ink_bounds("archivo", ax, "1", size)
        tx, ty = 50 - (bx0 + bx1) / 2, 50 + (by1 - by0) / 2
        one = f'<path transform="translate({tx:.2f} {ty:.2f})" d="{d}" fill="{one_col}"/>'
        brk = f'<path d="{br}" fill="none" stroke="{acc}" stroke-width="{sw}"/>'
        if glow and not small:
            fid = uid("g")
            return f'<defs>{glow_filter(fid, 0.7, (-50, -50, 200, 200))}</defs><g filter="url(#{fid})">{brk}</g>{one}'
        return brk + one


# --------------------------------------------------------------------- 5 LIGHT LEAK
class Leak:
    key, name = "leak", "LIGHT LEAK"
    tagline = "Stencilled, with the light coming through the cuts."
    line = ("Airframe stencil caps: every letter is cut where a stencil needs its bridges, and "
            "green light leaks through each cut, as if the name were sprayed on a hatch with the "
            "machine running inside. The decompile, told as an object: opened, and lit from within.")
    type_note = "Archivo Expanded 900 (OFL), cut by hand"
    AX = {"wdth": 125, "wght": 900}
    header_cap = 17

    def cuts(self, rs, axes, stem):
        """x positions of the stencil cuts for each letter."""
        xs = []
        for ch, d, x, adv in rs:
            a, _, b, _ = ink(ch, axes)
            if ch == "O":
                xs.append(x + (a + b) / 2)
            elif ch in "PE":
                xs.append(x + a + stem + 0.6)
            elif ch in "NH":
                xs.append(x + a + stem + 0.6)
            elif ch == "1":
                xs.append(x + stem_left_of_one(axes) - CAP * 0.085 / 2)
        return xs

    def lockup(self, mode, beams=True):
        txt, acc, glow = colours(mode)
        stem = 24.7
        rs, w = runs("OPENH1", self.AX, adjust={i: 0.035 for i in range(5)})
        xs = self.cuts(rs, self.AX, stem)
        gap = CAP * 0.085
        mid = uid("m")
        cutrects = "".join(f'<rect x="{x - gap/2:.2f}" y="-200" width="{gap:.2f}" height="400" fill="#000"/>' for x in xs)
        x0 = ink("O", self.AX)[0]
        x1 = rs[-1][2] + ink("1", self.AX)[2]
        defs = (f'<mask id="{mid}" maskUnits="userSpaceOnUse" x="-100" y="-200" width="{x1+300:.0f}" height="400">'
                f'<rect x="-100" y="-200" width="{x1+300:.0f}" height="400" fill="#fff"/>{cutrects}</mask>')
        letters = f'<path d="{" ".join(r[1] for r in rs)}" fill="{txt}" mask="url(#{mid})"/>'
        if glow:
            fid, lg = uid("g"), uid("lg")
            ext = CAP * 0.30 if beams else 0
            bw = gap * 0.42
            defs += (f'<linearGradient id="{lg}" x1="0" y1="0" x2="0" y2="1">'
                     f'<stop offset="0" stop-color="{acc}" stop-opacity="0"/>'
                     f'<stop offset="{ext/(CAP+2*ext):.3f}" stop-color="{acc}"/>'
                     f'<stop offset="{1-ext/(CAP+2*ext):.3f}" stop-color="{acc}"/>'
                     f'<stop offset="1" stop-color="{acc}" stop-opacity="0"/></linearGradient>'
                     + glow_filter(fid, 0.8))
            light = "".join(f'<rect x="{x - bw/2:.2f}" y="{-CAP-ext:.2f}" width="{bw:.2f}" height="{CAP+2*ext:.2f}" fill="url(#{lg})"/>' for x in xs)
            body = f'<defs>{defs}</defs><g filter="url(#{fid})">{light}</g>{letters}'
            return body, (x0, -CAP - ext * 0.6, x1 - x0, CAP + ext * 1.2)
        return f'<defs>{defs}</defs>{letters}', (x0, -CAP, x1 - x0, CAP)

    def mark(self, mode, small=False):
        txt, acc, glow = colours(mode)
        ax = {"wdth": 100, "wght": 900}
        size = 118 if small else 104
        d, adv = g.glyph_path("archivo", ax, "O", size)
        a, b0, b, b1 = g.ink_bounds("archivo", ax, "O", size)
        tx, ty = 50 - (a + b) / 2, 50 + (b1 - b0) / 2 + b0
        cx = 50
        gap = 9 if small else 7.5
        mid = uid("m")
        out = (f'<defs><mask id="{mid}" maskUnits="userSpaceOnUse" x="-20" y="-20" width="140" height="140">'
               f'<rect x="-20" y="-20" width="140" height="140" fill="#fff"/><rect x="{cx-gap/2}" y="-20" width="{gap}" height="140" fill="#000"/></mask></defs>')
        letter = f'<g mask="url(#{mid})"><path transform="translate({tx:.2f} {ty:.2f})" d="{d}" fill="{txt}"/></g>'
        if mode == "color":
            bw = gap * (0.62 if small else 0.45)
            if small:
                return out + f'<rect x="{cx-bw/2}" y="10" width="{bw}" height="80" fill="{acc}"/>' + letter
            fid, lg = uid("g"), uid("lg")
            out += (f'<defs><linearGradient id="{lg}" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{acc}" stop-opacity="0"/>'
                    f'<stop offset=".2" stop-color="{acc}"/><stop offset=".8" stop-color="{acc}"/><stop offset="1" stop-color="{acc}" stop-opacity="0"/></linearGradient>'
                    f'{glow_filter(fid, 0.8, (-50, -50, 200, 200))}</defs>')
            out += f'<g filter="url(#{fid})"><rect x="{cx-bw/2}" y="2" width="{bw}" height="96" fill="url(#{lg})"/></g>'
        return out + letter


# --------------------------------------------------------------------- 6 RISING ONE
class Rising:
    key, name = "rising", "RISING ONE"
    tagline = "The H and the 1 share a stem."
    line = ("A ligature you can own: the H's right stem keeps going up and becomes the 1, so the "
            "last two letters lock into one H1 monogram. Same palette as the current header (Open in white, "
            "H1 in green), made into a real mark that works alone as the icon.")
    type_note = "Archivo Expanded 800 (OFL), drawn H1 ligature"
    AX = {"wdth": 112, "wght": 800}
    header_cap = 17
    EXT = 0.66

    def lig(self, x0):
        """The H1 ligature at x0 (left stem's left edge). Returns (path d, right edge, top).
        The right stem runs on above the cap height and carries the 1's flag, which hangs
        clear of the H, so the H and the 1 read as two letters sharing one stem."""
        stem = 19.1
        hx0, _, hx1, _ = ink("H", self.AX)
        hw = hx1 - hx0
        ext = CAP * self.EXT
        top = -CAP - ext
        rx = x0 + hw
        sx = rx - stem
        bar_h = stem * 0.84
        bar_y = -CAP * 0.5 - bar_h / 2
        left = rect(x0, -CAP, stem, CAP)
        bar = rect(x0 + stem - 0.5, bar_y, sx - x0 - stem + 1, bar_h)
        right = rect(sx, top, stem, CAP + ext)
        # Archivo's flag: the top edge falls at about 0.5, the cut end is vertical
        fl, ft = stem * 1.12, stem * 0.86
        flag = poly([(sx + 0.5, top), (sx + 0.5, top + ft), (sx - fl, top + ft + fl * 0.6), (sx - fl, top + fl * 0.6)])
        return f"{left} {bar} {right} {flag}", rx, top

    def lockup(self, mode):
        txt, acc, glow = colours(mode)
        rs, w = runs("OPENH", self.AX)
        hx = rs[-1][2] + ink("H", self.AX)[0]
        lig, right, top = self.lig(hx)
        text = f'<path d="{" ".join(r[1] for r in rs[:-1])}" fill="{txt}"/>'
        body = text
        if glow:
            fid = uid("g")
            body = f'<defs>{glow_filter(fid, 0.85)}</defs>' + text + f'<g filter="url(#{fid})"><path d="{lig}" fill="{acc}"/></g>'
        else:
            body += f'<path d="{lig}" fill="{acc}"/>'
        x0 = ink("O", self.AX)[0]
        return body, (x0, top, right - x0, -top)

    def mark(self, mode, small=False):
        txt, acc, glow = colours(mode)
        lig, right, top = self.lig(0)
        h = -top
        sc = (84 if small else 72) / h
        w = right * sc
        tx, ty = 50 - w / 2, 50 + h * sc / 2
        body = f'<path transform="translate({tx:.2f} {ty:.2f}) scale({sc:.4f})" d="{lig}" fill="{acc}"/>'
        if glow and not small:
            fid = uid("g")
            return f'<defs>{glow_filter(fid, 0.8, (-50, -50, 200, 200))}</defs><g filter="url(#{fid})">{body}</g>'
        return body


# --------------------------------------------------------------------- 7 TAIL CODE
class Plate:
    key, name = "plate", "TAIL CODE"
    tagline = "H1 on a plate, like a squadron's tail code."
    line = ("OPEN set light and wide, then H1 knocked out of a solid green plate with one clipped "
            "corner, the way a jet carries its two-letter tail code or hardware carries its model "
            "number. The plate alone is the icon, and it is the boldest at 16 px.")
    type_note = "Archivo Expanded 300 and 900 (OFL), drawn plate"
    AXL = {"wdth": 125, "wght": 300}
    AXH = {"wdth": 125, "wght": 900}
    header_cap = 15

    def plate(self, x, size=100, pad_x=0.26, pad_y=0.24, clip=0.30):
        """The plate with H1 knocked out. Returns (plate path, H1 path, width, top, bottom)."""
        rs, w = runs("H1", self.AXH, size=size, x=0, tracking=0.02)
        cap = CAP * size / 100
        a = g.ink_bounds("archivo", self.AXH, "H", size)[0]
        b = rs[-1][2] + g.ink_bounds("archivo", self.AXH, "1", size)[2]
        px, py = cap * pad_x * 1.15, cap * pad_y
        L, R = x, x + (b - a) + 2 * px
        T, B = -cap - py, py
        c = cap * clip
        plate = poly([(L, T), (R - c, T), (R, T + c), (R, B), (L, B)])
        dx = x + px - a
        h1 = " ".join(g.glyph_path("archivo", self.AXH, ch, size, dx + cx, 0)[0] for ch, d, cx, adv in rs)
        return plate, h1, R - L, T, B

    def lockup(self, mode):
        txt, acc, glow = colours(mode)
        rs, w = runs("OPEN", self.AXL, tracking=0.06)
        x0 = ink("O", self.AXL)[0]
        right = rs[-1][2] + ink("N", self.AXL)[2]
        px = right + CAP * 0.30
        plate, h1, pw, T, B = self.plate(px)
        mid = uid("m")
        knock = (f'<mask id="{mid}" maskUnits="userSpaceOnUse" x="{px-10:.0f}" y="-200" width="{pw+20:.0f}" height="400">'
                 f'<rect x="{px-10:.0f}" y="-200" width="{pw+20:.0f}" height="400" fill="#fff"/><path d="{h1}" fill="#000"/></mask>')
        text = f'<path d="{" ".join(r[1] for r in rs)}" fill="{txt}"/>'
        pl = f'<path d="{plate}" fill="{acc}" mask="url(#{mid})"/>'
        if glow:
            fid = uid("g")
            body = f'<defs>{knock}{glow_filter(fid, 0.6)}</defs>{text}<g filter="url(#{fid})">{pl}</g>'
        else:
            body = f'<defs>{knock}</defs>{text}{pl}'
        return body, (x0, T, px + pw - x0, B - T)

    def mark(self, mode, small=False):
        txt, acc, glow = colours(mode)
        size = 100
        if small:
            keep = self.AXH
            self.AXH = {"wdth": 100, "wght": 900}
            plate, h1, pw, T, B = self.plate(0, size=size, pad_x=0.10, pad_y=0.42, clip=0.42)
            self.AXH = keep
        else:
            plate, h1, pw, T, B = self.plate(0, size=size, pad_x=0.22, pad_y=0.26, clip=0.34)
        sc = (96 if small else 84) / max(pw, B - T)
        tx, ty = 50 - pw * sc / 2, 50 - (T + B) / 2 * sc
        mid = uid("m")
        return (f'<defs><mask id="{mid}" maskUnits="userSpaceOnUse" x="-20" y="-20" width="140" height="140">'
                f'<rect x="-20" y="-20" width="140" height="140" fill="#fff"/>'
                f'<path transform="translate({tx:.2f} {ty:.2f}) scale({sc:.4f})" d="{h1}" fill="#000"/></mask></defs>'
                f'<g mask="url(#{mid})"><path transform="translate({tx:.2f} {ty:.2f}) scale({sc:.4f})" d="{plate}" fill="{acc}"/></g>')


ALL = [Plate(), Leak(), Power(), Rising(), Contact(), Bracket()]
DROPPED = [Counter()]


def svg(inner, vb, height=None, width=None, cls="", pad=0.0, style=""):
    x, y, w, h = vb
    x, y, w, h = x - pad, y - pad, w + 2 * pad, h + 2 * pad
    size = ""
    if height:
        size = f' height="{height:.2f}" width="{height * w / h:.2f}"'
    elif width:
        size = f' width="{width:.2f}" height="{width * h / w:.2f}"'
    c = f' class="{cls}"' if cls else ""
    st = f' style="{style}"' if style else ""
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{x:.2f} {y:.2f} {w:.2f} {h:.2f}"{size}{c}{st} '
            f'overflow="visible" role="img" aria-label="OpenH1">{inner}</svg>')


def lockup_svg(c, mode="color", cap_px=None, **kw):
    inner, vb = c.lockup(mode)
    cap_units = getattr(c, "cap_units", CAP)
    if cap_px:
        kw["height"] = cap_px * vb[3] / cap_units
    return svg(inner, vb, **kw)
