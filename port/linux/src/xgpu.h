/*
XGPU.H

Internals shared by the OpenGL implementation of the Xbox Direct3D API:
the NV2A shader translators (nv2a_vsh.c, nv2a_psh.c), texture decoding
(xbox_textures.c), guest memory write tracking (memory_watch.c) and the
device itself (d3d8_gl.c).
*/

#ifndef __HALO_LINUX_XGPU_H
#define __HALO_LINUX_XGPU_H

#include "platform.h"
#include "gl.h"

#ifdef HALO_GLES
/* OpenGL ES features that are optional (d3d8_gl.c gl_initialize) */
struct xgpu_capabilities
{
	BOOL copy_image;
	BOOL border_clamp;
	BOOL anisotropy;
	BOOL s3tc;
	/* ES 3.2, or the EXT/OES_draw_elements_base_vertex extension: glDrawElementsBaseVertex */
	BOOL base_vertex;
	/* ES 3.1 with fragment atomic counters: exact visibility test counts */
	BOOL atomic_counters;
	/* "300 es" or "310 es" */
	const char *shading_language;
};

extern struct xgpu_capabilities xgpu_capabilities;

/* port/android/guest/runtime/guest_host.h */
int host_gl_has_extension(const char *name);
void host_gl_read_buffer(unsigned int buffer, unsigned int offset, unsigned int size, void *data);
void host_gl_buffer_write(unsigned int target, unsigned int offset, unsigned int size, const void *data);
void host_gl_fence_frame(unsigned int slot);
void host_gl_wait_frame(unsigned int slot);
#elif defined(HALO_ARM64_GUEST)
/* the Linux arm64 build's desktop renderer (port/android/host/host_gl.c) */
void host_gl_buffer_write(unsigned int target, unsigned int offset, unsigned int size, const void *data);
void host_gl_fence_frame(unsigned int slot);
void host_gl_wait_frame(unsigned int slot);
int host_gl_map_results(unsigned int buffer, unsigned int size);
void host_gl_read_results(void *data, unsigned int size);
void host_gl_read_buffer(unsigned int buffer, unsigned int offset, unsigned int size, void *data);
#endif

/* ---------- GL state

The device caches the GL state it sets for draws (d3d8_gl.c); code that
changes GL state behind it (binding a texture to upload it, deleting one)
must call this afterwards. */

void xgpu_gl_state_invalidate(void);
/* a texture about to be deleted: the binding shadows forget it, since GL
may give its name to a new texture (call before glDeleteTextures) */
void xgpu_gl_texture_deleted(unsigned int texture);

/* a compiled shader, or a linked program of two, or 0 with the log
written */
GLuint xgpu_compile_shader(GLenum type, const char *code, const char *what);
GLuint xgpu_link_program(GLuint vertex_shader, GLuint fragment_shader, const char *what);

/* ---------- generated source text */

struct xgpu_text
{
	char *buffer;
	unsigned long length;
	unsigned long capacity;
};

void xgpu_text_append(struct xgpu_text *text, const char *format, ...) __attribute__((format(printf, 2, 3)));

/* ---------- vertex shaders */

#define XGPU_VERTEX_ATTRIBUTE_COUNT 16
#define XGPU_VERTEX_CONSTANT_COUNT 192
/* D3D constant register -96 is hardware register 0 */
#define XGPU_VERTEX_CONSTANT_BIAS 96

/* where one of the game's model lighting programs (d3d8_gl.c
halo_vertex_shader_lighting) has the normal, and the world position, that it
lights the diffuse color by: the temporary register that holds each before
the instruction given */
struct nv2a_vertex_lighting
{
	/* 1 by the ambient and distant lights, 2 by the point lights too */
	int lights;
	unsigned long normal_instruction, normal_register;
	/* (with the point lights only) */
	unsigned long position_instruction, position_register;
	/* the world position the program projects to the screen (the
	temporary its clip position is computed from, before the instruction
	given), for the enhanced dynamic lights (display.dynamic_lights): with
	the point lights, the same as theirs; world_found FALSE if none */
	BOOL world_found;
	unsigned long world_instruction, world_register;
};

/* the lights value of a structure's (the environment's) lightmap pass lit
by the enhanced dynamic lights: its vertices' world position, normal,
binormal and tangent (v0 to v3) handed on as they are */
#define XGPU_ENVIRONMENT_LIGHTING 3

/* finds them in a model lighting program, checking that it lights its
diffuse color as nv2a_psh.c does for each pixel; FALSE if it does not */
BOOL nv2a_vertex_shader_lighting(const DWORD *instructions, unsigned long instruction_count,
	struct nv2a_vertex_lighting *lighting);

