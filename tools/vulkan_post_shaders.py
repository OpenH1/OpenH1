#!/usr/bin/env python3
"""Build Vulkan's anti-aliasing passes from the OpenGL backend's.

Run with glslangValidator and spirv-val on PATH:
    python3 tools/vulkan_post_shaders.py
The generated C (port/linux/vulkan/xgpu_vulkan_post_shaders.c) embeds the
SPIR-V of display.anti_aliasing's passes and SMAA's lookup textures,
inflated; the game needs no shader compiler and no zlib for them.
--check verifies that the checked-in file still matches its sources.

The passes are port/linux/src/xgpu_post.c's GLSL: its vertex shader, FXAA,
and the main functions of SMAA's three passes after SMAA.hlsl
(port/third_party/smaa), with the defines program_build gives it. Only the
interface changes here: the uniforms become a block (set 0, binding 1, as
the backend's other draws have their uniforms), the samplers take the
texture units program_build gives them as their bindings (set 1), the
output a location, and gl_VertexID is gl_VertexIndex. The rotate program
(ROTATE_SOURCE) is Vulkan's own: the present step's turn for a swapchain the
display turns. The shadow blur (shadow_blur_source) is display.shadow_resolution's
blur of a larger shadow map (port/linux/src/xgpu_shadow_blur.h). display.hdr's tone map and encoding are xgpu_hdr.c's GLSL
(hdr_programs), the same source tools/metal_post_shaders.py compiles for
Metal: the tone map reads the pixel it draws over as its input attachment
(set 2, binding 0: VK_KHR_dynamic_rendering_local_read), and again as each
sample's own for a multisampled target (hdr_tone_map_ms). The coordinates
stay:
xgpu_post.c's textures and targets have their first row at the top, as
Vulkan's images do, and gl_FragCoord counts rows from it in both. glslang
16.6.0 and SPIRV-Tools 2026.4 (Homebrew's) produce the checked-in words.

Beside them, CLEAR is the pixel shader of the core's clears of some of a
target's colour channels (xgpu_vulkan.c, xgpu_vulkan_clear), which OpenGL
clears through glColorMask and vkCmdClearAttachments cannot: drawn with the
vertex shader above, through the channels' write mask, it writes the
uniforms' colour.
"""

from __future__ import annotations

import argparse
import ast
from pathlib import Path
import re
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "port/linux/src/xgpu_post.c"
# display.hdr's tone map and encoding (their functions and main functions)
HDR_SOURCE = ROOT / "port/linux/src/xgpu_hdr.c"
SMAA = ROOT / "port/third_party/smaa"
OUTPUT = ROOT / "port/linux/vulkan/xgpu_vulkan_post_shaders.c"

STRINGS = re.compile(r'"(?:\\.|[^"\\])*"')
COMMENTS = re.compile(r"/\*.*?\*/", re.S)
UNIFORM = re.compile(r"^uniform vec4 (\w+);\n", re.M)
SAMPLER = re.compile(r"^uniform sampler2D (\w+);\n", re.M)
# the uniforms' block: each pass's vec4s, in this order (std140); hdr only
# in display.hdr's passes, whose block has it
UNIFORMS = ("metrics", "bounds", "hdr")
# the fragment programs, in xgpu_vulkan_post_shaders.h's order
PROGRAMS = ("fxaa", "smaa_edges", "smaa_weights", "smaa_blend", "rotate", "hdr_tone_map", "hdr_tone_map_ms",
            "hdr_encode", "shadow_blur")
# Vulkan's own program, which the OpenGL backend has no need of: the frame
# turned for a swapchain the display turns (a phone held sideways:
# xgpu_vulkan_post_rotate). Each pixel takes the texel the bounds' matrix
# (its rows xy and zw) turns it to, about the image's centre.
ROTATE_SOURCE = (
    "uniform sampler2D color_texture;\n"
    "/* 1 / width, 1 / height, width, height of the turned image */\n"
    "uniform vec4 metrics;\n"
    "/* the turn: rows of a 2x2 matrix, from the turned image's coordinates to the frame's */\n"
    "uniform vec4 bounds;\n"
    "out vec4 result;\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tvec2 centred = gl_FragCoord.xy * metrics.xy - 0.5;\n"
    "\tvec2 coordinates = vec2(dot(bounds.xy, centred), dot(bounds.zw, centred)) + 0.5;\n"
    "\tresult = vec4(texture(color_texture, coordinates).rgb, 1.0);\n"
    "}\n"
)
# the clears' pixel shader (its uniforms at the passes' binding)
CLEAR = """#version 450
layout(std140, set = 0, binding = 1) uniform ClearUniforms
{
\tvec4 color;
};
layout(location = 0) out vec4 result;
void main()
{
\tresult = color;
}
"""
# SMAA's lookup textures: their file, size and channels
TEXTURES = (("area", "area_tex.zlib", 160, 560, 2), ("search", "search_tex.zlib", 64, 16, 1))


