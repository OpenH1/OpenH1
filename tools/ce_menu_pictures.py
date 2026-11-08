#!/usr/bin/env python3
"""Writes the PC version's pictures that tools/ce_menus.py's --pictures
reads, decoded from the bitmap tags of halopc-restored:

    python tools/ce_menu_pictures.py --tags .../halopc-restored/tags \\
        --definitions .../invader/src/tag/hek/definition --out .../pictures

Only the pictures of the frames drawn as the placeholder are written (those
port/assets/menus/NON_HANDDRAWN.md lists), and only their alpha: ce_menus.py
reads nothing else of them (where each sits in its frame). A PNG for each
frame, <tag path below ui\\>__<index>.png, as ce_menus.py names them."""

import argparse
import re
import struct
import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ce_menus import MENUS, Tags  # noqa: E402

DXT3 = 15


def dxt3_alpha(pixels: bytes, offset: int, width: int, height: int) -> bytes:
    alpha = bytearray(width * height)
    block = offset
    for y in range(0, height, 4):
        for x in range(0, width, 4):
            bits = struct.unpack_from("<Q", pixels, block)[0]
            for texel in range(16):
                alpha[(y + texel // 4) * width + x + texel % 4] = (bits >> (4 * texel) & 15) * 17
            block += 16
    return bytes(alpha)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tags", type=Path, required=True)
    parser.add_argument("--definitions", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    arguments = parser.parse_args()
    tags = Tags(arguments.tags, arguments.definitions)
    listed = re.findall(r"\| `ce/[^`]*` \| \d+x\d+ \| `([^`]*)__\d+\.png` \|",
                        (MENUS / "NON_HANDDRAWN.md").read_text())
    for relative in sorted(set(listed)):
        tag = "ui\\" + relative.replace("/", "\\")
        group = tags.load(tag, "bitmap")
        for index, frame in enumerate(group["bitmap data"]):
            width, height = frame["width"], frame["height"]
            if frame["format"] != DXT3:
                sys.exit(f"{tag}: frame {index} is of format {frame['format']}, not DXT3")
            image = Image.new("RGBA", (width, height))
            image.putalpha(Image.frombytes("L", (width, height), dxt3_alpha(
                group["processed pixel data"], frame["pixel data offset"], width, height)))
            path = arguments.out / f"{relative}__{index}.png"
            path.parent.mkdir(parents=True, exist_ok=True)
            image.save(path)
    print(f"{len(set(listed))} bitmaps' pictures in {arguments.out}")


if __name__ == "__main__":
    main()