/* GLSL for an NV2A vertex program (the instruction words after the program
header). Attributes whose bit is set in packed_attribute_mask are fed as
NORMPACKED3 32-bit integers and unpacked in the shader. With lighting (else
NULL), the normal and world position go to the pixel shader too, which
lights the diffuse color for each pixel (nv2a_pixel_shader_key
per_pixel_lighting). With multiview XGPU_MULTIVIEW_EYES, the program draws
both of the VR mode's eyes at once (GL_OVR_multiview2): the clip position
goes through eye_correction[gl_ViewID_OVR] (d3d8_gl.c); with
XGPU_MULTIVIEW_LEFT_EYE or XGPU_MULTIVIEW_RIGHT_EYE, that eye's layer alone.
Returns a malloc'd string. */
enum
{
	XGPU_MULTIVIEW_NONE = 0,
	XGPU_MULTIVIEW_EYES,
	/* the eye pass's visibility tests: queries in a multiview pass would
	count in as many query slots as views (Vulkan), which Zink does not
	allow for, and the GPU writes past them */
	XGPU_MULTIVIEW_LEFT_EYE,
	/* a draw into each eye's layer in turn, each with its own screen
	correction (the crosshairs: halo_vr_draw_eye) */
	XGPU_MULTIVIEW_RIGHT_EYE,
};
/* The VR mode's eye pass is foveated (vr.foveation, d3d8_gl.c): its targets
have four layers, two for each eye (layer 2 eye + 0, the whole eye at the
periphery's resolution; 2 eye + 1, the inset about the gaze at full
resolution, as large in pixels). XGPU_MULTIVIEW_EYES then draws all four
(eye_correction[VIEW >> 1], then fovea_view[VIEW] scales and moves the
inset's views), XGPU_MULTIVIEW_LEFT_EYE and XGPU_MULTIVIEW_RIGHT_EYE that
eye's two (its visibility tests are counted by the pixel shaders instead),
and a layered target is sampled in an inset's layer through
fovea_sample[layer] (its pixels from the whole eye's), from the whole eye's
layer where the inset does not have the point. Set before the first VR
shader is made. */
extern int xgpu_foveated;
char *nv2a_vertex_shader_to_glsl(const DWORD *instructions, unsigned long instruction_count,
	unsigned long packed_attribute_mask, const struct nv2a_vertex_lighting *lighting, int multiview);

/* ---------- pixel shaders */

enum
{
	_xgpu_sampler_none = 0,
	_xgpu_sampler_2d,
	_xgpu_sampler_3d,
	_xgpu_sampler_cube,
	/* a 2-layer render target (the VR mode's eye pass; 4 foveated,
	xgpu_foveated): the view's layer */
	_xgpu_sampler_2d_layered,
};

/* everything a translated pixel shader depends on; the GLSL program cache
is keyed by these bytes */
struct nv2a_pixel_shader_key
{
	DWORD combiner_state[D3DRS_PS_MAX];
	/* D3DRS_PSTEXTUREMODES lies past D3DRS_PS_MAX */
	DWORD texture_modes;
	unsigned char sampler_type[4];
	unsigned char alpha_kill[4];
	/* D3DTSS_COLORSIGN: channels (bit 0 alpha ... bit 3 blue, as
	D3DTSIGN_*) that hold signed data in an unsigned texture format */
	unsigned char color_sign[4];
	/* D3DCMP_* function for the alpha test, or 0 when disabled */
	unsigned long alpha_test_function;
	unsigned char fog_enable;
	unsigned char fog_table_mode;
	/* inside a visibility test: count the samples that pass (Android) */
	unsigned char count_samples;
	/* a high-res HUD meter (hud_hires.h) drawn with the meter's blend (the
	destination kept by the source's alpha): that alpha is eased to 1 by the
	coverage texture 0's green holds, so that the meter darkens what is
	behind it only where it covers it (the Xbox's point-sampled meters stop
	at their texels' edges; filtered ones have a fringe of faint texels) */
	unsigned char coverage_alpha;
	/* Discrete meter thresholds in texture 0's red are read at level zero,
	without filtering. Coverage/brightness still use the filtered lookup. */
	unsigned char point_threshold;
	/* a model lighting program's draw lit for each pixel
	(display.per_pixel_lighting): nv2a_vertex_lighting's lights, or 0 for
	the diffuse color the vertex shader computed */
	unsigned char per_pixel_lighting;
	/* drawn into a multisampled target (display.anti_aliasing's
	multisampling), its samples a pixel: the alpha test covers samples in
	proportion to how far alpha is past the reference, not all of the pixel
	or none of it, so that cut-out edges (foliage, grates) are smoothed too */
	unsigned char alpha_test_samples;
	/* drawn into both eyes at once (the VR mode, d3d8_gl.c): layered
	render targets are sampled at the view's layer (XGPU_MULTIVIEW_*) */
	unsigned char multiview;
	/* the source's blend factor, applied here instead (1 the blend color, 2
	one less it, 3 its alpha, 4 one less that; 0 none): WebGL refuses a
	blend of a constant color factor with a constant alpha one, as the
	meters' (shader_transparent_meter) */
	unsigned char source_blend;
	/* the enhanced dynamic lights (display.dynamic_lights, d3d8_gl.c) added
	for each pixel: XGPU_DYNAMIC_LIGHTS_ENVIRONMENT to a structure's lightmap
	pass (into the light the textures are then multiplied by, the bump map
	in texture 0), XGPU_DYNAMIC_LIGHTS_MODEL to a model's lighting lit for
	each pixel in place of its two point lights; 0 none */
	unsigned char dynamic_lights;
	unsigned char pad[3];
};

