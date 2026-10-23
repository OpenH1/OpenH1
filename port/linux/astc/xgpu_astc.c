/*
XGPU_ASTC.C

DXT blocks transcoded to ASTC 4x4 blocks (xgpu_astc.h). A DXT block is two
endpoint colors and a 2-bit index a texel; an ASTC block with one partition
and a weight a texel holds the same: its endpoints (8 bits a channel, as
RGB565 expanded) and its weights (0, 1/3, 2/3 and 1 of the way, or 0, 1/2
and 1 for DXT1's three-color blocks) give the DXT block's colors, but for
ASTC's thirds, 21/64 and 43/64, within a third of a step of 8 bits. So:

- a DXT1 block of four colors, or of three without its transparent black:
  RGB endpoints (CEM 8), weights of four or three values;
- a DXT1 block wholly transparent: a constant block of transparent black;
- a DXT1 block of transparent black and one color: RGBA endpoints (CEM 12),
  transparent black and the color, weights of three values (the first and
  the last);
- a DXT3 or DXT5 block whose alpha is the same at every texel: RGBA
  endpoints with that alpha, weights of four values.

The others (transparent black among two or more colors, alpha that varies
across the block) are left to the encoder: a weight a texel for color and
another for alpha (ASTC's two planes) leaves too few bits for the endpoints
and for DXT5's eight alpha levels or DXT3's sixteen.

The block's layout is the ASTC specification's (Khronos Data Format
Specification, "ASTC Compressed Texture Image Formats"): the block mode in
bits 0 to 10 (4x4 weights: 0x042 four values, 0x051 three), the
partitions less one in 11 and 12 (0), the endpoint mode in 13 to 16, the
endpoints from bit 17 (8 bits each, the mode's values in order: r0 r1 g0
g1 b0 b1 a0 a1), the weights from bit 127 downwards, bit-reversed, in the
integer sequence encoding (three values: five weights to 8 bits). RGB and
RGBA endpoints are swapped by the decoder (and blue contracted) when the
second's R+G+B is less than the first's: the transcoder orders them, and
reverses the weights, so that it never is.
*/

#include "xgpu_astc.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---------- constants */

enum
{
	/* the 4x4 weight grid's block modes, by the weights' values (not two:
	16 bits of weights, fewer than the 24 a block's must be) */
	_mode_weights_3 = 0x051,
	_mode_weights_4 = 0x042,
	/* the endpoint modes: LDR RGB and RGBA, direct */
	_endpoints_rgb = 8,
	_endpoints_rgba = 12,
};

/* ---------- the DXT blocks */

unsigned xgpu_astc_dxt_block_bytes(int kind)
{
	return kind == XGPU_ASTC_DXT1 ? 8 : 16;
}

/* (in any case, as the game's settings take a word) */
int xgpu_astc_chosen(const char *setting, int block_compression, int astc)
{
	if (!astc || (setting && !strcasecmp(setting, "off")))
		return 0;
	return (setting && !strcasecmp(setting, "always")) || !block_compression;
}

struct color_block
{
	/* the endpoints' RGB565 values and RGB (8 bits), the indices */
	unsigned c0, c1;
	uint8_t rgb[2][3];
	uint32_t indices;
};

static void color_block_read(const uint8_t *block, struct color_block *colors)
{
	int endpoint;

	colors->c0 = block[0] | (block[1] << 8);
	colors->c1 = block[2] | (block[3] << 8);
	colors->indices = block[4] | (block[5] << 8) | ((uint32_t)block[6] << 16) | ((uint32_t)block[7] << 24);
	for (endpoint = 0; endpoint < 2; endpoint++)
	{
		unsigned value = endpoint ? colors->c1 : colors->c0;
		unsigned r = value >> 11, g = (value >> 5) & 0x3f, b = value & 0x1f;

		colors->rgb[endpoint][0] = (uint8_t)((r << 3) | (r >> 2));
		colors->rgb[endpoint][1] = (uint8_t)((g << 2) | (g >> 4));
		colors->rgb[endpoint][2] = (uint8_t)((b << 3) | (b >> 2));
	}
}

