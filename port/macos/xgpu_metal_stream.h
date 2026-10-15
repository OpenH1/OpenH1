/*
XGPU_METAL_STREAM.H

The Metal backend as the command stream's backend table
(xgpu_metal_stream.m; port/android/host/host_render_stream.h).
*/

#ifndef __HALO_MACOS_XGPU_METAL_STREAM_H
#define __HALO_MACOS_XGPU_METAL_STREAM_H

#include <stdint.h>

struct host_render_backend;
struct xgpu_stream_pipeline_define;
struct xgpu_stream_vertex_program;

/* the table (made once; xgpu_metal_initialize first) */
const struct host_render_backend *xgpu_metal_stream_backend(void);

/* Where a pipeline's shaders come from: make sets the MSL functions
(xgpu_metal_shader_create's handles) for the pipeline's vertex program, in
its key's variant, and its pixel program, and returns nonzero; program is
the vertex program's record (NULL if none was sent). The MSL emitter's
place, until it is wired; tests give their own. */
struct xgpu_metal_stream_shaders
{
	void *context;
	int (*make)(void *context, const struct xgpu_stream_pipeline_define *definition,
		const struct xgpu_stream_vertex_program *program, uint32_t *vertex_shader, uint32_t *pixel_shader);
};

void xgpu_metal_stream_set_shaders(const struct xgpu_metal_stream_shaders *shaders);

#endif
