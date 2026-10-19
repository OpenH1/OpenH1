/*
XGPU_VULKAN_EFFECTS.C

The VR mode's effects' arithmetic on the CPU (xgpu_vulkan_effects.h): an
effect bitmap's thickness, and each eye's transforms between the world and
its pixels.

The distances are Felzenszwalb and Huttenlocher's exact squared Euclidean
distance transform ("Distance Transforms of Sampled Functions", 2012): the
lower envelope of parabolas, a pass down each column and then along each
row, linear in the texels.
*/

#include "xgpu_vulkan_effects.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* an inside texel's distance before the transform: farther than any (finite,
so that two of them make no infinity less infinity) */
#define FAR_AWAY 1.0e20f

/* ---------- the distances */

/* one line of the transform: f of n samples in, d out (v and z the
envelope's workspace: n and n + 1) */
static float intersection(const float *f, int q, int p)
{
	return ((f[q] + (float)q * (float)q) - (f[p] + (float)p * (float)p)) / (2.0f * (float)(q - p));
}

static void distance_line(const float *f, int n, float *d, int *v, float *z)
{
	int k = 0, q;

	v[0] = 0;
	z[0] = -INFINITY;
	z[1] = INFINITY;
	for (q = 1; q < n; q++)
	{
		float s = intersection(f, q, v[k]);

		while (s <= z[k])
		{
			k--;
			s = intersection(f, q, v[k]);
		}
		k++;
		v[k] = q;
		z[k] = s;
		z[k + 1] = INFINITY;
	}
	k = 0;
	for (q = 0; q < n; q++)
	{
		while (z[k + 1] < (float)q)
			k++;
		d[q] = ((float)q - v[k]) * ((float)q - v[k]) + f[v[k]];
	}
}

int xgpu_vulkan_thickness_distances(const uint8_t *inside, uint32_t width, uint32_t height, float *squared)
{
	/* (a border of texels outside about the bitmap: its edges are its
	sprites' edges) */
	uint32_t padded_width = width + 2, padded_height = height + 2, x, y;
	uint32_t longest = padded_width > padded_height ? padded_width : padded_height;
	float *grid = malloc(sizeof(float) * padded_width * padded_height);
	float *line = malloc(sizeof(float) * longest), *result = malloc(sizeof(float) * longest);
	float *z = malloc(sizeof(float) * (longest + 1));
	int *v = malloc(sizeof(int) * longest);
	int made = grid && line && result && z && v;

	if (made)
	{
		for (y = 0; y < padded_height; y++)
		{
			for (x = 0; x < padded_width; x++)
			{
				int in = x > 0 && y > 0 && x <= width && y <= height && inside[(y - 1) * width + (x - 1)];

				grid[y * padded_width + x] = in ? FAR_AWAY : 0.0f;
			}
		}
		for (x = 0; x < padded_width; x++)
		{
			for (y = 0; y < padded_height; y++)
				line[y] = grid[y * padded_width + x];
			distance_line(line, (int)padded_height, result, v, z);
			for (y = 0; y < padded_height; y++)
				grid[y * padded_width + x] = result[y];
		}
		for (y = 0; y < padded_height; y++)
		{
			distance_line(&grid[y * padded_width], (int)padded_width, result, v, z);
			memcpy(&grid[y * padded_width], result, sizeof(float) * padded_width);
		}
		for (y = 0; y < height; y++)
			for (x = 0; x < width; x++)
				squared[y * width + x] = grid[(y + 1) * padded_width + (x + 1)];
	}
	free(grid);
	free(line);
	free(result);
	free(z);
	free(v);
	return made;
}

/* ---------- the thickness */

static uint32_t root_of(uint32_t *parent, uint32_t index)
{
	while (parent[index] != index)
	{
		parent[index] = parent[parent[index]];
		index = parent[index];
	}
	return index;
}

static void join(uint32_t *parent, uint32_t a, uint32_t b)
{
	a = root_of(parent, a);
	b = root_of(parent, b);
	if (a != b)
		parent[a < b ? b : a] = a < b ? a : b;
}