/* a DXT3 or DXT5 block's alpha at each texel */
static void alpha_block_read(int kind, const uint8_t *block, uint8_t alpha[16])
{
	int index;

	if (kind == XGPU_ASTC_DXT3)
	{
		for (index = 0; index < 16; index++)
		{
			unsigned value = (block[index / 2] >> ((index & 1) * 4)) & 0xf;

			alpha[index] = (uint8_t)(value * 17);
		}
	}
	else
	{
		unsigned a0 = block[0], a1 = block[1], values[8];
		uint64_t bits = 0;

		for (index = 0; index < 6; index++)
			bits |= (uint64_t)block[2 + index] << (index * 8);
		values[0] = a0;
		values[1] = a1;
		if (a0 > a1)
		{
			for (index = 2; index < 8; index++)
				values[index] = ((8 - index) * a0 + (index - 1) * a1) / 7;
		}
		else
		{
			for (index = 2; index < 6; index++)
				values[index] = ((6 - index) * a0 + (index - 1) * a1) / 5;
			values[6] = 0;
			values[7] = 255;
		}
		for (index = 0; index < 16; index++)
			alpha[index] = (uint8_t)values[(bits >> (index * 3)) & 7];
	}
}

/* as port/linux/src/xbox_texture_decode.c decodes them (the texels the
renderers drew from a decoded DXT texture) */
void xgpu_astc_decode_dxt(int kind, const uint8_t *block, uint8_t rgba[64])
{
	struct color_block colors;
	uint8_t palette[4][4], alpha[16];
	int index, channel;

	color_block_read(kind == XGPU_ASTC_DXT1 ? block : block + 8, &colors);
	for (channel = 0; channel < 3; channel++)
	{
		unsigned a = colors.rgb[0][channel], b = colors.rgb[1][channel];

		palette[0][channel] = (uint8_t)a;
		palette[1][channel] = (uint8_t)b;
		if (colors.c0 > colors.c1 || kind != XGPU_ASTC_DXT1)
		{
			palette[2][channel] = (uint8_t)((2 * a + b) / 3);
			palette[3][channel] = (uint8_t)((a + 2 * b) / 3);
		}
		else
		{
			palette[2][channel] = (uint8_t)((a + b) / 2);
			palette[3][channel] = 0;
		}
	}
	palette[0][3] = palette[1][3] = palette[2][3] = 255;
	palette[3][3] = colors.c0 > colors.c1 || kind != XGPU_ASTC_DXT1 ? 255 : 0;
	if (kind != XGPU_ASTC_DXT1)
		alpha_block_read(kind, block, alpha);
	for (index = 0; index < 16; index++)
	{
		const uint8_t *color = palette[(colors.indices >> (index * 2)) & 3];

		rgba[index * 4 + 0] = color[0];
		rgba[index * 4 + 1] = color[1];
		rgba[index * 4 + 2] = color[2];
		rgba[index * 4 + 3] = kind == XGPU_ASTC_DXT1 ? color[3] : alpha[index];
	}
}

/* ---------- the ASTC blocks

A block is built as its low and high 64 bits, then stored little-endian. */

/* five trits' 8 bits in the integer sequence encoding (the specification's
decoding, inverted once) */
static uint8_t trit_packing[243];
/* four DXT indices (a byte of them) as weights of four values: 0, 1/3, 2/3
and 1 of the way from c0 to c1 (the four-color palette's 0, 1, 2, 3 are
c0, c1, 2/3 c0 + 1/3 c1, 1/3 c0 + 2/3 c1) */
static uint8_t four_weights[256];

static void trits_unpack(unsigned packed, unsigned trits[5])
{
	unsigned c;

	if (((packed >> 2) & 7) == 7)
	{
		c = (((packed >> 5) & 7) << 2) | (packed & 3);
		trits[4] = 2;
		trits[3] = 2;
	}
	else
	{
		c = packed & 0x1f;
		if (((packed >> 5) & 3) == 3)
		{
			trits[4] = 2;
			trits[3] = (packed >> 7) & 1;
		}
		else
		{
			trits[4] = (packed >> 7) & 1;
			trits[3] = (packed >> 5) & 3;
		}
	}
	if ((c & 3) == 3)
	{
		trits[2] = 2;
		trits[1] = (c >> 4) & 1;
		trits[0] = (((c >> 3) & 1) << 1) | ((c >> 2) & 1 & ~(c >> 3) & 1);
	}
	else if (((c >> 2) & 3) == 3)
	{
		trits[2] = 2;
		trits[1] = 2;
		trits[0] = c & 3;
	}
	else
	{
		trits[2] = (c >> 4) & 1;
		trits[1] = (c >> 2) & 3;
		trits[0] = (((c >> 1) & 1) << 1) | (c & 1 & ~(c >> 1) & 1);
	}
}

