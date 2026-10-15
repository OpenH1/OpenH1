/*
XGPU_BACKEND.H

What the renderer's front end (the Xbox Direct3D 8 device: what the game's
states mean) hands its backend (how a GPU API draws it: OpenGL today), for
each draw: a pipeline key, everything a pipeline object bakes in (shaders,
vertex layout, blend, depth and stencil, raster state, the targets'
formats), and the dynamic state no API bakes in (viewport, scissor, depth
range, stencil reference, blend color, depth bias). Neither holds an Xbox
or a GL value: the front end translates the game's D3D states into these,
each backend into its API's. Refer to the renderer's design notes in
port/linux/README.md ("The renderer").
*/

#ifndef __HALO_LINUX_XGPU_BACKEND_H
#define __HALO_LINUX_XGPU_BACKEND_H

/* ---------- the pipeline key's values */

enum
{
	XGPU_COMPARE_NEVER, XGPU_COMPARE_LESS, XGPU_COMPARE_EQUAL, XGPU_COMPARE_LESS_EQUAL, XGPU_COMPARE_GREATER,
	XGPU_COMPARE_NOT_EQUAL, XGPU_COMPARE_GREATER_EQUAL, XGPU_COMPARE_ALWAYS,
};

enum
{
	XGPU_BLEND_ZERO, XGPU_BLEND_ONE, XGPU_BLEND_SOURCE_COLOR, XGPU_BLEND_ONE_MINUS_SOURCE_COLOR,
	XGPU_BLEND_SOURCE_ALPHA, XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA, XGPU_BLEND_DESTINATION_ALPHA,
	XGPU_BLEND_ONE_MINUS_DESTINATION_ALPHA, XGPU_BLEND_DESTINATION_COLOR, XGPU_BLEND_ONE_MINUS_DESTINATION_COLOR,
	XGPU_BLEND_SOURCE_ALPHA_SATURATE, XGPU_BLEND_CONSTANT_COLOR, XGPU_BLEND_ONE_MINUS_CONSTANT_COLOR,
	XGPU_BLEND_CONSTANT_ALPHA, XGPU_BLEND_ONE_MINUS_CONSTANT_ALPHA,
};

enum
{
	XGPU_BLEND_ADD, XGPU_BLEND_SUBTRACT, XGPU_BLEND_REVERSE_SUBTRACT, XGPU_BLEND_MIN, XGPU_BLEND_MAX,
};

enum
{
	XGPU_STENCIL_KEEP, XGPU_STENCIL_ZERO, XGPU_STENCIL_REPLACE, XGPU_STENCIL_INCREMENT_CLAMP,
	XGPU_STENCIL_DECREMENT_CLAMP, XGPU_STENCIL_INVERT, XGPU_STENCIL_INCREMENT_WRAP, XGPU_STENCIL_DECREMENT_WRAP,
};

enum
{
	XGPU_FILL_SOLID, XGPU_FILL_LINES, XGPU_FILL_POINTS,
};

/* the color write mask's bits */
enum
{
	XGPU_WRITE_RED = 1, XGPU_WRITE_GREEN = 2, XGPU_WRITE_BLUE = 4, XGPU_WRITE_ALPHA = 8,
};

/* the targets' formats */
enum
{
	XGPU_FORMAT_NONE, XGPU_FORMAT_BGRA8, XGPU_FORMAT_D24S8,
};

/* the vertex shader's lighting variant: none, a model lighting program's
for each pixel (its lights: 1 the ambient and distant lights, 2 the point
lights too), a structure's lightmap pass's for the enhanced dynamic lights */
enum
{
	XGPU_LIGHTING_NONE, XGPU_LIGHTING_MODEL, XGPU_LIGHTING_MODEL_POINT, XGPU_LIGHTING_ENVIRONMENT,
};

/* ---------- the pipeline key

Zeroed before it is filled (keys are hashed and compared whole). */

struct xgpu_pipeline_key
{
	/* the vertex program (its microcode's hash: xgpu_shader.h), drawn from
	immediate mode's registers (all floats) or from streams with these
	attributes packed (NORMPACKED3), lit as XGPU_LIGHTING_*, into these
	views (XGPU_MULTIVIEW_*: xgpu.h) */
	unsigned long long vertex_program;
	unsigned long packed_attributes;
	/* (the stencil masks tested and written: below) */
	unsigned long stencil_read_mask, stencil_write_mask;
	unsigned char immediate, lighting, views;
	/* blend: enabled, the source and destination factors (XGPU_BLEND_*) and
	the operation, and the channels written (XGPU_WRITE_*) */
	unsigned char blend, blend_source, blend_destination, blend_operation;
	unsigned char color_mask;
	/* depth: tested (XGPU_COMPARE_*), written */
	unsigned char depth_test, depth_function, depth_write;
	/* stencil: tested, the operations where the stencil test fails, where
	the depth test does, and where both pass (XGPU_STENCIL_*), and the masks
	(above) */
	unsigned char stencil_test, stencil_function, stencil_fail, stencil_depth_fail, stencil_pass;
	/* raster: culled; the front faces' winding where the Xbox's screen has
	them (rows from the top), counter-clockwise or not, and the front faces
	culled (else the back); filled (XGPU_FILL_*); depth biased (the bias is
	dynamic) */
	unsigned char cull, front_counter_clockwise, cull_front;
	unsigned char fill, depth_bias;
	/* the targets: their formats (XGPU_FORMAT_*) and samples a pixel */
	unsigned char color_format, depth_format, samples;
	unsigned char pad[2];
	/* the pixel shader */
	struct nv2a_pixel_shader_key pixel;
};

/* ---------- dynamic state */

struct xgpu_dynamic_state
{
	/* the viewport and the scissor rectangle in the targets' pixels (left,
	top from the first row, width, height), the depth range */
	long viewport[4];
	long scissor[4];
	float depth_range[2];
	/* the stencil test's reference; the blend color (RGBA, 0 to 1); the
	depth bias, its slope factor and its offset in the depth's units */
	unsigned long stencil_reference;
	float blend_color[4];
	float depth_bias[2];
};

#endif