char *nv2a_pixel_shader_to_glsl(const struct nv2a_pixel_shader_key *key);

#ifdef HALO_GLES
/* ES samplers have no LOD bias of their own */
#define XGPU_PIXEL_UNIFORMS_ES "uniform vec4 texture_lod_bias;\n"
#else
#define XGPU_PIXEL_UNIFORMS_ES ""
#endif

/* the combiner registers that live in uniforms rather than in the program:
C0/C1 of each stage and the final combiner, and texture constants */
#define XGPU_PIXEL_UNIFORMS \
	"uniform vec4 ps_c0[8];\n" \
	"uniform vec4 ps_c1[8];\n" \
	"uniform vec4 ps_final_c0;\n" \
	"uniform vec4 ps_final_c1;\n" \
	"uniform vec4 fog_color;\n" \
	"uniform vec4 fog_parameters;\n" \
	"uniform float alpha_reference;\n" \
	"uniform vec4 bump_matrix[4];\n" \
	"uniform vec4 bump_luminance[4];\n" \
	"uniform vec4 texture_scale[4];\n" \
	XGPU_PIXEL_UNIFORMS_ES

/* the vertex constants the per-pixel model lighting reads, in a uniform of
their own (the vertex shader's 192 would pass OpenGL ES's least fragment
uniform space): [0] c[-82] (the translucency in z), [1] to [11] c[-79] to
c[-69] (rasterizer_set_model_lighting's two point lights, two distant
lights and the ambient light) */
#define XGPU_MODEL_LIGHT_COUNT 12

/* ---------- enhanced dynamic lights

display.dynamic_lights "enhanced" (d3d8_gl.c, halo_dynamic_lights_set):
the frame's dynamic lights (up to the game's 128 a window) light the
structure's lightmap pass and the models lit for each pixel, each pixel by
the lights near it alone. A grid about the camera, its cells boxes in the
world (eye-independent: the VR mode's eyes share it), holds for each cell a
128-bit mask of the lights whose spheres reach it (an integer 3D texture at
XGPU_DYNAMIC_LIGHTS_UNIT); the lights are a uniform block at
XGPU_DYNAMIC_LIGHTS_BINDING. The cells grow away from the camera: a cell's
index along each axis is log2(1 + |offset| / XGPU_DYNAMIC_LIGHTS_CELL) times
that axis's scale, from the middle, so that the grid reaches
XGPU_DYNAMIC_LIGHTS_REACH world units each way with small cells near the
camera, where lights are seen large. Desktop OpenGL only. */
#define XGPU_DYNAMIC_LIGHTS_MAXIMUM 128
#define XGPU_DYNAMIC_LIGHTS_GRID_X 32
#define XGPU_DYNAMIC_LIGHTS_GRID_Y 32
#define XGPU_DYNAMIC_LIGHTS_GRID_Z 16
#define XGPU_DYNAMIC_LIGHTS_CELL 0.5f
#define XGPU_DYNAMIC_LIGHTS_REACH 128.0f
/* the largest soft core of a light's falloff, world units (nv2a_psh.c) */
#define XGPU_DYNAMIC_LIGHTS_CORE 0.6
#define XGPU_DYNAMIC_LIGHTS_UNIT 12
#define XGPU_DYNAMIC_LIGHTS_BINDING 1
enum
{
	XGPU_DYNAMIC_LIGHTS_ENVIRONMENT = 1,
	XGPU_DYNAMIC_LIGHTS_MODEL,
};

