/*
XGPU_VULKAN_TEXTURES.H

The Vulkan backend's cache of the textures the command stream's stages draw
that are not render targets (xgpu_vulkan_textures.c): the Xbox's textures,
read from the guest's memory, and the high-res replacements' images. Called
on the render thread.
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_TEXTURES_H
#define __HALO_LINUX_XGPU_VULKAN_TEXTURES_H

#include <stdint.h>

struct xgpu_stream_texture_stage;
struct xgpu_vulkan_image;

/* the image a stage draws: an Xbox texture's (XGPU_SOURCE_TEXTURE), made
from the guest's memory and made again once its pages have been written,
or an image's (XGPU_STREAM_SOURCE_IMAGE); NULL if there is none */
struct xgpu_vulkan_image *xgpu_vulkan_textures_get(const struct xgpu_stream_texture_stage *stage);

/* A texture whose header is named by the VR stream (the visor's plasma
noise), checked and decoded through the same cache as a draw's stage. */
struct xgpu_vulkan_image *xgpu_vulkan_textures_header(uint32_t header);

/* a frame has been played: the textures no stage has drawn for a while are
let go */
void xgpu_vulkan_textures_frame_end(void);

/* statistics since the last call: textures made, and made again from pages
that were written; images; their bytes read from the guest */
struct xgpu_vulkan_texture_statistics
{
	uint32_t made, remade, images;
	uint64_t bytes;
};
void xgpu_vulkan_textures_statistics(struct xgpu_vulkan_texture_statistics *statistics);

#endif
