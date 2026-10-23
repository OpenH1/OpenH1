/*
XGPU_SHADOW_BLUR.H

The blur of a shadow map drawn larger than the Xbox's
(display.shadow_resolution), which every backend draws as two passes
(xgpu_backend_shadow_blur): the game's own, a pass of the Xbox's and two
passes for each doubling of the map (rasterizer_shadow_convolve,
source/rasterizer/xbox/rasterizer_xbox_shadows.c), weighed into one blur.

Each of the game's passes takes four taps diagonally about each texel, and
weighs its neighbours the same way across as down: the Xbox's 1, 2, 1 (taps
half a texel off, each the average of four texels), and a widening pass's
1, 0, ..., 0, 1 (taps distance texels off), at distance 1, 2, ..., scale / 2
twice each. Each tap outside the map reads the border's black, so that what
a pass spreads beyond an edge is gone from the next. A pass is the same
blur across and down, the edges too (the map is square), so the passes
together are a blur across followed by one down: each texel the sum of the
texels in a line through it, out to the radius, 2 * scale - 1, each of
those weighed as the passes' chain weighs it. A texel's weights depend only
on how far it is from the nearer edge, up to the radius (the far edge's
mirrored), so that the table has radius + 1 rows: row e for a texel e from
an edge (its weights for the texels from radius before it to radius after
it, those beyond the edge 0), row radius for every texel further in.

The table is a texture of 2 * radius + 1 by radius + 1 RGBA8 texels, each
weight in 65536ths (red the high byte, green the low: every weight is a
whole number of them, below 1), which the
pass reads with texelFetch, as it reads the map. The passes' arithmetic is
the chain's but for the 8-bit stores between the game's passes: one store
in the middle (half floats, or 8 bits where a backend has no half-float
target) and the map's at the end.
*/

#ifndef __HALO_XGPU_SHADOW_BLUR_H
#define __HALO_XGPU_SHADOW_BLUR_H

/* the largest scale (d3d8_device.c's SHADOW_MAP_MAXIMUM_SCALE) and its
radius */
#define XGPU_SHADOW_BLUR_MOST_SCALE 8
#define XGPU_SHADOW_BLUR_MOST_RADIUS (2 * XGPU_SHADOW_BLUR_MOST_SCALE - 1)

/* the blur's radius for a scale: 2 to XGPU_SHADOW_BLUR_MOST_SCALE, a power
of two; 0 for any other */
static inline int xgpu_shadow_blur_radius(int scale)
{
	return scale >= 2 && scale <= XGPU_SHADOW_BLUR_MOST_SCALE && !(scale & (scale - 1)) ? 2 * scale - 1 : 0;
}

/* the table of a scale's weights: (2 * radius + 1) * (radius + 1) texels
of four bytes, row after row; 0 for a scale with no radius */
static inline int xgpu_shadow_blur_weights(int scale, unsigned char *texels)
{
	/* (a line long enough that a texel of an edge's row is far from the
	other edge, as is the middle row's texel from both) */
	enum { LINE = 4 * XGPU_SHADOW_BLUR_MOST_RADIUS + 2 };
	int radius = xgpu_shadow_blur_radius(scale);
	int length = 4 * radius + 2;
	/* the passes' distances in the game's order: the Xbox's (0: its taps
	half a texel off), then each widening distance twice */
	int distances[1 + 2 * 3];
	int count = 0;
	int row, stage, distance, index, offset;

	if (!radius)
		return 0;
	distances[count++] = 0;
	for (distance = 1; distance < scale; distance *= 2)
	{
		distances[count++] = distance;
		distances[count++] = distance;
	}
	for (row = 0; row <= radius; row++)
	{
		double weights[LINE], spread[LINE];
		/* the texel the row is for: row from the line's start, or the
		middle row's in the middle */
		int texel = row < radius ? row : 2 * radius;

		/* the chain's weights for that texel, from the last pass back to
		the first: each pass spreads the weights as it spreads texels (its
		taps are the same either way about), and what it would take from
		beyond the line, the border's black, weighs nothing */
		for (index = 0; index < length; index++)
			weights[index] = index == texel;
		for (stage = count - 1; stage >= 0; stage--)
		{
			/* (the Xbox's taps take the texels 1 off, and the texel itself
			twice) */
			int apart = distances[stage] ? distances[stage] : 1;

			for (index = 0; index < length; index++)
			{
				double sides = (index >= apart ? weights[index - apart] : 0.0) +
					(index + apart < length ? weights[index + apart] : 0.0);

				spread[index] = distances[stage] ? sides / 2.0 : (sides + 2.0 * weights[index]) / 4.0;
			}
			for (index = 0; index < length; index++)
				weights[index] = spread[index];
		}
		for (offset = -radius; offset <= radius; offset++)
		{
			double weight = texel + offset >= 0 ? weights[texel + offset] : 0.0;
			unsigned long fixed = (unsigned long)(weight * 65536.0 + 0.5);
			unsigned char *out = texels + ((size_t)row * (2 * radius + 1) + (size_t)(offset + radius)) * 4;

			out[0] = (unsigned char)(fixed >> 8);
			out[1] = (unsigned char)(fixed & 0xff);
			out[2] = 0;
			out[3] = 0;
		}
	}
	return radius;
}

#endif
