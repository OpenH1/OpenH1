#!/usr/bin/env python3
"""Build Vulkan's fixed VR shaders from their GLSL (xgpu_vulkan_vr_glsl.inc).

Run with glslangValidator and spirv-val on PATH:
    python3 tools/vulkan_vr_shaders.py
The generated C embeds SPIR-V; the game does not need a shader compiler.
--check verifies that the checked-in words still match their sources.

The game shaders have their own direct SPIR-V emitter (xgpu_spirv.c).
These fixed passes are written in OpenGL's GLSL, as OpenCE's OpenGL backend's
VR mode had them, and keep its equations and uniform names. Only interface
declarations, built-ins, row orientation and sRGB framebuffer operations
change here. glslang 12.0.0 and SPIRV-Tools 2023.1 produce the checked-in
words.
"""

from __future__ import annotations

import argparse
import ast
from dataclasses import dataclass
from pathlib import Path
import os
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "port/linux/vulkan/xgpu_vulkan_vr_glsl.inc"
OUTPUT = ROOT / "port/linux/vulkan/xgpu_vulkan_vr_shaders.c"

PASSES = (
    ("fovea", "vr_fovea_composite", 2),
    ("light", "vr_light_map", 1),
    ("hud", "vr_copy_hud_image", 1),
    ("scope", "vr_copy_scope", 1),
    ("visor", "vr_draw_visor", 2),
    ("rim", "vr_draw_rim", 2),
    ("feed", "vr_draw_feed", 2),
    ("cockpit", "vr_draw_cockpit_display", 2),
    ("vignette", "vr_draw_vignette", 2),
    ("tint", "vr_draw_tint", 2),
    ("depth", "vr_copy_depth", 2),
    ("scope_window", "vr_draw_scope_window", 2),
    ("copy", None, 1),
)
# These textures are drawn by the eye pass or a fixed pass into GL images
# whose row zero is their bottom. Vulkan keeps every image top-down.
FLIPPED_TEXTURES = {"fovea", "eyes", "light_map", "depth_texture", "window_texture"}
UNIFORM = re.compile(r"^uniform (\w+) (\w+)(?:\[(\d+)\])?;\n", re.M)
INTERFACE = re.compile(r"^(flat )?(in|out) (\w+) (\w+);\n", re.M)
STRINGS = re.compile(r'"(?:\\.|[^"\\])*"')
DECLARATION = re.compile(
    r"static const char (vertex_source|fragment_source)\[\]\s*=\s*"
    r'((?:(?:\s|/\*.*?\*/)|"(?:\\.|[^"\\])*")+);', re.S
)
SRGB = """
vec3 vr_decode_srgb(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)),
        greaterThan(c, vec3(0.04045)));
}
vec3 vr_encode_srgb(vec3 c)
{
    c = max(c, vec3(0.0));
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
        greaterThan(c, vec3(0.0031308)));
}
"""


@dataclass
class Uniform:
    kind: str
    name: str
    count: int
    offset: int = 0
    stride: int = 0

    @property
    def components(self):
        return {"float": 1, "int": 1, "vec2": 2, "vec3": 3, "vec4": 4}[self.kind]

    @property
    def integer(self):
        return self.kind == "int"


def sources(source: str, function: str | None) -> dict[str, str]:
    if function is None:
        return {
            "vert": """#version 450
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
""",
            "frag": """#version 450
uniform sampler2D source_texture;
out vec4 colour;
void main()
{
    colour = texture(source_texture, gl_FragCoord.xy / vec2(textureSize(source_texture, 0)));
}
""",
        }
    start = re.search(r"^static [^\n]+\b" + function + r"\(", source, re.M)
    if not start:
        raise ValueError(f"function {function} not found")
    declarations = list(DECLARATION.finditer(source, start.start()))[:2]
    result = {}
    for declaration in declarations:
        # Comments may contain quoted prose; remove them before reading literals.
        literals = re.sub(r"/\*.*?\*/", "", declaration[2], flags=re.S)
        result["vert" if declaration[1] == "vertex_source" else "frag"] = "".join(
            ast.literal_eval(literal) for literal in STRINGS.findall(literals)
        )
    if set(result) != {"vert", "frag"}:
        raise ValueError(f"shader declarations missing in {function}")
    return result


def interface(program: dict[str, str]):
    uniforms, textures = {}, {}
    for code in program.values():
        for kind, name, count in UNIFORM.findall(code):
            field = (kind, int(count) if count else 0)
            if kind.startswith("sampler"):
                if name in textures and textures[name] != kind:
                    raise ValueError(f"sampler type differs: {name}")
                textures[name] = kind
            else:
                if name in uniforms and (uniforms[name].kind, uniforms[name].count) != field:
                    raise ValueError(f"uniform type differs: {name}")
                uniforms[name] = Uniform(kind, name, field[1])
    uniforms["framebuffer_height"] = Uniform("float", "framebuffer_height", 0)
    offset = 0
    for value in uniforms.values():
        align = 16 if value.count or value.components >= 3 else value.components * 4
        offset = (offset + align - 1) // align * align
        value.offset = offset
        value.stride = 16 if value.count else value.components * 4
        offset += value.stride * max(value.count, 1)
    size = (offset + 15) // 16 * 16
    return uniforms, textures, size


