"""display.hdr's shared contract (port/linux/src/xgpu_hdr.h, xgpu_hdr.c,
docs/hdr.md): the classification of the draws into the HDR back buffer, the
reference tone map and encoding the backends' shaders are held to (the Metal
backend's on the GPU: port/macos/tests/xgpu_metal_test.m, test_stream_hdr),
and the shaders' GLSL against the reference's constants. Compiled from the
sources with the host's compiler, beside a stand-in for the platform header,
as the macOS host builds the shared renderer files (tools/macos_build.py)."""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "port/linux/src"
COMPILER = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")

PLATFORM = """#include <stddef.h>
#include <stdint.h>
typedef int BOOL;
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned int UINT;
typedef DWORD D3DCOLOR;
#define TRUE 1
#define FALSE 0
#define D3DRS_PS_MAX 47
"""

DRIVER = r"""
#include <stdio.h>
#include <string.h>
#include "xgpu_backend.h"
#define XGPU_HDR_REFERENCE
#include "xgpu_hdr.h"

static int failures;
#define CHECK(condition, ...) do { if (!(condition)) { failures++; printf("FAIL line %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned short classify(int blend, int source, int destination, int operation, int mask, int category,
	int interface, int kind, int mode)
{
	struct xgpu_hdr_draw draw;

	memset(&draw, 0, sizeof(draw));
	draw.blend = blend;
	draw.blend_source = source;
	draw.blend_destination = destination;
	draw.blend_operation = operation;
	draw.color_mask = mask;
	draw.category = category;
	draw.interface = interface;
	draw.kind = kind;
	draw.mode = mode;
	return xgpu_hdr_classify(&draw);
}

#define RGB (XGPU_WRITE_RED | XGPU_WRITE_GREEN | XGPU_WRITE_BLUE)
#define ALL (RGB | XGPU_WRITE_ALPHA)
#define ADD XGPU_BLEND_ADD
#define F XGPU_HDR_MODE_FAITHFUL
#define P XGPU_HDR_MODE_PUNCHY

int main(void)
{
	static const float peaks[4] = { 1.0f, 1.6f, 4.0f, 16.0f };
	unsigned short tag;
	int mode, peak, step;

	/* the classification: what keeps its light past white */
	CHECK(classify(0, 0, 0, 0, ALL, 2, 0, 0, F) == 0, "an opaque draw is untagged");
	CHECK(classify(1, XGPU_BLEND_SOURCE_ALPHA, XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA, ADD, RGB, 22, 0, 0, F) == 0,
		"an alpha blend cannot leave [0, 1]: untagged");
	CHECK(classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE, ADD, RGB, 22, 0, 0, F) == 0,
		"an additive effect keeps its light (no ceiling)");
	CHECK(classify(1, XGPU_BLEND_SOURCE_ALPHA, XGPU_BLEND_ONE, ADD, RGB, 24, 0, 0, F) == 0, "a lens flare's light too");
	CHECK(classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE, ADD, RGB, 5, 0, 0, F) == 0, "the dynamic lights' too");
	tag = classify(1, XGPU_BLEND_DESTINATION_COLOR, XGPU_BLEND_SOURCE_COLOR, ADD, ALL, 9, 0, 0, F);
	CHECK(XGPU_HDR_CEILING_OF(tag) == XGPU_HDR_CEILING_PAPER_WHITE && (tag & XGPU_HDR_ALPHA_LIMIT) &&
		XGPU_HDR_BLEND_SOURCE(tag) == XGPU_BLEND_DESTINATION_COLOR &&
		XGPU_HDR_BLEND_DESTINATION(tag) == XGPU_BLEND_SOURCE_COLOR, "a double multiply decal stops at paper white, "
		"its alpha at 1, blended in the shader: %04x", tag);
	tag = classify(1, XGPU_BLEND_DESTINATION_ALPHA, XGPU_BLEND_ONE, ADD, RGB, 15, 0, 0, F);
	CHECK(XGPU_HDR_CEILING_OF(tag) == XGPU_HDR_CEILING_PAPER_WHITE, "Faithful: the reflections stop at paper white");
	CHECK(classify(1, XGPU_BLEND_DESTINATION_ALPHA, XGPU_BLEND_ONE, ADD, RGB, 15, 0, 0, P) == 0,
		"Punchy: the reflections keep their light");
	tag = classify(1, XGPU_BLEND_SOURCE_ALPHA, XGPU_BLEND_ONE, ADD, RGB, 22, 0, HALO_HDR_KIND_SPECULAR, F);
	CHECK(XGPU_HDR_CEILING_OF(tag) == XGPU_HDR_CEILING_PAPER_WHITE, "Faithful: glass's reflection stops at paper white");
	tag = classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA, ADD, RGB, 27, 1, 0, F);
	CHECK(XGPU_HDR_CEILING_OF(tag) == XGPU_HDR_CEILING_PAPER_WHITE, "the HUD's additive art stops at paper white");
	CHECK(classify(1, XGPU_BLEND_SOURCE_ALPHA, XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA, ADD, RGB, -1, 1, 0, F) == 0,
		"the menus' alpha blend is untagged");
	tag = classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE, ADD, RGB, 2, 0, HALO_HDR_KIND_FIRST_PERSON | HALO_HDR_KIND_NUMERIC, F);
	CHECK((tag & XGPU_HDR_EXPAND) && XGPU_HDR_CEILING_OF(tag) == XGPU_HDR_CEILING_WEAPON,
		"the assault rifle's ammunition counter glows to the weapon's ceiling: %04x", tag);
	tag = classify(1, XGPU_BLEND_SOURCE_ALPHA, XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA, ADD, RGB, 22, 0,
		HALO_HDR_KIND_FIRST_PERSON | HALO_HDR_KIND_NUMERIC, F);
	CHECK((tag & XGPU_HDR_EXPAND) && XGPU_HDR_CEILING_OF(tag) == XGPU_HDR_CEILING_WEAPON, "... whatever its blend");
	CHECK(classify(1, XGPU_BLEND_SOURCE_ALPHA, XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA, ADD, RGB, 22, 0,
		HALO_HDR_KIND_FIRST_PERSON, F) == 0, "a first-person part's alpha blend is no glow");
	tag = classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA, ADD, RGB, 27, 1, HALO_HDR_KIND_HUD_GLOW, F);
	CHECK((tag & XGPU_HDR_EXPAND) && XGPU_HDR_CEILING_OF(tag) == XGPU_HDR_CEILING_HUD, "the motion tracker glows to "
		"the HUD's ceiling: %04x", tag);
	tag = classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE, XGPU_BLEND_REVERSE_SUBTRACT, RGB, 22, 0, 0, F);
	CHECK(XGPU_HDR_CEILING_OF(tag) != 0, "a subtraction is kept from going below 0");
	tag = classify(1, XGPU_BLEND_ONE_MINUS_DESTINATION_COLOR, XGPU_BLEND_ZERO, ADD, RGB, 28, 1, 0, F);
	CHECK(XGPU_HDR_CEILING_OF(tag) != 0, "one less a destination past white is kept from going below 0");
	tag = classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE, ADD, ALL, 22, 0, 0, F);
	CHECK(tag == (XGPU_HDR_ALPHA_LIMIT | XGPU_HDR_BLEND(XGPU_BLEND_ONE, XGPU_BLEND_ONE, ADD)),
		"an additive draw writing alpha keeps alpha at most 1, its colour unlimited: %04x", tag);
	CHECK(classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA, ADD, ALL, 22, 0, 0, F) == 0,
		"the over operator's alpha cannot pass 1");

	/* the reference: Faithful leaves under white as it is, and stays
	under the peak, rising with the light, past it */
	for (mode = F; mode <= P; mode++)
	{
		for (peak = 0; peak < 4; peak++)
		{
			float last = 0.0f;

			for (step = 0; step <= 400; step++)
			{
				float value = (float)step / 100.0f;
				float rgb[3] = { value, value * 0.8f, value * 0.5f };
				float most;

				xgpu_hdr_tone_map(rgb, mode, peaks[peak]);
				most = xgpu_hdr_linear(rgb[0] > rgb[1] ? (rgb[0] > rgb[2] ? rgb[0] : rgb[2]) : (rgb[1] > rgb[2] ? rgb[1] : rgb[2]));
				CHECK(most <= peaks[peak] * 1.0001f, "%s at peak %.1f: %.3f stays under the peak (%.4f)",
					mode == F ? "faithful" : "punchy", peaks[peak], value, most);
				CHECK(most >= last - 1.0e-4f, "%s at peak %.1f: brighter with more light (%.3f: %.4f after %.4f)",
					mode == F ? "faithful" : "punchy", peaks[peak], value, most, last);
				if (mode == F && value <= 1.0f)
					CHECK(rgb[0] == value && rgb[1] == value * 0.8f && rgb[2] == value * 0.5f,
						"faithful at peak %.1f leaves %.3f as it is", peaks[peak], value);
				last = most;
			}
		}
	}
	{
		/* Punchy lifts the scene: a mid grey brighter, keeping its hue */
		float grey[3] = { 0.5f, 0.5f, 0.5f }, colour[3] = { 0.2f, 0.4f, 0.8f };

		xgpu_hdr_tone_map(grey, P, 4.0f);
		xgpu_hdr_tone_map(colour, P, 4.0f);
		CHECK(grey[0] > 0.5f && grey[0] == grey[1] && grey[1] == grey[2], "punchy lifts a grey: %.4f", grey[0]);
		CHECK(xgpu_hdr_linear(colour[1]) / xgpu_hdr_linear(colour[2]) > 0.99f * xgpu_hdr_linear(0.4f) / xgpu_hdr_linear(0.8f) &&
			xgpu_hdr_linear(colour[1]) / xgpu_hdr_linear(colour[2]) < 1.01f * xgpu_hdr_linear(0.4f) / xgpu_hdr_linear(0.8f),
			"punchy keeps a colour's hue");
	}
	{
		/* a light far past white is white-hot, as the SDR frame has it */
		float core[3] = { 3.0f, 2.5f, 1.5f };

		xgpu_hdr_tone_map(core, F, 4.0f);
		CHECK(core[0] > 1.0f && core[1] > 1.0f && core[2] > 1.0f && core[0] == core[1] && core[1] == core[2],
			"faithful: an explosion's core white-hot, brighter than paper white: %.3f %.3f %.3f", core[0], core[1],
			core[2]);
	}
	{
		/* the encoding: paper white times the light, at most the peak; PQ's
		paper white at its nits */
		struct xgpu_hdr_frame edr = { F, XGPU_HDR_OUTPUT_EXTENDED_LINEAR, 1.25f, 2.0f };
		struct xgpu_hdr_frame pq = { F, XGPU_HDR_OUTPUT_PQ, 203.0f / 10000.0f, 4.0f };
		float white[3] = { 1.0f, 1.0f, 1.0f }, bright[3] = { 4.0f, 4.0f, 4.0f }, pq_white[3] = { 1.0f, 1.0f, 1.0f };

		xgpu_hdr_encode(white, &edr);
		xgpu_hdr_encode(bright, &edr);
		xgpu_hdr_encode(pq_white, &pq);
		CHECK(white[0] > 1.2499f && white[0] < 1.2501f, "EDR: paper white at its scale: %.5f", white[0]);
		CHECK(bright[0] > 2.4999f && bright[0] < 2.5001f, "EDR: at most the peak times paper white: %.5f", bright[0]);
		CHECK(pq_white[0] > 0.5805f && pq_white[0] < 0.5809f, "PQ: 203 nits is 0.5807: %.5f", pq_white[0]);
	}
	{
		/* a glow brightened toward its ceiling by its own light, at most to it */
		float had[3] = { 0.1f, 0.2f, 0.3f }, rgb[3] = { 0.6f, 0.95f, 1.0f };

		xgpu_hdr_expand(rgb, had, XGPU_HDR_CEILING_WEAPON);
		CHECK(rgb[2] > 1.0f, "the counter's digits past paper white: %.3f", rgb[2]);
		CHECK(rgb[0] - 0.6f < rgb[2] - 1.0f, "by its own light (its colour)");
	}
	{
		/* the dynamic lights: the same under white, more past it */
		CHECK(xgpu_hdr_lights(0.2f, 0.1f, 1) == xgpu_hdr_lights(0.2f, 0.1f, 0), "the lights' shoulder under white");
		CHECK(xgpu_hdr_lights(0.8f, 3.0f, 1) > 1.2f && xgpu_hdr_lights(0.8f, 3.0f, 0) <= 1.0f,
			"the lights' core past white with HDR: %.3f", xgpu_hdr_lights(0.8f, 3.0f, 1));
	}
	{
		/* the screenshot key's picture: the SDR frame's pixels as they are, a
		highlight inside white keeping its hue, a core white */
		float sdr[4] = { 0.5f, 0.25f, 1.0f, 0.3f }, glow[4] = { 1.6f, 0.8f, 0.4f, 1.0f },
			core[4] = { 3.0f, 2.5f, 1.5f, 1.0f };
		unsigned char bgra[4];

		xgpu_hdr_sdr_pixel(sdr, bgra);
		CHECK(bgra[0] == 255 && bgra[1] == 64 && bgra[2] == 128 && bgra[3] == 77,
			"an SDR pixel as it is, as BGRA: %u %u %u %u", bgra[0], bgra[1], bgra[2], bgra[3]);
		xgpu_hdr_sdr_pixel(glow, bgra);
		CHECK(bgra[2] == 255 && bgra[1] > 64 && bgra[1] < 255 && bgra[0] < bgra[1],
			"a glow past white in SDR, its hue kept: %u %u %u", bgra[2], bgra[1], bgra[0]);
		xgpu_hdr_sdr_pixel(core, bgra);
		CHECK(bgra[0] == 255 && bgra[1] == 255 && bgra[2] == 255, "a core of light white: %u %u %u", bgra[2], bgra[1],
			bgra[0]);
	}
	printf("%d failed\n", failures);
	return failures != 0;
}
"""


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    if not COMPILER:
        pytest.skip("needs a C compiler")
    folder = tmp_path_factory.mktemp("hdr")
    for name in ("xgpu.h", "xgpu_backend.h", "xgpu_hdr.h", "xgpu_hdr.c"):
        shutil.copy(SOURCE / name, folder / name)
    (folder / "platform.h").write_text(PLATFORM)
    (folder / "driver.c").write_text(DRIVER)
    binary = folder / "driver"
    subprocess.run([COMPILER, "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function", "-I", str(folder),
                    str(folder / "driver.c"), str(folder / "xgpu_hdr.c"), "-lm", "-o", str(binary)], check=True)
    return binary