/* a box blur of radius 1 along x, then y, in place (scratch: count floats) */
static void blur(float *values, float *scratch, uint32_t width, uint32_t height)
{
	uint32_t x, y;

	for (y = 0; y < height; y++)
	{
		for (x = 0; x < width; x++)
		{
			float sum = values[y * width + x], count = 1.0f;

			if (x > 0)
				sum += values[y * width + x - 1], count += 1.0f;
			if (x + 1 < width)
				sum += values[y * width + x + 1], count += 1.0f;
			scratch[y * width + x] = sum / count;
		}
	}
	for (y = 0; y < height; y++)
	{
		for (x = 0; x < width; x++)
		{
			float sum = scratch[y * width + x], count = 1.0f;

			if (y > 0)
				sum += scratch[(y - 1) * width + x], count += 1.0f;
			if (y + 1 < height)
				sum += scratch[(y + 1) * width + x], count += 1.0f;
			values[y * width + x] = sum / count;
		}
	}
}

static uint8_t encoded(float texels)
{
	float value = texels / XGPU_VULKAN_THICKNESS_UNIT * 255.0f + 0.5f;

	return (uint8_t)(value < 0.0f ? 0.0f : value > 255.0f ? 255.0f : value);
}

int xgpu_vulkan_thickness_bake(const uint32_t *texels, uint32_t width, uint32_t height, uint8_t *thickness)
{
	size_t count = (size_t)width * height, index;
	float *mask, *squared, *height_map, *radius, *scratch, *most;
	uint8_t *inside;
	uint32_t *parent;
	unsigned int least_alpha = 255, most_alpha = 0;
	int alpha_mask, made, pass;
	double radius_sum = 0.0, area = 0.0;
	float typical;
	uint32_t x, y;

	if (!width || !height || width > XGPU_VULKAN_THICKNESS_LARGEST || height > XGPU_VULKAN_THICKNESS_LARGEST)
		return 0;
	mask = malloc(sizeof(float) * count);
	squared = malloc(sizeof(float) * count);
	height_map = malloc(sizeof(float) * count);
	radius = malloc(sizeof(float) * count);
	scratch = malloc(sizeof(float) * count);
	most = malloc(sizeof(float) * count);
	inside = malloc(count);
	parent = malloc(sizeof(uint32_t) * count);
	made = mask && squared && height_map && radius && scratch && most && inside && parent;
	if (!made)
		goto done;
	/* the mask: the alpha, unless it is the same everywhere (an added
	effect's), then the brightest channel */
	for (index = 0; index < count; index++)
	{
		unsigned int alpha = texels[index] >> 24;

		least_alpha = alpha < least_alpha ? alpha : least_alpha;
		most_alpha = alpha > most_alpha ? alpha : most_alpha;
	}
	alpha_mask = most_alpha - least_alpha > 16;
	for (index = 0; index < count; index++)
	{
		uint32_t texel = texels[index];
		unsigned int red = (texel >> 16) & 0xff, green = (texel >> 8) & 0xff, blue = texel & 0xff;
		unsigned int brightest = red > green ? (red > blue ? red : blue) : (green > blue ? green : blue);

		mask[index] = (float)(alpha_mask ? texel >> 24 : brightest) / 255.0f;
		inside[index] = mask[index] > XGPU_VULKAN_THICKNESS_INSIDE;
	}
	if (!xgpu_vulkan_thickness_distances(inside, width, height, squared))
	{
		made = 0;
		goto done;
	}
	/* each sprite (its texels inside, joined across their edges) and the
	most of its texels' distances to its edge */
	for (index = 0; index < count; index++)
		parent[index] = (uint32_t)index;
	for (y = 0; y < height; y++)
	{
		for (x = 0; x < width; x++)
		{
			index = (size_t)y * width + x;
			if (!inside[index])
				continue;
			if (x + 1 < width && inside[index + 1])
				join(parent, (uint32_t)index, (uint32_t)index + 1);
			if (y + 1 < height && inside[index + width])
				join(parent, (uint32_t)index, (uint32_t)(index + width));
		}
	}
	for (index = 0; index < count; index++)
		most[index] = 0.0f;
	for (index = 0; index < count; index++)
	{
		/* (the edge lies halfway to the texel outside) */
		float distance = inside[index] ? sqrtf(squared[index]) - 0.5f : 0.0f;
		uint32_t root;

		squared[index] = distance > 0.0f ? distance : 0.0f;
		if (!inside[index])
			continue;
		root = root_of(parent, (uint32_t)index);
		most[root] = squared[index] > most[root] ? squared[index] : most[root];
	}
	/* the sphere's profile: at a fraction q of the radius in from the edge,
	sqrt(q (2 - q)) of it high */
	for (index = 0; index < count; index++)
	{
		height_map[index] = radius[index] = 0.0f;
		if (inside[index])
		{
			float sprite = most[root_of(parent, (uint32_t)index)];
			float q = sprite > 0.0f ? squared[index] / sprite : 0.0f;

			height_map[index] = sprite * sqrtf(q * (2.0f - q));
			radius[index] = sprite + 0.5f;
			radius_sum += radius[index];
			area += 1.0f;
		}
	}
	/* smoothed (the transform's ridges along each sprite's middle, and its
	steps), and thin where the mask is faint */
	for (pass = 0; pass < 2; pass++)
		blur(height_map, scratch, width, height);
	for (index = 0; index < count; index++)
		height_map[index] *= sqrtf(mask[index]);
	/* the radius into the clear texels about each sprite, and the typical
	radius past them */
	typical = area > 0.0 ? (float)(radius_sum / area) : (float)(width < height ? width : height) * 0.25f;
	for (pass = 0; pass < 3; pass++)
	{
		memcpy(scratch, radius, sizeof(float) * count);
		for (y = 0; y < height; y++)
		{
			for (x = 0; x < width; x++)
			{
				float largest = scratch[(size_t)y * width + x];

				if (largest > 0.0f)
					continue;
				if (x > 0 && scratch[(size_t)y * width + x - 1] > largest)
					largest = scratch[(size_t)y * width + x - 1];
				if (x + 1 < width && scratch[(size_t)y * width + x + 1] > largest)
					largest = scratch[(size_t)y * width + x + 1];
				if (y > 0 && scratch[(size_t)(y - 1) * width + x] > largest)
					largest = scratch[(size_t)(y - 1) * width + x];
				if (y + 1 < height && scratch[(size_t)(y + 1) * width + x] > largest)
					largest = scratch[(size_t)(y + 1) * width + x];
				radius[(size_t)y * width + x] = largest;
			}
		}
	}
	for (index = 0; index < count; index++)
	{
		thickness[index * 4 + 0] = encoded(height_map[index]);
		thickness[index * 4 + 1] = encoded(radius[index] > 0.0f ? radius[index] : typical);
		thickness[index * 4 + 2] = 0;
		thickness[index * 4 + 3] = 0;
	}
done:
	free(mask);
	free(squared);
	free(height_map);
	free(radius);
	free(scratch);
	free(most);
	free(inside);
	free(parent);
	return made;
}

