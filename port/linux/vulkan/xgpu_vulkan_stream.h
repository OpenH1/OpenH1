/*
XGPU_VULKAN_STREAM.H

The Vulkan backend as the command stream's backend table
(xgpu_vulkan_stream.c; port/android/host/host_render_stream.h).
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_STREAM_H
#define __HALO_LINUX_XGPU_VULKAN_STREAM_H

struct host_render_backend;

/* the table (made once; xgpu_vulkan_initialize first) */
const struct host_render_backend *xgpu_vulkan_stream_backend(void);

#endif
