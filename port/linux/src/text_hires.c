/*
TEXT_HIRES.C

The game's text drawn with fonts at the display's resolution (text_hires.h).

Each font tag the fonts draw (port/assets/fonts/fonts.json) gets its font
sized so that its capitals are as tall as the tag's, and its fallback, the
font that draws a character the first has not (Overpass, for the few that
Cyborg Interface Gothic lacks), sized and fitted the same way: the game's layout,
from the tag's character widths, is kept, and the fonts were chosen to fit
the Xbox maps' widths. A tag whose letters are narrower (the fonts of a
Custom Edition loc.map) gets its font narrowed until its letters are, on
the whole, as wide as the tag's, so that they do not run into each other;
the Xbox maps' fonts are narrowed by a few hundredths.

The glyphs are rasterized at the display's pixels per unit of the 480
lines, into an 8-bit atlas, on the thread that draws the text, as they are
first wanted: a font's letters, digits and punctuation (ASCII's) as the font
is made, which the maps' fonts are as each map loads (rasterizer_text.c's
hires_text_fonts_ahead), and any other character as the first string that
has it is drawn (rasterizer_text.c rasterizes a string's glyphs before it
draws any of them: text_hires_rasterize). A glyph takes some tens of
microseconds. A character neither font has is drawn as the font's missing
glyph; a string is never drawn from its font tag's bitmaps for want of a
glyph.

The atlas is an image (xgpu_images.h) whose pixels the host reads a frame
or two after the game draws with it: glyphs are only ever added where
nothing drawn is, and when the atlas starts over (a change of the display's
scale, a window made fullscreen; or a full atlas) it is a new image, the old
one kept until the frames that drew it are done (xgpu_image_delete). Its
glyphs are then rasterized again as they are drawn.

The atlas is not kept on disk: the maps' fonts' ASCII (some 470 glyphs)
takes milliseconds, and reading it back would save nothing a player sees.
*/

#include "text_hires.h"
#include "embedded_assets.h"
#include "platform.h"
#include "port_config.h"
/* (the renderer's backend is on the host: the pictures are images) */
#include "xgpu_images.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/stb/stb_truetype.h"

/* (the Xbox maps' tags and a Custom Edition loc.map's, which share names,
each at its oversamples) */
#define MAXIMUM_FONTS 16
#define ATLAS_SIZE 2048
/* clear texels around each glyph, so that filtering never reaches the next */
#define ATLAS_PADDING 2
#define GLYPH_CACHE_SIZE 4096

/* a font file as it draws a tag (none where scale is 0) */
struct font_face
{
	stbtt_fontinfo info;
	/* font units to units of the 480 lines */
	float scale;
	/* the glyphs' widths, against their heights, to fit the tag's letters */
	float narrowing;
};

/* the font, and the fallback that draws the characters it has not */
#define FONT_FACES 2

static struct
{
	char tag[64];
	/* the tag's measures it was made for */
	float cap_height;
	long letters_width;
	struct font_face faces[FONT_FACES];
	/* how many times the display's pixels its glyphs are rasterized with */
	float oversample;
} fonts[MAXIMUM_FONTS];
static long font_count;

/* the glyphs in the atlas (used: rasterized there, or one with no ink) */
static struct
{
	short used;
	long font;
	unsigned long code;
	short x, y, width, height;
	short left, top;    /* the glyph's corner, in pixels from the pen on the baseline */
	float advance;      /* in units of the 480 lines */
	float ink_left, ink_right;  /* its outline's left and right, in pixels from the pen */
} glyphs[GLYPH_CACHE_SIZE];

static unsigned char *atlas;
static float atlas_scale;   /* the pixels per unit its glyphs have */
/* where the next glyph goes */
static int pack_x, pack_y, pack_row_height;
static int atlas_changed;
static unsigned long placeholder_data, placeholder_width, placeholder_height;
/* (the atlas's image, which owns its pixels; 0 until it is first bound) */
static unsigned int atlas_texture;

/* tag names compared as the game does, ignoring case (and without the POSIX
strcasecmp, which the Windows build has not) */
static int names_differ(const char *a, const char *b)
{
	while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b))
	{
		a++;
		b++;
	}
	return tolower((unsigned char)*a) != tolower((unsigned char)*b);
}

int text_hires_enabled(void)
{
	static int enabled;
	static unsigned long read_at = (unsigned long)-1;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		enabled = config_boolean("display.high_res_text");
	}
	return enabled;
}

static void (*reset_hook)(void);

