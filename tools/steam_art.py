#!/usr/bin/env python3
"""Renders Steam's library artwork for OpenH1's shortcut (a non-Steam game):
port/assets/steam/art.html, drawn by headless Chrome, as the macOS disk
image's background is (port/macos/dmg/render.sh), into
port/assets/steam/<variant>/<kind>.png, for OpenH1 (openh1) and OpenH1 VR
(openh1-vr):

    capsule.png   600 x 900    the library's tile (Steam's <appid>p.png)
    wide.png      920 x 430    the wide capsule (<appid>.png)
    hero.png      1920 x 620   the game's page's banner (<appid>_hero.png)
    logo.png      1280 x 360   the logo over the hero, transparent (<appid>_logo.png)
    icon.png      256 x 256    the icon (<appid>_icon.png)

    python3 tools/steam_art.py [--chrome <path>]

The PNGs are committed: the Linux release's tree has its variant's in
share/openh1/steam/ (tools/linux_package.py), the Steam Frame's package
OpenH1 VR's as steam-<kind>.png (tools/frame_package.py), and install.sh
(tools/linux_install.sh) copies them into Steam's grid folder. Needs Chrome
and Pillow.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
ART = ROOT / "port/assets/steam"
PAGE = ART / "art.html"
# each picture's size, and whether it is drawn on nothing (an alpha channel)
KINDS = {
    "capsule": (600, 900, False),
    "wide": (920, 430, False),
    "hero": (1920, 620, False),
    "logo": (1280, 360, True),
    "icon": (256, 256, True),
}
VARIANTS = {"openh1": False, "openh1-vr": True}
CHROMES = ["/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", "google-chrome", "google-chrome-stable",
           "chromium", "chromium-browser"]


def find_chrome(named):
    for candidate in ([named] if named else CHROMES):
        found = candidate if os.path.isfile(candidate) else shutil.which(candidate)
        if found:
            return found
    sys.exit("no Chrome: name it with --chrome")


def render(chrome, kind, vr, output):
    width, height, transparent = KINDS[kind]
    with tempfile.TemporaryDirectory() as scratch:
        shot = Path(scratch) / "shot.png"
        # (headless Chrome's window keeps some of its height for itself: drawn
        # taller, then cropped)
        subprocess.run([chrome, "--headless=new", "--disable-gpu", "--hide-scrollbars",
                        "--allow-file-access-from-files", "--force-device-scale-factor=1",
                        "--default-background-color=00000000", "--virtual-time-budget=3000",
                        f"--window-size={width},{height + 200}",
                        f"--screenshot={shot}", f"{PAGE.as_uri()}?kind={kind}&vr={int(vr)}"],
                       check=True, capture_output=True)
        picture = Image.open(shot).crop((0, 0, width, height))
        picture = picture.convert("RGBA" if transparent else "RGB")
        output.parent.mkdir(parents=True, exist_ok=True)
        picture.save(output, optimize=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--chrome", help="the Chrome (or Chromium) to draw with")
    args = parser.parse_args()
    chrome = find_chrome(args.chrome)
    for variant, vr in VARIANTS.items():
        for kind in KINDS:
            output = ART / variant / f"{kind}.png"
            render(chrome, kind, vr, output)
            print(f"{output.relative_to(ROOT)}  {output.stat().st_size:,} bytes", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
