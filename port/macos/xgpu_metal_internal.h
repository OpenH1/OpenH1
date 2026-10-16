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
/* the library of the clears' and the presentation's functions
(xgpu_metal_fixed_source's) */
id<MTLLibrary> xgpu_metal_fixed_library(void);
/* a command buffer's GPU start and end, counted in the GPU's busy time */
void xgpu_metal_gpu_interval(double start, double end);
/* the render thread waits for the GPU to finish a frame (xgpu_metal_frame_wait) */
int xgpu_metal_frame_waiting(void);
/* a texture's Metal texture (nil for none) */
id<MTLTexture> xgpu_metal_texture(uint32_t texture);

/* ---------- host_metal.m */

/* a window shows the frames presented */
int host_metal_window_active(void);
/* takes a frame's picture (a texture the frame drew) as one of the
presentation's images, in the place of one neither queued nor on screen:
waits while there is none, so that the game runs no faster than the display
shows its frames. Returns the image it replaces, for the next frame to draw
into, where that is of the picture's size and format (else nil) */
id<MTLTexture> host_metal_present_exchange(id<MTLTexture> picture);
/* the image is shown at the display's next refresh after those queued
before it (after the command buffers committed before this call: the
frame's must be) */
void host_metal_present_queue(id<MTLTexture> image);

#endif
