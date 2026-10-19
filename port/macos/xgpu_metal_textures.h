/*
XGPU_METAL_TEXTURES.H

The Metal backend's cache of the textures the command stream's stages draw
that are not render targets (xgpu_metal_textures.c): the Xbox's textures,
read from the guest's memory, and the high-res replacements' images.
Called on the render thread.
*/

#ifndef __HALO_MACOS_XGPU_METAL_TEXTURES_H
#define __HALO_MACOS_XGPU_METAL_TEXTURES_H

#include <stdint.h>

struct xgpu_stream_texture_stage;

/* the backend's texture (xgpu_metal.h) a stage draws: an Xbox texture's
(XGPU_SOURCE_TEXTURE), made from the guest's memory and made again once its
pages have been written, or an image's (XGPU_STREAM_SOURCE_IMAGE); 0 if
there is none */
uint32_t xgpu_metal_textures_get(const struct xgpu_stream_texture_stage *stage);

/* a frame has been played: the textures no stage has drawn for half a
minute of time are let go */
void xgpu_metal_textures_frame_end(void);

/* statistics since the last call: textures made, and made again from
pages that were written; their bytes read from the guest */
struct xgpu_metal_texture_statistics
{
	uint32_t made, remade, images;
	uint64_t bytes;
};
void xgpu_metal_textures_statistics(struct xgpu_metal_texture_statistics *statistics);

#endif