def literal(text: str) -> str:
    """The C string literals of text (comments removed), joined."""
    return "".join(ast.literal_eval(value) for value in STRINGS.findall(COMMENTS.sub("", text)))


def declaration(source: str, name: str) -> str:
    match = re.search(r"(?:static )?const char " + name + r"\[\]\s*=\s*(.*?);\n", source, re.S)
    if not match:
        raise ValueError(f"{name} not found in {SOURCE.name}")
    return literal(match[1])


def smaa_mains(source: str) -> list[str]:
    """smaa_mains's three strings, each its adjacent literals joined."""
    match = re.search(r"static const char \*const smaa_mains\[\]\s*=\s*\{(.*?)\n\};", source, re.S)
    if not match:
        raise ValueError(f"smaa_mains not found in {SOURCE.name}")
    body = COMMENTS.sub("", match[1])
    mains = []
    for group in re.findall(r'((?:\s*"(?:\\.|[^"\\])*")+)\s*,', body):
        mains.append(literal(group))
    if len(mains) != 3:
        raise ValueError(f"smaa_mains has {len(mains)} strings, not 3")
    return mains


def smaa_prefix(source: str) -> str:
    """What program_build puts before SMAA.hlsl and a pass's main: the
    defines' text, as far as its first %s."""
    match = re.search(r'xgpu_text_append\(&text,((?:\s*"(?:\\.|[^"\\])*")+)\s*,\s*\(const char \*\)xgpu_smaa_shader',
                      source)
    if not match:
        raise ValueError(f"program_build's SMAA defines not found in {SOURCE.name}")
    text = literal(match[1])
    if not text.endswith("%s\n%s"):
        raise ValueError("program_build's SMAA format changed")
    return text[:-len("%s\n%s")]


def units(source: str) -> dict[str, int]:
    """program_build's samplers' texture units, which become bindings."""
    found = {name: int(unit) for name, unit in re.findall(r'\{ "(\w+_texture)", (\d+) \}', source)}
    if not found:
        raise ValueError(f"program_build's texture units not found in {SOURCE.name}")
    return found


def hdr_programs() -> dict[str, str]:
    """display.hdr's fragment programs (xgpu_hdr.c): the tone map, in place
    (the pixel drawn over is the pass's input attachment), and the encoding,
    each its main function after the functions they share."""
    source = HDR_SOURCE.read_text()
    functions = declaration(source, "xgpu_hdr_functions_source")
    return {name: functions + declaration(source, f"{name}_source") for name in ("tone_map", "encode")}


def multisampled(code: str) -> str:
    """The tone map of a multisampled target: each sample's own pixel (which
    makes the pass run for each sample)."""
    changed = code.replace("uniform subpassInput ", "uniform subpassInputMS ").replace(
        "subpassLoad(frame_input)", "subpassLoad(frame_input, gl_SampleID)")
    if changed.count("subpassInputMS") != 1 or "gl_SampleID" not in changed:
        raise ValueError("the tone map's input attachment changed")
    return changed


def translate(stage: str, code: str, bindings: dict[str, int]) -> str:
    found = UNIFORM.findall(code)
    unknown = set(found) - set(UNIFORMS)
    if unknown:
        raise ValueError(f"uniforms {sorted(unknown)} are not in the block")
    code = UNIFORM.sub("", code)

    def sampler(match):
        return f"layout(set = 1, binding = {bindings[match[1]]}) uniform sampler2D {match[1]};\n"

    code = SAMPLER.sub(sampler, code)
    code = code.replace("out vec4 result;\n", "layout(location = 0) out vec4 result;\n")
    code = code.replace("gl_VertexID", "gl_VertexIndex")
    block = ""
    if stage == "frag":
        block = "layout(std140, set = 0, binding = 1) uniform PostUniforms\n{\n"
        block += "".join(f"\tvec4 {name};\n" for name in UNIFORMS if name != "hdr" or "hdr" in found) + "};\n"
    return "#version 450\n" + block + code


