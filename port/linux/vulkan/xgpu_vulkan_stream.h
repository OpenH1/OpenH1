/*
XGPU_VULKAN_STREAM.H

The Vulkan backend as the command stream's backend table
(xgpu_vulkan_stream.c; port/android/host/host_render_stream.h).
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_STREAM_H
#define __HALO_LINUX_XGPU_VULKAN_STREAM_H

#include <stdint.h>

struct host_render_backend;
struct xgpu_vulkan_image;

/* the table (made once; xgpu_vulkan_initialize first) */
const struct host_render_backend *xgpu_vulkan_stream_backend(void);

/* Host VR composition uses the same numbered targets as the stream. A
replacement image remains owned by the composer, including at target deletion. */
struct xgpu_vulkan_image *xgpu_vulkan_stream_target(uint32_t number);
void xgpu_vulkan_stream_set_vr_target(uint32_t number, struct xgpu_vulkan_image *image);
void xgpu_vulkan_stream_vr_changed(void);

#endif
