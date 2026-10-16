/*
XGPU_SPIRV.H

SPIR-V from the shader intermediate form (xgpu_shader.h), for the Vulkan
backend (port/linux/vulkan): xgpu_spirv.c. The programs are the GLSL
emitter's (xgpu_glsl.c, desktop OpenGL's), written as SPIR-V words directly
rather than as GLSL for a compiler: the shaders are made from pipeline keys
as the game first draws with them, and a GLSL compiler in the host would
cost a library of megabytes and milliseconds a shader there, where these
programs (no control flow but discarding, a few dozen kinds of operation)
are a walk over the form. spirv-val checks the words
(tools/spirv_corpus).

The conventions (SPIR-V 1.3, Vulkan 1.1):

- clip space is Vulkan's (z 0 to 1, the first row at the top of the
  framebuffer, y down): the game's position (Direct3D's, y up) has its y
  negated, as OpenGL ES's (xgpu_glsl.c), so that the targets' first row is
  the Xbox's top one, as desktop OpenGL's glClipControl makes it;
- vertex attributes are at location n for v<n>. A D3DCOLOR is
  VK_FORMAT_B8G8R8A8_UNORM (read as RGBA, as GL_BGRA gives it); NORMPACKED3
  is VK_FORMAT_R32_UINT, unpacked here; the unnormalized shorts
  (integer_attribute_mask) are VK_FORMAT_R16*_SINT, read as integers and
  converted here; everything else is read as a vec4, the missing components
  0, 0, 0, 1;
- the draw's values are uniform buffers in descriptor set
  XGPU_SPIRV_SET_DRAW: the vertex constants (192 vec4) at
  XGPU_SPIRV_BINDING_CONSTANTS, struct xgpu_spirv_uniforms at
  XGPU_SPIRV_BINDING_UNIFORMS (both stages), the enhanced dynamic lights'
  block and cells (struct xgpu_dynamic_lights_block, a usampler3D) and the
  visibility tests' counters (a storage buffer of uints) at theirs;
- each stage's texture is a combined image sampler at binding n of
  XGPU_SPIRV_SET_TEXTURES; the samplers have D3DTSS_MIPMAPLODBIAS
  (mipLodBias) and the border colors (VK_EXT_custom_border_color), as
  OpenGL's do, or the shader draws the border as the Metal emitter's does
  (xgpu_spirv_options border);
- the stages hand on, at locations XGPU_SPIRV_LOCATION_*: the colors, the
  texture coordinates, fog, and with lighting the world normal, position,
  binormal and tangent;
- the VR mode uses Vulkan multiview, with ViewIndex relative to the
  attachment view's first layer. A draw of one eye uses that eye's layer
  (its two layers when foveated) as the attachment view. The eyes and all
  other targets have their first row at the top; unlike OpenXR's OpenGL
  images, neither the eye positions nor layered texture lookups flip rows.
*/

#ifndef __HALO_LINUX_XGPU_SPIRV_H
#define __HALO_LINUX_XGPU_SPIRV_H

#define XGPU_SPIRV_SET_DRAW 0
#define XGPU_SPIRV_BINDING_CONSTANTS 0
#define XGPU_SPIRV_BINDING_UNIFORMS 1
#define XGPU_SPIRV_BINDING_DYNAMIC_LIGHTS 2
#define XGPU_SPIRV_BINDING_DYNAMIC_LIGHT_CELLS 3
#define XGPU_SPIRV_BINDING_SAMPLES 4
#define XGPU_SPIRV_SET_TEXTURES 1

/* the locations of what the vertex shader hands the pixel shader */
enum
{
	XGPU_SPIRV_LOCATION_D0, XGPU_SPIRV_LOCATION_D1,
	XGPU_SPIRV_LOCATION_T0, XGPU_SPIRV_LOCATION_T1, XGPU_SPIRV_LOCATION_T2, XGPU_SPIRV_LOCATION_T3,
	XGPU_SPIRV_LOCATION_FOG,
	XGPU_SPIRV_LOCATION_WORLD_NORMAL, XGPU_SPIRV_LOCATION_WORLD_POSITION,
	XGPU_SPIRV_LOCATION_WORLD_BINORMAL, XGPU_SPIRV_LOCATION_WORLD_TANGENT,
};

