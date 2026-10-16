"""The renderer's manifest (port/linux/src/xgpu_manifest.h): the C reader
reads what tools/render_manifest.py writes, refuses a pixel key that is not
its hash's, a merge is the same file however its recordings come, the
game's vertex shaders are read from its code, and the check of a map's
enumeration compares draws as a backend compiles them."""
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "port" / "linux" / "src"

READER = r"""
#include "xgpu_manifest.h"
#include <stdio.h>
int main(int argc, char **argv)
{
	struct xgpu_manifest manifest;
	char error[256];
	uint32_t index;

	(void)argc;
	if (!xgpu_manifest_read(argv[1], &manifest, error, sizeof(error)))
	{
		printf("error: %s\n", error);
		return 1;
	}
	printf("programs %u draws %u\n", manifest.vertex_program_count, manifest.draw_count);
	for (index = 0; index < manifest.vertex_shader_count; index++)
	{
		const struct xgpu_manifest_draw *shader = &manifest.vertex_shaders[index];

		printf("shader %08x%08x: program %s, packed %#x, elements %u\n", shader->key.vertex_program[1],
			shader->key.vertex_program[0], xgpu_manifest_vertex_program(&manifest, shader->key.vertex_program[0] |
			((uint64_t)shader->key.vertex_program[1] << 32)) ? "found" : "missing",
			shader->key.packed_attributes, shader->element_count);
	}
	for (index = 0; index < manifest.draw_count; index++)
	{
		const struct xgpu_manifest_draw *draw = &manifest.draws[index];
		const struct xgpu_manifest_vertex_program *program = xgpu_manifest_vertex_program(&manifest,
			draw->key.vertex_program[0] | ((uint64_t)draw->key.vertex_program[1] << 32));

		printf("draw %u: program %s, blend %u/%u/%u, samples %u, texture modes %#x, combiner 53 %#x, "
			"elements %u, stride %u, immediate %#x, stage 0 %u.%u border %#x\n", index,
			program ? "found" : "missing", draw->key.blend, draw->key.blend_source, draw->key.blend_destination,
			draw->key.samples, draw->key.pixel.texture_modes, draw->key.pixel.combiner_state[53], draw->element_count,
			draw->strides[0], draw->immediate_written, draw->stages[0].source, draw->stages[0].sampler_type,
			draw->stages[0].border_color);
	}
	xgpu_manifest_free(&manifest);
	return 0;
}
"""


def fnv(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def pixel_key(combiner, texture_modes, samplers, fog):
    """an xgpu_stream_pixel_key's name and its record (its 264 bytes: three of
    pad[] and three of the structure's own padding after them)"""
    states = [0] * 57
    for index, value in combiner.items():
        states[index] = value
    data = struct.pack("<57I", *states) + struct.pack("<I", texture_modes) + bytes(samplers) + bytes(4) + bytes(4)
    data += struct.pack("<I", 0) + bytes([fog, 0, 0, 0, 0, 0, 0, 0, 0, 0]) + bytes(6)
    assert len(data) == 264
    name = f"{fnv(data):016x}"
    cs = ",".join(f"{index}:{value:#x}" for index, value in sorted(combiner.items()))
    fields = (f"tm={texture_modes:#x} sa={','.join(map(str, samplers))} ak=0,0,0,0 sg=0,0,0,0 at=0 fe={fog} ft=0 "
              "cn=0 ca=0 pt=0 pl=0 as=0 mv=0 sb=0 dl=0")
    return name, f"p {name} cs={cs} {fields}"


def manifest_text():
    pixel, pixel_line = pixel_key({0: 0x18200000, 53: 0x11001}, 0x1, [1, 0, 0, 0], 1)
    program = "00000000000000ab"
    words = "00000000" + "0020061b" + "0836106c" + "2070f800"
    draws = [
        f"d vp={program} pa=0x0 im=0 li=0 vw=0 bl=1,4,5,0 cm=15 dt=1,4,1 st=0,0,0,0,0 sm=0,0 cu=1,0,0 fi=0 db=0 "
        f"fm=1,2,1 px={pixel} l=0.0.3.12.0/3.0.5.4.12 w=0:16 t=1.1.3.3.0.0xff102030/0.0.0.0.0.0x0/0.0.0.0.0.0x0/"
        "0.0.0.0.0.0x0",
        f"d vp={program} pa=0x0 im=1 li=0 vw=0 bl=0,0,0,0 cm=15 dt=0,0,0 st=0,0,0,0,0 sm=0,0 cu=0,0,0 fi=0 db=0 "
        f"fm=1,0,4 px={pixel} i=0x9 t=0.0.0.0.0.0x0/0.0.0.0.0.0x0/0.0.0.0.0.0x0/0.0.0.0.0.0x0",
    ]
    return pixel_line, f"v {program} 1 {words}", draws


@pytest.fixture(scope="module")
def reader(tmp_path_factory):
    cc = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        pytest.skip("no C compiler")
    folder = tmp_path_factory.mktemp("reader")
    (folder / "reader.c").write_text(READER)
    binary = folder / "reader"
    subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-DXGPU_MANIFEST_READER_ONLY", f"-I{SOURCE}",
                    str(folder / "reader.c"), str(SOURCE / "xgpu_manifest.c"), "-o", str(binary)], check=True)
    return binary


def merge(*arguments):
    subprocess.run([sys.executable, str(ROOT / "tools" / "render_manifest.py"), "merge", *map(str, arguments)],
                   check=True, capture_output=True)