def words_array(name: str, data: bytes, public: bool = False) -> str:
    data += b"\0" * (-len(data) % 4)
    words = struct.unpack(f"<{len(data) // 4}I", data)
    lines = [f"{'' if public else 'static '}const uint32_t {name}[] =\n{{"]
    for index in range(0, len(words), 8):
        lines.append("\t" + ", ".join(f"0x{word:08x}" for word in words[index:index + 8]) + ",")
    lines.append("};\n")
    return "\n".join(lines)


def generate(args):
    build = args.build_dir
    build.mkdir(parents=True, exist_ok=True)
    source = SOURCE.read_text()
    # (as tools/embed_assets.py gives it to the OpenGL backend: ASCII)
    hlsl = bytes(byte if byte < 0x80 else 0x20 for byte in (SMAA / "SMAA.hlsl").read_bytes()).decode("ascii")
    bindings = units(source)
    prefix = smaa_prefix(source)
    mains = smaa_mains(source)
    programs = {
        "post": ("vert", declaration(source, "vertex_source")),
        "fxaa": ("frag", declaration(source, "fxaa_source")),
    }
    for name, main in zip(PROGRAMS[1:4], mains):
        programs[name] = ("frag", prefix + hlsl + "\n" + main)
    programs["rotate"] = ("frag", ROTATE_SOURCE)
    hdr = hdr_programs()
    programs["hdr_tone_map"] = ("frag", hdr["tone_map"])
    programs["hdr_tone_map_ms"] = ("frag", multisampled(hdr["tone_map"]))
    programs["hdr_encode"] = ("frag", hdr["encode"])
    programs["shadow_blur"] = ("frag", declaration(source, "shadow_blur_source"))
    programs["clear"] = ("frag", CLEAR)
    text = [
        "/* Generated by tools/vulkan_post_shaders.py from xgpu_post.c, xgpu_hdr.c and port/third_party/smaa. "
        "Do not edit. */\n",
        '#include "xgpu_vulkan_post_shaders.h"\n',
    ]
    for name, (stage, code) in programs.items():
        path = build / f"{name}.{stage}"
        path.write_text(code if name == "clear" else translate(stage, code, bindings))
        spv = path.with_suffix(f".{stage}.spv")
        spv.unlink(missing_ok=True)
        subprocess.run([args.compiler, "-V", "--target-env", "vulkan1.1", "-o", str(spv), str(path)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        subprocess.run([args.validator, "--target-env", "vulkan1.1", str(spv)], check=True)
        text.append(words_array(f"{name}_{stage}", spv.read_bytes()))
        print(f"{name}: {spv.stat().st_size} bytes of SPIR-V")
    text.append("const struct xgpu_vulkan_post_module xgpu_vulkan_post_vertex =\n{")
    text.append("\tpost_vert, sizeof(post_vert) / 4,\n};\n")
    text.append("const struct xgpu_vulkan_post_module xgpu_vulkan_post_programs[XGPU_VULKAN_POST_PROGRAM_COUNT] =\n{")
    for name in PROGRAMS:
        text.append(f"\t{{ {name}_frag, sizeof({name}_frag) / 4 }},")
    text.append("};\n")
    text.append("const struct xgpu_vulkan_post_module xgpu_vulkan_post_clear =\n{")
    text.append("\tclear_frag, sizeof(clear_frag) / 4,\n};\n")
    for name, file, width, height, channels in TEXTURES:
        texels = zlib.decompress((SMAA / file).read_bytes())
        if len(texels) != width * height * channels:
            raise ValueError(f"{file} is {len(texels)} bytes, not {width}x{height}x{channels}")
        text.append(words_array(f"xgpu_vulkan_post_{name}_texture", texels, public=True))
    generated = "\n".join(text)
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text() != generated:
            raise SystemExit(f"{OUTPUT.relative_to(ROOT)} differs; regenerate it")
    else:
        OUTPUT.write_text(generated)
    print(f"{len(programs)} modules validated for Vulkan 1.1")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--compiler", default="glslangValidator")
    parser.add_argument("--validator", default="spirv-val")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/vulkan_post_shaders")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        generate(args)
    except subprocess.CalledProcessError as error:
        if error.stdout:
            print(error.stdout.decode())
        raise


if __name__ == "__main__":
    main()
