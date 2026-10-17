/* The loading artwork and 640x480 glow, independent of game clocks and
 * memory. Integer filtering and display interpolation are not a bit-exact
 * reproduction of the console framebuffer. */
#ifndef LOADING_ART_H
#define LOADING_ART_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "loading_presentation.h"
#include "loading_art_tables.h"

#define LOADING_ART_WIDTH 640
#define LOADING_ART_HEIGHT 480
#define LOADING_ART_PIXELS (LOADING_ART_WIDTH * LOADING_ART_HEIGHT)
#define LOADING_ART_BYTES (LOADING_ART_PIXELS * 4)
#define LOADING_ART_FRAME_WIDTH LOADING_ART_WIDTH
#define LOADING_ART_FRAME_HEIGHT LOADING_ART_HEIGHT
#define LOADING_ART_FRAME_PIXELS (LOADING_ART_FRAME_WIDTH * LOADING_ART_FRAME_HEIGHT)

struct loading_art_axis
{
	uint16_t index[3], weight[3];
};

struct loading_art
{
	uint8_t color[LOADING_ART_FRAME_PIXELS * 3];
	uint8_t history[2][LOADING_ART_FRAME_PIXELS * 4];
	uint8_t output[LOADING_ART_FRAME_PIXELS * 4];
	struct loading_art_axis x[LOADING_ART_FRAME_WIDTH], y[LOADING_ART_FRAME_HEIGHT];
	uint64_t epoch, step;
	unsigned int valid, started, current;
};

/* Average bilinear samples at +/-0.1 pixels. Two such axes are the four
 * diagonal samples. Weights sum to 2048, including texture-edge clamping. */
static inline void loading_art_axis_set(struct loading_art_axis *axis,
	int coordinate, unsigned int limit)
{
	unsigned int sample, tap, slot;
	int point, index;
	unsigned int fraction, weight;
	memset(axis, 0, sizeof(*axis));
	for (sample = 0; sample < 2; sample++)
	{
		point = coordinate + (sample ? 102 : -102);
		if (point < 0)
			point = 0;
		if (point > (int)((limit - 1) * 1024))
			point = (int)((limit - 1) * 1024);
		index = point / 1024;
		fraction = (unsigned int)point & 1023;
		for (tap = 0; tap < 2; tap++)
		{
			unsigned int pixel = (unsigned int)index + tap;
			if (pixel >= limit)
				pixel = limit - 1;
			weight = tap ? fraction : 1024 - fraction;
			if (!weight)
				continue;
			for (slot = 0; slot < 3; slot++)
				if (!axis->weight[slot] || axis->index[slot] == pixel)
					break;
			if (slot < 3)
			{
				axis->index[slot] = (uint16_t)pixel;
				axis->weight[slot] += (uint16_t)weight;
			}
		}
	}
}

static inline void loading_art_set(struct loading_art *art, const uint8_t *rgba)
{
	unsigned int index, channel;
	memset(art, 0, sizeof(*art));
	if (!rgba)
		return;
	/* Stock fade quad spans +/-129/128. Its source coordinates are
	 * 3..637 horizontally and 0..480 vertically. */
	for (index = 0; index < LOADING_ART_FRAME_WIDTH; index++)
		loading_art_axis_set(&art->x[index], 2560 + (int)(
			(UINT64_C(634) * ((2 * index + 1) * 128 + LOADING_ART_FRAME_WIDTH) * 1024) /
			(LOADING_ART_FRAME_WIDTH * 258)), LOADING_ART_FRAME_WIDTH);
	for (index = 0; index < LOADING_ART_FRAME_HEIGHT; index++)
		loading_art_axis_set(&art->y[index], (int)(
			(UINT64_C(480) * ((2 * index + 1) * 128 + LOADING_ART_FRAME_HEIGHT) * 1024) /
			(LOADING_ART_FRAME_HEIGHT * 258)) - 512, LOADING_ART_FRAME_HEIGHT);
	/* Keep the supplied artwork colors; alpha is opaque in the presentation. */
	for (index = 0; index < LOADING_ART_PIXELS; index++)
		for (channel = 0; channel < 3; channel++)
			art->color[index * 3 + channel] = rgba[index * 4 + channel];
	art->valid = 1;
}

static inline unsigned int loading_art_mask(int x, unsigned int y)
{
	if (x < 0 || x >= 128)
		return 5;
	return loading_art_mask_table[y * 128 + (unsigned int)x];
}