void text_hires_set_reset_hook(void (*hook)(void))
{
	reset_hook = hook;
}

/* the atlas started over, empty, for a display of this scale: a new image
(the old one's pixels kept for the frames that drew them) */
static void atlas_reset(float scale)
{
	/* (text batched from the glyphs there drawn first, from the old image) */
	if (reset_hook && atlas)
		reset_hook();
	if (atlas_texture)
		xgpu_image_delete(atlas_texture);
	else
		free(atlas);
	atlas_texture = 0;
	atlas = calloc(ATLAS_SIZE, ATLAS_SIZE);
	pack_x = pack_y = pack_row_height = 0;
	memset(glyphs, 0, sizeof(glyphs));
	atlas_scale = scale;
	atlas_changed = 1;
}

/* the atlas, for the display's scale now (started over if it was another's);
0 if there is none */
static int atlas_ready(void)
{
	float scale = halo_screen_pixel_scale();

	if (scale <= 0.0f)
		return 0;
	if (!atlas || scale != atlas_scale)
		atlas_reset(scale);
	return atlas != NULL;
}

/* the face of a font that draws a character, and the character's glyph in
it: the font's own, else its fallback's; else the font's missing glyph */
static struct font_face *font_glyph(long font, unsigned long code, int *index)
{
	int face;

	for (face = 0; face < FONT_FACES; face++)
	{
		if (fonts[font].faces[face].scale > 0.0f &&
			(*index = stbtt_FindGlyphIndex(&fonts[font].faces[face].info, (int)code)) != 0)
		{
			return &fonts[font].faces[face];
		}
	}
	*index = 0;
	return &fonts[font].faces[0];
}

/* the glyph's slot in the cache: its own, else the free one it would take,
else -1 (the cache is full) */
static long glyph_slot(long font, unsigned long code)
{
	unsigned long slot = ((unsigned long)font * 2654435761UL ^ code * 40503UL) & (GLYPH_CACHE_SIZE - 1);
	long probe;

	for (probe = 0; probe < GLYPH_CACHE_SIZE; probe++, slot = (slot + 1) & (GLYPH_CACHE_SIZE - 1))
	{
		if (!glyphs[slot].used || (glyphs[slot].font == font && glyphs[slot].code == code))
			return (long)slot;
	}
	return -1;
}

/* a glyph rasterized into the atlas, in its slot; 0 if the atlas has no
room for it */
static int glyph_rasterize(long slot, long font, unsigned long code)
{
	int index, x0 = 0, y0 = 0, x1 = 0, y1 = 0, advance = 0, bearing = 0, ink_x0 = 0, ink_x1 = 0, width, height;
	struct font_face *face = font_glyph(font, code, &index);
	float pixels = face->scale * atlas_scale * fonts[font].oversample;
	float pixels_x = pixels * face->narrowing;

	glyphs[slot].ink_left = glyphs[slot].ink_right = 0.0f;
	stbtt_GetGlyphBitmapBox(&face->info, index, pixels_x, pixels, &x0, &y0, &x1, &y1);
	stbtt_GetGlyphHMetrics(&face->info, index, &advance, &bearing);
	/* (the outline's own left and right, which the box rounds out to whole
	pixels) */
	if (stbtt_GetGlyphBox(&face->info, index, &ink_x0, NULL, &ink_x1, NULL))
	{
		glyphs[slot].ink_left = (float)ink_x0 * pixels_x;
		glyphs[slot].ink_right = (float)ink_x1 * pixels_x;
	}
	width = x1 - x0;
	height = y1 - y0;
	if (width <= 0 || height <= 0 || width + 2 * ATLAS_PADDING > ATLAS_SIZE || height + 2 * ATLAS_PADDING > ATLAS_SIZE)
		width = height = 0;
	if (width)
	{
		if (pack_x + width + 2 * ATLAS_PADDING > ATLAS_SIZE)
		{
			pack_x = 0;
			pack_y += pack_row_height;
			pack_row_height = 0;
		}
		if (pack_y + height + 2 * ATLAS_PADDING > ATLAS_SIZE)
			return 0;
		glyphs[slot].x = (short)(pack_x + ATLAS_PADDING);
		glyphs[slot].y = (short)(pack_y + ATLAS_PADDING);
		stbtt_MakeGlyphBitmap(&face->info, atlas + (size_t)glyphs[slot].y * ATLAS_SIZE + glyphs[slot].x,
			width, height, ATLAS_SIZE, pixels_x, pixels, index);
		pack_x += width + 2 * ATLAS_PADDING;
		if (height + 2 * ATLAS_PADDING > pack_row_height)
			pack_row_height = height + 2 * ATLAS_PADDING;
		atlas_changed = 1;
	}
	glyphs[slot].used = 1;
	glyphs[slot].font = font;
	glyphs[slot].code = code;
	glyphs[slot].width = (short)width;
	glyphs[slot].height = (short)height;
	glyphs[slot].left = (short)x0;
	glyphs[slot].top = (short)y0;
	glyphs[slot].advance = advance * face->scale * face->narrowing;
	return 1;
}

