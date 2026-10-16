#!/usr/bin/env python3
"""The GPU machine code a Metal function compiles to on this Mac, disassembled.

Apple ships no disassembler for its GPUs. Dougall Johnson's applegpu
(github.com/dougallj/applegpu, the Asahi Linux project's reference) decodes
the M1 family's (G13) instructions. This tool compiles a function on this
Mac's GPU, specialised with function constants if given, harvests the
machine code from a binary archive (port/macos/tools/gpu_isa_harvest.m:
MTLBinaryArchive addRenderPipelineFunctions, serialised), extracts it with
applegpu's archive extractor and disassembles it, then counts the
instructions by kind, the registers and any stack (spill) use.

The machine code is this Mac's: on an M1, M1 Pro or M1 Max (applegpu_g13*)
the listing is exact; on later GPUs the decoding is not known.

    python3 tools/gpu_isa.py shader.metal --entry xgpu_fragment
    python3 tools/gpu_isa.py template.metal --entry f --constants 5,0,3   # constants 0, 1, 2
    python3 tools/gpu_isa.py template.metal --entry f -c 4=1 -c 9=0x20    # by index
    python3 tools/gpu_isa.py template.metal --entry f --constants 5,0,3 --literal
                                     # the constants written into the source as literals
    python3 tools/gpu_isa.py shader.metal --entry f --math safe   # as MTLMathModeSafe compiles it
    python3 tools/gpu_isa.py lib.metallib --entry f --vertex v --vertex-source v.metal
    python3 tools/gpu_isa.py ... --counts           # the counts alone
    python3 tools/gpu_isa.py ... --json out.json    # the listing and counts as JSON

A fragment function needs a vertex function for its pipeline: --vertex
(with --vertex-source if it is in another file), or, for a .metal source,
one made from the fragment's [[stage_in]] structure. applegpu is fetched
into ~/.cache/gpu_isa (GPU_ISA_CACHE) at its pinned commit the first time,
and its extractor and the harvester built there; nothing is vendored.
"""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HARVEST_SOURCE = ROOT / "port" / "macos" / "tools" / "gpu_isa_harvest.m"
APPLEGPU_URL = "https://github.com/dougallj/applegpu.git"
APPLEGPU_COMMIT = "28dd804"
CACHE = Path(os.environ.get("GPU_ISA_CACHE", Path.home() / ".cache" / "gpu_isa"))

# the instructions' kinds, by mnemonic (applegpu's names)
KINDS = [
    ("select", re.compile(r"^(f|i)cmpsel$")),
    ("compare", re.compile(r"^(f|i)cmp|^(set|test)")),
    ("float", re.compile(r"^(fadd|fmul|fmadd|fsub|fmin|fmax|fround|floor|ceil|trunc|frcp|rcp|rsqrt|sqrt|log2|exp2|"
                         r"sin|cos|dfdx|dfdy|fsat)")),
    ("integer", re.compile(r"^(iadd|isub|imul|imadd|imad|ishl|ishr|ushr|asr|bfi|bfeil|extr|shlhi|shrhi|and|or|xor|"
                           r"not|bitop|popcount|bitrev|ffs|clz|ctz)")),
    ("move", re.compile(r"^(mov|movi|convert|get_sr|get_sr_barrier|unpack|pack)")),
    ("texture", re.compile(r"^(texture|image|sample)")),
    ("varying", re.compile(r"^(iter|ldcf)")),
    ("stack", re.compile(r"^stack")),
    ("memory", re.compile(r"^(device|threadgroup|uniform_store|local)")),
    ("uniform", re.compile(r"^uniform")),
    ("control", re.compile(r"^(if_|else_|while_|pop_exec|jmp|call|ret|stop|trap|break|jmp_exec)")),
    ("discard", re.compile(r"^(zs_emit|discard)")),
    ("output", re.compile(r"^(st_tile|ld_tile|st_var|emit)")),
    ("wait", re.compile(r"^(wait|barrier)")),
]
LINE = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f]+)\s+(\S+)\s*(.*)$")


def run(command, **kwargs):
    return subprocess.run(command, check=True, **kwargs)