/* ---------- each eye's transforms */

/* a = b c (4x4, rows) */
static void multiply(float a[4][4], const float b[4][4], const float c[4][4])
{
	float result[4][4];
	int row, column, k;

	for (row = 0; row < 4; row++)
	{
		for (column = 0; column < 4; column++)
		{
			float sum = 0.0f;

			for (k = 0; k < 4; k++)
				sum += b[row][k] * c[k][column];
			result[row][column] = sum;
		}
	}
	memcpy(a, result, sizeof(result));
}

/* inverse (rows) by Gauss-Jordan elimination with partial pivoting, in
double precision (the depth's rows are nearly parallel); 0 if singular */
static int invert(float inverse[4][4], const float m[4][4])
{
	double a[4][8];
	int row, column, pivot, k;

	for (row = 0; row < 4; row++)
	{
		for (column = 0; column < 4; column++)
		{
			a[row][column] = m[row][column];
			a[row][column + 4] = row == column ? 1.0 : 0.0;
		}
	}
	for (column = 0; column < 4; column++)
	{
		double largest = 0.0;

		pivot = column;
		for (row = column; row < 4; row++)
		{
			if (fabs(a[row][column]) > largest)
			{
				largest = fabs(a[row][column]);
				pivot = row;
			}
		}
		if (largest < 1.0e-30)
			return 0;
		if (pivot != column)
		{
			for (k = 0; k < 8; k++)
			{
				double swap = a[column][k];

				a[column][k] = a[pivot][k];
				a[pivot][k] = swap;
			}
		}
		{
			double scale = 1.0 / a[column][column];

			for (k = 0; k < 8; k++)
				a[column][k] *= scale;
		}
		for (row = 0; row < 4; row++)
		{
			double factor = a[row][column];

			if (row == column || factor == 0.0)
				continue;
			for (k = 0; k < 8; k++)
				a[row][k] -= factor * a[column][k];
		}
	}
	for (row = 0; row < 4; row++)
		for (column = 0; column < 4; column++)
			inverse[row][column] = (float)a[row][column + 4];
	return 1;
}

