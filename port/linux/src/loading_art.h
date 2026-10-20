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
#include "loading_ring.h"

#define LOADING_ART_WIDTH 640
#define LOADING_ART_HEIGHT 480
#define LOADING_ART_PIXELS (LOADING_ART_WIDTH * LOADING_ART_HEIGHT)
#define LOADING_ART_BYTES (LOADING_ART_PIXELS * 4)
#define LOADING_ART_FRAME_WIDTH LOADING_ART_WIDTH
#define LOADING_ART_FRAME_HEIGHT LOADING_ART_HEIGHT
#define LOADING_ART_FRAME_PIXELS (LOADING_ART_FRAME_WIDTH * LOADING_ART_FRAME_HEIGHT)
/* how much of the menu's scene shows between the sweeps, of 256 */
#ifndef LOADING_ART_SCENE_BASE
#define LOADING_ART_SCENE_BASE 24
#endif
/* what the sweep lights of the scene: its brightness, its detail's edges
and the sweep's light on the ring, of 16 */
#ifndef LOADING_ART_FEED_TONE
#define LOADING_ART_FEED_TONE 10
#endif
#ifndef LOADING_ART_FEED_EDGE
#define LOADING_ART_FEED_EDGE 40
#endif
#ifndef LOADING_ART_FEED_GLINT
#define LOADING_ART_FEED_GLINT 7
#endif
/* the brightest the sweep is fed the scene but in its white specks, of 255,
 * and the brightness from which it is fed less (a third), so that a bright,
 * busy part of the ring does not whiten the feedback where the picture's
 * etching would not */
#ifndef LOADING_ART_FEED_KNEE
#define LOADING_ART_FEED_KNEE 110
#endif
#ifndef LOADING_ART_FEED_LIMIT
#define LOADING_ART_FEED_LIMIT 190
#endif
/* the most red and green there is beside blue in what is shown, of 256 */
#ifndef LOADING_ART_GREY_RED
#define LOADING_ART_GREY_RED 196
#endif
#ifndef LOADING_ART_GREY_GREEN
#define LOADING_ART_GREY_GREEN 228
#endif
/* the picture's hues (loading_art_tint_set) */
#define LOADING_ART_TINTS 16
/* the scene's specks: of 128 detailed pixels (their edges past this), how many */
#ifndef LOADING_ART_SPECKS
#define LOADING_ART_SPECKS 4
#endif
#ifndef LOADING_ART_SPECK_EDGE
#define LOADING_ART_SPECK_EDGE 40
#endif
/* the progress from which a load is complete, and how long the picture
then takes to fade out, in nanoseconds */
#define LOADING_ART_COMPLETE 0.995f
#define LOADING_ART_FADE_OUT_NS UINT64_C(500000000)
/* how long VR's depth takes to come in: the first sweep, in nanoseconds */
#define LOADING_ART_REVEAL_NS UINT64_C(4000000000)
/* and how long it takes to come up as a load begins, in nanoseconds */
#define LOADING_ART_SCENE_FADE_NS UINT64_C(400000000)

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
	/* the menu's scene (loading_ring.h), when the presenter has one: drawn
	in the picture's place, in the picture's hue, with the picture's words
	over it (their coverage, text), and what the sweep lights of it (feed);
	VR's right eye's too, and its picture (stereo) */
	struct loading_ring ring;
	uint8_t scene[LOADING_ART_FRAME_PIXELS * 3];
	uint8_t feed[LOADING_ART_FRAME_PIXELS * 3];
	uint8_t text[LOADING_ART_FRAME_PIXELS];
	uint8_t scene_right[LOADING_ART_FRAME_PIXELS * 3];
	uint8_t output_right[LOADING_ART_FRAME_PIXELS * 4];
	/* the load's progress (negative: none told this load), and when it
	completed (the fade out's start) */
	float progress;
	uint64_t progress_epoch, completed_ns;
	unsigned int text_valid, stereo, eyes_valid, composed;
	/* the feedback's rows blurred across (loading_art_row) */
	uint32_t rows[4][LOADING_ART_FRAME_WIDTH * 3];
	unsigned int row_tags[4], row_next;
	float eyes[2][3];
	/* the picture's cumulative histogram, and the scene's brightness
	matched to it (loading_art_histogram, loading_art_tone) */
	uint32_t histogram[256];
	uint8_t tone[256], tints[LOADING_ART_TINTS][256][3];
	unsigned int histogram_valid, tone_valid;
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