def test_read(tmp_path, reader):
    pixel, program, draws = manifest_text()
    recording = tmp_path / "recording.txt"
    recording.write_text("manifest 1\n" + "\n".join([program, pixel, *draws]) + "\n")
    manifest = tmp_path / "manifest.txt"
    merge("--replace", manifest, recording)
    result = subprocess.run([str(reader), str(manifest)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout
    lines = result.stdout.splitlines()
    assert lines[0] == "programs 1 draws 2"
    # (sorted: im=0 before im=1)
    assert ("program found, blend 1/4/5, samples 1, texture modes 0x1, combiner 53 0x11001, elements 2, stride 16, "
            "immediate 0, stage 0 1.1 border 0xff102030") in lines[1]
    assert "samples 4" in lines[2] and "elements 0" in lines[2] and "immediate 0x9" in lines[2]


def test_refuses_a_wrong_pixel_key(tmp_path, reader):
    pixel, program, draws = manifest_text()
    manifest = tmp_path / "manifest.txt"
    manifest.write_text("manifest 1\n" + "\n".join([program, pixel.replace("fe=1", "fe=0"), *draws]) + "\n")
    result = subprocess.run([str(reader), str(manifest)], capture_output=True, text=True)
    assert result.returncode == 1 and "its hash's" in result.stdout


def test_merge_is_the_same_file(tmp_path):
    pixel, program, draws = manifest_text()
    first, second = tmp_path / "first.txt", tmp_path / "second.txt"
    first.write_text("manifest 1\n" + "\n".join([program, pixel, draws[1]]) + "\n")
    second.write_text("manifest 1\n" + "\n".join([pixel, program, draws[0], draws[1]]) + "\n")
    one, other = tmp_path / "one.txt", tmp_path / "other.txt"
    merge("--replace", one, first, second)
    merge("--replace", other, second)
    merge(other, first)
    assert one.read_text() == other.read_text()


def test_vertex_shaders_from_the_code(tmp_path, reader):
    """the 67 programs of rasterizer_xbox_vertex_shaders.c, each with its
    declaration's layout (the first program a structure's: its normal,
    binormal and tangent packed, as recorded in play)"""
    manifest = tmp_path / "vertex.txt"
    subprocess.run([sys.executable, str(ROOT / "tools" / "render_manifest.py"), "vertex", str(manifest)], check=True,
                   capture_output=True)
    result = subprocess.run([str(reader), str(manifest)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout
    lines = result.stdout.splitlines()
    assert lines[0] == "programs 67 draws 0"
    shaders = [line for line in lines if line.startswith("shader ")]
    assert len(shaders) == 67 and all("program found" in line for line in shaders)
    assert "shader 0040b0e2af81b3dc: program found, packed 0xe, elements 7" in shaders
ENUMERATED = ("d vp=0000000000000001 pa=0x0 im=0 li=0 vw=0 bl=1,4,5,0 cm=7 dt=1,3,1 st=0,0,0,0,0 sm=0,0 "
              "cu=1,0,0 fi=0 db=0 fm=1,2,1 px=00000000000000aa l=0.0.3.12.0 w=0:12 "
              "t=1.1.0.0.0.0x0/1.1.3.0.2.0x5050505/0.0.0.0.0.0x0/0.0.0.0.0.0x0")


def recording(tmp_path, *after):
    """a recording: its enumeration (ENUMERATED), then each draw after it
    marked missed"""
    lines = ["manifest 1", "# enumerating: the map's", ENUMERATED, "# prepared: the misses"]
    for draw in after:
        lines += ["# missed: 70000010 70000020", draw]
    path = tmp_path / "recording.txt"
    path.write_text("\n".join(lines) + "\n")
    return path


def check(*arguments):
    return subprocess.run([sys.executable, str(ROOT / "tools" / "render_manifest.py"), "check", *map(str, arguments)],
                          capture_output=True, text=True)


def test_compiled_leaves_out_what_each_draw_sets():
    sys.path.insert(0, str(ROOT / "tools"))
    import render_manifest

    stenciled = ENUMERATED.replace("st=0,0,0,0,0", "st=1,2,0,0,0").replace("dt=1,3,1", "dt=0,0,0")
    assert render_manifest.compiled(stenciled) == render_manifest.compiled(ENUMERATED)
    # (a stage's border drawn by the shader: clamped to a colour the sampler has not)
    assert "t=1.1.0/1.1.1/0.0.0/0.0.0" in render_manifest.compiled(ENUMERATED)
    unblended = ENUMERATED.replace("bl=1,4,5,0", "bl=0,4,5,0")
    assert " bl=0 " in render_manifest.compiled(unblended)
    assert render_manifest.compiled(unblended) == render_manifest.compiled(unblended.replace("bl=0,4,5,0", "bl=0,1,1,0"))


def test_check_passes_what_only_the_draw_state_changes(tmp_path):
    result = check(recording(tmp_path, ENUMERATED.replace("cu=1,0,0", "cu=0,0,0")))
    assert result.returncode == 0, result.stdout
    assert "1 draws enumerated" in result.stdout and "0 missed" in result.stdout


def test_check_fails_another_program(tmp_path):
    result = check("--verbose", "1", recording(tmp_path, ENUMERATED.replace("px=00000000000000aa", "px=00000000000000bb")))
    assert result.returncode == 1
    assert "1 missed" in result.stdout and "differs in px" in result.stdout
    assert check("--exact", recording(tmp_path, ENUMERATED.replace("cu=1,0,0", "cu=0,0,0"))).returncode == 1
