#!/usr/bin/env python3
"""The renderer's manifest (port/linux/src/xgpu_manifest.h): merges
recordings (debug.render_manifest) into the committed manifest, and reports
what one holds.

    python tools/render_manifest.py merge port/assets/render/manifest.txt <recording>...
    python tools/render_manifest.py merge --replace port/assets/render/manifest.txt <recording>...
    python tools/render_manifest.py stats port/assets/render/manifest.txt [<recording>...]
    python tools/render_manifest.py vertex <manifest to write>

merge adds the recordings' draws to the manifest (--replace: the manifest is
made of the recordings alone), keeps each vertex program and pixel key a
draw names, and writes the records sorted, so that a manifest made again from
the same draws is the same file. stats counts a manifest's records, and with
recordings, the draws each has that the manifest does not (a recording made
with the manifest's shaders built in should have none). vertex writes the
game's vertex shaders as the game creates them (its 67 programs' microcode and
their declarations' layouts, read from the decompilation: no maps, no play),
v and s records, which a backend's build makes every vertex function from.

Port/assets/render/README.md says how the committed manifest is made.
"""
import argparse
import re
import sys
from pathlib import Path

VERSION = 1
HEADER = f"""# The renderer's manifest: every shader and pipeline the game draws with on
# the stock maps, as the shared front end describes them (format:
# port/linux/src/xgpu_manifest.h; how it is made: port/assets/render/README.md).
# Written by tools/render_manifest.py: do not edit.
manifest {VERSION}
"""

HASH = re.compile(r"[0-9a-f]{16}")


class Manifest:
    def __init__(self):
        self.vertex_programs = {}
        self.pixels = {}
        self.draws = set()

    def read(self, path):
        version = None
        with open(path, encoding="ascii") as file:
            for number, line in enumerate(file, 1):
                line = line.rstrip("\n")
                if not line or line.startswith("#"):
                    continue
                kind, _, rest = line.partition(" ")
                if kind == "manifest":
                    version = int(rest)
                    if version != VERSION:
                        raise SystemExit(f"{path}:{number}: version {version}, not {VERSION}")
                    continue
                if version is None:
                    raise SystemExit(f"{path}:{number}: no version first")
                if kind == "v":
                    name = rest[:16]
                    if not HASH.fullmatch(name):
                        raise SystemExit(f"{path}:{number}: a vertex program's hash")
                    self.vertex_programs[name] = line
                elif kind == "p":
                    name = rest[:16]
                    if not HASH.fullmatch(name):
                        raise SystemExit(f"{path}:{number}: a pixel key's hash")
                    self.pixels[name] = line
                elif kind == "d":
                    self.draws.add(line)
                elif kind == "s":
                    pass
                else:
                    raise SystemExit(f"{path}:{number}: an unknown record")
        return self

    @staticmethod
    def draw_names(draw):
        fields = dict(part.split("=", 1) for part in draw.split(" ")[1:])
        return fields["vp"], fields["px"]

    def write(self, path):
        programs, pixels = set(), set()
        for draw in self.draws:
            program, pixel = self.draw_names(draw)
            programs.add(program)
            pixels.add(pixel)
        missing = sorted(p for p in programs if p not in self.vertex_programs)
        missing += sorted(p for p in pixels if p not in self.pixels)
        if missing:
            raise SystemExit(f"draws name records no recording has: {', '.join(missing[:5])}")
        with open(path, "w", encoding="ascii", newline="\n") as file:
            file.write(HEADER)
            for name in sorted(programs):
                file.write(self.vertex_programs[name] + "\n")
            for name in sorted(pixels):
                file.write(self.pixels[name] + "\n")
            for draw in sorted(self.draws):
                file.write(draw + "\n")

    def summary(self):
        programs = {self.draw_names(d)[0] for d in self.draws}
        pixels = {self.draw_names(d)[1] for d in self.draws}
        return (f"{len(self.draws)} draws, {len(pixels)} pixel keys, {len(programs)} vertex programs")


def merge(arguments):
    manifest = Manifest()
    target = Path(arguments.manifest)
    if target.exists() and not arguments.replace:
        manifest.read(target)
    before = len(manifest.draws)
    for recording in arguments.recordings:
        added = Manifest().read(recording)
        new = added.draws - manifest.draws
        manifest.vertex_programs.update(added.vertex_programs)
        manifest.pixels.update(added.pixels)
        manifest.draws |= added.draws
        print(f"{recording}: {len(added.draws)} draws, {len(new)} new")
    target.parent.mkdir(parents=True, exist_ok=True)
    manifest.write(target)
    print(f"{target}: {manifest.summary()} ({len(manifest.draws) - before} added)")


def stats(arguments):
    manifest = Manifest().read(arguments.manifest)
    print(f"{arguments.manifest}: {manifest.summary()}")
    for recording in arguments.recordings:
        recorded = Manifest().read(recording)
        new = recorded.draws - manifest.draws
        print(f"{recording}: {len(recorded.draws)} draws, {len(new)} not in the manifest")
        for draw in sorted(new)[:10]:
            print("    " + draw[:200])


