/*
XGPU_VULKAN_EFFECTS.H

The VR mode's effects' arithmetic that runs on the CPU (xgpu_vulkan_effects.c),
apart from the backend so that it can be tested alone: the thickness of an
effect's bitmap, baked once from its alpha as the texture is made
(xgpu_vulkan_textures.c), and each eye's transforms between the world and
its pixels, which the effects' shaders reconstruct the world from
(xgpu_vulkan_stream.c, host_vulkan_vr.c). "VR" in port/linux/vulkan/README.md
says what they draw.
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_EFFECTS_H
#define __HALO_LINUX_XGPU_VULKAN_EFFECTS_H

#include <stdint.h>

/* ---------- an effect bitmap's thickness

For each texel of an effect's bitmap (a sprite sheet: its sprites apart,
with clear texels between them), how thick the effect is there, from its
silhouette: the texels its mask covers (the alpha, or, where the alpha is
the same everywhere, as an added effect's is, the brightest channel) are
the inside; each texel's distance to the nearest texel outside (exact,
Euclidean) over the most of its sprite (its connected texels inside) is a
fraction of that sprite's radius, which a sphere's profile makes a height
(a disc becomes a hemisphere), smoothed, and scaled by the square root of
the mask (thin wisps are thin). The texels are written as RGBA8:
red, the height, and green, the sprite's radius, each in texels over
XGPU_VULKAN_THICKNESS_UNIT (the shader multiplies them back); blue and
alpha 0. Outside every sprite, the height is 0 and the radius the nearest
sprite's (blurred into the clear texels about it) or, far from any, the
bitmap's typical one. */

/* the texels a stored value of 1 is */
#define XGPU_VULKAN_THICKNESS_UNIT 128.0f
/* the mask's least value inside a sprite (of 1) */
#define XGPU_VULKAN_THICKNESS_INSIDE 0.08f
/* the largest bitmap baked (texels a side) */
#define XGPU_VULKAN_THICKNESS_LARGEST 1024u

/* texels: width x height, 32-bit ARGB (D3DCOLOR: blue in the low byte),
rows from the first; thickness: width x height x 4 bytes. 0 when out of
memory or too large (nothing written). */
int xgpu_vulkan_thickness_bake(const uint32_t *texels, uint32_t width, uint32_t height, uint8_t *thickness);

/* (the bake's parts, for the tests) the squared Euclidean distance of each
texel to the nearest one outside (inside: nonzero), the texels past the
edges outside; 0 when out of memory */
int xgpu_vulkan_thickness_distances(const uint8_t *inside, uint32_t width, uint32_t height, float *squared);

/* ---------- each eye's transforms

A view of a multiview draw (an eye, or with foveation, an eye's
periphery or inset) as the game's vertex programs place a world point
(xgpu_spirv.c's vertex shader): the world's projection (c[-96] to c[-93]),
the eye's correction (the clip position's move from the head's centre to
the eye, column-major), the screen conversion (c[-38], c[-37], the
viewport's scale and offset, the screen offset), y turned down, the inset's
scale and offset, then the viewport. */
struct xgpu_vulkan_effects_view
{
	/* a world point's clip position in the view: rows */
	float clip[4][4];
	/* the world point (homogeneous) at a pixel (gl_FragCoord's x and y) and
	depth (the depth buffer's): columns */
	float unproject[4][4];
	/* the view's eye in the world (w 1) */
	float eye[4];
};

/* projection: c[-96] to c[-93], c[-38], c[-37]; eye_correction NULL for
none; fovea: the inset's scale (x, y) and offset (z, w), NULL for none;
viewport: x, y, width, height; depth_range: the viewport's least and most
depth. 0 if the transform cannot be inverted (nothing to draw). */
int xgpu_vulkan_effects_view(const float projection[6][4], const float viewport_scale[4],
	const float viewport_offset[4], float screen_offset, const float *eye_correction, const float *fovea,
	const int32_t viewport[4], const float depth_range[2], struct xgpu_vulkan_effects_view *view);

#endif
