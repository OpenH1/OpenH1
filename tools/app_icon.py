#!/usr/bin/env python3
"""Makes the game's icons from its logo, port/assets/icon/openh1-mark.svg:

    python tools/app_icon.py

The logo is Tail Code's mark (openh1.com's, website/public/favicon.svg): H1
knocked out of a green plate with one clipped corner. It is drawn first as
the artwork the icons are made from:

- openh1-icon.png, the plate on the site's near-black (512x512): the Windows
  executable's icon (port/windows/openh1-icon.ico, which port/windows/halo.rc
  puts in halo.exe) and the desktop builds' window icon (openh1-icon-256.png,
  which tools/embed_assets.py embeds and sdl_platform.c gives the window);
- openh1-icon-render.png, the plate alone on transparency (1024x1024), smaller
  in its square, so that a launcher's round mask leaves its corners: the
  Android app's launcher icon;
- openh1-icon.png again: the web page's icons (port/web/site/icons), its tab's,
  the installed web app's and the home screen's; the Linux release's
  (port/assets/icon/hicolor/openh1-<size>.png, which tools/linux_package.py
  puts in share/icons/hicolor/<size>x<size>/apps/openh1.png for the desktop
  entry, port/assets/linux/openh1.desktop);
- the render on the background, in the rounded square of macOS's
  icons (port/macos/openh1-icon.icns): the macOS app's icon, which Finder and
  the Dock show (tools/macos_build.py puts it in OpenH1.app);
- the render on the background, filling an opaque square of 1024 pixels
  (port/ios/openh1-icon-1024.png), which iOS masks to its own shape: the iOS
  app's icon (tools/ios_app.py makes the app's sizes of it with actool).

The launcher icon is an adaptive one (Android 8 and later): the render as its
foreground layer, the background as its background layer, and the
render's light parts (the plate, H1 still knocked out of it) as its
monochrome (themed icon) layer. Launchers mask adaptive icons to their own shapes and show only
the middle 72 dp of the 108 dp layers, so the render fills those 72 dp, as
the picture fills the other icons, and its edges are carried out to the
layers' edges for the launchers that move the layers. Writes the layers for
every screen density into port/android/app/src/main/res (mipmap-*), and the
icon's definition (mipmap-anydpi-v26/ic_launcher.xml) and background
(drawable/ic_launcher_background.xml). Run it again when the artwork
changes. Needs Pillow, NumPy and rsvg-convert (librsvg).
"""

import shutil
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
ARTWORK = ROOT / "port/assets/icon"
MARK = ARTWORK / "openh1-mark.svg"
# openh1.com's near-black, behind the plate (its favicon's)
BACKGROUND = (0x0A, 0x0D, 0x09)
# the plate's size in each artwork's square: the favicon's in the icon; in
# the render, small enough that its corners stay inside the 66 dp circle
# launchers keep of the 72 dp they show
ICON_SCALE = 0.88
RENDER_SCALE = 0.72
ICON = ARTWORK / "openh1-icon.png"
RENDER = ARTWORK / "openh1-icon-render.png"
WINDOW_ICON = ARTWORK / "openh1-icon-256.png"
WINDOWS_ICON = ROOT / "port/windows/openh1-icon.ico"
RESOURCES = ROOT / "port/android/app/src/main/res"

# the layers' size, and the part of them a launcher shows, in dp
LAYER_DP = 108
VISIBLE_DP = 72
# pixels per dp of each density
DENSITIES = {"mdpi": 1.0, "hdpi": 1.5, "xhdpi": 2.0, "xxhdpi": 3.0, "xxxhdpi": 4.0}
WINDOWS_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
# the web page's: its tab's (32), iOS's home screen's (180) and the web app
# manifest's (192, 512)
WEB_ICONS = ROOT / "port/web/site/icons"
WEB_SIZES = [32, 180, 192, 512]
# the Linux release's, for the freedesktop icon theme's hicolor sizes
LINUX_ICONS = ARTWORK / "hicolor"
LINUX_SIZES = [16, 24, 32, 48, 64, 128, 256, 512]
# macOS's icon: Apple's grid for app icons (macOS 11 and later), a rounded
# square of 824 pixels in a canvas of 1024, its corners' radius 185.4, with a
# drop shadow below it; the .icns holds every size from it, 16 to 1024
MACOS_ICON = ROOT / "port/macos/openh1-icon.icns"
MACOS_CANVAS = 1024
MACOS_SQUARE = 824
MACOS_RADIUS = 185.4
# iOS's icon: one opaque square, which the system masks and scales
IOS_ICON = ROOT / "port/ios/openh1-icon-1024.png"
IOS_SIZE = 1024


