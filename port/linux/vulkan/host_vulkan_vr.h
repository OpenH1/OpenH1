/* The Vulkan renderer's VR passes and composition, behind the shared host
VR stream and late latch (host_vr_stream.h). */
#ifndef __HALO_HOST_VULKAN_VR_H
#define __HALO_HOST_VULKAN_VR_H

#include <stdint.h>
struct host_render_backend;
struct xgpu_spirv_uniforms;
struct xgpu_vulkan_image;

void host_vulkan_vr_backend(struct host_render_backend *backend);
int host_vulkan_vr_active(void);
int host_vulkan_vr_foveated(void);
int host_vulkan_vr_sky(void);
void host_vulkan_vr_uniforms(struct xgpu_spirv_uniforms *uniforms, const int32_t viewport[4]);
struct xgpu_vulkan_image *host_vulkan_vr_hud_group(uint32_t hud, int group);

#endif