/* the draw's values both stages read, laid out alike in C and in the
shaders' uniform block (std140: vec4s, and floats in fours): the backend's
draw uniforms, the per-pixel lighting's model lights, the blend color the
source factor takes in the shader, and each stage's border color */
struct xgpu_spirv_uniforms
{
	float viewport_scale[4];
	float viewport_offset[4];
	float point_size, screen_offset, alpha_reference, screen_points;
	float ps_c0[8][4];
	float ps_c1[8][4];
	float ps_final_c0[4];
	float ps_final_c1[4];
	float fog_color[4];
	float fog_parameters[4];
	float bump_matrix[4][4];
	float bump_luminance[4][4];
	float texture_scale[4][4];
	float blend_constant[4];
	/* each stage's border color (RGBA, 0 to 1), and whether its sampler
	filters linearly (1) or takes the nearest texel (0), for the stages
	whose border the shader draws (xgpu_spirv_options border) */
	float texture_border[4][4];
	float texture_border_linear[4];
	float model_lights[XGPU_MODEL_LIGHT_COUNT][4];
	/* the clip and screen corrections, as the stream delivers them;
	the matrices are column-major (four consecutive vec4 columns) */
	float eye_correction[2][16];
	float screen_correction[2][4];
	/* each foveated layer's scale and offset: fovea_view in Vulkan clip
	space (x/y down from the top left after the y negation), fovea_sample
	in texture space (its offset in texels). The whole eyes' are identity. */
	float fovea_view[4][4];
	float fovea_sample[4][4];
	/* this draw's counter in the visibility tests' uint storage buffer */
	float visibility_counter, vr_unused[3];
};

/* what the SPIR-V is written for (xgpu_spirv.c):
- count_samples: the pixel shaders of a visibility test count their samples
  themselves, an atomic counter at XGPU_SPIRV_BINDING_SAMPLES (else the
  backend counts with occlusion queries, and the shaders do nothing);
- border: for each stage, the axes (bit 0 u, 1 v, 2 w) whose border color
  the shader draws (its sampler clamping to the edge) where the sampler
  cannot (no VK_EXT_custom_border_color): part of the pipeline, as the
  pixel key is;
- multiview: the vertex shader's XGPU_MULTIVIEW_* mode (the pixel
  program carries its own); foveated: four layers, periphery and inset
  for each eye, rather than the two eyes alone;
- the pixel shaders' debugging settings, as the GLSL's. */
struct xgpu_spirv_options
{
	BOOL count_samples;
	unsigned char border[4];
	BOOL debug_texture0, debug_flat;
	int multiview;
	BOOL foveated;
};

/* a module's words (SPIR-V's own: 32 bits each), malloc'd, count of them */
struct xgpu_spirv_module
{
	unsigned int *words;
	unsigned long count;
};

/* the vertex program (xgpu_shader.h) as a vertex shader, entry point
"main": packed_attribute_mask's attributes are NORMPACKED3,
integer_attribute_mask's unnormalized shorts; with lighting (else NULL), the
normal and world position go to the pixel shader too, as the GLSL's.
FALSE if out of memory. */
BOOL xgpu_spirv_vertex_shader(const struct xgpu_vertex_program *program, unsigned long packed_attribute_mask,
	unsigned long integer_attribute_mask, const struct nv2a_vertex_lighting *lighting,
	const struct xgpu_spirv_options *options, struct xgpu_spirv_module *module);

/* the vertex inputs (bit n v<n>) the vertex shader of a program and its
lighting reads: the pipeline's vertex layout has these alone */
unsigned long xgpu_spirv_vertex_inputs(const struct xgpu_vertex_program *program,
	const struct nv2a_vertex_lighting *lighting);

/* the pixel program (xgpu_shader.h) as a fragment shader, entry point
"main". FALSE if out of memory. */
BOOL xgpu_spirv_pixel_shader(const struct xgpu_pixel_program *program, const struct xgpu_spirv_options *options,
	struct xgpu_spirv_module *module);

#endif