/* The picture's colours: a narrow band of hues about its blue, from cyan
 * to violet (LOADING_ART_TINTS steps of LOADING_ART_TINT_STEP degrees from
 * LOADING_ART_TINT_FIRST), by brightness: black, deep navy, the blue, and
 * nearly white at the brightest, as its etching goes. */
#ifndef LOADING_ART_TINT_FIRST
#define LOADING_ART_TINT_FIRST 198
#endif
#ifndef LOADING_ART_TINT_STEP
#define LOADING_ART_TINT_STEP 3
#endif
/* the band's middle, where a colourless surface is */
#ifndef LOADING_ART_TINT_MIDDLE
#define LOADING_ART_TINT_MIDDLE 9
#endif
/* how far the ring's own colour moves it along the band (its channels'
 * differences over this, steps) */
#ifndef LOADING_ART_TINT_SPREAD
#define LOADING_ART_TINT_SPREAD 40
#endif
/* of 8 pixels, how many are not moved toward violet by its grain (2 of
 * them toward cyan) */
#ifndef LOADING_ART_TINT_WANDER
#define LOADING_ART_TINT_WANDER 6
#endif
/* the saturation of the band's colours, of 256, below the brightness where
 * they begin to whiten */
#ifndef LOADING_ART_TINT_SATURATION
#define LOADING_ART_TINT_SATURATION 176
#endif
#ifndef LOADING_ART_TINT_WHITEN
#define LOADING_ART_TINT_WHITEN 185
#endif

static inline void loading_art_tint_set(unsigned int tint, unsigned int level, uint8_t *rgb)
{
	unsigned int hue = LOADING_ART_TINT_FIRST + tint * LOADING_ART_TINT_STEP; /* degrees */
	unsigned int saturation = level <= LOADING_ART_TINT_WHITEN ? LOADING_ART_TINT_SATURATION :
		LOADING_ART_TINT_SATURATION - (LOADING_ART_TINT_SATURATION - 24) * (level - LOADING_ART_TINT_WHITEN) /
		(255 - LOADING_ART_TINT_WHITEN);
	unsigned int sector = hue / 60 % 6, fraction = (hue % 60) * 256 / 60;
	unsigned int p = level * (256 - saturation) >> 8;
	unsigned int q = level * (256 - (saturation * fraction >> 8)) >> 8;
	unsigned int t = level * (256 - (saturation * (256 - fraction) >> 8)) >> 8;
	unsigned int r, g, b;
	switch (sector)
	{
	case 0: r = level; g = t; b = p; break;
	case 1: r = q; g = level; b = p; break;
	case 2: r = p; g = level; b = t; break;
	case 3: r = p; g = q; b = level; break;
	case 4: r = t; g = p; b = level; break;
	default: r = level; g = p; b = q; break;
	}
	rgb[0] = (uint8_t)(r < 255 ? r : 255);
	rgb[1] = (uint8_t)(g < 255 ? g : 255);
	rgb[2] = (uint8_t)(b < 255 ? b : 255);
}

/* Where in the band a pixel of the scene is: the ring's own colour turned
 * into it (a blue surface toward violet, a green one toward cyan, a grey one
 * at its middle), and, here and there in its detail, a speck of cyan,
 * lavender or near white, as the picture's etching has, at texels the
 * ring's texture picks (so that they move with it, not with the screen). */