int xgpu_vulkan_effects_view(const float projection[6][4], const float viewport_scale[4],
	const float viewport_offset[4], float screen_offset, const float *eye_correction, const float *fovea,
	const int32_t viewport[4], const float depth_range[2], struct xgpu_vulkan_effects_view *view)
{
	float m[4][4], step[4][4], window[4][4], inverse[4][4];
	const float offset[3] = { 0.5f + screen_offset, 0.5f, 0.0f };
	int row, column;

	memset(view, 0, sizeof(*view));
	if (viewport[2] <= 0 || viewport[3] <= 0)
		return 0;
	/* the world's projection */
	for (row = 0; row < 4; row++)
		for (column = 0; column < 4; column++)
			m[row][column] = projection[row][column];
	/* the eye's correction (column-major) */
	if (eye_correction)
	{
		for (row = 0; row < 4; row++)
			for (column = 0; column < 4; column++)
				step[row][column] = eye_correction[column * 4 + row];
		multiply(m, step, m);
	}
	/* the screen conversion: (clip.xyz c[-38].xyz + (c[-37].xyz + offset -
	viewport_offset.xyz) clip.w) / scale, and y turned down */
	memset(step, 0, sizeof(step));
	for (row = 0; row < 3; row++)
	{
		float scale = viewport_scale[row] != 0.0f ? viewport_scale[row] : 1.0f;
		float sign = row == 1 ? -1.0f : 1.0f;

		step[row][row] = sign * projection[4][row] / scale;
		step[row][3] = sign * (projection[5][row] + offset[row] - viewport_offset[row]) / scale;
	}
	step[3][3] = 1.0f;
	multiply(m, step, m);
	/* the inset's scale and offset */
	if (fovea)
	{
		memset(step, 0, sizeof(step));
		step[0][0] = fovea[0];
		step[0][3] = fovea[2];
		step[1][1] = fovea[1];
		step[1][3] = fovea[3];
		step[2][2] = step[3][3] = 1.0f;
		multiply(m, step, m);
	}
	memcpy(view->clip, m, sizeof(m));
	/* the viewport: pixels and depth */
	memset(window, 0, sizeof(window));
	window[0][0] = 0.5f * (float)viewport[2];
	window[0][3] = (float)viewport[0] + 0.5f * (float)viewport[2];
	window[1][1] = 0.5f * (float)viewport[3];
	window[1][3] = (float)viewport[1] + 0.5f * (float)viewport[3];
	window[2][2] = depth_range[1] - depth_range[0];
	window[2][3] = depth_range[0];
	window[3][3] = 1.0f;
	multiply(m, window, m);
	if (!invert(inverse, m))
		return 0;
	for (row = 0; row < 4; row++)
		for (column = 0; column < 4; column++)
			view->unproject[column][row] = inverse[row][column];
	/* (the eye: the point every pixel's ray leaves from, where the clip
	position's x, y and w are 0: the inverse's third column) */
	if (fabsf(view->unproject[2][3]) < 1.0e-20f)
		return 0;
	for (row = 0; row < 3; row++)
		view->eye[row] = view->unproject[2][row] / view->unproject[2][3];
	view->eye[3] = 1.0f;
	return 1;
}
