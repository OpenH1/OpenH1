/*
XGPU_VULKAN_POST.H

display.anti_aliasing's passes for the Vulkan backend (xgpu_vulkan_post.c):
FXAA or SMAA antialias a window's 3D view in place before the HUD and menus
are drawn over it, as the OpenGL backend's (port/linux/src/xgpu_post.c).
Supersampling and multisampling are the front end's and the core's.
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_POST_H
#define __HALO_LINUX_XGPU_VULKAN_POST_H

#include <stdint.h>

struct xgpu_vulkan_image;

/* the pipelines of FXAA, or of SMAA's passes, and SMAA's lookup textures,
made now; queued for the pipelines' compiler instead where preparing (a
map's preparation, which waits for them); 0 if they cannot be made (logged;
once 0, it stays so) */
int xgpu_vulkan_post_prepare(int smaa, int preparing);

/* FXAA, or SMAA, over the corners x0, y0 to x1, y1 (pixels from row 0) of a
layer of a BGRA8 target, in the stream's order; the targets bound before
are not bound after. 0 if its pipelines cannot be made */
int xgpu_vulkan_post_anti_alias(int smaa, struct xgpu_vulkan_image *target, uint32_t layer, const int32_t corners[4]);

/* The present step's turn, for a swapchain made for the display's own
orientation while the phone is held another way (Android's pre-rotation:
the compositor then has nothing to turn). prepare makes its pipeline, at the
start (not while a map is prepared, since rotate may be called by the
loading screen's presentation then): 0 if it cannot be made (logged).
rotate draws an image turned by quarter turns clockwise (1 to 3) into an
image of its own (its width and height swapped for an odd count), in the
stream's order, and returns it; the targets bound before are not bound
after. NULL if it cannot. */
int xgpu_vulkan_post_rotate_prepare(void);
struct xgpu_vulkan_image *xgpu_vulkan_post_rotate(struct xgpu_vulkan_image *image, int quarter_turns);

#endif
