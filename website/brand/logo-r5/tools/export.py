"""Write each concept as standalone SVG files: lockup (colour, one-colour dark and light), mark, favicon."""
import os

import build as B
import concepts as C

OUT = os.path.join(B.R5, "svg")
os.makedirs(OUT, exist_ok=True)
for n, c in enumerate(C.ALL, 1):
    base = os.path.join(OUT, f"{n:02d}-{c.key}")
    files = {
        "-lockup.svg": C.lockup_svg(c, "color", cap_px=60),
        "-lockup-white.svg": C.lockup_svg(c, "#eef4ea", cap_px=60),
        "-lockup-black.svg": C.lockup_svg(c, "#040504", cap_px=60),
        "-mark.svg": f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" width="512" height="512">{c.mark("color")}</svg>',
        "-favicon.svg": B.favicon_svg(c),
        "-app-icon-macos.svg": B.mac_icon(c, 1024),
    }
    for suffix, svg in files.items():
        open(base + suffix, "w").write(svg)
print("wrote", len(os.listdir(OUT)), "files to", OUT)