/* the glyph's slot, rasterized now if it is not yet (the atlas started over
if it is full); -1 if there is no atlas */
static long glyph_get(long font, unsigned long code)
{
	long slot;
	int attempt;

	if (!placeholder_data || !atlas_ready())
		return -1;
	for (attempt = 0; attempt < 2; attempt++)
	{
		slot = glyph_slot(font, code);
		if (slot >= 0 && (glyphs[slot].used || glyph_rasterize(slot, font, code)))
			return slot;
		atlas_reset(atlas_scale);
		if (!atlas)
			return -1;
	}
	return -1;
}

/* a font's letters, digits and punctuation rasterized as it is made, before
they are drawn */
static void font_ahead(long font)
{
	unsigned long code;

	for (code = '!'; code <= '~'; code++)
		glyph_get(font, code);
}

/* a font file made a face that draws a tag: sized so that its capitals are
cap_height units tall, narrowed where its letters are wider than the tag's
(letter_widths); FALSE (and no face) if it cannot be read */
static int face_make(struct font_face *face, const unsigned char *data, float cap_height, short const *letter_widths)
{
	float tag_width = 0.0f;
	float font_width = 0.0f;
	int x0, y0, x1, y1;
	int letter;

	face->scale = 0.0f;
	face->narrowing = 1.0f;
	if (!data || !stbtt_InitFont(&face->info, data, stbtt_GetFontOffsetForIndex(data, 0)) ||
		!stbtt_GetCodepointBox(&face->info, 'H', &x0, &y0, &x1, &y1) || y1 <= y0)
	{
		return 0;
	}
	face->scale = cap_height / (float)(y1 - y0);
	for (letter = 0; letter < TEXT_HIRES_FIT_LETTERS; letter++)
	{
		int glyph = stbtt_FindGlyphIndex(&face->info, letter < 26 ? 'A' + letter : 'a' + letter - 26);
		int advance = 0, bearing = 0;

		if (letter_widths[letter] <= 0 || !glyph)
			continue;
		stbtt_GetGlyphHMetrics(&face->info, glyph, &advance, &bearing);
		tag_width += letter_widths[letter];
		font_width += advance * face->scale;
	}
	if (tag_width > 0.0f && font_width > tag_width)
		face->narrowing = tag_width / font_width;
	return 1;
}

long text_hires_font(char const *tag_name, float cap_height, short const *letter_widths, float oversample)
{
	unsigned int index;
	long font;
	long letters_width = 0;
	int letter;

	if (!text_hires_enabled() || !tag_name || cap_height <= 0.0f || !letter_widths)
		return -1;
	/* (text is never drawn with fewer pixels than the display's) */
	if (oversample < 1.0f)
		oversample = 1.0f;
	for (letter = 0; letter < TEXT_HIRES_FIT_LETTERS; letter++)
		letters_width += letter_widths[letter] > 0 ? letter_widths[letter] : 0;
	for (font = 0; font < font_count; font++)
	{
		if (!names_differ(fonts[font].tag, tag_name) && fonts[font].cap_height == cap_height &&
			fonts[font].letters_width == letters_width && fonts[font].oversample == oversample)
		{
			return fonts[font].faces[0].scale > 0.0f ? font : -1;
		}
	}
	if (font_count >= MAXIMUM_FONTS)
		return -1;
	font = font_count++;
	snprintf(fonts[font].tag, sizeof(fonts[font].tag), "%s", tag_name);
	fonts[font].cap_height = cap_height;
	fonts[font].letters_width = letters_width;
	memset(fonts[font].faces, 0, sizeof(fonts[font].faces));
	fonts[font].oversample = oversample;
	for (index = 0; index < text_hires_embedded_count; index++)
	{
		const struct text_hires_embedded *embedded = &text_hires_embedded[index];

		if (!embedded->tag || names_differ(embedded->tag, tag_name))
			continue;
		if (!face_make(&fonts[font].faces[0],
			(const unsigned char *)embedded_assets_data(embedded->data_offset, embedded->size), cap_height,
			letter_widths))
		{
			platform_log("high-res text: could not read %s for %s", embedded->file, tag_name);
			break;
		}
		/* (its fallback: one that cannot be read draws nothing, a
		character the font lacks drawn as its missing glyph, as without one) */
		if (embedded->fallback_file && !face_make(&fonts[font].faces[1],
			(const unsigned char *)embedded_assets_data(embedded->fallback_data_offset, embedded->fallback_size),
			cap_height, letter_widths))
		{
			platform_log("high-res text: could not read %s for %s", embedded->fallback_file, tag_name);
		}
		platform_log("high-res text: %s (capitals %.0f units) drawn with %s, %.0f%% wide%s%s",
			tag_name, cap_height, embedded->file, fonts[font].faces[0].narrowing * 100.0f,
			fonts[font].faces[1].scale > 0.0f ? ", what it lacks with " : "",
			fonts[font].faces[1].scale > 0.0f ? embedded->fallback_file : "");
		break;
	}
	if (fonts[font].faces[0].scale <= 0.0f)
		return -1;
	font_ahead(font);
	return font;
}