def test_contract(built):
    result = subprocess.run([str(built)], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr


def test_glsl_constants():
    """the shaders' curves are the reference's: Punchy's lift, bright lift and
    knees, and Faithful's clipped colour's start, written into the GLSL as
    numbers"""
    header = (SOURCE / "xgpu_hdr.h").read_text()
    glsl = (SOURCE / "xgpu_hdr.c").read_text()

    def macro(name):
        return float(re.search(rf"#define {name} ([\d.]+)f", header)[1])

    assert f"{macro('XGPU_HDR_PUNCHY_LIFT'):g} * (1.0 + {macro('XGPU_HDR_PUNCHY_BRIGHT'):g} * smoothstep(" \
           f"{macro('XGPU_HDR_PUNCHY_BRIGHT_KNEE'):g}, 1.0, sdr_luminance))" in glsl
    assert f"min(1.0, {macro('XGPU_HDR_PUNCHY_KNEE'):g} * peak)" in glsl
    assert f"smoothstep(1.0, {macro('XGPU_HDR_FAITHFUL_CLIPPED'):.1f}, max(v.r, max(v.g, v.b)))" in glsl


def test_hooks_in_the_game():
    """the game's renderer tells the front end what it draws: its categories,
    its transparent groups (the first-person weapon's), the HUD's glows"""
    rasterizer = ROOT / "source/rasterizer/xbox"
    assert "halo_hdr_category(profile);" in (rasterizer / "rasterizer_xbox_profile.c").read_text()
    assert "halo_hdr_draw_kind(transparent_hdr_kind(group));" in \
        (rasterizer / "rasterizer_xbox_transparent_geometry.c").read_text()
    assert "HALO_HDR_KIND_HUD_GLOW" in (rasterizer / "rasterizer_xbox_motion_sensor.c").read_text()
    assert "HALO_HDR_KIND_HUD_GLOW" in (rasterizer / "rasterizer_xbox_dynavobgeom.c").read_text()