static inline unsigned int loading_art_tint(uint32_t value, int edge, int *level, int *white)
{
	int r = (int)(value & 255), g = (int)((value >> 8) & 255), b = (int)((value >> 16) & 255);
	int tint = LOADING_ART_TINT_MIDDLE + (b - g) / LOADING_ART_TINT_SPREAD + (r - g) * 3 / (LOADING_ART_TINT_SPREAD * 2);
	unsigned int grain = ((unsigned int)r * 7u + (unsigned int)g * 13u + (unsigned int)b * 29u) & 127u;
	/* (the picture's grain: its hue wanders a step either way, pixel by
	pixel, as its etching's does) */
	unsigned int wander = ((unsigned int)r * 3u + (unsigned int)g * 5u + (unsigned int)b * 11u) >> 1 & 7u;
	tint += wander < 2 ? -1 : wander < LOADING_ART_TINT_WANDER ? 0 : 1;
	if (edge > LOADING_ART_SPECK_EDGE && grain < LOADING_ART_SPECKS)
	{
		if (grain % 3 == 0)
			tint = 0;
		else if (grain % 3 == 1)
			tint = LOADING_ART_TINTS - 1;
		else
		{
			*level += 96;
			*white = 1;
		}
	}
	return (unsigned int)(tint < 0 ? 0 : tint >= LOADING_ART_TINTS ? LOADING_ART_TINTS - 1 : tint);
}

static inline void loading_art_axes(struct loading_art *art)
{
	unsigned int index;
	/* (and the picture's colours, loading_art_tint_set's) */
	for (index = 0; index < LOADING_ART_TINTS * 256; index++)
		loading_art_tint_set(index / 256, index % 256, art->tints[index / 256][index % 256]);
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
}

/* The words of the supplied picture ("LOADING..."), bright blue over its
 * darker ring: their coverage in the box they are drawn in. */
#define LOADING_ART_TEXT_LEFT 232
#define LOADING_ART_TEXT_TOP 220
#define LOADING_ART_TEXT_RIGHT 408
#define LOADING_ART_TEXT_BOTTOM 256

static inline void loading_art_text(struct loading_art *art, const uint8_t *rgba)
{
	unsigned int x, y;
	memset(art->text, 0, sizeof(art->text));
	for (y = LOADING_ART_TEXT_TOP; y < LOADING_ART_TEXT_BOTTOM; y++)
		for (x = LOADING_ART_TEXT_LEFT; x < LOADING_ART_TEXT_RIGHT; x++)
		{
			unsigned int pixel = y * LOADING_ART_FRAME_WIDTH + x, blue = rgba[pixel * 4 + 2];
			art->text[pixel] = (uint8_t)(blue <= 130 ? 0 : blue >= 210 ? 255 : (blue - 130) * 255 / 80);
		}
	art->text_valid = 1;
}

static inline unsigned int loading_art_luminance(unsigned int r, unsigned int g, unsigned int b)
{
	return (r * 54 + g * 183 + b * 19) >> 8;
}

/* How much of the picture is at most each brightness (its cumulative
 * histogram, of 65536), for the scene's brightness to match. */
static inline void loading_art_histogram(struct loading_art *art, const uint8_t *rgba)
{
	uint32_t count[256], total = 0;
	unsigned int index, level;
	memset(count, 0, sizeof(count));
	for (index = 0; index < LOADING_ART_PIXELS; index++)
		count[loading_art_luminance(rgba[index * 4], rgba[index * 4 + 1], rgba[index * 4 + 2])]++;
	for (level = 0; level < 256; level++)
	{
		total += count[level];
		art->histogram[level] = (uint32_t)((uint64_t)total * 65536 / LOADING_ART_PIXELS);
	}
	art->histogram_valid = 1;
}

static inline void loading_art_set(struct loading_art *art, const uint8_t *rgba)
{
	unsigned int index, channel, stereo = art->stereo;
	struct loading_ring ring = art->ring;
	memset(art, 0, sizeof(*art));
	art->ring = ring;
	art->stereo = stereo;
	loading_art_axes(art);
	if (!rgba)
		return;
	/* Keep the supplied artwork colors; alpha is opaque in the presentation. */
	for (index = 0; index < LOADING_ART_PIXELS; index++)
		for (channel = 0; channel < 3; channel++)
			art->color[index * 3 + channel] = rgba[index * 4 + channel];
	loading_art_text(art, rgba);
	loading_art_histogram(art, rgba);
	art->valid = 1;
}