def mark(size: int, scale: float) -> Image.Image:
    """the logo on transparency, size pixels square, scaled about its middle"""
    drawn = round(size * scale)
    png = subprocess.run(["rsvg-convert", "-w", str(drawn), "-h", str(drawn), str(MARK)],
                         capture_output=True, check=True).stdout
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / "mark.png"
        path.write_bytes(png)
        plate = Image.open(path).convert("RGBA")
        plate.load()
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    canvas.alpha_composite(plate, ((size - drawn) // 2, (size - drawn) // 2))
    return canvas


def artwork() -> None:
    """the icon and the render, from the logo"""
    icon = Image.new("RGBA", (512, 512), BACKGROUND + (255,))
    icon.alpha_composite(mark(512, ICON_SCALE))
    icon.save(ICON, optimize=True)
    mark(1024, RENDER_SCALE).save(RENDER, optimize=True)
    print(f"{ICON.relative_to(ROOT)}, {RENDER.relative_to(ROOT)}: from {MARK.relative_to(ROOT)}")


def carried_out(image: Image.Image, size: int) -> Image.Image:
    """image in the middle of a square of size, its edge pixels carried out
    to the square's edges"""
    pad = (size - image.width) // 2
    data = np.asarray(image)
    padded = np.pad(data, ((pad, size - image.height - pad), (pad, size - image.width - pad), (0, 0)), mode="edge")
    return Image.fromarray(padded, "RGBA")


def monochrome(render: Image.Image) -> Image.Image:
    """the render's light parts, white with their lightness as opacity: the
    armour without the darker visor"""
    data = np.asarray(render, dtype=np.float64)
    lightness = (0.2126 * data[..., 0] + 0.7152 * data[..., 1] + 0.0722 * data[..., 2]) / 255.0
    alpha = np.clip(data[..., 3] / 255.0 * np.clip((lightness - 0.25) / 0.5, 0.0, 1.0), 0.0, 1.0)
    out = np.zeros(data.shape, dtype=np.uint8)
    out[..., :3] = 255
    out[..., 3] = np.round(alpha * 255).astype(np.uint8)
    return Image.fromarray(out, "RGBA")


def web_icons(icon: Image.Image) -> None:
    WEB_ICONS.mkdir(parents=True, exist_ok=True)
    for size in WEB_SIZES:
        icon.resize((size, size), Image.LANCZOS).save(WEB_ICONS / f"icon-{size}.png", optimize=True)
    print(f"{WEB_ICONS.relative_to(ROOT)}: {', '.join(str(size) for size in WEB_SIZES)}")


def linux_icons(icon: Image.Image) -> None:
    LINUX_ICONS.mkdir(parents=True, exist_ok=True)
    for size in LINUX_SIZES:
        icon.resize((size, size), Image.LANCZOS).save(LINUX_ICONS / f"openh1-{size}.png", optimize=True)
    print(f"{LINUX_ICONS.relative_to(ROOT)}: {', '.join(str(size) for size in LINUX_SIZES)}")


def macos_icon(render: Image.Image, top: tuple, bottom: tuple) -> None:
    """the render over the background, filling the rounded square as the
    picture fills the other icons, with Apple's shadow"""
    scale = 4
    square = MACOS_SQUARE
    offset = (MACOS_CANVAS - square) // 2
    rows = np.linspace(0.0, 1.0, square)[:, None]
    gradient = np.array(top, dtype=np.float64) * (1.0 - rows) + np.array(bottom, dtype=np.float64) * rows
    body = np.empty((square, square, 4), dtype=np.uint8)
    body[..., :3] = np.round(gradient[:, None, :]).astype(np.uint8)
    body[..., 3] = 255
    picture = Image.fromarray(body, "RGBA")
    picture.alpha_composite(render.resize((square, square), Image.LANCZOS))
    # the shape, drawn larger and reduced for smooth edges
    mask = Image.new("L", (square * scale, square * scale), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, square * scale - 1, square * scale - 1),
                                           radius=round(MACOS_RADIUS * scale), fill=255)
    mask = mask.resize((square, square), Image.LANCZOS)
    picture.putalpha(mask)
    canvas = Image.new("RGBA", (MACOS_CANVAS, MACOS_CANVAS), (0, 0, 0, 0))
    shadow = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    shadow.paste(Image.new("RGBA", (square, square), (0, 0, 0, 128)), (offset, offset + 12), mask)
    canvas.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(14)))
    canvas.alpha_composite(picture, (offset, offset))
    MACOS_ICON.parent.mkdir(parents=True, exist_ok=True)
    if shutil.which("iconutil"):
        # macOS's own writer: every size, the small ones too (Pillow writes
        # 16 and 32 only at twice their size)
        with tempfile.TemporaryDirectory() as temporary:
            iconset = Path(temporary) / "icon.iconset"
            iconset.mkdir()
            for size in (16, 32, 128, 256, 512):
                for factor, suffix in ((1, ""), (2, "@2x")):
                    canvas.resize((size * factor, size * factor), Image.LANCZOS).save(
                        iconset / f"icon_{size}x{size}{suffix}.png")
            subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(MACOS_ICON)], check=True)
    else:
        canvas.save(MACOS_ICON)
    print(f"{MACOS_ICON.relative_to(ROOT)}: 16 to {MACOS_CANVAS}")