static inline void loading_art_step(struct loading_art *art, unsigned int phase)
{
	const uint8_t *previous = art->history[art->current];
	uint8_t *output = art->history[art->current ^ 1];
	unsigned int x, y, channel, tx, ty, fade, brightness;
	int mask_x, mask_y, ix, iy;
	unsigned int fx, fy, mask, upper, lower, pixel, value, sum[3], weight;
	const uint8_t *sample;
	const struct loading_art_axis *ax, *ay;
	fade = phase <= 216 ? 216 : (240 - phase) * 9;
	brightness = loading_art_brightness[phase];
	for (y = 0; y < LOADING_ART_FRAME_HEIGHT; y++)
	{
		ay = &art->y[y];
		mask_y = (int)((2 * y + 1) * 16 * 128 / LOADING_ART_FRAME_HEIGHT) - 128;
		if (mask_y < 0)
			mask_y = 0;
		if (mask_y > 15 * 256)
			mask_y = 15 * 256;
		iy = mask_y / 256;
		fy = (unsigned int)mask_y & 255;
		for (x = 0; x < LOADING_ART_FRAME_WIDTH; x++)
		{
			ax = &art->x[x];
			mask_x = (int)(x * 256 + 32768) - (int)(phase * 768 * 256 / 240);
			/* Floor division, including the border to the left of the mask. */
			ix = mask_x >= 0 ? mask_x / 256 : -((-mask_x + 255) / 256);
			fx = (unsigned int)(mask_x - ix * 256);
			upper = loading_art_mask(ix, (unsigned int)iy) * (256 - fx) +
				loading_art_mask(ix + 1, (unsigned int)iy) * fx;
			lower = loading_art_mask(ix, iy < 15 ? (unsigned int)iy + 1 : 15) * (256 - fx) +
				loading_art_mask(ix + 1, iy < 15 ? (unsigned int)iy + 1 : 15) * fx;
			mask = (upper * (256 - fy) + lower * fy + 32768) >> 16;
			pixel = y * LOADING_ART_FRAME_WIDTH + x;
			sum[0] = sum[1] = sum[2] = 0;
			for (ty = 0; ty < 3; ty++)
			{
				if (!ay->weight[ty])
					continue;
				for (tx = 0; tx < 3; tx++)
				{
					if (!ax->weight[tx])
						continue;
					weight = (unsigned int)ax->weight[tx] * ay->weight[ty];
					sample = previous + (ay->index[ty] * LOADING_ART_FRAME_WIDTH + ax->index[tx]) * 4;
					sum[0] += sample[0] * weight;
					sum[1] += sample[1] * weight;
					sum[2] += sample[2] * weight;
				}
			}
			for (channel = 0; channel < 3; channel++)
			{
				/* Average and fade before quantizing. The blur shader does
				 * not use the near-white diffuse RGB supplied by the draw. */
				value = (unsigned int)(((uint64_t)sum[channel] * fade + UINT64_C(503316480)) /
					UINT64_C(1006632960));
				value += (art->color[pixel * 3 + channel] * brightness * mask + 32512) / 65025;
				output[pixel * 4 + channel] = (uint8_t)(value < 255 ? value : 255);
			}
			output[pixel * 4 + 3] = 255;
		}
	}
	art->current ^= 1;
}

static inline const uint8_t *loading_art_render(struct loading_art *art,
	const struct loading_presentation_frame *frame)
{
	uint64_t target;
	unsigned int reset, skipped;
	unsigned int pixel, channel, fraction;
	const uint8_t *previous, *current;
	if (!art->valid)
		return NULL;
	target = (frame->elapsed_ns / UINT64_C(1000000000)) * 60 +
		(frame->elapsed_ns % UINT64_C(1000000000)) * 60 / UINT64_C(1000000000);
	reset = !art->started || art->epoch != frame->epoch || target < art->step;
	skipped = art->started && target > art->step && target - art->step > 1;
	if (reset || (target != art->step && target / 240 != art->step / 240))
		memset(art->history, 0, sizeof(art->history));
	/* At most one feedback step, even after a producer or display stall.
	 * Elapsed time selects the sweep phase; no costly fast-forward loop. */
	if (reset || target != art->step)
	{
		loading_art_step(art, (unsigned int)(target % 240));
		if (reset || skipped)
			memcpy(art->history[art->current ^ 1], art->history[art->current],
				sizeof(art->history[0]));
	}
	art->epoch = frame->epoch;
	art->step = target;
	art->started = 1;
	/* Interpolate the fixed-rate histories for each display refresh. This
	 * adds one visual step of delay without changing the feedback clock. */
	fraction = (unsigned int)(((frame->elapsed_ns % UINT64_C(1000000000)) * 60 %
		UINT64_C(1000000000)) * 256 / UINT64_C(1000000000));
	previous = art->history[art->current ^ 1];
	current = art->history[art->current];
	for (pixel = 0; pixel < LOADING_ART_FRAME_PIXELS; pixel++)
	{
		for (channel = 0; channel < 3; channel++)
			art->output[pixel * 4 + channel] = (uint8_t)((previous[pixel * 4 + channel] *
				(256 - fraction) + current[pixel * 4 + channel] * fraction + 128) >> 8);
		art->output[pixel * 4 + 3] = 255;
	}
	return art->output;
}

#endif