def tools() -> tuple[Path, Path, Path]:
    """applegpu (fetched), its archive extractor and the harvester (built)"""
    applegpu = CACHE / "applegpu"
    if not (applegpu / "applegpu.py").exists():
        CACHE.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "-q", APPLEGPU_URL, str(applegpu)])
    head = run(["git", "-C", str(applegpu), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    if not head.startswith(APPLEGPU_COMMIT):
        run(["git", "-C", str(applegpu), "fetch", "-q", "origin"])
        run(["git", "-C", str(applegpu), "checkout", "-q", APPLEGPU_COMMIT])
    extractor = applegpu / "compiler_explorer_tools" / "metal-archive-extractor"
    if not extractor.exists():
        run(["clang++", "-std=c++11", "-O2", str(extractor) + ".cpp", "-o", str(extractor)])
    harvest = CACHE / "gpu_isa_harvest"
    if not harvest.exists() or harvest.stat().st_mtime < HARVEST_SOURCE.stat().st_mtime:
        run(["clang", "-O2", "-fobjc-arc", "-framework", "Metal", "-framework", "Foundation", str(HARVEST_SOURCE),
             "-o", str(harvest)])
    return applegpu, extractor, harvest


def literal_source(source: str, values: dict[int, str]) -> str:
    """the source with each given function constant's value written in"""
    pattern = re.compile(r"constant\s+(\w+)\s+(\w+)\s*\[\[\s*function_constant\((\d+)\)\s*\]\]\s*;")

    def replace(match):
        index = int(match.group(3))
        if index not in values:
            return match.group(0)
        value = values[index]
        kind = match.group(1)
        if kind in ("uint", "ushort", "uchar") and not value.lower().endswith("u"):
            value += "u"
        if kind == "bool":
            value = "true" if int(value, 0) else "false"
        return f"constant {kind} {match.group(2)} = {value};"

    return pattern.sub(replace, source)


def stage_in_vertex(source: str, entry: str) -> str | None:
    """a vertex function writing what the fragment function's [[stage_in]]
    structure reads (from a buffer, so that nothing is a constant)"""
    match = re.search(r"\b" + re.escape(entry) + r"\s*\(([^{]*)\)", source, re.S)
    if not match:
        return None
    stage_in = re.search(r"(\w+)\s+\w+\s*\[\[\s*stage_in\s*\]\]", match.group(1))
    if not stage_in:
        return None
    body = re.search(r"struct\s+" + stage_in.group(1) + r"\s*\{(.*?)\};", source, re.S)
    if not body:
        return None
    members = re.findall(r"(\w+)\s+(\w+)\s*(\[\[[^;]*\]\])\s*;", body.group(1))
    lines = ["#include <metal_stdlib>", "using namespace metal;", "struct gpu_isa_vertex_out", "{",
             "\tfloat4 gpu_isa_position [[position]];"]
    lines += [f"\t{kind} {name} {attributes};" for kind, name, attributes in members if "position" not in attributes]
    lines += ["};", "vertex gpu_isa_vertex_out gpu_isa_vertex(uint id [[vertex_id]], constant float4 *v [[buffer(0)]])",
              "{", "\tgpu_isa_vertex_out out;", "\tout.gpu_isa_position = v[id * 32];"]
    for index, (kind, name, attributes) in enumerate(members):
        if "position" in attributes:
            continue
        width = re.search(r"(\d)$", kind)
        swizzle = ".xyzw"[:int(width.group(1)) + 1] if width else ".x"
        lines.append(f"\tout.{name} = {kind}(v[id * 32 + {index + 1}]{swizzle});")
    lines += ["\treturn out;", "}", ""]
    return "\n".join(lines)


def harvest(path: Path, entry: str, values: dict[int, str], literal: bool, vertex: str | None,
            vertex_source: Path | None, math: str | None, work: Path) -> tuple[str, list[tuple[str, bytes]]]:
    """the GPU's name and the function's machine code: (name, code) for its prolog and main
    program, from the archive the harvester writes"""
    _, extractor, harvester = tools()
    source_path = path
    if path.suffix != ".metallib":
        source = path.read_text()
        if literal:
            source = literal_source(source, values)
            values = {}
        source_path = work / "source.metal"
        source_path.write_text(source)
        if vertex is None:
            made = stage_in_vertex(source, entry)
            if made:
                vertex_source = work / "vertex.metal"
                vertex_source.write_text(made)
                vertex = "gpu_isa_vertex"
    elif literal:
        sys.exit("gpu_isa: --literal needs a .metal source")
    command = [str(harvester), "--output", str(work / "archive.bin"), "--entry", entry]
    if vertex:
        command += ["--vertex", vertex]
    if vertex_source:
        command += ["--vertex-source", str(vertex_source)]
    for index, value in sorted(values.items()):
        command += ["--constant", f"{index}={value}"]
    if math:
        command += ["--math", math]
    command.append(str(source_path))
    device = run(command, capture_output=True, text=True).stdout.strip()
    programs = []
    for kind in ("fragment", "vertex", "compute"):
        if kind == "vertex" and vertex:
            continue
        shader = run([str(extractor), f"--extract-{kind}", "-", str(work / "archive.bin")], capture_output=True).stdout
        if not shader:
            continue
        names = run([str(extractor), "--list-shaders", "-"], input=shader, capture_output=True).stdout.decode()
        order = {"_agc.main.constant_program": 0, "_agc.main": 1}
        for line in sorted(names.splitlines(), key=lambda line: order.get(line.partition(" ")[2], 2)):
            name = line.partition(" ")[2].strip()
            if not name:
                continue
            code = run([str(extractor), "--extract-named-shader", name, "--output", "-", "-"], input=shader,
                       capture_output=True).stdout
            while len(code) > 2 and code[-2:] == b"\x08\x00":
                code = code[:-2]
            if any(code):
                label = {"_agc.main.constant_program": "prolog", "_agc.main": "main"}.get(name, name)
                programs.append((f"{kind} {label}", code))
    return device, programs


def disassemble(code: bytes) -> list[str]:
    applegpu = CACHE / "applegpu"
    sys.path.insert(0, str(applegpu))
    import disassemble as applegpu_disassemble  # noqa: E402 (fetched at run time)
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        applegpu_disassemble.disassemble(code)
    return [line for line in output.getvalue().splitlines() if line.strip()]


def kind_of(mnemonic: str) -> str:
    for kind, pattern in KINDS:
        if pattern.search(mnemonic):
            return kind
    return "other"


def counts(listing: list[str]) -> dict:
    mnemonics, kinds = Counter(), Counter()
    registers, uniforms = set(), set()
    size = 0
    for line in listing:
        match = LINE.match(line)
        if not match:
            continue
        mnemonic, operands = match.group(3), match.group(4)
        size += len(match.group(2)) // 2
        mnemonics[mnemonic] += 1
        kinds[kind_of(mnemonic)] += 1
        for name in re.findall(r"\b(r\d+)(?:[lh])?\b", operands):
            registers.add(int(name[1:]))
        for first, last in re.findall(r"\br(\d+)_r(\d+)", operands):
            registers.update(range(int(first), int(last) + 1))
        for name in re.findall(r"\bu(\d+)", operands):
            uniforms.add(int(name))
    return {
        "instructions": sum(mnemonics.values()),
        "bytes": size,
        "kinds": dict(kinds.most_common()),
        "mnemonics": dict(mnemonics.most_common()),
        "registers": (max(registers) + 1) if registers else 0,
        "uniforms": (max(uniforms) + 1) if uniforms else 0,
        "stack": kinds.get("stack", 0),
    }


def parse_values(constants: str | None, pairs: list[str]) -> dict[int, str]:
    values: dict[int, str] = {}
    if constants:
        text = Path(constants[1:]).read_text() if constants.startswith("@") else constants
        for index, value in enumerate(v.strip() for v in text.split(",") if v.strip()):
            values[index] = value
    for pair in pairs:
        index, _, value = pair.partition("=")
        values[int(index, 0)] = value
    return values


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("file", type=Path, help=".metal source or .metallib")
    parser.add_argument("--entry", required=True, help="the function")
    parser.add_argument("--constants", help="function constants 0, 1, ... in order (a,b,c; or @file)")
    parser.add_argument("-c", "--constant", action="append", default=[], metavar="INDEX=VALUE")
    parser.add_argument("--literal", action="store_true",
                        help="write the constants into the source as literals instead of specialising")
    parser.add_argument("--vertex", help="the vertex function for a fragment function's pipeline")
    parser.add_argument("--vertex-source", type=Path, help="the vertex function's source, if not the file's")
    parser.add_argument("--math", choices=("fast", "relaxed", "safe"),
                        help="a source's maths mode (MTLCompileOptions.mathMode; fast if not given)")
    parser.add_argument("--counts", action="store_true", help="print the counts alone")
    parser.add_argument("--json", type=Path, help="write the listing and counts as JSON")
    args = parser.parse_args()

    values = parse_values(args.constants, args.constant)
    with tempfile.TemporaryDirectory() as directory:
        device, programs = harvest(args.file, args.entry, values, args.literal, args.vertex, args.vertex_source,
                                   args.math, Path(directory))
    if not programs:
        print("gpu_isa: no machine code in the archive", file=sys.stderr)
        return 1
    if "M1" not in device:
        print(f"gpu_isa: {device}: applegpu decodes the M1 family's code; this listing may be wrong",
              file=sys.stderr)
    report = []
    for name, code in programs:
        listing = disassemble(code)
        summary = counts(listing)
        report.append({"program": name, "counts": summary, "listing": listing})
        print(f"; {name} ({device}): {summary['instructions']} instructions, {summary['bytes']} bytes, "
              f"{summary['registers']} registers, {summary['uniforms']} uniform registers, "
              f"{summary['stack']} stack")
        print("; kinds: " + ", ".join(f"{kind} {count}" for kind, count in summary["kinds"].items()))
        if not args.counts:
            print("\n".join(listing))
            print()
    if args.json:
        args.json.write_text(json.dumps({"device": device, "programs": report}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