static void tables_make(void)
{
	static const uint8_t four[4] = { 0, 3, 1, 2 };
	static int made;
	uint8_t found[243];
	int packed, byte;

	if (made)
		return;
	memset(found, 0, sizeof(found));
	/* (the lowest packing of each: five trits' with the last zero leave the
	top bits zero, as a sequence's last group of fewer needs) */
	for (packed = 0; packed < 256; packed++)
	{
		unsigned trits[5], number;

		trits_unpack((unsigned)packed, trits);
		number = trits[0] + 3 * (trits[1] + 3 * (trits[2] + 3 * (trits[3] + 3 * trits[4])));
		if (!found[number])
		{
			found[number] = 1;
			trit_packing[number] = (uint8_t)packed;
		}
	}
	for (byte = 0; byte < 256; byte++)
	{
		four_weights[byte] = (uint8_t)(four[byte & 3] | four[(byte >> 2) & 3] << 2 | four[(byte >> 4) & 3] << 4 |
			four[(byte >> 6) & 3] << 6);
	}
	made = 1;
}

static uint32_t bits_reversed(uint32_t value)
{
	value = ((value >> 1) & 0x55555555u) | ((value & 0x55555555u) << 1);
	value = ((value >> 2) & 0x33333333u) | ((value & 0x33333333u) << 2);
	value = ((value >> 4) & 0x0f0f0f0fu) | ((value & 0x0f0f0f0fu) << 4);
	value = ((value >> 8) & 0x00ff00ffu) | ((value & 0x00ff00ffu) << 8);
	return (value >> 16) | (value << 16);
}

static void block_store(uint8_t astc[16], uint64_t low, uint64_t high)
{
	int byte;

	for (byte = 0; byte < 8; byte++)
	{
		astc[byte] = (uint8_t)(low >> (byte * 8));
		astc[8 + byte] = (uint8_t)(high >> (byte * 8));
	}
}

/* 16 weights of three values as their integer sequence (the stream's
first bit lowest): five to 8 bits, the last one's 2 */
static uint32_t trits_stream(const uint8_t weights[16])
{
	uint32_t stream = 0;
	int group;

	for (group = 0; group < 3; group++)
	{
		const uint8_t *trits = weights + group * 5;

		stream |= (uint32_t)trit_packing[trits[0] + 3 * (trits[1] + 3 * (trits[2] + 3 * (trits[3] + 3 * trits[4])))] <<
			(group * 8);
	}
	return stream | (uint32_t)trit_packing[weights[15]] << 24;
}

/* an ASTC block of one partition: endpoints (count values, 8 bits each),
16 weights of values levels (3 or 4) as their stream */
static void block_write(uint8_t astc[16], unsigned endpoint_mode, const uint8_t *endpoints, unsigned endpoint_count,
	uint32_t weights, unsigned values)
{
	uint64_t low = (values == 3 ? _mode_weights_3 : _mode_weights_4) | (uint64_t)endpoint_mode << 13, high;
	unsigned index;

	/* (the weights from the top, bit-reversed) */
	high = (uint64_t)bits_reversed(weights) << 32;
	for (index = 0; index < endpoint_count; index++)
	{
		unsigned position = 17 + index * 8;
		uint64_t value = endpoints[index];

		if (position + 8 <= 64)
			low |= value << position;
		else if (position >= 64)
			high |= value << (position - 64);
		else
		{
			low |= value << position;
			high |= value >> (64 - position);
		}
	}
	block_store(astc, low, high);
}

/* a constant block of RGBA (8 bits a channel; the block's are 16) */
static void constant_block_write(uint8_t astc[16], const uint8_t rgba[4])
{
	/* (void extent, LDR; its reserved bits set; no extent: all ones) */
	uint64_t low = 0x1fcu | 3u << 10 | (uint64_t)0x1fff << 12 | (uint64_t)0x1fff << 25 | (uint64_t)0x1fff << 38 |
		(uint64_t)0x1fff << 51;
	uint64_t high = 0;
	int channel;

	for (channel = 0; channel < 4; channel++)
		high |= (uint64_t)(rgba[channel] * 257u) << (channel * 16);
	block_store(astc, low, high);
}

/* the block's mean color, a stand-in until it is encoded */
static void mean_block_write(int kind, const uint8_t *dxt, uint8_t astc[16])
{
	uint8_t texels[64], mean[4];
	int channel, index;

	xgpu_astc_decode_dxt(kind, dxt, texels);
	for (channel = 0; channel < 4; channel++)
	{
		unsigned sum = 0;

		for (index = 0; index < 16; index++)
			sum += texels[index * 4 + channel];
		mean[channel] = (uint8_t)((sum + 8) / 16);
	}
	constant_block_write(astc, mean);
}

