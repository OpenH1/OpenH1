"""tools/gpu_isa.py's source rewriting and counting (no GPU needed), and,
on an M1-family Mac, a shader disassembled with and without its constants."""

import platform
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gpu_isa  # noqa: E402

SOURCE = """#include <metal_stdlib>
using namespace metal;
constant uint mode [[function_constant(0)]];
constant bool tint [[function_constant(1)]];
struct fragment_in
{
	float4 colour [[user(colour)]];
	float2 uv [[user(uv)]];
};
fragment float4 shade(fragment_in in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]])
{
	float4 c = t.sample(s, in.uv / in.colour.w);
	if (mode == 1u) c *= in.colour;
	if (tint) c.rgb = 1.0 - c.rgb;
	return c;
}
"""


def test_literal_source():
    text = gpu_isa.literal_source(SOURCE, {0: "1", 1: "0"})
    assert "constant uint mode = 1u;" in text
    assert "constant bool tint = false;" in text
    assert "function_constant" not in text
    assert "function_constant(1)" in gpu_isa.literal_source(SOURCE, {0: "2"})


def test_stage_in_vertex():
    text = gpu_isa.stage_in_vertex(SOURCE, "shade")
    assert "float4 colour [[user(colour)]];" in text
    assert "float2 uv [[user(uv)]];" in text
    assert "out.uv = float2(v[id * 32 + 2].xy);" in text
    assert gpu_isa.stage_in_vertex(SOURCE, "missing") is None


def test_parse_values(tmp_path):
    (tmp_path / "values").write_text("5, 0x10,\n7\n")
    assert gpu_isa.parse_values("@" + str(tmp_path / "values"), ["1=9"]) == {0: "5", 1: "9", 2: "7"}


def test_counts():
    listing = [
        "   0: 61950c4000400000     iterproj         r5, cf12, cf0, elide, center, 0b0, 0, 0, 0, 0, 0",
        "   8: 8259c002008e010c     fcmpsel          eq, r22.cache, r0.discard, 0.0, u7, r0.discard",
        "  10: fe41ac229900         and              r16.cache, r22.cache, u9",
        "  16: 8a299a82             rcp              r10.cache, r13.cache",
        "  1a: 09010004f0fc8003     st_tile          r0_r1_r2_r3, u8norm, 0, xyzw, 0, 255, 0",
        "  22: 8800                 stop",
    ]
    counts = gpu_isa.counts(listing)
    assert counts["instructions"] == 6
    assert counts["bytes"] == 8 + 8 + 6 + 4 + 8 + 2
    assert counts["kinds"] == {"varying": 1, "select": 1, "integer": 1, "float": 1, "output": 1, "control": 1}
    assert counts["registers"] == 23
    assert counts["uniforms"] == 10
    assert counts["stack"] == 0


def m1_mac():
    if sys.platform != "darwin" or platform.machine() != "arm64":
        return False
    brand = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True).stdout
    return "M1" in brand and shutil.which("git") is not None


@pytest.mark.skipif(not m1_mac(), reason="applegpu decodes the M1 family's GPUs")
def test_specialised_as_literal(tmp_path):
    """the function specialised with its constants compiles to the code of
    the source with them written in"""
    path = tmp_path / "shade.metal"
    path.write_text(SOURCE)
    listings = []
    for literal in (False, True):
        work = tmp_path / str(literal)
        work.mkdir()
        _, programs = gpu_isa.harvest(path, "shade", {0: "1", 1: "1"}, literal, None, None, None, work)
        main = [code for name, code in programs if name == "fragment main"][0]
        listings.append(gpu_isa.disassemble(main))
    assert listings[0] == listings[1]
    counts = gpu_isa.counts(listings[0])
    assert counts["kinds"].get("texture") == 1 and counts["stack"] == 0
