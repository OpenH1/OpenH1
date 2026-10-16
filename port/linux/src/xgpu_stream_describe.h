/*
XGPU_STREAM_DESCRIBE.H

The interface's pipeline keys and pixel programs as the command stream's
fixed-width structures (xgpu_stream_describe.c).
*/

#ifndef __HALO_XGPU_STREAM_DESCRIBE_H
#define __HALO_XGPU_STREAM_DESCRIBE_H

struct nv2a_pixel_shader_key;
struct xgpu_pipeline_key;
struct xgpu_pixel_program;
struct xgpu_stream_pipeline_key;
struct xgpu_stream_pixel_key;
struct xgpu_stream_pixel_program;

void xgpu_stream_pixel_key_describe(const struct nv2a_pixel_shader_key *key, struct xgpu_stream_pixel_key *wire);
/* (zeroed, then filled) */
void xgpu_stream_pipeline_key_describe(const struct xgpu_pipeline_key *key, struct xgpu_stream_pipeline_key *wire);
void xgpu_stream_pixel_program_describe(const struct xgpu_pixel_program *program,
	struct xgpu_stream_pixel_program *wire);
/* the key a wire key describes (zeroed, then filled) */
void xgpu_stream_pixel_key_restore(const struct xgpu_stream_pixel_key *wire, struct nv2a_pixel_shader_key *key);

#endif