/* whether RGB (or RGBA) endpoints are to be swapped for the decoder: the
second's R+G+B less than the first's */
static int endpoints_swapped(const uint8_t rgb[2][3])
{
	return rgb[1][0] + rgb[1][1] + rgb[1][2] < rgb[0][0] + rgb[0][1] + rgb[0][2];
}

/* the endpoints' values in order (r0 r1 g0 g1 b0 b1, a0 a1 with alpha),
swapped or not */
static unsigned endpoints_order(uint8_t endpoints[8], const uint8_t rgb[2][3], const uint8_t *alpha, int swapped)
{
	int channel;

	for (channel = 0; channel < 3; channel++)
	{
		endpoints[channel * 2] = rgb[swapped][channel];
		endpoints[channel * 2 + 1] = rgb[!swapped][channel];
	}
	if (!alpha)
		return 6;
	endpoints[6] = alpha[swapped];
	endpoints[7] = alpha[!swapped];
	return 8;
}

/* the four-color palette's indices as a stream of weights of four values,
reversed (3 less each) when the endpoints are swapped */
static uint32_t four_stream(uint32_t indices, int swapped)
{
	uint32_t stream = four_weights[indices & 0xff] | (uint32_t)four_weights[(indices >> 8) & 0xff] << 8 |
		(uint32_t)four_weights[(indices >> 16) & 0xff] << 16 | (uint32_t)four_weights[indices >> 24] << 24;

	return swapped ? ~stream : stream;
}

/* a DXT1 block as ASTC; zero if it cannot be exactly */
static int dxt1_transcode(const uint8_t *dxt, uint8_t astc[16])
{
	/* the three-color palette's 0, 1, 2 (c0, c1, half of each) as weights
	of three values */
	static const uint8_t three[4] = { 0, 2, 1, 0 };
	struct color_block colors;
	uint8_t endpoints[8], weights[16];
	uint32_t low_bits, high_bits;
	unsigned used, count;
	int index, swapped;

	color_block_read(dxt, &colors);
	if (colors.c0 > colors.c1)
	{
		swapped = endpoints_swapped((const uint8_t (*)[3])colors.rgb);
		count = endpoints_order(endpoints, (const uint8_t (*)[3])colors.rgb, NULL, swapped);
		block_write(astc, _endpoints_rgb, endpoints, count, four_stream(colors.indices, swapped), 4);
		return 1;
	}
	/* (the indices used: 0, 1, 2, 3 as bits) */
	low_bits = colors.indices & 0x55555555u;
	high_bits = (colors.indices >> 1) & 0x55555555u;
	used = ((~low_bits & ~high_bits & 0x55555555u) ? 1 : 0) | ((low_bits & ~high_bits) ? 2 : 0) |
		((~low_bits & high_bits & 0x55555555u) ? 4 : 0) | ((low_bits & high_bits) ? 8 : 0);
	if (!(used & 8))
	{
		swapped = endpoints_swapped((const uint8_t (*)[3])colors.rgb);
		for (index = 0; index < 16; index++)
		{
			uint8_t weight = three[(colors.indices >> (index * 2)) & 3];

			weights[index] = (uint8_t)(swapped ? 2 - weight : weight);
		}
		count = endpoints_order(endpoints, (const uint8_t (*)[3])colors.rgb, NULL, swapped);
		block_write(astc, _endpoints_rgb, endpoints, count, trits_stream(weights), 3);
		return 1;
	}
	if (used == 8)
	{
		static const uint8_t transparent[4] = { 0, 0, 0, 0 };

		constant_block_write(astc, transparent);
		return 1;
	}
	if (used == (8 | 1) || used == (8 | 2) || used == (8 | 4))
	{
		/* transparent black, and the one color (the mid color for index 2),
		never swapped (black's sum is the least); weights of three values,
		the first and the last of them: a 4x4 grid of two values' is 16 bits,
		fewer than a block's weights may be */
		static const uint8_t alpha[2] = { 0, 255 };
		uint8_t rgb[2][3];
		int channel, color = used & 1 ? 0 : used & 2 ? 1 : 2;

		for (channel = 0; channel < 3; channel++)
		{
			rgb[0][channel] = 0;
			rgb[1][channel] = (uint8_t)(color == 2 ? (colors.rgb[0][channel] + colors.rgb[1][channel]) / 2 :
				colors.rgb[color][channel]);
		}
		for (index = 0; index < 16; index++)
			weights[index] = ((colors.indices >> (index * 2)) & 3) != 3 ? 2 : 0;
		count = endpoints_order(endpoints, (const uint8_t (*)[3])rgb, alpha, 0);
		block_write(astc, _endpoints_rgba, endpoints, count, trits_stream(weights), 3);
		return 1;
	}
	return 0;
}