/* The menu's scene (loading_scene.h) to draw in the picture's place; NULL
 * (or a block that is not one) returns to the picture. */
static inline int loading_art_scene(struct loading_art *art, const void *data, size_t bytes)
{
	int valid = loading_ring_set(&art->ring, data, bytes);
	loading_art_axes(art);
	art->started = 0;
	return valid;
}

/* VR's stereo pair (loading_presentation.h): with a scene, each render
 * also draws the right eye's picture (output_right), the left eye's being
 * output; the sweep's light, one for both, lies on the window. */
static inline void loading_art_stereo(struct loading_art *art, unsigned int stereo)
{
	if (art->stereo != stereo)
	{
		art->stereo = stereo;
		art->started = 0;
	}
}

/* VR: where the eyes are, in metres from the window's middle (right, up,
 * out toward the head: loading_ring_draw's), as the frame being drawn
 * predicts them. Without them, the eyes are taken to be level before the
 * window's middle at its distance. */
static inline void loading_art_stereo_eyes(struct loading_art *art, const float *left, const float *right)
{
	unsigned int axis;
	for (axis = 0; axis < 3; axis++)
	{
		art->eyes[0][axis] = left[axis];
		art->eyes[1][axis] = right[axis];
	}
	art->eyes_valid = 1;
}

/* The eyes a frame is drawn from: the memory first, as the picture was on
 * a screen (both eyes at the window's middle: no depth), then, as the first
 * sweep crosses, the eyes come apart and follow the head, and the ring is
 * found deep behind the glass. */
static inline void loading_art_eyes(const struct loading_art *art, uint64_t elapsed_ns, float eyes[2][3])
{
	float reveal = elapsed_ns >= LOADING_ART_REVEAL_NS ? 1.0f : (float)elapsed_ns / (float)LOADING_ART_REVEAL_NS;
	unsigned int eye, axis;
	reveal = reveal * reveal * (3.0f - 2.0f * reveal);
	for (eye = 0; eye < 2; eye++)
		for (axis = 0; axis < 3; axis++)
		{
			float middle = axis == 2 ? LOADING_PRESENTATION_VR_DISTANCE : 0.0f;
			float place = art->eyes_valid ? art->eyes[eye][axis] :
				axis == 0 ? (eye ? 0.5f : -0.5f) * LOADING_RING_VR_EYES : middle;
			eyes[eye][axis] = middle + (place - middle) * reveal;
		}
}

/* VR: the tangent of half the angle the window is wide from the eyes' middle
 * (the menu's camera's field of view), or 0 without a scene. */
static inline float loading_art_stereo_tangent(const struct loading_art *art)
{
	return art->ring.valid && art->ring.focal > 0.0f ? 0.5f / art->ring.focal : 0.0f;
}

/* The load's progress, 0 to 1, for the load being drawn: as it completes
 * the picture fades out to black, as the original's did (its colour the
 * sine of the progress), so that the level comes up out of the dark. */
static inline void loading_art_progress(struct loading_art *art, float progress)
{
	art->progress = progress == progress ? (progress < 0.0f ? 0.0f : progress > 1.0f ? 1.0f : progress) : 0.0f;
	art->progress_epoch = art->epoch;
}

/* How much of the picture is left as the load completes, of 256. */
static inline unsigned int loading_art_completion(struct loading_art *art,
	const struct loading_presentation_frame *frame)
{
	uint64_t since;
	if (art->progress_epoch != frame->epoch || art->progress < LOADING_ART_COMPLETE)
	{
		art->completed_ns = 0;
		return 256;
	}
	if (!art->completed_ns)
		art->completed_ns = frame->elapsed_ns ? frame->elapsed_ns : 1;
	since = frame->elapsed_ns > art->completed_ns ? frame->elapsed_ns - art->completed_ns : 0;
	return since >= LOADING_ART_FADE_OUT_NS ? 0 : (unsigned int)(256 - since * 256 / LOADING_ART_FADE_OUT_NS);
}