def translate(name, stage, code, views, uniforms, textures, locations):
    code = re.sub(r"^#version .*\n", "", code, flags=re.M)
    code = re.sub(r"^#extension .*\n", "", code, flags=re.M)
    code = re.sub(r"^layout\(num_views = \d+\) in;\n", "", code, flags=re.M)
    code = code.replace("gl_VertexID", "gl_VertexIndex").replace("gl_ViewID_OVR", "gl_ViewIndex")
    code = UNIFORM.sub("", code)

    def varying(match):
        flat, direction, kind, field = match.groups()
        location = 0 if stage == "frag" and direction == "out" else locations[field]
        return f"layout(location = {location}) {flat or ''}{direction} {kind} {field};\n"

    code = INTERFACE.sub(varying, code)
    helpers = ""
    if stage == "frag":
        # GL's fragment coordinates count rows from the image's bottom.
        # The ray-producing vertex shaders already account for clip-control's
        # upper-left origin, so their position and varying equations stay as-is.
        if name != "copy" and "gl_FragCoord" in code:
            code = code.replace("gl_FragCoord", "vr_frag_coord()")
            helpers += """
vec4 vr_frag_coord()
{
    return vec4(gl_FragCoord.x, framebuffer_height - gl_FragCoord.y, gl_FragCoord.zw);
}
"""
        if name in {"light", "visor"}:
            helpers += SRGB
        for texture, kind in textures.items():
            if texture not in FLIPPED_TEXTURES:
                continue
            dims = "vec3" if kind == "sampler2DArray" else "vec2"
            for operation in ("texture", "texelFetch"):
                pattern = re.compile(r"\b" + operation + r"\(" + texture + r",\s*")
                if not pattern.search(code):
                    continue
                function = f"vr_{operation}_{texture}"
                code = pattern.sub(function + "(", code)
                if operation == "texture":
                    expression = f"texture({texture}, p)"
                    if name == "light" and texture == "eyes":
                        # The GL sampler decodes each texel before filtering.
                        # Decoding a filtered UNORM value would darken high
                        # contrast regions and change the HUD's response.
                        helpers += f"""
vec4 {function}(vec3 p)
{{
    p.y = 1.0 - p.y;
    ivec2 size = textureSize(eyes, 0).xy;
    vec2 texel = p.xy * vec2(size) - 0.5;
    ivec2 first = ivec2(floor(texel)), last = size - 1;
    vec2 f = fract(texel);
    int eye = int(p.z);
    vec3 a = vr_decode_srgb(texelFetch(eyes, ivec3(clamp(first, ivec2(0), last), eye), 0).rgb);
    vec3 b = vr_decode_srgb(texelFetch(eyes, ivec3(clamp(first + ivec2(1, 0), ivec2(0), last), eye), 0).rgb);
    vec3 c = vr_decode_srgb(texelFetch(eyes, ivec3(clamp(first + ivec2(0, 1), ivec2(0), last), eye), 0).rgb);
    vec3 d = vr_decode_srgb(texelFetch(eyes, ivec3(clamp(first + ivec2(1, 1), ivec2(0), last), eye), 0).rgb);
    return vec4(mix(mix(a, b, f.x), mix(c, d, f.x), f.y), 1.0);
}}
"""
                        continue
                    helpers += (
                        f"\nvec4 {function}({dims} p)\n{{\n"
                        f"    p.y = 1.0 - p.y;\n    return {expression};\n}}\n"
                    )
                else:
                    helpers += (
                        f"\nvec4 {function}(i{dims} p, int lod)\n{{\n"
                        f"    p.y = textureSize({texture}, lod).y - 1 - p.y;\n"
                        f"    return texelFetch({texture}, p, lod);\n}}\n"
                    )
        if name == "visor":
            # GL_FRAMEBUFFER_SRGB blends the linear visor with decoded eyes.
            # The Vulkan eye target uses an UNORM view for the game's encoded
            # colors. Do this one pass's decode/blend/encode explicitly.
            final = code.rfind("}")
            code = code[:final] + """
    vec3 pixel = vec3(gl_FragCoord.xy / vec2(textureSize(eyes_for_blend, 0).xy), float(gl_ViewIndex));
    vec4 under = texture(eyes_for_blend, pixel);
    colour = vec4(vr_encode_srgb(colour.rgb + vr_decode_srgb(under.rgb) * (1.0 - colour.a)),
        colour.a + under.a * (1.0 - colour.a));
""" + code[final:]

    declarations = "#version 450\n"
    if views == 2:
        declarations += "#extension GL_EXT_multiview : require\n"
    declarations += "layout(std140, set = 0, binding = 1) uniform VrUniforms\n{\n"
    for value in uniforms.values():
        array = f"[{value.count}]" if value.count else ""
        declarations += f"    layout(offset = {value.offset}) {value.kind} {value.name}{array};\n"
    declarations += "};\n"
    for binding, (texture, kind) in enumerate(textures.items()):
        declarations += f"layout(set = 1, binding = {binding}) uniform {kind} {texture};\n"
    return declarations + helpers + code