/* a DXT3 or DXT5 block's alpha where it is the same at every texel; -1
where it varies */
static int alpha_constant(int kind, const uint8_t *dxt)
{
	uint8_t alpha[16];
	uint64_t bits = 0;
	int index;

	for (index = 0; index < 8; index++)
		bits |= (uint64_t)dxt[index] << (index * 8);
	if (kind == XGPU_ASTC_DXT3)
		return bits == (bits & 0xf) * 0x1111111111111111ull ? (int)(bits & 0xf) * 17 : -1;
	/* (DXT5: one index throughout is one alpha; others can be too, where
	the endpoints are the same) */
	if ((bits >> 16) != ((bits >> 16) & 7) * 0x249249249249ull && dxt[0] != dxt[1])
		return -1;
	alpha_block_read(kind, dxt, alpha);
	for (index = 1; index < 16; index++)
	{
		if (alpha[index] != alpha[0])
			return -1;
	}
	return alpha[0];
}

/* a DXT3 or DXT5 block as ASTC; zero if it cannot be exactly */
static int dxt_alpha_transcode(int kind, const uint8_t *dxt, uint8_t astc[16])
{
	struct color_block colors;
	uint8_t endpoints[8], endpoint_alpha[2];
	unsigned count;
	int alpha = alpha_constant(kind, dxt), swapped;

	if (alpha < 0)
		return 0;
	/* (always four colors: DXT3's and DXT5's color blocks have no
	three-color mode) */
	color_block_read(dxt + 8, &colors);
	endpoint_alpha[0] = endpoint_alpha[1] = (uint8_t)alpha;
	swapped = endpoints_swapped((const uint8_t (*)[3])colors.rgb);
	count = endpoints_order(endpoints, (const uint8_t (*)[3])colors.rgb, endpoint_alpha, swapped);
	block_write(astc, _endpoints_rgba, endpoints, count, four_stream(colors.indices, swapped), 4);
	return 1;
}

uint32_t xgpu_astc_transcode(int kind, const uint8_t *dxt, uint32_t count, uint8_t *astc, uint8_t *encode)
{
	unsigned block_bytes = xgpu_astc_dxt_block_bytes(kind);
	uint32_t index, left = 0;

	tables_make();
	for (index = 0; index < count; index++, dxt += block_bytes, astc += XGPU_ASTC_BLOCK_BYTES)
	{
		int exact = kind == XGPU_ASTC_DXT1 ? dxt1_transcode(dxt, astc) : dxt_alpha_transcode(kind, dxt, astc);

		if (!exact)
		{
			mean_block_write(kind, dxt, astc);
			left++;
		}
		if (encode)
			encode[index] = (uint8_t)!exact;
	}
	return left;
}

/* ---------- transcoded and encoded */

long xgpu_astc_convert(struct xgpu_astc_encoder *encoder, int kind, const uint8_t *dxt, uint32_t count,
	uint8_t *astc, uint8_t *encode)
{
	unsigned block_bytes = xgpu_astc_dxt_block_bytes(kind);
	uint8_t *flags = encode ? encode : malloc(count ? count : 1);
	uint8_t *texels = NULL, *encoded = NULL;
	uint32_t left, index, at;
	long result = -1;

	if (!flags)
		return -1;
	left = xgpu_astc_transcode(kind, dxt, count, astc, flags);
	if (!left)
		result = 0;
	else if (encoder && (texels = malloc((size_t)left * 64)) != NULL &&
		(encoded = malloc((size_t)left * XGPU_ASTC_BLOCK_BYTES)) != NULL)
	{
		for (index = at = 0; index < count; index++)
		{
			if (flags[index])
				xgpu_astc_decode_dxt(kind, dxt + (size_t)index * block_bytes, texels + (size_t)at++ * 64);
		}
		if (xgpu_astc_encode(encoder, texels, left, encoded))
		{
			for (index = at = 0; index < count; index++)
			{
				if (flags[index])
					memcpy(astc + (size_t)index * XGPU_ASTC_BLOCK_BYTES, encoded + (size_t)at++ * XGPU_ASTC_BLOCK_BYTES,
						XGPU_ASTC_BLOCK_BYTES);
			}
			result = (long)left;
		}
	}
	free(texels);
	free(encoded);
	if (flags != encode)
		free(flags);
	return result;
}