/* Whether the last render drew the stereo pair. */
static inline int loading_art_stereo_drawn(const struct loading_art *art)
{
	return art->stereo && art->ring.valid && art->started;
}

/* The scene's brightness to the picture's: the scene's cumulative
 * histogram matched to the picture's, as a load's first frame has it. */
static inline void loading_art_tone(struct loading_art *art, const uint32_t *pixels)
{
	uint32_t count[256], total = 0;
	unsigned int index, level, target = 0;
	memset(count, 0, sizeof(count));
	for (index = 0; index < LOADING_ART_PIXELS; index++)
		count[loading_art_luminance(pixels[index] & 255, (pixels[index] >> 8) & 255, (pixels[index] >> 16) & 255)]++;
	for (level = 0; level < 256; level++)
	{
		uint32_t share;
		total += count[level];
		share = (uint32_t)((uint64_t)total * 65536 / LOADING_ART_PIXELS);
		while (art->histogram_valid && target < 255 && art->histogram[target] < share)
			target++;
		art->tone[level] = (uint8_t)(art->histogram_valid ? target : level);
	}
	art->tone_valid = 1;
}

/* The sweep's light on the ring: where the sweep's band is across the
 * picture at a phase (loading_art_step's mask at its brightest), in pixels. */
static inline float loading_art_band(unsigned int phase)
{
	return 3.2f * (float)phase - 64.5f;
}

/* The scene for one eye (NULL: the menu's camera; else VR's, loading_ring_draw's) `seconds` into
 * a load, the sweep at `phase`: in the picture's one hue (its brightness
 * matched to the picture's, its detail etched brighter, as the picture's
 * ring is), the picture's words over it; and how much the sweep's light
 * catches each pixel (the ring's rim and highlight, its detail's edges). */
static inline int loading_art_scene_draw(struct loading_art *art, float seconds, unsigned int phase, const float *eye,
	uint8_t *scene, uint8_t *feed)
{
	const uint32_t *pixels = loading_ring_draw(&art->ring, LOADING_ART_FRAME_WIDTH, LOADING_ART_FRAME_HEIGHT, seconds,
		eye, loading_art_band(phase));
	unsigned int x, y;
	if (!pixels)
		return 0;
	if (!art->tone_valid)
		loading_art_tone(art, pixels);
	for (y = 0; y < LOADING_ART_FRAME_HEIGHT; y++)
		for (x = 0; x < LOADING_ART_FRAME_WIDTH; x++)
		{
			unsigned int pixel = y * LOADING_ART_FRAME_WIDTH + x, value = pixels[pixel];
			int level = (int)art->tone[loading_art_luminance(value & 255, (value >> 8) & 255, (value >> 16) & 255)];
			int edge = 0, coverage, light, channel, white;
			unsigned int tint;
			uint8_t rgb[3];
			if (x > 0 && y > 0 && x + 1 < LOADING_ART_FRAME_WIDTH && y + 1 < LOADING_ART_FRAME_HEIGHT)
			{
				const uint32_t *p = pixels + pixel;
				int gx = (int)((p[1] >> 8) & 255) - (int)((p[-1] >> 8) & 255);
				int gy = (int)((p[LOADING_ART_FRAME_WIDTH] >> 8) & 255) - (int)((p[-LOADING_ART_FRAME_WIDTH] >> 8) & 255);
				edge = (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy);
			}
			coverage = art->text_valid ? art->text[pixel] : 0;
			white = 0;
			tint = loading_art_tint(value, edge, &level, &white);
			memcpy(scene + pixel * 3, art->tints[tint][level + edge > 255 ? 255 : level + edge], 3);
			if (feed)
			{
				/* what the sweep lights: the ring etched as the picture's is
				(its detail's edges strong over its darker surface), the
				sweep's light on its rim and highlight, in its detail */
				light = (level * LOADING_ART_FEED_TONE + edge * LOADING_ART_FEED_EDGE) / 16 +
					art->ring.glint[pixel] * (64 + (edge < 192 ? edge : 192)) * LOADING_ART_FEED_GLINT / 2048;
				/* (white only in the specks: the feedback's whites are grey
				as they fade, the picture's few) */
				if (light > LOADING_ART_FEED_KNEE && !white)
					light = LOADING_ART_FEED_KNEE + (light - LOADING_ART_FEED_KNEE) / 3;
				if (light > LOADING_ART_FEED_LIMIT && !white)
					light = LOADING_ART_FEED_LIMIT;
				memcpy(feed + pixel * 3, art->tints[tint][light > 255 ? 255 : light], 3);
			}
			/* (the words, where they are) */
			if (coverage)
				for (channel = 0; channel < 3; channel++)
				{
					rgb[channel] = art->color[pixel * 3 + channel];
					scene[pixel * 3 + channel] = (uint8_t)(scene[pixel * 3 + channel] +
						(rgb[channel] - scene[pixel * 3 + channel]) * coverage / 255);
					if (feed)
						feed[pixel * 3 + channel] = (uint8_t)(feed[pixel * 3 + channel] +
							(rgb[channel] - feed[pixel * 3 + channel]) * coverage / 255);
				}
		}
	return 1;
}