void text_hires_rasterize(long font, unsigned long code)
{
	if (font >= 0 && font < font_count && fonts[font].faces[0].scale > 0.0f && code > ' ')
		glyph_get(font, code);
}

int text_hires_glyph(long font, unsigned long code, struct text_hires_glyph *glyph)
{
	float pixel_scale;
	long slot;

	if (font < 0 || font >= font_count || fonts[font].faces[0].scale <= 0.0f || code <= ' ')
		return 0;
	slot = glyph_get(font, code);
	if (slot < 0 || !glyphs[slot].width || !glyphs[slot].height)
		return 0;
	pixel_scale = atlas_scale * fonts[font].oversample;
	glyph->left = glyphs[slot].left / pixel_scale;
	glyph->top = glyphs[slot].top / pixel_scale;
	glyph->right = (glyphs[slot].left + glyphs[slot].width) / pixel_scale;
	glyph->bottom = (glyphs[slot].top + glyphs[slot].height) / pixel_scale;
	glyph->advance = glyphs[slot].advance;
	/* (no outline's: its box's) */
	glyph->ink_left = glyphs[slot].ink_right > glyphs[slot].ink_left ? glyphs[slot].ink_left / pixel_scale :
		glyph->left;
	glyph->ink_right = glyphs[slot].ink_right > glyphs[slot].ink_left ? glyphs[slot].ink_right / pixel_scale :
		glyph->right;
	glyph->u0 = (float)glyphs[slot].x * placeholder_width / ATLAS_SIZE;
	glyph->v0 = (float)glyphs[slot].y * placeholder_height / ATLAS_SIZE;
	glyph->u1 = (float)(glyphs[slot].x + glyphs[slot].width) * placeholder_width / ATLAS_SIZE;
	glyph->v1 = (float)(glyphs[slot].y + glyphs[slot].height) * placeholder_height / ATLAS_SIZE;
	return 1;
}

float text_hires_advance(long font, unsigned long code)
{
	struct font_face *face;
	int index, advance = 0, bearing = 0;

	if (font < 0 || font >= font_count || fonts[font].faces[0].scale <= 0.0f)
		return -1.0f;
	face = font_glyph(font, code, &index);
	stbtt_GetGlyphHMetrics(&face->info, index, &advance, &bearing);
	return advance * face->scale * face->narrowing;
}

float text_hires_kerning(long font, unsigned long first, unsigned long second)
{
	struct font_face *face;
	int first_index, second_index;

	if (font < 0 || font >= font_count || fonts[font].faces[0].scale <= 0.0f)
		return 0.0f;
	face = font_glyph(font, first, &first_index);
	if (!first_index || face != font_glyph(font, second, &second_index) || !second_index)
		return 0.0f;
	if (!face->info.kern && !face->info.gpos)
		return 0.0f;
	return stbtt_GetGlyphKernAdvance(&face->info, first_index, second_index) * face->scale * face->narrowing;
}

void text_hires_register_atlas(const unsigned long *texture, unsigned long width, unsigned long height)
{
	placeholder_data = texture ? texture[1] : 0;
	placeholder_width = width;
	placeholder_height = height;
}

