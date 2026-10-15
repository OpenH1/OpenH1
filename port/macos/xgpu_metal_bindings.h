/*
XGPU_METAL_BINDINGS.H

Where the Metal backend (xgpu_metal.m) binds what a draw's shaders read: the
argument table indices the MSL emitter (port/linux/src/xgpu_msl.c) writes in
its [[buffer(n)]], [[texture(n)]] and [[sampler(n)]] attributes. They are
kept here alone, so that the two agree by changing one file. Plain #defines,
usable from C, Objective-C and MSL.

Until the emitter's layout is settled these are the backend's choice.
*/

#ifndef __HALO_MACOS_XGPU_METAL_BINDINGS_H
#define __HALO_MACOS_XGPU_METAL_BINDINGS_H

/* the vertex constants, 192 float4 (3 KB: setVertexBytes) */
#define XGPU_METAL_BUFFER_VERTEX_CONSTANTS 0
#define XGPU_METAL_VERTEX_CONSTANT_COUNT 192
/* the draw's other uniforms (struct xgpu_draw_uniforms, as the front end
writes it), bound to both stages (setVertexBytes, setFragmentBytes) */
#define XGPU_METAL_BUFFER_DRAW_UNIFORMS 1
/* (the VR mode's uniforms, should the macOS app ever draw both eyes) */
#define XGPU_METAL_BUFFER_VIEW_UNIFORMS 2
/* the values of the inputs no stream feeds, 16 float4 (setVertexBytes),
read by the vertex descriptor at a constant step: the shaders read every
input as [[attribute(n)]] whether a stream feeds it or not */
#define XGPU_METAL_BUFFER_CONSTANT_ATTRIBUTES 13
/* the vertex streams: stream n is buffer XGPU_METAL_BUFFER_STREAM(n), and
attribute n of the vertex descriptor is [[attribute(n)]] */
#define XGPU_METAL_STREAM_COUNT 16
#define XGPU_METAL_BUFFER_STREAM(stream) (14 + (stream))

/* texture stage n samples texture n with sampler n */
#define XGPU_METAL_TEXTURE_STAGE(stage) (stage)
#define XGPU_METAL_SAMPLER_STAGE(stage) (stage)
#define XGPU_METAL_TEXTURE_STAGE_COUNT 4

#endif