static inline unsigned int loading_art_mask(int x, unsigned int y)
{
	if (x < 0 || x >= 128)
		return 5;
	return loading_art_mask_table[y * 128 + (unsigned int)x];
}

/* A row of the previous history blurred across (its horizontal taps): the
 * diagonal samples' weights are the axes' products, so the sum over rows of
 * these, weighted, is the same sum, in fewer multiplications. A few rows
 * are kept, as consecutive output rows read the same ones. */
static inline const uint32_t *loading_art_row(struct loading_art *art, const uint8_t *previous, unsigned int row)
{
	unsigned int slot, x, tap;
	uint32_t *sums;
	for (slot = 0; slot < 4; slot++)
		if (art->row_tags[slot] == row + 1)
			return art->rows[slot];
	slot = art->row_next++ & 3;
	art->row_tags[slot] = row + 1;
	sums = art->rows[slot];
	for (x = 0; x < LOADING_ART_FRAME_WIDTH; x++)
	{
		const struct loading_art_axis *ax = &art->x[x];
		uint32_t r = 0, g = 0, b = 0;
		for (tap = 0; tap < 3; tap++)
		{
			const uint8_t *sample;
			uint32_t weight = ax->weight[tap];
			if (!weight)
				continue;
			sample = previous + (row * LOADING_ART_FRAME_WIDTH + ax->index[tap]) * 4;
			r += sample[0] * weight;
			g += sample[1] * weight;
			b += sample[2] * weight;
		}
		sums[x * 3] = r;
		sums[x * 3 + 1] = g;
		sums[x * 3 + 2] = b;
	}
	return sums;
}