def ios_icon(render: Image.Image, top: tuple, bottom: tuple) -> None:
    """the render over the background, filling the square as the picture
    fills the macOS icon's rounded square"""
    rows = np.linspace(0.0, 1.0, IOS_SIZE)[:, None]
    gradient = np.array(top, dtype=np.float64) * (1.0 - rows) + np.array(bottom, dtype=np.float64) * rows
    body = np.empty((IOS_SIZE, IOS_SIZE, 4), dtype=np.uint8)
    body[..., :3] = np.round(gradient[:, None, :]).astype(np.uint8)
    body[..., 3] = 255
    picture = Image.fromarray(body, "RGBA")
    picture.alpha_composite(render.resize((IOS_SIZE, IOS_SIZE), Image.LANCZOS))
    IOS_ICON.parent.mkdir(parents=True, exist_ok=True)
    # (no alpha channel: iOS refuses app icons with one)
    picture.convert("RGB").save(IOS_ICON, optimize=True)
    print(f"{IOS_ICON.relative_to(ROOT)}: {IOS_SIZE}x{IOS_SIZE}")


def main() -> None:
    artwork()
    icon = Image.open(ICON).convert("RGBA")
    render = Image.open(RENDER).convert("RGBA")

    # the desktop's icons
    icon.resize((256, 256), Image.LANCZOS).save(WINDOW_ICON, optimize=True)
    print(f"{WINDOW_ICON.relative_to(ROOT)}: 256x256")
    icon.save(WINDOWS_ICON, sizes=[(size, size) for size in WINDOWS_SIZES])
    print(f"{WINDOWS_ICON.relative_to(ROOT)}: {', '.join(str(size) for size in WINDOWS_SIZES)}")
    web_icons(icon)
    linux_icons(icon)

    # the launcher's layers
    themed = monochrome(render)
    for density, scale in DENSITIES.items():
        size = round(LAYER_DP * scale)
        visible = round(VISIBLE_DP * scale)
        directory = RESOURCES / f"mipmap-{density}"
        directory.mkdir(parents=True, exist_ok=True)
        for name, layer in (("ic_launcher_foreground", render), ("ic_launcher_monochrome", themed)):
            carried_out(layer.resize((visible, visible), Image.LANCZOS), size).save(
                directory / f"{name}.png", optimize=True)
        print(f"{directory.relative_to(ROOT)}: {size}x{size}")

    top = bottom = BACKGROUND
    drawable = RESOURCES / "drawable"
    drawable.mkdir(parents=True, exist_ok=True)
    (drawable / "ic_launcher_background.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        "<!-- the launcher icon's background (tools/app_icon.py) -->\n"
        '<shape xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <gradient android:angle="270" android:startColor="#{:02X}{:02X}{:02X}"'
        ' android:endColor="#{:02X}{:02X}{:02X}" />\n'.format(*top, *bottom) +
        "</shape>\n", encoding="utf-8")
    adaptive = RESOURCES / "mipmap-anydpi-v26"
    adaptive.mkdir(parents=True, exist_ok=True)
    (adaptive / "ic_launcher.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        "<!-- the launcher icon (tools/app_icon.py, from port/assets/icon) -->\n"
        '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <background android:drawable="@drawable/ic_launcher_background" />\n'
        '    <foreground android:drawable="@mipmap/ic_launcher_foreground" />\n'
        '    <monochrome android:drawable="@mipmap/ic_launcher_monochrome" />\n'
        "</adaptive-icon>\n", encoding="utf-8")
    print("background #{:02X}{:02X}{:02X} to #{:02X}{:02X}{:02X}".format(*top, *bottom))
    macos_icon(render, top, bottom)
    ios_icon(render, top, bottom)


if __name__ == "__main__":
    main()
