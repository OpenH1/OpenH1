"""The weights of display.shadow_resolution's blur (port/linux/src/xgpu_shadow_blur.h)
against the game's passes run one after another on a line of texels: the Xbox's
1, 2, 1 and each widening pass's taps distance texels off, twice for each
distance, what falls beyond an edge lost (the border's black)."""

from __future__ import annotations

from fractions import Fraction
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]

PROGRAM = r"""
#include <stdio.h>
#include <stddef.h>
#include "xgpu_shadow_blur.h"

int main(void)
{
	unsigned char texels[(2 * XGPU_SHADOW_BLUR_MOST_RADIUS + 1) * (XGPU_SHADOW_BLUR_MOST_RADIUS + 1) * 4];
	int scale;

	for (scale = 1; scale <= 16; scale++)
	{
		int radius = xgpu_shadow_blur_weights(scale, texels), index;

		printf("%d %d", scale, radius);
		for (index = 0; index < (2 * radius + 1) * (radius + 1); index++)
			printf(" %d", texels[index * 4] * 256 + texels[index * 4 + 1] + 65536 * (texels[index * 4 + 2] |
				texels[index * 4 + 3]));
		printf("\n");
	}
	return 0;
}
"""


def chain(scale: int, line: list[Fraction]) -> list[Fraction]:
    """The game's passes over a line, each reading black beyond its ends."""
    length = len(line)

    def at(values, index):
        return values[index] if 0 <= index < length else Fraction(0)

    values = [at(line, i - 1) / 4 + at(line, i) / 2 + at(line, i + 1) / 4 for i in range(length)]
    distance = 1
    while distance < scale:
        for _ in range(2):
            values = [(at(values, i - distance) + at(values, i + distance)) / 2 for i in range(length)]
        distance *= 2
    return values


@pytest.fixture(scope="module")
def tables(tmp_path_factory):
    cc = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        pytest.skip("needs a C compiler")
    folder = tmp_path_factory.mktemp("shadow_blur")
    source = folder / "weights.c"
    source.write_text(PROGRAM)
    program = folder / "weights"
    subprocess.run([cc, "-std=c99", "-Wall", "-Werror", "-I", str(ROOT / "port/linux/src"), "-o", str(program),
                    str(source)], check=True)
    found = {}
    for line in subprocess.run([str(program)], check=True, capture_output=True, text=True).stdout.splitlines():
        scale, radius, *weights = (int(value) for value in line.split())
        found[scale] = (radius, weights)
    return found


def test_only_the_larger_maps_scales_have_a_blur(tables):
    assert {scale for scale, (radius, _) in tables.items() if radius} == {2, 4, 8}
    for scale in (2, 4, 8):
        assert tables[scale][0] == 2 * scale - 1


@pytest.mark.parametrize("scale", [2, 4, 8])
def test_the_weights_are_the_passes_chain(tables, scale):
    radius, weights = tables[scale]
    width = 2 * radius + 1
    # every weight is a whole number of 65536ths below 1
    assert all(0 <= weight < 65536 for weight in weights)
    length = 6 * radius + 4
    for texel in range(length):
        response = [chain(scale, [Fraction(int(j == source)) for j in range(length)])[texel] for source in
                    range(length)]
        nearer = min(texel, length - 1 - texel)
        row = min(nearer, radius)
        mirror = 1 if texel <= length - 1 - texel else -1
        for offset in range(-radius, radius + 1):
            source = texel + offset
            expected = response[source] if 0 <= source < length else Fraction(0)
            if not 0 <= source < length:
                continue
            assert weights[row * width + radius + offset * mirror] == expected * 65536, (texel, offset)
        # nothing further than the radius is weighed
        assert all(weight == 0 for index, weight in enumerate(response) if abs(index - texel) > radius)


def test_the_middle_row_is_the_xbox_tent_scaled(tables):
    # a tent scale times as wide as the Xbox's: 2 * scale - |d| for |d| up
    # to 2 * scale - 1, out of 4 * scale * scale
    for scale in (2, 4, 8):
        radius, weights = tables[scale]
        width = 2 * radius + 1
        middle = weights[radius * width:(radius + 1) * width]
        assert middle == [(2 * scale - abs(offset)) * 65536 // (4 * scale * scale) for offset in range(-radius, radius + 1)]