static inline void loading_art_step(struct loading_art *art, unsigned int phase)
{
	const uint8_t *previous = art->history[art->current];
	uint8_t *output = art->history[art->current ^ 1];
	unsigned int x, y, channel, ty, fade, brightness;
	int mask_x, mask_y, ix, iy;
	unsigned int fx, fy, mask, upper, lower, pixel, value, sum[3], weight;
	const struct loading_art_axis *ay;
	const uint8_t *source = art->ring.valid ? art->feed : art->color;
	const uint32_t *rows[3];
	fade = phase <= 216 ? 216 : (240 - phase) * 9;
	brightness = loading_art_brightness[phase];
	memset(art->row_tags, 0, sizeof(art->row_tags));
	for (y = 0; y < LOADING_ART_FRAME_HEIGHT; y++)
	{
		ay = &art->y[y];
		for (ty = 0; ty < 3; ty++)
			rows[ty] = ay->weight[ty] ? loading_art_row(art, previous, ay->index[ty]) : NULL;
		mask_y = (int)((2 * y + 1) * 16 * 128 / LOADING_ART_FRAME_HEIGHT) - 128;
		if (mask_y < 0)
			mask_y = 0;
		if (mask_y > 15 * 256)
			mask_y = 15 * 256;
		iy = mask_y / 256;
		fy = (unsigned int)mask_y & 255;
		for (x = 0; x < LOADING_ART_FRAME_WIDTH; x++)
		{
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
				if (!rows[ty])
					continue;
				weight = ay->weight[ty];
				sum[0] += rows[ty][x * 3] * weight;
				sum[1] += rows[ty][x * 3 + 1] * weight;
				sum[2] += rows[ty][x * 3 + 2] * weight;
			}
			for (channel = 0; channel < 3; channel++)
			{
				/* Average and fade before quantizing. The blur shader does
				 * not use the near-white diffuse RGB supplied by the draw. */
				value = (unsigned int)(((uint64_t)sum[channel] * fade + UINT64_C(503316480)) /
					UINT64_C(1006632960));
				value += (source[pixel * 3 + channel] * brightness * mask + 32512) / 65025;
				output[pixel * 4 + channel] = (uint8_t)(value < 255 ? value : 255);
			}
			output[pixel * 4 + 3] = 255;
		}
	}
	art->current ^= 1;
}

/* What is shown of the feedback and of an eye's scene: the scene stays in
 * view between the sweeps, dimly (faded in over the load's first moments),
 * added to what is shown, never to the feedback; VR's window darkens to
 * its edges, into the dark about it. */
static inline void loading_art_compose(struct loading_art *art, const uint8_t *previous, const uint8_t *current,
	unsigned int fraction, const uint8_t *scene, unsigned int base, uint8_t *output)
{
	unsigned int x, y, channel, pixel;
	if (!scene)
	{
		for (pixel = 0; pixel < LOADING_ART_FRAME_PIXELS; pixel++)
		{
			for (channel = 0; channel < 3; channel++)
				output[pixel * 4 + channel] = (uint8_t)((previous[pixel * 4 + channel] *
					(256 - fraction) + current[pixel * 4 + channel] * fraction + 128) >> 8);
			output[pixel * 4 + 3] = 255;
		}
		return;
	}
	for (y = 0; y < LOADING_ART_FRAME_HEIGHT; y++)
	{
		unsigned int vy = art->stereo ? (y < 40 ? y * 256 / 40 : y >= LOADING_ART_FRAME_HEIGHT - 40 ?
			(LOADING_ART_FRAME_HEIGHT - 1 - y) * 256 / 40 : 256) : 256;
		for (x = 0; x < LOADING_ART_FRAME_WIDTH; x++)
		{
			unsigned int value, edge = vy;
			pixel = y * LOADING_ART_FRAME_WIDTH + x;
			/* (VR's window darkens toward its edges) */
			if (art->stereo)
				edge = (x < 48 ? x * 256 / 48 : x >= LOADING_ART_FRAME_WIDTH - 48 ?
					(LOADING_ART_FRAME_WIDTH - 1 - x) * 256 / 48 : 256) * vy >> 8;
			for (channel = 0; channel < 3; channel++)
			{
				value = ((previous[pixel * 4 + channel] * (256 - fraction) +
					current[pixel * 4 + channel] * fraction + 128) >> 8) + (scene[pixel * 3 + channel] * base >> 16);
				output[pixel * 4 + channel] = (uint8_t)(((value < 255 ? value : 255) * edge) >> 8);
			}
			/* (the feedback's channels saturate apart, and its whites fade
			grey: kept in the picture's blues, its whites a pale blue) */
			value = output[pixel * 4 + 2] * LOADING_ART_GREY_RED >> 8;
			if (output[pixel * 4] > value)
				output[pixel * 4] = (uint8_t)value;
			value = output[pixel * 4 + 2] * LOADING_ART_GREY_GREEN >> 8;
			if (output[pixel * 4 + 1] > value)
				output[pixel * 4 + 1] = (uint8_t)value;
			output[pixel * 4 + 3] = 255;
		}
	}
}

