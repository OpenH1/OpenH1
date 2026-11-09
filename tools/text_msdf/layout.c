/*
LAYOUT.C (test)

The high-res text's glyphs laid out the same whichever way they are drawn
(display.text_renderer, port/linux/src/text_hires.c): each character's quad,
advance and ink, from text_hires_glyph, rasterized ("raster") and from the
distance fields ("msdf"), at the display's scales of 720p, 1080p and 4K and
drawn twice as large, for each of the shipped fonts' tags. The baked
characters, and some the atlas has not (made at run time) and the missing glyph's. tools/
test_text_msdf.py builds it with text_hires.c and stand-ins for the
platform (generated.c: the embedded fonts and atlas, in a blob it reads).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "text_hires.h"

/* ---------- stand-ins */

static unsigned char *blob;
static float screen_scale = 2.25f;
static const char *renderer = "raster";
static unsigned long changes = 1;

const void *embedded_assets_data(unsigned int offset, unsigned int size)
{
	(void)size;
	return blob + offset;
}

unsigned long config_changes(void)
{
	return changes;
}

int config_boolean(const char *name)
{
	(void)name;
	return 1;
}

const char *config_string(const char *name)
{
	return !strcmp(name, "display.text_renderer") ? renderer : "";
}

float halo_screen_pixel_scale(void)
{
	return screen_scale;
}

void platform_log(const char *format, ...)
{
	(void)format;
}

unsigned int xgpu_image_create(void *pixels, unsigned long width, unsigned long height, unsigned long levels,
	int format, int owned)
{
	(void)pixels; (void)width; (void)height; (void)levels; (void)format; (void)owned;
	return 1;
}

void xgpu_image_changed(unsigned int image)
{
	(void)image;
}

void xgpu_image_delete(unsigned int image)
{
	(void)image;
}

/* ---------- the test */

/* (some the atlas has not, and some no font has: the missing glyph) */
static const unsigned long extra[] = { 0x2026, 0x20ac, 0x2122, 0x100, 0x101, 0x141, 0x3a9, 0x4e2d, 0x1f600 };

static int glyph_of(long font, unsigned long code, struct text_hires_glyph *glyph)
{
	text_hires_rasterize(font, code);
	return text_hires_glyph(font, code, glyph);
}

static int same(float a, float b)
{
	return a == b;
}

int main(int argc, char **argv)
{
	static const struct
	{
		const char *tag;
		float cap;
		short width;
	} tags[] = {
		{ "ui\\large_ui", 13.0f, 9 }, { "ui\\small_ui", 10.0f, 7 }, { "old tags\\internal system plain", 9.0f, 6 },
		{ "pc\\heading", 13.0f, 9 },
	};
	static const float scales[] = { 1.5f, 2.25f, 4.5f };
	static const float oversamples[] = { 1.0f, 2.0f };
	FILE *file;
	long size;
	int tag, scale, oversample, checked = 0, made = 0, failed = 0;
	unsigned long texture[2] = { 0, 0x1000 };

	if (argc != 2 || !(file = fopen(argv[1], "rb")))
		return 2;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	blob = malloc((size_t)size);
	if (fread(blob, 1, (size_t)size, file) != (size_t)size)
		return 2;
	fclose(file);
	text_hires_register_atlas(texture, 256, 256);
	for (scale = 0; scale < 3; scale++)
	{
		screen_scale = scales[scale];
		for (oversample = 0; oversample < 2; oversample++)
		{
			for (tag = 0; tag < 4; tag++)
			{
				short widths[TEXT_HIRES_FIT_LETTERS];
				long font;
				unsigned long code;
				int letter;

				for (letter = 0; letter < TEXT_HIRES_FIT_LETTERS; letter++)
					widths[letter] = (short)(tags[tag].width - (letter >= 26));
				font = text_hires_font(tags[tag].tag, tags[tag].cap, widths, oversamples[oversample]);
				if (font < 0)
				{
					printf("FAIL %s: no font\n", tags[tag].tag);
					return 1;
				}
				for (code = 0x21; code < 0x100 + sizeof(extra) / sizeof(extra[0]); code++)
				{
					unsigned long character = code < 0x100 ? code : extra[code - 0x100];
					struct text_hires_glyph raster, msdf;
					int had_raster, had_msdf;

					if (character >= 0x7f && character < 0xa1)
						continue;
					renderer = "raster";
					changes++;
					memset(&raster, 0, sizeof(raster));
					memset(&msdf, 0, sizeof(msdf));
					had_raster = glyph_of(font, character, &raster);
					renderer = "msdf";
					changes++;
					had_msdf = glyph_of(font, character, &msdf);
					if (had_raster != had_msdf || (had_raster == 1 && (!same(raster.left, msdf.left) ||
						!same(raster.top, msdf.top) || !same(raster.right, msdf.right) ||
						!same(raster.bottom, msdf.bottom) || !same(raster.advance, msdf.advance) ||
						!same(raster.ink_left, msdf.ink_left) || !same(raster.ink_right, msdf.ink_right))))
					{
						printf("FAIL %s U+%04lX at %g x%g: raster %d (%g %g %g %g, %g, ink %g %g), "
							"msdf %d (%g %g %g %g, %g, ink %g %g)\n", tags[tag].tag, character, scales[scale],
							oversamples[oversample], had_raster, raster.left, raster.top, raster.right, raster.bottom,
							raster.advance, raster.ink_left, raster.ink_right, had_msdf, msdf.left, msdf.top, msdf.right,
							msdf.bottom, msdf.advance, msdf.ink_left, msdf.ink_right);
						failed++;
					}
					/* (its quad within the atlas, the right way round) */
					if (had_msdf == 1 && !(msdf.u0 < msdf.u1 && msdf.v0 < msdf.v1 && msdf.u0 >= 0.0f &&
						msdf.v0 >= 0.0f && msdf.u1 <= 256.0f && msdf.v1 <= 256.0f))
					{
						printf("FAIL %s U+%04lX: its field's quad %g %g %g %g\n", tags[tag].tag, character, msdf.u0,
							msdf.v0, msdf.u1, msdf.v1);
						failed++;
					}
					checked += had_msdf == 1;
					made += had_msdf == 1 && code >= 0x100;
				}
			}
		}
	}
	printf("%d glyphs checked, %d of them made at run time, %d failed\n", checked, made, failed);
	return failed != 0;
}
