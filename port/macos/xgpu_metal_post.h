/*
XGPU_METAL_POST.H

display.anti_aliasing's passes for the Metal backend (xgpu_metal_post.c):
FXAA and SMAA over the rectangle of a window's 3D view, in the target,
before the HUD and the menus are drawn over it, as the OpenGL and Vulkan
backends run them.
*/

#ifndef __HALO_MACOS_XGPU_METAL_POST_H
#define __HALO_MACOS_XGPU_METAL_POST_H

#include <stdint.h>

/* SMAA's (smaa nonzero), or FXAA's, functions, pipelines and lookup
textures, made as the setting is chosen or a map is prepared: the
functions and pipelines on the backend's queues (which a map's preparation
waits for); 0 (logged once) if they cannot be */
int xgpu_metal_post_prepare(int smaa);

/* FXAA or SMAA over the corners (x0, y0, x1, y1, from the top left) of a
BGRA8 target's texture (xgpu_metal_texture_create's, not multisampled), of
its size; nonzero if it was antialiased. The backend's targets, pipeline,
dynamic state, textures and draw uniforms are the passes' afterwards: the
caller sets its own again. */
int xgpu_metal_post_anti_alias(int smaa, uint32_t target, uint32_t width, uint32_t height, const int32_t corners[4]);

#endif