static inline const uint8_t *loading_art_render(struct loading_art *art,
	const struct loading_presentation_frame *frame)
{
	uint64_t target;
	unsigned int reset, skipped, fraction, base, completion, stepped;
	unsigned int stereo = art->stereo && art->ring.valid;
	if (!art->valid && !art->ring.valid)
		return NULL;
	target = (frame->elapsed_ns / UINT64_C(1000000000)) * 60 +
		(frame->elapsed_ns % UINT64_C(1000000000)) * 60 / UINT64_C(1000000000);
	reset = !art->started || art->epoch != frame->epoch || target < art->step;
	skipped = art->started && target > art->step && target - art->step > 1;
	if (reset || (target != art->step && target / 240 != art->step / 240))
		memset(art->history, 0, sizeof(art->history));
	if (reset)
		art->tone_valid = 0;
	/* At most one feedback step, even after a producer or display stall.
	 * Elapsed time selects the sweep phase; no costly fast-forward loop. */
	stepped = reset || target != art->step;
	if (stepped)
	{
		float seconds = (float)((double)frame->elapsed_ns * 1e-9);
		unsigned int phase = (unsigned int)(target % 240);
		/* (the scene every other step: its motion is slow, the sweep's is
		the feedback's, at every step) */
		if (art->ring.valid && (reset || skipped || !(target & 1)))
		{
			float eyes[2][3];
			if (stereo)
				loading_art_eyes(art, frame->elapsed_ns, eyes);
			loading_art_scene_draw(art, seconds, phase, stereo ? eyes[0] : NULL, art->scene, art->feed);
			if (stereo)
				loading_art_scene_draw(art, seconds, phase, eyes[1], art->scene_right, NULL);
		}
		loading_art_step(art, phase);
		if (reset || skipped)
			memcpy(art->history[art->current ^ 1], art->history[art->current],
				sizeof(art->history[0]));
	}
	art->epoch = frame->epoch;
	art->step = target;
	art->started = 1;
	completion = loading_art_completion(art, frame);
	/* (VR's pair, at the headset's rate, between the steps: the pictures
	 * of the step, as they were; the eyes' two pictures cost twice) */
	if (stereo && !stepped && art->composed && completion == 256)
		return art->output;
	art->composed = stereo;
	/* Interpolate the fixed-rate histories for each display refresh. This
	 * adds one visual step of delay without changing the feedback clock. */
	fraction = (unsigned int)(((frame->elapsed_ns % UINT64_C(1000000000)) * 60 %
		UINT64_C(1000000000)) * 256 / UINT64_C(1000000000));
	base = frame->elapsed_ns >= LOADING_ART_SCENE_FADE_NS ? LOADING_ART_SCENE_BASE * 256u :
		(unsigned int)(LOADING_ART_SCENE_BASE * 256u * frame->elapsed_ns / LOADING_ART_SCENE_FADE_NS);
	loading_art_compose(art, art->history[art->current ^ 1], art->history[art->current], fraction,
		art->ring.valid ? art->scene : NULL, base, art->output);
	if (stereo)
		loading_art_compose(art, art->history[art->current ^ 1], art->history[art->current], fraction,
			art->scene_right, base, art->output_right);
	if (completion < 256)
	{
		unsigned int pixel, channel;
		for (pixel = 0; pixel < LOADING_ART_FRAME_PIXELS; pixel++)
			for (channel = 0; channel < 3; channel++)
			{
				art->output[pixel * 4 + channel] = (uint8_t)(art->output[pixel * 4 + channel] * completion >> 8);
				if (stereo)
					art->output_right[pixel * 4 + channel] =
						(uint8_t)(art->output_right[pixel * 4 + channel] * completion >> 8);
			}
	}
	return art->output;
}

#endif