/* the uniform block, as std140 lays it out: [0] the grid's middle (the
camera) and the light count, [1] each axis's cells a doubling of the offset
and 1 / XGPU_DYNAMIC_LIGHTS_CELL, [2] the look (nv2a_psh.c); then each
light's position and 1 / radius (less than 0: models only, the structure
lit by the game's own pass: a spotlight's gel), its color (linear, the
look's brightness in it) and its cone's offset, its cone's axis and scale
(omni: 0, 0 and an offset of 1) */
struct xgpu_dynamic_lights_block
{
	float grid[3][4];
	float position[XGPU_DYNAMIC_LIGHTS_MAXIMUM][4];
	float color[XGPU_DYNAMIC_LIGHTS_MAXIMUM][4];
	float cone[XGPU_DYNAMIC_LIGHTS_MAXIMUM][4];
};

/* the GLSL of the block, the grid and the functions that light by them
(nv2a_psh.c) */
void xgpu_dynamic_lights_glsl(struct xgpu_text *text);

/* ---------- textures */

struct xgpu_texture_description
{
	DWORD format;       /* D3DFMT_* */
	unsigned long width, height, depth, levels;
	BOOL cube_map;
	BOOL linear;        /* not swizzled; addressed with texel coordinates */
	BOOL compressed;
	unsigned long pitch; /* linear textures */
	BOOL hires;         /* a high-res HUD texture drawn in the texture's place (hud_hires.h) */
	BOOL hires_coverage; /* ... whose green is its coverage (a meter's) */
	BOOL hires_point_threshold; /* ... whose red holds discrete segment data */
};

void xgpu_texture_describe(DWORD format_word, DWORD size_word, struct xgpu_texture_description *description);
/* bytes of one face, mip levels included (cube faces are padded) */
unsigned long xgpu_texture_face_size(const struct xgpu_texture_description *description);
unsigned long xgpu_texture_level_offset(const struct xgpu_texture_description *description, unsigned long level);
unsigned long xgpu_texture_level_pitch(const struct xgpu_texture_description *description, unsigned long level);

/* the GL texture for an Xbox texture header, uploading or refreshing it
from guest memory as needed; *target receives GL_TEXTURE_2D etc. */
GLuint xgpu_texture_get(const DWORD *resource, const D3DCOLOR *palette, GLenum *target,
	struct xgpu_texture_description *description);
void xgpu_texture_cache_begin_frame(void);

/* ---------- render targets */

struct xgpu_render_target
{
	unsigned long data;  /* physical address */
	unsigned long width, height;
	BOOL depth;
	GLuint texture;
	/* pixels per unit of width and height: more than 1 for the screen's
	targets when the game draws at the display's resolution (d3d8_gl.c) */
	float scale[2];
	unsigned long gl_width, gl_height;
	/* with multisampling, the multisampled renderbuffer draws go to, its
	samples a pixel (0 when it has none), and whether it has been drawn into
	since the texture last had its pixels (d3d8_gl.c,
	render_target_multisample) */
	GLuint multisample;
	int samples;
	BOOL unresolved;
	/* changes whenever the target is drawn into or cleared (d3d8_gl.c,
	bind_targets) */
	unsigned long written;

	/* 2: a texture array, a layer for each eye, drawn into with multiview
	(the VR mode's eye pass, d3d8_gl.c); 4 foveated (xgpu_foveated); else 1 */
	int layers;
};

/* the GL texture holding a render target with this physical address, or 0 */
struct xgpu_render_target *xgpu_render_target_find(unsigned long data);

/* ---------- anti-aliasing

display.anti_aliasing's passes (xgpu_post.c): FXAA or SMAA antialias each
window's 3D view in place before the HUD and menus are drawn over it, so
that their text stays sharp. Supersampling and multisampling are the
device's (d3d8_gl.c). */

/* the programs and textures of FXAA, or of SMAA, made now; FALSE if they
cannot be (once FALSE, it stays so) */
BOOL xgpu_post_prepare(BOOL smaa);

/* FXAA, or SMAA, on the corners x0, y0 to x1, y1 (from row 0) of a render
target's framebuffer, width by height, GL_RGBA8; FALSE if its programs do
not build */
BOOL xgpu_post_anti_alias(BOOL smaa, GLuint framebuffer, unsigned long width, unsigned long height,
	const GLint corners[4]);

#endif