unsigned int text_hires_atlas_texture(unsigned long data)
{
	if (!data || data != placeholder_data || !atlas)
		return 0;
	/* (the host reads the atlas where it is, again once it has changed; the
	image owns it, and frees it once it is deleted and done with) */
	if (!atlas_texture)
	{
		atlas_texture = xgpu_image_create(atlas, ATLAS_SIZE, ATLAS_SIZE, 1, XGPU_IMAGE_COVERAGE, TRUE);
		if (!atlas_texture)
		{
			atlas = NULL;
			return 0;
		}
		atlas_changed = 0;
	}
	if (atlas_changed)
	{
		xgpu_image_changed(atlas_texture);
		atlas_changed = 0;
	}
	return atlas_texture;
}

unsigned char *text_hires_line(char const *tag_name, char const *text, float cap_pixels,
	int *width, int *height)
{
	stbtt_fontinfo info;
	const unsigned char *data = NULL;
	unsigned char *pixels;
	unsigned int index;
	int x0, y0, x1, y1, pass, w = 0, h, pen, baseline;
	float scale;

	*width = *height = 0;
	if (!tag_name || !text || cap_pixels <= 0.0f || cap_pixels > 512.0f)
		return NULL;
	for (index = 0; index < text_hires_embedded_count && !data; index++)
		if (text_hires_embedded[index].tag && !names_differ(text_hires_embedded[index].tag, tag_name))
			data = (const unsigned char *)embedded_assets_data(text_hires_embedded[index].data_offset,
				text_hires_embedded[index].size);
	if (!data || !stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0)) ||
		!stbtt_GetCodepointBox(&info, 'H', &x0, &y0, &x1, &y1) || y1 <= y0)
		return NULL;
	scale = cap_pixels / (float)(y1 - y0);
	/* (room above the capitals for accents, below the baseline for
	descenders: the font's own ascent and descent are not to be trusted
	for it, OpenCE's being smaller than its capitals) */
	baseline = (int)ceilf(cap_pixels * 1.3f);
	h = baseline + (int)ceilf(cap_pixels * 0.4f) + 1;
	/* (twice: the width, then the glyphs) */
	pixels = NULL;
	for (pass = 0; pass < 2; pass++)
	{
		const unsigned char *p = (const unsigned char *)text;
		float x = 1.0f;
		int previous = 0;

		while (*p)
		{
			int code = *p++, advance, bearing;

			/* (UTF-8's two- and three-byte letters; others as Latin-1) */
			if ((code & 0xe0) == 0xc0 && (p[0] & 0xc0) == 0x80)
				code = ((code & 0x1f) << 6) | (*p++ & 0x3f);
			else if ((code & 0xf0) == 0xe0 && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80)
			{
				code = ((code & 0x0f) << 12) | ((p[0] & 0x3f) << 6) | (p[1] & 0x3f);
				p += 2;
			}
			if (previous)
				x += (float)stbtt_GetCodepointKernAdvance(&info, previous, code) * scale;
			stbtt_GetCodepointHMetrics(&info, code, &advance, &bearing);
			if (pass)
			{
				int left, top, right, bottom;
				float shift = x - floorf(x);

				unsigned char *glyph;

				stbtt_GetCodepointBitmapBoxSubpixel(&info, code, scale, scale, shift, 0.0f, &left, &top, &right, &bottom);
				pen = (int)floorf(x) + left;
				glyph = right > left && bottom > top ? calloc((size_t)(right - left) * (bottom - top), 1) : NULL;
				if (glyph)
				{
					int gx, gy;

					stbtt_MakeCodepointBitmapSubpixel(&info, glyph, right - left, bottom - top, right - left,
						scale, scale, shift, 0.0f, code);
					/* (each pixel the most of the glyphs over it, the line's
					edges clipping them) */
					for (gy = 0; gy < bottom - top; gy++)
						for (gx = 0; gx < right - left; gx++)
						{
							int px = pen + gx, py = baseline + top + gy;
							unsigned char value = glyph[gy * (right - left) + gx];

							if (px >= 0 && px < w && py >= 0 && py < h && value > pixels[py * w + px])
								pixels[py * w + px] = value;
						}
					free(glyph);
				}
			}
			x += (float)advance * scale;
			previous = code;
		}
		if (!pass)
		{
			w = (int)ceilf(x) + 2;
			if (w <= 2 || w > 8192)
				return NULL;
			pixels = calloc((size_t)w * (size_t)h, 1);
			if (!pixels)
				return NULL;
		}
	}
	*width = w;
	*height = h;
	return pixels;
}