def words_array(name, data):
    words = struct.unpack(f"<{len(data) // 4}I", data)
    lines = [f"static const uint32_t {name}[] =\n{{"]
    for index in range(0, len(words), 8):
        lines.append("\t" + ", ".join(f"0x{word:08x}" for word in words[index:index + 8]) + ",")
    lines.append("};\n")
    return "\n".join(lines)


def generate(args):
    build = args.build_dir
    build.mkdir(parents=True, exist_ok=True)
    source = SOURCE.read_text()
    command = (["docker", "run", "--rm", "-u", f"{os.getuid()}:{os.getgid()}",
                "-v", f"{ROOT}:{ROOT}", "-w", str(ROOT), args.docker_image]
               if args.docker_image else [])
    text = [
        "/* Generated by tools/vulkan_vr_shaders.py from xgpu_vulkan_vr_glsl.inc. Do not edit. */\n",
        '#include "xgpu_vulkan_vr_shaders.h"\n',
    ]
    descriptions = []
    for name, function, views in PASSES:
        program = sources(source, function)
        if name == "visor":
            program["frag"] = "uniform sampler2DArray eyes_for_blend;\n" + program["frag"]
        uniforms, textures, size = interface(program)
        # The visor's destination copy follows its two original noise textures.
        if name == "visor":
            textures = {key: textures[key] for key in ("noise0", "noise1", "eyes_for_blend")}
        locations = {}
        for flat, direction, kind, field in INTERFACE.findall(program["vert"]):
            if direction == "out":
                locations[field] = len(locations)
        for stage, code in program.items():
            code = translate(name, stage, code, views, uniforms, textures, locations)
            path = build / f"{name}.{stage}"
            path.write_text(code)
            spv = path.with_suffix(f".{stage}.spv")
            # Replace only this generated module, including an older Docker
            # run's root-owned output in the caller-owned build directory.
            spv.unlink(missing_ok=True)
            subprocess.run(
                command + [args.compiler, "-V", "--target-env", "vulkan1.1", "-o", str(spv), str(path)],
                check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            subprocess.run(command + [args.validator, "--target-env", "vulkan1.1", str(spv)], check=True)
            text.append(words_array(f"{name}_{stage}", spv.read_bytes()))
        text.append(f"static const struct xgpu_vulkan_vr_uniform {name}_uniforms[] =\n{{")
        for value in uniforms.values():
            text.append(
                f'\t{{ "{value.name}", {value.offset}, {value.components}, '
                f'{max(value.count, 1)}, {value.stride}, {int(value.integer)} }},'
            )
        text.append("};\n")
        sampler_names = ", ".join(f'"{texture}"' for texture in textures)
        sampler_names = "{ " + (sampler_names or "NULL") + " }"
        descriptions.append(
            f'\t{{ "{name}", {name}_vert, {name}_frag, sizeof({name}_vert), sizeof({name}_frag),\n'
            f'\t\t{name}_uniforms, {len(uniforms)}, {size}, {sampler_names}, {len(textures)}, {views} }},'
        )
        print(f"{name}: {len(uniforms)} uniforms ({size} bytes), {len(textures)} samplers, {views} views")
    text.append("const struct xgpu_vulkan_vr_program xgpu_vulkan_vr_programs[XGPU_VULKAN_VR_PROGRAM_COUNT] =\n{")
    text.extend(descriptions)
    text.append("};\n")
    generated = "\n".join(text)
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text() != generated:
            raise SystemExit(f"{OUTPUT.relative_to(ROOT)} differs; regenerate it")
    else:
        OUTPUT.write_text(generated)
    print(f"{len(PASSES) * 2} modules validated for Vulkan 1.1")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="glslangValidator")
    parser.add_argument("--validator", default="spirv-val")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/vulkan_vr_shaders")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--docker-image", help="run the compiler and validator in this Docker image")
    args = parser.parse_args()
    try:
        generate(args)
    except subprocess.CalledProcessError as error:
        if error.stdout:
            print(error.stdout.decode())
        raise


if __name__ == "__main__":
    main()
