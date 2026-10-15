/*
XGPU_METAL_INTERNAL.H

What the Metal backend (xgpu_metal.m) and the window's presentation
(host_metal.m) share. Objective-C.
*/

#ifndef __HALO_MACOS_XGPU_METAL_INTERNAL_H
#define __HALO_MACOS_XGPU_METAL_INTERNAL_H

#import <Metal/Metal.h>

#include "xgpu_metal.h"

/* ---------- xgpu_metal.m */

/* the device and the queue every command buffer goes to (nil before
xgpu_metal_initialize) */
id<MTLDevice> xgpu_metal_device(void);
id<MTLCommandQueue> xgpu_metal_queue(void);
/* a command buffer's GPU start and end, counted in the GPU's busy time */
void xgpu_metal_gpu_interval(double start, double end);
/* a texture's Metal texture (nil for none) */
id<MTLTexture> xgpu_metal_texture(uint32_t texture);

/* ---------- host_metal.m */

/* a window shows the frames presented */
int host_metal_window_active(void);
/* one of the presentation's images, width by height (BGRA8), to copy a
frame's picture into: waits while every image is queued or on screen, so
that the game runs no faster than the display shows its frames */
id<MTLTexture> host_metal_present_acquire(uint32_t width, uint32_t height);
/* the image is shown at the display's next refresh after those queued
before it (after the command buffers committed before this call: the
frame's must be) */
void host_metal_present_queue(id<MTLTexture> image);

#endif