# ---------- the vertex shaders, from the game's code

VERTEX_SOURCE = Path(__file__).resolve().parent.parent / "source/rasterizer/xbox"
# D3DVSDT_* (port/include/xdk/xdk_d3d8.h): XGPU_VERTEX_* (xgpu_backend.h)
# and bytes, as d3d8_device.c's vertex_format and vertex_type_bytes
VERTEX_TYPES = {
    0x02: (0, 0), 0x12: (1, 4), 0x22: (2, 8), 0x32: (3, 12), 0x42: (4, 16), 0x40: (5, 4),
    0x15: (6, 2), 0x25: (7, 4), 0x35: (8, 6), 0x45: (9, 8),
    0x11: (10, 2), 0x21: (11, 4), 0x31: (12, 6), 0x41: (13, 8),
    0x14: (14, 1), 0x24: (15, 2), 0x34: (16, 3), 0x44: (17, 4), 0x16: (18, 4), 0x72: (19, 12),
}
D3DVSDT_NORMPACKED3 = 0x16


def fnv_words(words):
    value = 14695981039346656037
    for word in words:
        value = ((value ^ word) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def declaration_layout(tokens):
    """d3d8_device.c's parse_declaration: the elements and the packed mask"""
    stream, offsets, elements, packed = 0, [0] * 16, [], 0
    for token in tokens:
        if token == 0xFFFFFFFF:
            break
        kind = token >> 29
        if kind == 1:
            stream = token & 0xF
        elif kind == 2:
            if token & 0x10000000:
                count = (token >> 16) & 0xF
                offsets[stream] += count if token & 0x08000000 else count * 4
            elif len(elements) < 16:
                data_type = (token >> 16) & 0xFF
                register = token & 0x1F
                vertex_format, size = VERTEX_TYPES.get(data_type, (4, 0))
                elements.append(f"{register}.{stream}.{vertex_format}.{size}.{offsets[stream]}")
                offsets[stream] += size
                if data_type == D3DVSDT_NORMPACKED3:
                    packed |= 1 << register
    return elements, packed


def vertex(arguments):
    shaders = (VERTEX_SOURCE / "rasterizer_xbox_vertex_shaders.c").read_text()
    initialize = (VERTEX_SOURCE / "rasterizer_xbox_vertex_shaders_initialize.c").read_text()
    data = (VERTEX_SOURCE / "rasterizer_xbox_vertex_shaders_data.inc").read_text()
    code = [int(word, 16) for word in re.findall(r"0x([0-9A-Fa-f]{8})", data.split("*/", 1)[1])]
    entries = [(int(offset, 16), int(size, 16)) for offset, size in
               re.findall(r"VERTEX_SHADER_ENTRY\((0x[0-9A-F]+), (0x[0-9A-F]+)\)", shaders)]
    table = initialize.split("vertex_shader_declarations[] =", 1)[1].split("};", 1)[0]
    declarations = [int(word, 16) for word in re.findall(r"0x([0-9A-Fa-f]+)", table)]
    assigned = {int(index): int(offset, 16) // 4 for index, offset in re.findall(
        r"vertex_shader_table\[(\d+)\]\.declaration = &vertex_shader_declarations\[(0x[0-9A-F]+) /", initialize)}
    if len(entries) != 67 or sorted(assigned) != list(range(67)):
        raise SystemExit("vertex: the decompilation's vertex shader tables are not as expected")
    programs, records = {}, []
    for index, (offset, size) in enumerate(entries):
        header = code[offset // 4]
        count = header >> 16
        if 4 + count * 16 != size:
            raise SystemExit(f"vertex: program {index}'s size")
        words = code[offset // 4 + 1:offset // 4 + 1 + count * 4]
        name = f"{fnv_words(words):016x}"
        programs[name] = f"v {name} {count} " + "".join(f"{word:08x}" for word in words)
        elements, packed = declaration_layout(declarations[assigned[index]:])
        records.append(f"s vp={name} pa={packed:#x} l={'/'.join(elements)}")
    with open(arguments.manifest, "w", encoding="ascii", newline="\n") as file:
        file.write(HEADER.replace("every shader and pipeline the game draws with on\n# the stock maps",
                                  "the vertex shaders the game creates,\n# from its code"))
        for name in sorted(programs):
            file.write(programs[name] + "\n")
        for record in sorted(set(records)):
            file.write(record + "\n")
    print(f"{arguments.manifest}: {len(programs)} vertex programs, {len(set(records))} vertex shaders")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("merge")
    command.add_argument("--replace", action="store_true")
    command.add_argument("manifest")
    command.add_argument("recordings", nargs="+")
    command.set_defaults(run=merge)
    command = commands.add_parser("vertex")
    command.add_argument("manifest")
    command.set_defaults(run=vertex)
    command = commands.add_parser("stats")
    command.add_argument("manifest")
    command.add_argument("recordings", nargs="*")
    command.set_defaults(run=stats)
    arguments = parser.parse_args()
    arguments.run(arguments)


if __name__ == "__main__":
    sys.exit(main())
