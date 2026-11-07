"""The loading screen's caption (text_hires_line, port/linux/src/text_hires.c)
set in the menus' font, Cyborg Interface Gothic, and drawn under the words
at the screen's own pixels (loading_art_caption_shown): its letters are all
there (a font whose ascent is smaller than its capitals once lost every
letter but the dots), drawn where they belong, as tall as ten of the 480
lines are on that screen (not the loading frame's pixels scaled up: a 4K
screen shows a frame of 1080 rows), over the frame as the presenter's
filter shows it."""
from pathlib import Path
import os
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FONT = ROOT / "port/assets/fonts/CyborgInterfaceGothic-Regular.ttf"


def function(text, start):
    """a C function's text, from its signature to its closing brace"""
    begin = text.index(start)
    return text[begin:text.index("\n}\n", begin) + 3]


def test_caption_letters_are_set_and_drawn(tmp_path):
    source = (ROOT / "port/linux/src/text_hires.c").read_text()
    # (the embedded fonts' list as text_hires.h has it)
    embedded = re.search(r"^struct text_hires_embedded\n\{.*?^\};", (ROOT / "port/linux/src/text_hires.h").read_text(),
                         re.M | re.S).group(0)
    program = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/stb/stb_truetype.h"
#include "loading_art.h"
''' + embedded + r'''
static struct text_hires_embedded text_hires_embedded[1];
static const unsigned int text_hires_embedded_count = 1;
/* (the assets' file, embedded_assets.h: the font alone, at offset 16) */
static unsigned int font[1 << 20];
static const void *embedded_assets_data(unsigned int offset, unsigned int size)
{ (void)size; return offset == 16 ? font : NULL; }
''' + function(source, "static int names_differ(") + function(source, "unsigned char *text_hires_line(") + r'''
int main(int argc, char **argv) {
    FILE *file = fopen(argv[1], "rb");
    static struct loading_art art;
    static uint8_t frame[1916 * 1080 * 4];
    int width, height, x, y, inked_columns = 0, top = 1 << 30, bottom = -1;
    long sum = 0;
    unsigned char *line;
    const uint8_t *tile;
    unsigned int tile_left, tile_top, tile_width, tile_height, drawn = 0, ink_top = ~0u, ink_bottom = 0, i;
    assert(file && fread(font, 1, sizeof(font), file) > 1000); fclose(file);
    text_hires_embedded[0].tag = "ui\\large_ui"; text_hires_embedded[0].data_offset = 16;
    text_hires_embedded[0].size = sizeof(font);
    assert(!text_hires_line("no such font", "x", 80.0f, &width, &height));
    /* (as sdl_platform.c sets it: its capitals ten of the 480 lines, at
    LOADING_PRESENTATION_CAPTION_SCALE times their size) */
    line = text_hires_line("ui\\large_ui", "Preparing menu pictures \xc2\xb7 4 of 23",
        10.0f * LOADING_PRESENTATION_CAPTION_SCALE, &width, &height);
    assert(line && width > 1000 && width <= 8192 && height >= 80 && height <= 512);
    for (x = 0; x < width; x++) {
        int inked = 0;
        for (y = 0; y < height; y++) {
            sum += line[y * width + x];
            if (line[y * width + x] > 128) { inked = 1; top = y < top ? y : top; bottom = y > bottom ? y : bottom; }
        }
        inked_columns += inked;
    }
    /* (every letter: ink across most of the line, as tall as the capitals) */
    assert(inked_columns > width / 2);
    assert(bottom - top >= 76);
    assert(sum > (long)width * height * 255 / 10);
    /* shown on a 3840x2160 screen over the sharp frame (1916x1080, as on
    one): 8 lines a pixel of the caption, 4.5 pixels a line of the screen */
    loading_art_caption(&art, line, (unsigned int)width, (unsigned int)height);
    art.output_width = 1916;
    art.output_height = 1080;
    art.caption_completion = 256;
    for (i = 0; i < 1916u * 1080u; i++) {
        frame[i * 4] = 10; frame[i * 4 + 1] = 20; frame[i * 4 + 2] = 30; frame[i * 4 + 3] = 255;
    }
    tile = loading_art_caption_shown(&art, frame, 3840, 2160, &tile_left, &tile_top, &tile_width, &tile_height);
    assert(tile);
    assert(tile_top == LOADING_ART_CAPTION_TOP * 2160 / 480);
    assert(tile_width <= 3840 && tile_left == (3840 - tile_width) / 2);
    assert(tile_width >= (unsigned int)width * 2160 / 3840 && tile_width <= (unsigned int)width * 2160 / 3840 + 1);
    for (i = 0; i < tile_width * tile_height; i++) {
        const uint8_t *p = tile + i * 4;
        assert(p[3] == 255);
        if (p[2] > 30) {
            drawn++;
        } else {
            /* (where it has none: the frame, as it is shown) */
            assert(p[0] == 10 && p[1] == 20 && p[2] == 30);
        }
        if (p[2] > 160) {
            unsigned int row = i / tile_width;
            ink_top = row < ink_top ? row : ink_top;
            ink_bottom = row > ink_bottom ? row : ink_bottom;
        }
    }
    /* (its ink as tall as the line's at 8 lines a pixel, shown at 4.5: its
    capitals 45 of the screen's rows, not the frame's 22.5 scaled up) */
    assert((int)(ink_bottom - ink_top) * 3840 >= (bottom - top - 4) * 2160 &&
        (int)(ink_bottom - ink_top) * 3840 <= (bottom - top + 4) * 2160);
    assert(drawn > (unsigned int)(sum / 255) * 2160 / 3840 * 2160 / 3840 / 2);
    /* in VR's frame, at its 480 rows */
    memset(frame, 0, sizeof(frame));
    loading_art_caption_draw(&art, frame, 640, 480, 256);
    for (i = drawn = 0; i < 640u * 480u; i++)
        if (frame[i * 4 + 2]) {
            unsigned int row = i / 640;
            assert(row >= LOADING_ART_CAPTION_TOP && row < LOADING_ART_CAPTION_TOP + 20);
            drawn++;
        }
    assert(drawn > (unsigned int)(sum / 255) / 64 / 2);
    return 0;
}
'''
    path = tmp_path / "caption.c"
    path.write_text(program)
    binary = tmp_path / "caption"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-O1", "-Wall", "-Werror", "-Wno-unused-function",
                    "-pthread", "-I", str(ROOT / "port/linux/src"), str(path), "-o", str(binary), "-lm"],
                   check=True, timeout=120)
    subprocess.run([str(binary), str(FONT)], check=True, timeout=60)
