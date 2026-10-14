/*
D3D8_GL.C

The Xbox Direct3D 8 device, implemented with OpenGL 4.5.

The game drives the device through the XDK's inline functions, which keep
the "simple" render states in D3D__RenderState and call into this file for
everything else. At each draw the full state is read back from there and
translated: the vertex program into GLSL once per shader (nv2a_vsh.c), the
pixel shader - texture stages and register combiners, 57 render states -
into GLSL once per combination (nv2a_psh.c), and the rest into GL state.

Conventions carried over from the Xbox:
- Clip space is D3D's (depth 0..1, y down in window space). glClipControl
  (GL_UPPER_LEFT, GL_ZERO_TO_ONE) makes GL agree, so viewports, scissors and
  texture rows line up with D3D's top-left origin; the window blit at
  Present flips the image back for display.
- Render targets and textures are identified by the physical address in
  their Data field. A texture whose data is a render target samples the GL
  render target directly (render-to-texture).
- Vertex data is read from guest memory at draw time.
*/

#include "xgpu.h"
#include "sdl_platform.h"
#ifdef HALO_VR
#include <SDL3/SDL.h>
#endif
#include "halo_ui_pointer.h"
#include "halo_dynamic_lights.h"
#include "port_config.h"
#include "main/console.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void d3d8_surface_initialize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height);
void d3d8_surface_resize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height);

#ifdef HALO_GLES
/* OpenGL ES 3 (port/android/README.md): the desktop formats, enumerants
and entry points used below that ES lacks */
#define GL_BGRA GL_RGBA
#define glDepthRange glDepthRangef
#define glClearDepth glClearDepthf
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84fe
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84ff
#endif
#ifndef GL_TEXTURE_BORDER_COLOR
#define GL_TEXTURE_BORDER_COLOR 0x1004
#endif
#ifndef GL_CLAMP_TO_BORDER
#define GL_CLAMP_TO_BORDER 0x812d
#endif

/* what the context supports (gl_initialize) */
struct xgpu_capabilities xgpu_capabilities;
#endif

/* ---------- the screen's width

The Xbox screen is 640x480. The native ports can draw a wider one: 480
lines, and as many columns as the display's shape gives. On Android that is
display.screen_width (port_config.c; 640 keeps 4:3); on the desktop, the
shape of the window (of the display while the game is fullscreen, or of
display.resolution), and 640 where display.resolution_scaling is "original".
The game's camera derives its horizontal field of view from the viewport, so
the 3D view simply widens. The menus and full-screen overlays are laid out
for 640 columns; while they draw (halo_screen_ui_offset), everything shifts
right to center them.

The desktop also draws at that resolution (platform_screen_mode): render
targets the size of the screen get that many pixels (screen_scale), and
viewports, clears and visibility counts are scaled to match, so the game
still works in its 480 lines; "original" draws 640x480, scaled up at
presentation. The width and the scale change only between frames, after one
is presented (halo_screen_commit). */

#define SCREEN_HEIGHT 480
#define SCREEN_MAXIMUM_WIDTH 1920

/* the width the game draws, 0 until first asked, and how many pixels a
render target the size of the screen has per unit of it */
static long screen_width;
static float screen_scale[2] = { 1.0f, 1.0f };
/* how many times the display's pixels each way supersampling draws (with
the scale screen_mode_choose last chose) */
static float screen_supersampling = 1.0f;
static long ui_offset;
#define UI_OFFSET ((GLint)ui_offset)

/* ---------- anti-aliasing

display.anti_aliasing, off unless it is set: the Xbox drew without any.
"fxaa" and "smaa" are passes over each window's 3D view before the HUD and
menus are drawn over it (halo_screen_anti_alias, xgpu_post.c); "ssaa2x"
draws the screen's targets at twice the resolution each way
(screen_mode_choose), which the display blit scales down; "msaa2x" to
"msaa8x" draw the back buffer and its depth buffer with that many samples a
pixel (render_target_multisample). The setting is read again between frames
(halo_screen_commit), so that a change applies from the next one. */

enum
{
	_anti_aliasing_off,
	_anti_aliasing_fxaa,
	_anti_aliasing_smaa,
	_anti_aliasing_ssaa,
	_anti_aliasing_msaa,
};

static const struct
{
	const char *name;
	int mode;
	int samples;
} anti_aliasing_values[] =
{
	{ "off", _anti_aliasing_off, 0 },
	{ "fxaa", _anti_aliasing_fxaa, 0 },
#ifdef HALO_GLES
	/* (SMAA's three passes and supersampling's four times the pixels are
	more than a phone's GPU has to spare: FXAA in SMAA's place, and none) */
	{ "smaa", _anti_aliasing_fxaa, 0 },
	{ "ssaa2x", _anti_aliasing_off, 0 },
#else
	{ "smaa", _anti_aliasing_smaa, 0 },
	{ "ssaa2x", _anti_aliasing_ssaa, 0 },
#endif
	{ "msaa2x", _anti_aliasing_msaa, 2 },
	{ "msaa4x", _anti_aliasing_msaa, 4 },
	{ "msaa8x", _anti_aliasing_msaa, 8 },
};

#define NUMBER_OF_ANTI_ALIASING_VALUES ((int)(sizeof(anti_aliasing_values) / sizeof(anti_aliasing_values[0])))

/* the value in effect, -1 until the setting is first read; multisampling's
samples a pixel, at most the GPU's (anti_aliasing_prepare); and the GPU's
most samples and largest target (texture and renderbuffer), 0 until the GL
context exists */
static int anti_aliasing_value = -1;
static int anti_aliasing_samples;
static GLint anti_aliasing_maximum_samples, maximum_target_size;

static void anti_aliasing_prepare(void);

/* display.anti_aliasing's value (none of them: the first, off) */
static void anti_aliasing_read(void)
{
	const char *setting = config_string("display.anti_aliasing");
	int value;

	for (value = NUMBER_OF_ANTI_ALIASING_VALUES - 1;
		value > 0 && strcmp(setting, anti_aliasing_values[value].name);
		value--)
	{
	}
	if (value == anti_aliasing_value)
		return;
	anti_aliasing_value = value;
	if (strcmp(setting, anti_aliasing_values[value].name))
		platform_log("anti-aliasing: \"%s\" is unknown, so off", setting);
	else
		platform_log("anti-aliasing: %s", setting);
	anti_aliasing_prepare();
}

static int anti_aliasing(void)
{
	if (anti_aliasing_value < 0)
		anti_aliasing_read();
	return anti_aliasing_values[anti_aliasing_value].mode;
}

#ifdef HALO_VR
static BOOL vr_screen_mode(long *width, float scale[2]);
#endif

static void screen_mode_choose(long *width, float scale[2])
{
#ifdef HALO_ANDROID
	/* display.screen_width, or 0 for the display's shape, which the app
	passes (port/android/host/host_main.c) */
	const char *display = getenv("HALO_DISPLAY_WIDTH");

	*width = config_integer("display.screen_width");
	if (*width <= 0)
		*width = display ? atol(display) : 640;
	if (*width < 640)
		*width = 640;
	if (*width > 1600)
		*width = 1600;
	*width &= ~1L;
	scale[0] = scale[1] = 1.0f;
	screen_supersampling = 1.0f;
#else
	long display_width, display_height;

#ifdef HALO_VR
	/* the menus' 640x480, at the scale of the HUD's image (each VR pass sets
	its own: halo_vr_pass) */
	if (vr_screen_mode(width, scale))
		return;
#endif
	*width = 640;
	scale[0] = scale[1] = 1.0f;
	screen_supersampling = 1.0f;
	if (platform_screen_mode(&display_width, &display_height) && display_width > 0 && display_height > 0)
	{
		long wanted = (SCREEN_HEIGHT * display_width + display_height / 2) / display_height;

		*width = wanted < 640 ? 640 : wanted > SCREEN_MAXIMUM_WIDTH ? SCREEN_MAXIMUM_WIDTH : wanted & ~1L;
		scale[0] = (float)display_width / (float)*width;
		scale[1] = (float)display_height / (float)SCREEN_HEIGHT;
		/* a display narrower or wider than the game can be: the picture
		keeps its shape and the display blit letterboxes it */
		if (*width != wanted && *width != (wanted & ~1L))
			scale[0] = scale[1] = scale[0] < scale[1] ? scale[0] : scale[1];
		/* supersampling: twice the pixels each way, or as many as the GPU's
		largest target has (once it is known), which the display blit
		scales down; not with "original" resolution scaling, which draws
		the Xbox's 640x480 */
		if (anti_aliasing() == _anti_aliasing_ssaa && maximum_target_size > 0)
		{
			float factor = 2.0f;

			if (*width * scale[0] * factor > (float)maximum_target_size)
				factor = (float)maximum_target_size / (*width * scale[0]);
			if (SCREEN_HEIGHT * scale[1] * factor > (float)maximum_target_size)
				factor = (float)maximum_target_size / (SCREEN_HEIGHT * scale[1]);
			if (factor > 1.0f)
			{
				scale[0] *= factor;
				scale[1] *= factor;
				screen_supersampling = factor;
			}
		}
	}
#endif
}

long halo_screen_width(void)
{
	if (!screen_width)
	{
		screen_mode_choose(&screen_width, screen_scale);
		platform_log("screen: %ldx%d drawn at %.0fx%.0f", screen_width, SCREEN_HEIGHT,
			screen_width * screen_scale[0], SCREEN_HEIGHT * screen_scale[1]);
	}
	return screen_width;
}

#ifdef HALO_VR
static float vr_text_pixel_scale(void);
static int vr_drawing_eyes(void);
#endif

/* the display's pixels for each of the 480 lines (text_hires.c) */
float halo_screen_pixel_scale(void)
{
	halo_screen_width();
#ifdef HALO_VR
	if (vr_text_pixel_scale() > 0.0f)
		return vr_text_pixel_scale();
#endif
	return screen_scale[1];
}

/* display.model_detail: the size a model is measured at to choose its
level of detail (source/models/models.c).

The game measures a model's bounding sphere in the lines of its 480-line
screen (render_frustum_sphere_diameter_in_pixels), whatever the display has,
and compares that with the pixel sizes each level is meant for: on a display
of 1440 lines a model three times the size the levels were chosen for is
drawn as the Xbox drew it at 480. "stock" keeps that; "native" measures in
the display's lines (not supersampling's, which the display scales down;
"original" resolution scaling draws 480); "highest" measures as "native"
which models are too small to draw, and draws the others at their most
detailed level; "auto" is "native" in the headset's eyes, which have far more
lines than a display and are seen close, and "stock" elsewhere. level: the
scale for choosing the level, otherwise for whether to draw at all. */
enum
{
	_model_detail_stock,
	_model_detail_native,
	_model_detail_highest,
	_model_detail_auto,
};

static int model_detail = -1;
static unsigned long model_detail_read_at;

float halo_model_detail_scale(int level)
{
	float display_scale;
	int detail, eyes = FALSE;

	if (model_detail < 0 || model_detail_read_at != config_changes())
	{
		const char *setting = config_string("display.model_detail");
		int value = !strcmp(setting, "native") ? _model_detail_native :
			!strcmp(setting, "highest") ? _model_detail_highest :
			!strcmp(setting, "auto") ? _model_detail_auto : _model_detail_stock;

		model_detail_read_at = config_changes();
		if (value != model_detail)
		{
			if (value == _model_detail_stock && strcmp(setting, "stock"))
				platform_log("model detail: \"%s\" is unknown, so stock", setting);
			else
				platform_log("model detail: %s", setting);
		}
		model_detail = value;
	}
#ifdef HALO_VR
	eyes = vr_drawing_eyes();
#endif
	detail = model_detail == _model_detail_auto ? (eyes ? _model_detail_native : _model_detail_stock) : model_detail;
	if (detail == _model_detail_stock)
		return 1.0f;
	if (detail == _model_detail_highest && level)
		return 1.0e6f;
	halo_screen_width();
	/* (the eyes' scale is their images' own, which nothing supersamples) */
	display_scale = eyes ? screen_scale[1] : screen_scale[1] / screen_supersampling;
	return display_scale > 1.0f ? display_scale : 1.0f;
}

/* how many pixels the screen's targets draw to the Xbox's one, the larger
of the two ways (the screen effects' convolutions: rasterizer_xbox_screen_effect.c) */
float halo_screen_scale(void)
{
	halo_screen_width();
	return screen_scale[0] > screen_scale[1] ? screen_scale[0] : screen_scale[1];
}

/* display.shadow_resolution: the size the shadow maps are drawn at.

Each object's shadow is drawn from above into a 128x128 map, blurred into
another and projected onto the level under it (rasterizer_xbox_shadows.c).
On a large screen, a shadow's 128 texels show as steps along its edge, which
crawl as the object moves. The two maps, the game's only R5G6B5 render
targets (rasterizer_xbox.c), can be drawn larger as the screen's targets are
(render_target_get): the game's viewports, clears and quads, in its 128 units,
scale up with them. The scale is a power of two up to 8 (1024x1024), which
the blur needs to cover the same part of the map as the Xbox's
(rasterizer_shadow_convolve); 1, the default, draws them as the Xbox did. It
changes only between frames (halo_screen_commit). */
#define SHADOW_MAP_SIZE 128
#define SHADOW_MAP_MAXIMUM_SCALE 8

/* 0 until first asked */
static long shadow_scale;
static unsigned long shadow_scale_read_at;

static long shadow_scale_choose(void)
{
	long resolution = config_integer("display.shadow_resolution");
	long scale = 1;

	while (scale < SHADOW_MAP_MAXIMUM_SCALE && SHADOW_MAP_SIZE * scale * 2 <= resolution)
		scale *= 2;
	return scale;
}

/* the shadow maps' pixels for each of their 128 texels each way */
long halo_shadow_map_scale(void)
{
	if (!shadow_scale)
	{
		shadow_scale_read_at = config_changes();
		shadow_scale = shadow_scale_choose();
		platform_log("shadow maps: %ldx%ld", SHADOW_MAP_SIZE * shadow_scale, SHADOW_MAP_SIZE * shadow_scale);
	}
	return shadow_scale;
}

void halo_screen_ui_offset(unsigned char centered)
{
	ui_offset = centered ? (halo_screen_width() - 640) / 2 : 0;
}

/* ---------- state the XDK header's inline functions read and write */

DWORD D3D__RenderState[D3DRS_MAX];
DWORD D3D__TextureState[D3DTSS_MAXSTAGES][D3DTSS_MAX];
WORD *D3D__IndexData;
BYTE D3D__StateBlockDirty[1024];

/* ---------- vertex shaders */

#define VERTEX_SHADER_SIGNATURE 0x76736864UL /* 'vshd' */
#define VERTEX_PROGRAM_SLOTS 136

struct vertex_element
{
	unsigned char reg;
	unsigned char stream;
	unsigned char type;
	unsigned char bytes;
	unsigned short offset;
};

struct vertex_shader_object
{
	unsigned long signature;
	unsigned long id;
	DWORD *instructions;
	unsigned long instruction_count;
	struct vertex_element elements[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned long element_count;
	unsigned long packed_mask;
	/* [0] streams per the declaration, [1] immediate mode (all floats); [2]
	and [3] the same drawing both of the VR mode's eyes (multiview), [4] and
	[5] its left eye alone, [6] and [7] its right eye alone (xgpu.h,
	XGPU_MULTIVIEW_*) */
	GLuint shader[8];
	/* one of the game's model lighting programs (halo_vertex_shader_lighting),
	whose draws can be lit for each pixel (display.per_pixel_lighting): where
	its lighting's normal and position are (lighting.lights is 0 for the
	others), and its shaders that hand them on, as shader[] */
	struct nv2a_vertex_lighting lighting;
	GLuint lit_shader[8];
	/* a shader lit for each pixel failed to compile or link: lit as the
	vertex shader lights it from then on */
	BOOL lighting_failed;
#ifndef HALO_GLES
	/* the vertex array its draws last used, and the streams they had
	(setup_streams: the layout follows from the two) */
	struct vertex_array_entry *vertex_array;
	unsigned long vertex_array_streams;
#endif
	/* which of shader[] and lit_shader[] have been compiled (bits 0 and 1,
	and 2 and 3): one that failed stays 0 and is not compiled again at each
	draw */
	unsigned char shaders_tried;
	/* the program projects through clip space (its multiview variant moves
	it to each eye), as opposed to drawing in screen space */
	BOOL projects;
};

/* ---------- programs */

struct fragment_entry
{
	struct fragment_entry *next;
	unsigned long hash;
	struct nv2a_pixel_shader_key key;
	GLuint shader;
};

/* the uniforms a draw sets besides the vertex constants */
struct draw_uniforms
{
	float viewport_scale[4];
	float viewport_offset[4];
	float point_size;
	float ps_c0[8][4];
	float ps_c1[8][4];
	float ps_final_c0[4];
	float ps_final_c1[4];
	float fog_color[4];
	float fog_parameters[4];
	float alpha_reference;
	float bump_matrix[4][4];
	float bump_luminance[4][4];
	float texture_scale[4][4];
	float screen_offset;
	float texture_lod_bias[4];
};

struct program_entry
{
	struct program_entry *next;
	GLuint vertex_shader;
	GLuint fragment_shader;
	GLuint program;
	GLint constants;
	GLint viewport_scale;
	GLint viewport_offset;
	GLint point_size;
	GLint ps_c0, ps_c1, ps_final_c0, ps_final_c1;
	GLint fog_color, fog_parameters, alpha_reference;
	GLint bump_matrix, bump_luminance, texture_scale;
	GLint texture_lod_bias;
	GLint screen_offset;
	/* nv2a_pixel_shader_key's source_blend's color, and what it last had */
	GLint blend_constant;
	float blend_constant_value[4];
	/* the lights of a draw lit for each pixel (XGPU_MODEL_LIGHT_COUNT), and
	constants_serial at their last upload */
	GLint model_lights;
	unsigned long long model_lights_serial;
	/* the VR mode's multiview programs: their eye transforms (and those of
	their points in screen space), and vr_gl.eye_serial when they were last
	set */
	GLint eye_correction;
	GLint screen_correction;
	GLint screen_points;
	unsigned long eye_serial;
	/* foveated (xgpu.h): its views' and layers' insets, and
	vr_gl.fovea_serial when they were last set */
	GLint fovea_view;
	GLint fovea_sample;
	unsigned long fovea_serial;

	/* the vertex constants c[0..constant_count) the program uses; with
	consecutive locations, a changed range is uploaded by itself */
	unsigned long constant_count;
	BOOL constants_consecutive;
	/* constants_serial at the program's last constant upload (constants_store) */
	unsigned long long constants_serial;
	/* draw_uniforms_serial when the uniforms below were brought up to date */
	unsigned long uniforms_serial;
	/* what the program's other uniforms hold (all ones: unknown) */
	struct draw_uniforms uniforms;
};

#define FRAGMENT_BUCKETS 1024
#define PROGRAM_BUCKETS 1024

static struct fragment_entry *fragment_buckets[FRAGMENT_BUCKETS];
static struct program_entry *program_buckets[PROGRAM_BUCKETS];

/* ---------- render targets */

/* what a render target's scale follows (render_target_get): an entry whose
scale no longer fits is freed (render_targets_evict) */
enum render_target_kind
{
	_render_target_plain,
	/* the screen's targets and the secondary target: screen_scale's */
	_render_target_screen,
	/* a shadow map: halo_shadow_map_scale's */
	_render_target_shadow_map,
};

struct render_target_entry
{
	struct render_target_entry *next;
	/* the next with the same address bucket (render_target_bucket) */
	struct render_target_entry *next_in_bucket;
	struct xgpu_render_target target;
	unsigned long last_rendered;
	/* the back buffer or its depth buffer, which the 3D view is drawn into:
	multisampled with multisampling (render_target_multisample) */
	BOOL screen_buffer;
	enum render_target_kind kind;
};

/* every draw looks up its targets and whether its textures are render
targets, of which there are dozens */
#define RENDER_TARGET_BUCKET_COUNT 256

static struct render_target_entry *render_target_buckets[RENDER_TARGET_BUCKET_COUNT];

static struct render_target_entry **render_target_bucket(unsigned long data)
{
	return &render_target_buckets[((data >> 12) ^ (data >> 20)) % RENDER_TARGET_BUCKET_COUNT];
}

struct framebuffer_entry
{
	struct framebuffer_entry *next;
	GLuint color;
	GLuint depth;
	/* color and depth are multisampled renderbuffers, not textures */
	BOOL renderbuffers;
	/* the layers it attaches (framebuffer_views), from this one
	(framebuffer_base) */
	int views;
	int base;
	GLuint framebuffer;
};

static struct render_target_entry *render_targets;
static struct framebuffer_entry *framebuffers;

static void render_targets_evict(void);

/* ---------- the device */

#if defined(HALO_GLES) || defined(HALO_ARM64_GUEST)
/* Mobile drivers (Mali) keep every orphaned copy of a buffer until the GPU
is done with it, so a large buffer orphaned each frame costs its size per
frame in flight and more. Instead each frame streams into the next of a few
smaller buffers, reusing one only once the GPU has finished the frame that
last used it (host_gl_wait_frame). A busy frame streams about 5 MB of
vertices. The Linux arm64 build's desktop renderer streams so too: on Zink
(Turnip), as on Mali, a glBufferSubData for each draw costs much more than
the unsynchronized write the ring allows (host_gl_buffer_write). */
#define XGPU_STREAM_RING
#define STREAM_BUFFER_SIZE (16 * 1024 * 1024)
#define INDEX_BUFFER_SIZE (2 * 1024 * 1024)
#define STREAM_BUFFER_RING 3
#else
#define STREAM_BUFFER_SIZE (32 * 1024 * 1024)
#define INDEX_BUFFER_SIZE (8 * 1024 * 1024)
#endif
#define VISIBILITY_TEST_SLOTS 4096
/* the visibility tests counted by the pixel shaders (atomic counters), not
by queries: on ES, and in the VR mode, whose eye pass is a multiview pass,
where a query counts in as many slots as views in Vulkan, which Zink does
not allow for (the GPU writes past them, faulting) */
#if defined(HALO_GLES) || defined(HALO_VR)
#define XGPU_SAMPLE_COUNTERS
#endif
#ifdef HALO_GLES
#define VISIBILITY_QUERY GL_ANY_SAMPLES_PASSED
#define VISIBILITY_ALL_SAMPLES 1000000
#else
#define VISIBILITY_QUERY GL_SAMPLES_PASSED
#endif

struct gl_device
{
	D3DPRESENT_PARAMETERS presentation;
	D3DSurface back_buffer;
	D3DSurface depth_buffer;
	D3DSurface *render_target;
	D3DSurface *depth_stencil;
	D3DVIEWPORT8 viewport;
	D3DMATRIX transforms[D3DTS_MAX];
	D3DBaseTexture *textures[D3DTSS_MAXSTAGES];
	D3DPalette *palettes[D3DTSS_MAXSTAGES];
	D3DSHADERCONSTANTMODE shader_constant_mode;

	struct vertex_shader_object *vertex_shader;
	struct vertex_shader_object *program_slots[VERTEX_PROGRAM_SLOTS];
	unsigned long program_address;
	float constants[XGPU_VERTEX_CONSTANT_COUNT][4];
	float viewport_scale[4];
	float viewport_offset[4];

	struct
	{
		DWORD data;
		UINT stride;
	} streams[16];
	/* SetIndices' base vertex: added to every index of an indexed draw (the
	dynamic vertex buffers keep each buffer's vertices at an offset into one
	vertex buffer, and their triangles count from 0: contrails, lightning) */
	UINT base_vertex_index;

	/* the current value of each input register (SetVertexData) */
	float attributes[XGPU_VERTEX_ATTRIBUTE_COUNT][4];
	BOOL immediate_active;
	D3DPRIMITIVETYPE immediate_type;
	/* the input registers written since Begin, a bit each */
	unsigned long immediate_written;
	float *immediate_vertices;
	unsigned long immediate_count;
	unsigned long immediate_capacity;

	GLuint vertex_array;
	GLuint stream_buffer;
#ifdef XGPU_STREAM_RING
	GLuint stream_buffers[STREAM_BUFFER_RING];
	GLuint index_buffers[STREAM_BUFFER_RING];
	unsigned long buffer_ring;
#endif
	unsigned long stream_offset;
	GLuint index_buffer;
	unsigned long index_offset;
#ifdef HALO_WEB
	/* (web_chunk_get) the room in the stream and index buffers, the draws
	made so far, and the count at each buffer's latest write */
	unsigned long stream_capacity;
	unsigned long index_capacity;
	unsigned long draw_serial;
	unsigned long stream_serial;
	unsigned long index_serial;
#endif
	GLuint samplers[D3DTSS_MAXSTAGES];

	GLuint queries[VISIBILITY_TEST_SLOTS];
	BOOL query_pending[VISIBILITY_TEST_SLOTS];
	/* the pixels each of the game's pixels covered in the test's target
	(render_target_get), which its count is divided by */
	float query_area[VISIBILITY_TEST_SLOTS];
	GLuint active_query;
	/* the query a test runs on until it ends (swapped into its slot, or with
	the results mapped and the batch full, dropped): a slot of its own, so
	that every slot, 0 too, is a test's (the game's lens flares number
	theirs from 0) */
	GLuint scratch_query;
	BOOL visibility_test_active;
	/* (queries read on the CPU) each slot's latest count known, and whether
	its query has yet to be read: the game spins on a result it is told is
	incomplete, so a query is read only once it says it is available, and
	until then the slot's earlier count stands. A frame of The Library's
	lights makes hundreds of tests. */
	GLuint visibility_known[VISIBILITY_TEST_SLOTS];
	BOOL visibility_unread[VISIBILITY_TEST_SLOTS];
#ifdef XGPU_SAMPLE_COUNTERS
	/* with atomic counters: one counter per test, used as a ring */
	GLuint visibility_counters;
	unsigned long counter_next;
	unsigned long counter_active;
	/* Reading the counters waits for the draws that counted, which stops
	the CPU until the GPU has caught up (the game asks at the start of the
	next frame), halving the frame rate on drivers that queue frames (Zink,
	Turnip). Instead, at the end of each frame the GPU copies them into the
	frame's snapshot buffer of the stream ring, and the frame's tests (each
	a result slot and its counter) are listed with it. Once its fence has
	passed (two frames on, D3DDevice_Present) the CPU reads the snapshot
	into counter_values and gives each listed slot its count: a result is
	the latest count known, as the desktop's query buffer gives. */
	GLuint counter_snapshots[STREAM_BUFFER_RING];
	unsigned short ring_tests[STREAM_BUFFER_RING][VISIBILITY_TEST_SLOTS][2];
	unsigned long ring_test_count[STREAM_BUFFER_RING];
	GLuint counter_values[VISIBILITY_TEST_SLOTS];
	GLuint visibility_latest[VISIBILITY_TEST_SLOTS];
#endif
#ifndef HALO_GLES
	/* each test's latest result, which the GPU writes (as a query buffer)
	a frame after the test (visibility_copy_batch): the game waits for
	results at the start of the next frame, and a query would stop the CPU
	there until the GPU had caught up */
	GLuint visibility_results_buffer;
	volatile GLuint *visibility_results;
	/* the tests of this frame and of the one before, whose counts the GPU
	is yet to copy into their slots; each batch keeps its own query objects,
	which are not reused until their counts are copied */
	struct
	{
		GLuint queries[VISIBILITY_TEST_SLOTS];
		unsigned short slots[VISIBILITY_TEST_SLOTS];
		unsigned long count;
	} visibility_batches[2];
	unsigned long visibility_batch;
#ifdef HALO_ARM64_GUEST
	GLuint visibility_results_copy[VISIBILITY_TEST_SLOTS];
#endif
	/* a pipeline flush every flush_every draws (draw_flush), 0 never */
	unsigned long flush_every;
	unsigned long flush_draws;
#endif

	unsigned long frame;
	unsigned long next_vertex_shader_id;
	BOOL gl_ready;
	BOOL created;
};

static struct gl_device device;

#ifdef HALO_GLES
#define SAMPLE_COUNTING xgpu_capabilities.atomic_counters
#elif defined(XGPU_SAMPLE_COUNTERS)
/* the VR mode's (vr_gl_initialize) */
static BOOL sample_counting;
#define SAMPLE_COUNTING sample_counting
#endif

/* ---------- the VR mode (HALO_VR; vr.h, vr.c)

A VR frame is drawn in two passes. The eye pass draws the 3D view once, from
the centre of the head with the union of the eyes' fields of view, into both
eyes at once: the screen's render targets are 2-layer texture arrays drawn
into with GL_OVR_multiview2, the back buffer being the OpenXR swapchain's
image itself, and each vertex the game projected through clip space moves to
its eye by the transform halo_vr_set_eye_transforms gives for the current
projection. The HUD pass draws the HUD, the menus and everything else into
the quad layer's image, over transparent black. The game draws both in its
640x480 screen, each at its image's scale. */

/* (xgpu.h; vr_gl_initialize sets it) */
int xgpu_foveated;

#ifdef HALO_VR
#include "vr.h"
#include "vr_host.h"
#include "vr_visor.h"

#ifndef GL_DEPTH_CLAMP
#define GL_DEPTH_CLAMP 0x864F
#endif
#ifndef GL_MAX_VIEWS_OVR
#define GL_MAX_VIEWS_OVR 0x9631
#endif

/* vr.foveation: the blend from the inset to the periphery about it, as
wide as this part of the inset's smaller side (vr_fovea_composite) */
#define FOVEA_BLEND 0.15f

static struct
{
	BOOL ready;
	int pass;
	int eye_width, eye_height;
	int hud_width, hud_height;
	/* the back buffer in the eye pass: the swapchain's image (the HUD pass
	draws into a target of its own, copied into the HUD's image at the
	present); foveated, the four layers it is made from at the present
	(vr_fovea_composite), the swapchain's image after it */
	struct render_target_entry eye_target;
	GLuint eye_image;
	/* vr.foveation (xgpu.h, xgpu_foveated): the eye pass's back buffer, 4
	layers of fovea_width x fovea_height (layer 2 eye + 0 the whole eye,
	2 eye + 1 its inset, as many of the eye image's pixels), each eye's
	inset's top left in its image's pixels from its top left this frame,
	and what the shaders are given: each view's magnification and offset
	(of its clip position, for the viewport and target bound: fovea_view)
	and each layer's (of a point of a target sampled, in its pixels from its
	top left: fovea_sample), and a serial of their changes. debug.vr_
	foveation_show tints the periphery */
	BOOL foveated;
	int fovea_width, fovea_height;
	GLuint fovea_texture;
	int fovea_corners[2][2];
	float fovea_view[4][4];
	float fovea_sample[4][4];
	unsigned long fovea_serial;
	BOOL fovea_show;
	BOOL eyes_drawn, hud_drawn;
	/* the scope pass (vr.scopes): the 640x480 screen at the scale that
	makes 480 lines its square image's, its view in the middle 480x480
	(vr_scope.c), copied into the scope's image at the present; whether it
	was drawn this frame, and the image it was copied into */
	int scope_size;
	float scope_scale;
	BOOL scope_drawn;
	GLuint scope_image;
	/* the smart-link's window's image (vr_copy_scope's, as the scope's),
	drawn into the eyes' images: not a layer */
	GLuint scope_window_texture;
	/* the HUD's groups (vr.h's HALO_VR_HUD_GROUP_*, halo_vr_hud_group): the
	one drawn into, and whether the anchors choose it; while the HUD has
	panels (vr_visor.c, vr.hud_depth), each group but the centre's is drawn
	into a target of its own (vr_group_target), cleared as it is first
	drawn in a frame, and copied into its panel's image at the present */
	int hud_group, hud_group_mode;
	BOOL hud_panels;
	GLuint group_textures[HALO_VR_HUD_GROUPS];
	unsigned long group_width, group_height;
	unsigned int groups_drawn;
	/* the panels' images drawn this frame, else 0 */
	GLuint panel_images[VR_PANEL_COUNT];
	/* row-major (vr.h), and the screen's points' (vr.h); the serial counts
	their changes */
	float eye_transforms[2][16];
	float screen_corrections[2][4];
	BOOL screen_points;
	unsigned long eye_serial;
	/* the bound framebuffer draws both eyes (bind_targets), or one eye's
	layer alone: in the eye pass, a visibility test is drawn into the left
	eye's (XGPU_MULTIVIEW_LEFT_EYE), and what halo_vr_draw_eye asks for into
	that eye's (the crosshairs: each eye's screen correction applied in a
	draw of its own; in a multiview draw, on spark's NVIDIA driver, the
	right eye's came out moved by the left eye's correction and then its
	own) */
	BOOL multiview;
	BOOL left_eye;
	BOOL right_eye;
	BOOL visibility_test;
	int eye_alone;
	/* the model drawn is mirrored (halo_vr_mirrored_draws): its triangles'
	winding is the other way */
	BOOL mirrored;
	/* debug.gpu_stats: where a frame's time goes (vr_timing) */
	BOOL depth_failed;
	BOOL statistics;
	Uint64 marks[4];
	double spans[3];
	unsigned long timed;
	/* debug.vr_gpu_time: the GPU's time for each frame */
	BOOL gpu_time;
	double gpu_milliseconds, gpu_worst;
	unsigned long gpu_frames;
} vr_gl;

/* the frame's marks: the last present's end (then the game updates, and
waits for the runtime's frame), the eye pass begun, the HUD pass begun
after it, the present */
static void vr_timing(int mark)
{
	if (!vr_gl.statistics)
		return;
	vr_gl.marks[mark] = SDL_GetTicksNS();
	if (mark == 3 && vr_gl.marks[0] && vr_gl.marks[1] > vr_gl.marks[0] && vr_gl.marks[2] > vr_gl.marks[1])
	{
		vr_gl.spans[0] += (double)(vr_gl.marks[1] - vr_gl.marks[0]) * 1e-6;
		vr_gl.spans[1] += (double)(vr_gl.marks[2] - vr_gl.marks[1]) * 1e-6;
		vr_gl.spans[2] += (double)(vr_gl.marks[3] - vr_gl.marks[2]) * 1e-6;
		if (++vr_gl.timed == 300)
		{
			platform_log("vr: a frame's draws: %.2f ms before the eyes, %.2f ms the eyes, %.2f ms the HUD and present",
				vr_gl.spans[0] / vr_gl.timed, vr_gl.spans[1] / vr_gl.timed, vr_gl.spans[2] / vr_gl.timed);
			if (vr_gl.gpu_frames)
			{
				platform_log("vr: the GPU's time a frame: %.2f ms, worst %.2f ms (%dx%d eyes%s)",
					vr_gl.gpu_milliseconds / vr_gl.gpu_frames, vr_gl.gpu_worst, vr_gl.eye_width, vr_gl.eye_height,
					vr_gl.foveated ? ", foveated" : "");
			}
			vr_gl.gpu_milliseconds = vr_gl.gpu_worst = 0.0;
			vr_gl.gpu_frames = 0;
			memset(vr_gl.spans, 0, sizeof(vr_gl.spans));
			vr_gl.timed = 0;
		}
	}
}
#endif

/* debug.gpu_stats prints these once a second */
static struct
{
	unsigned long draws, immediate_draws, clears, presents;
	unsigned long skipped_no_program, skipped_no_target, skipped_link;
	unsigned long target_changes;
	/* vertex and index bytes drawn from the mirror, and streamed */
	unsigned long mirrored_bytes, streamed_bytes;
} stats;

static D3DDevice *device_pointer(void)
{
	return (D3DDevice *)&device;
}

static float dword_to_float(DWORD value)
{
	union { DWORD d; float f; } u;

	u.d = value;
	return u.f;
}

static void color_to_vec4(D3DCOLOR color, float *out)
{
	out[0] = ((color >> 16) & 0xff) / 255.0f;
	out[1] = ((color >> 8) & 0xff) / 255.0f;
	out[2] = (color & 0xff) / 255.0f;
	out[3] = ((color >> 24) & 0xff) / 255.0f;
}

/* ---------- debugging settings, read once (gl_initialize) */

static struct
{
	/* debug.gpu_skip_vertex_shaders "<id>,<id>..." drops draws by vertex
	shader, for finding which pass produces something (port_config.c) */
	const char *skip_vertex_shaders;
	const char *dump_shaders;
	BOOL statistics;
} debug_settings;

/* ---------- GL state cache

Consecutive draws share most of their state, but each sets all of it: the
setters here skip the call when GL already holds the value. Code that
changes GL state behind the cache's back (clears, presentation, texture
uploads, render target and framebuffer creation) calls
xgpu_gl_state_invalidate, after which every value is set again. Unknown
values are all ones, which no real value matches (floats become NaN, which
compares unequal to everything). */

#ifdef HALO_GLES
struct attribute_pointer
{
	GLuint buffer;
	GLint size;
	GLenum type;
	GLboolean normalized;
	GLboolean integer;
	GLsizei stride;
	unsigned long offset;
};
#else
/* desktop GL (4.3) separates an attribute's format from the buffer it
reads: the attributes of a stream share one binding, so a draw that moves
the stream rebinds it once instead of pointing each attribute again */
struct attribute_format
{
	GLint size;
	GLenum type;
	GLboolean normalized;
	GLboolean integer;
	GLuint relative_offset;
	GLuint binding;
};

struct vertex_binding
{
	GLuint buffer;
	unsigned long offset;
	GLsizei stride;
};

/* a binding per stream (setup_streams); GL has at least 16 */
#define VERTEX_BINDING_COUNT 16

/* a vertex array object for each vertex layout (the attributes a draw
enables, and each one's format and binding), made once: a draw binds its
layout's and points the bindings at its streams, rather than enabling,
formatting and binding each attribute again (setup_streams) */
struct vertex_layout
{
	unsigned long enabled;
	struct attribute_format formats[XGPU_VERTEX_ATTRIBUTE_COUNT];
};

struct vertex_array_entry
{
	struct vertex_array_entry *next;
	unsigned long hash;
	struct vertex_layout layout;
	GLuint vertex_array;
	/* its bindings, as last pointed (nothing else changes them) */
	struct vertex_binding bindings[VERTEX_BINDING_COUNT];
};

#define VERTEX_ARRAY_BUCKET_COUNT 64
static struct vertex_array_entry *vertex_array_buckets[VERTEX_ARRAY_BUCKET_COUNT];
static struct vertex_array_entry *current_vertex_array;
#endif

static struct
{
	GLuint program;
	GLuint framebuffer;
	GLint viewport[4];
	GLint scissor[4];
	float depth_range[2];
	unsigned char depth_test, stencil_test, blend, cull_face, offset_fill, offset_line;
	unsigned char scissor_test;
	GLenum depth_function;
	unsigned char depth_mask;
	GLenum stencil_function;
	GLint stencil_reference;
	GLuint stencil_value_mask;
	GLenum stencil_operations[3];
	GLuint stencil_write_mask;
	GLenum blend_source, blend_destination, blend_equation;
	float blend_color[4];
	unsigned char color_mask;
	GLenum front_face, cull_mode, polygon_mode;
	float polygon_offset[2];
	GLenum active_texture;
	/* per unit: the GL_TEXTURE_2D, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_3D and GL_TEXTURE_2D_ARRAY
	bindings */
	GLuint textures[D3DTSS_MAXSTAGES][4];
	GLuint samplers[D3DTSS_MAXSTAGES];
	GLuint array_buffer;
	GLuint element_array_buffer;
	unsigned char attribute_enabled[XGPU_VERTEX_ATTRIBUTE_COUNT];
#ifdef HALO_GLES
	struct attribute_pointer attribute_pointers[XGPU_VERTEX_ATTRIBUTE_COUNT];
#else
	GLuint vertex_array;
#endif
	/* a disabled attribute's value; kind 1 is the integer zero */
	unsigned char attribute_value_kind[XGPU_VERTEX_ATTRIBUTE_COUNT];
	float attribute_values[XGPU_VERTEX_ATTRIBUTE_COUNT][4];
} gl_state;

#ifdef HALO_WEB
static void vertex_array_cache_invalidate(void);
#endif

void xgpu_gl_state_invalidate(void)
{
	memset(&gl_state, 0xff, sizeof(gl_state));
#ifdef HALO_WEB
	vertex_array_cache_invalidate();
#endif
}

/* (a unit the shadows still said had it would never be bound to a new
texture GL gives its name to; and a stage no draw samples keeps what was
bound to it: bind_textures) */
void xgpu_gl_texture_deleted(unsigned int texture)
{
	int unit, slot;

	for (unit = 0; unit < D3DTSS_MAXSTAGES; unit++)
	{
		for (slot = 0; slot < 3; slot++)
		{
			if (gl_state.textures[unit][slot] == texture)
				gl_state.textures[unit][slot] = (GLuint)-1;
		}
	}
}

/* the source blend factor the pixel shader applies (nv2a_pixel_shader_key's
source_blend), when the draw's blend mixes a constant color factor with a
constant alpha one, which WebGL refuses; 0 otherwise */
static unsigned char source_blend_in_shader(void)
{
#ifdef HALO_WEB
	DWORD source = D3D__RenderState[D3DRS_SRCBLEND], destination = D3D__RenderState[D3DRS_DESTBLEND];
	int source_kind = source == D3DBLEND_CONSTANTCOLOR || source == D3DBLEND_INVCONSTANTCOLOR ? 1 :
		source == D3DBLEND_CONSTANTALPHA || source == D3DBLEND_INVCONSTANTALPHA ? 2 : 0;
	int destination_kind = destination == D3DBLEND_CONSTANTCOLOR || destination == D3DBLEND_INVCONSTANTCOLOR ? 1 :
		destination == D3DBLEND_CONSTANTALPHA || destination == D3DBLEND_INVCONSTANTALPHA ? 2 : 0;

	if (D3D__RenderState[D3DRS_ALPHABLENDENABLE] && source_kind && destination_kind && source_kind != destination_kind)
	{
		return source == D3DBLEND_CONSTANTCOLOR ? 1 : source == D3DBLEND_INVCONSTANTCOLOR ? 2 :
			source == D3DBLEND_CONSTANTALPHA ? 3 : 4;
	}
#endif
	return 0;
}

static void state_enable(unsigned char *shadow, GLenum capability, BOOL enabled)
{
	unsigned char value = enabled ? 1 : 0;

	if (*shadow == value)
		return;
	*shadow = value;
	if (value)
		glEnable(capability);
	else
		glDisable(capability);
}

static void state_program(GLuint program)
{
	if (gl_state.program != program)
	{
		gl_state.program = program;
		glUseProgram(program);
	}
}

static void state_framebuffer(GLuint framebuffer)
{
	if (gl_state.framebuffer != framebuffer)
	{
		gl_state.framebuffer = framebuffer;
		glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	}
}

/* gl_state.textures' slot of a target */
static int texture_slot(GLenum target)
{
	return target == GL_TEXTURE_CUBE_MAP ? 1 : target == GL_TEXTURE_3D ? 2 : target == GL_TEXTURE_2D_ARRAY ? 3 : 0;
}

#ifdef HALO_GLES
static void state_texture(int unit, GLenum target, GLuint texture)
{
	int slot = texture_slot(target);

	if (gl_state.textures[unit][slot] == texture)
		return;
	if (gl_state.active_texture != GL_TEXTURE0 + (GLenum)unit)
	{
		gl_state.active_texture = GL_TEXTURE0 + (GLenum)unit;
		glActiveTexture(gl_state.active_texture);
	}
	gl_state.textures[unit][slot] = texture;
	glBindTexture(target, texture);
}
#endif

static void state_sampler(int unit, GLuint sampler)
{
	if (gl_state.samplers[unit] != sampler)
	{
		gl_state.samplers[unit] = sampler;
		glBindSampler((GLuint)unit, sampler);
	}
}

static void state_array_buffer(GLuint buffer)
{
	if (gl_state.array_buffer != buffer)
	{
		gl_state.array_buffer = buffer;
		glBindBuffer(GL_ARRAY_BUFFER, buffer);
	}
}

static void state_element_array_buffer(GLuint buffer)
{
	if (gl_state.element_array_buffer != buffer)
	{
		gl_state.element_array_buffer = buffer;
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
	}
}

#ifdef HALO_GLES
/* enables the attribute, reading size elements of type from buffer: each
vertex is stride bytes on from the one before it, starting at
buffer_offset, with the attribute relative_offset bytes into it */
static void state_attribute_stream(GLuint index, GLuint binding, GLuint buffer, GLint size, GLenum type,
	GLboolean normalized, BOOL integer, GLsizei stride, unsigned long buffer_offset, unsigned long relative_offset)
{
	struct attribute_pointer *pointer = &gl_state.attribute_pointers[index];
	unsigned long offset = buffer_offset + relative_offset;

	(void)binding;
	if (gl_state.attribute_enabled[index] != 1)
	{
		gl_state.attribute_enabled[index] = 1;
		glEnableVertexAttribArray(index);
	}
	if (pointer->buffer == buffer && pointer->size == size && pointer->type == type &&
		pointer->normalized == normalized && pointer->integer == (integer ? GL_TRUE : GL_FALSE) &&
		pointer->stride == stride && pointer->offset == offset)
	{
		return;
	}
	state_array_buffer(buffer);
	if (integer)
		glVertexAttribIPointer(index, size, type, stride, (const void *)offset);
	else
		glVertexAttribPointer(index, size, type, normalized, stride, (const void *)offset);
	pointer->buffer = buffer;
	pointer->size = size;
	pointer->type = type;
	pointer->normalized = normalized;
	pointer->integer = integer ? GL_TRUE : GL_FALSE;
	pointer->stride = stride;
	pointer->offset = offset;
}
#else
/* the vertex array of a layout, made the first time (its attributes enabled,
formatted and bound to their bindings) */
static struct vertex_array_entry *vertex_array_get(const struct vertex_layout *layout)
{
	const unsigned char *bytes = (const unsigned char *)layout;
	unsigned long hash = 2166136261UL, index;
	struct vertex_array_entry **bucket, *entry;

	for (index = 0; index < sizeof(*layout); index++)
		hash = (hash ^ bytes[index]) * 16777619UL;
	bucket = &vertex_array_buckets[hash % VERTEX_ARRAY_BUCKET_COUNT];
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->hash == hash && !memcmp(&entry->layout, layout, sizeof(*layout)))
			return entry;
	}
	entry = calloc(1, sizeof(*entry));
	entry->hash = hash;
	entry->layout = *layout;
	memset(entry->bindings, 0xff, sizeof(entry->bindings));
	glGenVertexArrays(1, &entry->vertex_array);
	glBindVertexArray(entry->vertex_array);
	gl_state.vertex_array = entry->vertex_array;
	/* (the index buffer's binding is the vertex array's) */
	gl_state.element_array_buffer = (GLuint)-1;
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		const struct attribute_format *format = &layout->formats[index];

		if (!(layout->enabled & (1UL << index)))
			continue;
		glEnableVertexAttribArray((GLuint)index);
		if (format->integer)
			glVertexAttribIFormat((GLuint)index, format->size, format->type, format->relative_offset);
		else
			glVertexAttribFormat((GLuint)index, format->size, format->type, format->normalized, format->relative_offset);
		glVertexAttribBinding((GLuint)index, format->binding);
	}
	entry->next = *bucket;
	*bucket = entry;
	return entry;
}

static void state_vertex_array(struct vertex_array_entry *entry)
{
	if (gl_state.vertex_array != entry->vertex_array)
	{
		gl_state.vertex_array = entry->vertex_array;
		glBindVertexArray(entry->vertex_array);
		/* (the index buffer's binding is the vertex array's) */
		gl_state.element_array_buffer = (GLuint)-1;
	}
	current_vertex_array = entry;
}

/* points the bound vertex array's binding at buffer: each vertex is stride
bytes on from the one before it, starting at offset */
static void state_vertex_buffer(GLuint binding, GLuint buffer, unsigned long offset, GLsizei stride)
{
	struct vertex_binding *vertex_binding = &current_vertex_array->bindings[binding];

	if (vertex_binding->buffer != buffer || vertex_binding->offset != offset || vertex_binding->stride != stride)
	{
		glBindVertexBuffer(binding, buffer, (GLintptr)offset, stride);
		vertex_binding->buffer = buffer;
		vertex_binding->offset = offset;
		vertex_binding->stride = stride;
	}
}

/* an attribute of a layout */
static void layout_attribute(struct vertex_layout *layout, unsigned long index, GLuint binding, GLint size,
	GLenum type, GLboolean normalized, BOOL integer, unsigned long relative_offset)
{
	struct attribute_format *format = &layout->formats[index];

	format->size = size;
	format->type = type;
	format->normalized = normalized;
	format->integer = integer ? GL_TRUE : GL_FALSE;
	format->relative_offset = (GLuint)relative_offset;
	format->binding = binding;
	layout->enabled |= 1UL << index;
}
#endif

/* the value a disabled attribute reads (value, or the integer zero), which
is the context's, not the vertex array's */
static void state_attribute_constant(GLuint index, const float *value)
{
	unsigned char kind = value ? 0 : 1;

	if (gl_state.attribute_value_kind[index] == kind &&
		(!value || !memcmp(gl_state.attribute_values[index], value, sizeof(gl_state.attribute_values[index]))))
	{
		return;
	}
	gl_state.attribute_value_kind[index] = kind;
	if (value)
	{
		memcpy(gl_state.attribute_values[index], value, sizeof(gl_state.attribute_values[index]));
		glVertexAttrib4fv(index, value);
	}
	else
	{
		glVertexAttribI4ui(index, 0, 0, 0, 0);
	}
}

/* disables the attribute, which then reads value, or the integer zero */
static void state_attribute_value(GLuint index, const float *value)
{
	unsigned char kind = value ? 0 : 1;

#ifdef HALO_GLES
	if (gl_state.attribute_enabled[index] != 0)
	{
		gl_state.attribute_enabled[index] = 0;
		glDisableVertexAttribArray(index);
	}
#endif
	if (gl_state.attribute_value_kind[index] == kind &&
		(!value || !memcmp(gl_state.attribute_values[index], value, sizeof(gl_state.attribute_values[index]))))
	{
		return;
	}
	gl_state.attribute_value_kind[index] = kind;
	if (value)
	{
		memcpy(gl_state.attribute_values[index], value, sizeof(gl_state.attribute_values[index]));
		glVertexAttrib4fv(index, value);
	}
	else
	{
		glVertexAttribI4ui(index, 0, 0, 0, 0);
	}
}

/* ---------- vertical blank emulation */

#define VERTICAL_BLANK_NANOSECONDS (1000000000L / 60)

static pthread_mutex_t vertical_blank_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vertical_blank_condition = PTHREAD_COND_INITIALIZER;
static D3DCALLBACK vertical_blank_callback;
static unsigned long vertical_blank_count;
static volatile unsigned int flip_count;
static unsigned long pending_flips;
static BOOL vertical_blank_thread_started = FALSE;

static void *vertical_blank_thread(void *unused)
{
	struct timespec next;

	(void)unused;
	clock_gettime(CLOCK_MONOTONIC, &next);
	for (;;)
	{
		D3DCALLBACK callback;

		next.tv_nsec += VERTICAL_BLANK_NANOSECONDS;
		if (next.tv_nsec >= 1000000000L)
		{
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

		pthread_mutex_lock(&vertical_blank_lock);
		vertical_blank_count++;
		/* a presented frame becomes visible at the next vertical blank */
		if (pending_flips)
		{
			pending_flips--;
			flip_count++;
		}
		callback = vertical_blank_callback;
		pthread_cond_broadcast(&vertical_blank_condition);
		pthread_mutex_unlock(&vertical_blank_lock);

		if (callback)
			callback(0);
	}
	return NULL;
}

static void vertical_blank_start(void)
{
	pthread_mutex_lock(&vertical_blank_lock);
	if (!vertical_blank_thread_started)
	{
		pthread_t thread;

		if (pthread_create(&thread, NULL, vertical_blank_thread, NULL) == 0)
		{
			pthread_detach(thread);
			vertical_blank_thread_started = TRUE;
		}
		else
		{
			platform_log("cannot start the vertical blank thread");
		}
	}
	pthread_mutex_unlock(&vertical_blank_lock);
}

/* replaces main/d3d_intimacy.cpp, which reads the counter out of the Xbox
Direct3D runtime's private device structure */
volatile unsigned int *d3d_find_flipcount(void)
{
	return &flip_count;
}

void WINAPI D3DDevice_SetVerticalBlankCallback(D3DCALLBACK callback)
{
	pthread_mutex_lock(&vertical_blank_lock);
	vertical_blank_callback = callback;
	pthread_mutex_unlock(&vertical_blank_lock);
	vertical_blank_start();
}

void WINAPI D3DDevice_BlockUntilVerticalBlank(void)
{
	unsigned long count;

	vertical_blank_start();
	pthread_mutex_lock(&vertical_blank_lock);
	count = vertical_blank_count;
	while (vertical_blank_count == count)
		pthread_cond_wait(&vertical_blank_condition, &vertical_blank_lock);
	pthread_mutex_unlock(&vertical_blank_lock);
}

/* ---------- GL helpers */

GLuint xgpu_compile_shader(GLenum type, const char *source, const char *what)
{
	GLuint shader = glCreateShader(type);
	GLint status = 0;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		char log[4096];

		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		platform_log("cannot compile the %s shader:\n%s\n%s", what, log, source);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

GLuint xgpu_link_program(GLuint vertex_shader, GLuint fragment_shader, const char *what)
{
	GLuint program = glCreateProgram();
	GLint status = 0;

	glAttachShader(program, vertex_shader);
	glAttachShader(program, fragment_shader);
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (!status)
	{
		char log[4096];

		glGetProgramInfoLog(program, sizeof(log), NULL, log);
		platform_log("cannot link the %s program: %s", what, log);
		return 0;
	}
	return program;
}

#ifndef HALO_GLES
static void GLAPIENTRY gl_debug_callback(GLenum source, GLenum type, GLuint id, GLenum severity,
	GLsizei length, const GLchar *message, const void *user)
{
	(void)source; (void)id; (void)length; (void)user;
	if (severity != GL_DEBUG_SEVERITY_NOTIFICATION)
		platform_log("GL %s: %s", type == GL_DEBUG_TYPE_ERROR ? "error" : "debug", message);
}
#endif

/* ---------- render targets */

static void surface_dimensions(const D3DSurface *surface, unsigned long *width, unsigned long *height, BOOL *depth)
{
	struct xgpu_texture_description description;
	DWORD format;

	xgpu_texture_describe(surface->Format, surface->Size, &description);
	*width = description.width;
	*height = description.height;
	format = description.format;
	*depth = format == D3DFMT_D24S8 || format == D3DFMT_F24S8 || format == D3DFMT_D16 || format == D3DFMT_F16 ||
		format == D3DFMT_LIN_D24S8 || format == D3DFMT_LIN_F24S8 || format == D3DFMT_LIN_D16 || format == D3DFMT_LIN_F16;
}

/* the shadow maps: the game's only R5G6B5 render targets, 128x128
(rasterizer_xbox.c) */
static BOOL surface_is_shadow_map(const D3DSurface *surface)
{
	struct xgpu_texture_description description;

	xgpu_texture_describe(surface->Format, surface->Size, &description);
	return description.format == D3DFMT_R5G6B5 && description.width == SHADOW_MAP_SIZE &&
		description.height == SHADOW_MAP_SIZE;
}

/* the secondary render target, the size of the Xbox's (rasterizer_xbox.c) */
#define SECONDARY_TARGET_WIDTH 320
#define SECONDARY_TARGET_HEIGHT 240

/* the targets render_target_get found last, by what it found them from:
each draw asks again for the same two (bind_targets) */
#define RECENT_RENDER_TARGET_COUNT 4

static struct
{
	DWORD data, format, size;
	long screen_width;
	float scale[2];
	struct render_target_entry *entry;
} recent_render_targets[RECENT_RENDER_TARGET_COUNT];
static unsigned long recent_render_target_next;

static struct render_target_entry *render_target_remember(const D3DSurface *surface, long screen,
	struct render_target_entry *entry)
{
	unsigned long slot = recent_render_target_next++ % RECENT_RENDER_TARGET_COUNT;

	recent_render_targets[slot].data = surface->Data;
	recent_render_targets[slot].format = surface->Format;
	recent_render_targets[slot].size = surface->Size;
	recent_render_targets[slot].screen_width = screen;
	recent_render_targets[slot].scale[0] = screen_scale[0];
	recent_render_targets[slot].scale[1] = screen_scale[1];
	recent_render_targets[slot].entry = entry;
	return entry;
}

static struct render_target_entry *render_target_get(const D3DSurface *surface)
{
	struct render_target_entry *entry;
	unsigned long width, height, slot;
	long screen;
	BOOL depth;

	int layers = 1;

	if (!surface || !surface->Data)
		return NULL;
	float scale[2] = { 1.0f, 1.0f };
	enum render_target_kind kind = _render_target_plain;

	/* (an entry lives until its scale no longer fits: render_targets_evict) */
	screen = halo_screen_width();
	for (slot = 0; slot < RECENT_RENDER_TARGET_COUNT; slot++)
	{
		if (recent_render_targets[slot].entry && recent_render_targets[slot].data == surface->Data &&
			recent_render_targets[slot].format == surface->Format && recent_render_targets[slot].size == surface->Size &&
			recent_render_targets[slot].screen_width == screen && recent_render_targets[slot].scale[0] == screen_scale[0] &&
			recent_render_targets[slot].scale[1] == screen_scale[1])
		{
			return recent_render_targets[slot].entry;
		}
	}
	surface_dimensions(surface, &width, &height, &depth);
	/* the screen's targets are drawn at the screen's scale, and so is the
	secondary target (the reflections' and active camouflage's: its size is
	rasterizer_xbox.c's), which keeps the game's 320x240 units, that its
	shaders' constants are in, and gets the screen's pixels a unit; the
	shadow maps at display.shadow_resolution's (halo_shadow_map_scale) */
	if ((width == (unsigned long)halo_screen_width() && height == SCREEN_HEIGHT) ||
		(!depth && width == SECONDARY_TARGET_WIDTH && height == SECONDARY_TARGET_HEIGHT))
	{
		scale[0] = screen_scale[0];
		scale[1] = screen_scale[1];
		kind = _render_target_screen;
#ifdef HALO_VR
		/* in the eye pass the back buffer is the eyes' swapchain image, and
		the screen's other targets have a layer for each eye (foveated, two:
		xgpu.h) */
		if (vr_gl.pass == HALO_VR_PASS_EYES)
		{
			if (!depth && surface->Data == device.back_buffer.Data)
				return &vr_gl.eye_target;
			layers = xgpu_foveated ? 4 : 2;
		}
#endif
	}
	else if (!depth && surface_is_shadow_map(surface))
	{
		scale[0] = (float)halo_shadow_map_scale();
		scale[1] = scale[0];
		kind = _render_target_shadow_map;
	}
	for (entry = *render_target_bucket(surface->Data); entry; entry = entry->next_in_bucket)
	{
		if (entry->target.data == surface->Data && entry->target.width == width &&
			entry->target.height == height && entry->target.depth == depth &&
			entry->target.scale[0] == scale[0] && entry->target.scale[1] == scale[1] &&
			entry->target.layers == layers)
		{
			return render_target_remember(surface, screen, entry);
		}
	}
	entry = calloc(1, sizeof(*entry));
	entry->target.data = surface->Data;
	entry->target.width = width;
	entry->target.height = height;
	entry->target.depth = depth;
	entry->target.scale[0] = scale[0];
	entry->target.scale[1] = scale[1];
	entry->kind = kind;
	entry->target.gl_width = (unsigned long)(width * scale[0] + 0.5f);
	entry->target.gl_height = (unsigned long)(height * scale[1] + 0.5f);
	entry->target.layers = layers;
	glGenTextures(1, &entry->target.texture);
#ifdef HALO_VR
	if (layers > 1)
	{
		glBindTexture(GL_TEXTURE_2D_ARRAY, entry->target.texture);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
		if (depth)
			glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH24_STENCIL8, (GLsizei)entry->target.gl_width,
				(GLsizei)entry->target.gl_height, layers, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
		else
			glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, (GLsizei)entry->target.gl_width,
				(GLsizei)entry->target.gl_height, layers, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	}
	else
#endif
	{
	glBindTexture(GL_TEXTURE_2D, entry->target.texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	if (depth)
		glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, (GLsizei)entry->target.gl_width,
			(GLsizei)entry->target.gl_height, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
	else
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)entry->target.gl_width, (GLsizei)entry->target.gl_height,
			0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	}
	entry->screen_buffer = surface->Data == (depth ? device.depth_buffer.Data : device.back_buffer.Data);
	xgpu_gl_state_invalidate();
	entry->next = render_targets;
	render_targets = entry;
	entry->next_in_bucket = *render_target_bucket(entry->target.data);
	*render_target_bucket(entry->target.data) = entry;
	return entry;
}

struct xgpu_render_target *xgpu_render_target_find(unsigned long data)
{
	struct render_target_entry *entry, *best = NULL;

#ifdef HALO_VR
	if (vr_gl.pass == HALO_VR_PASS_EYES && data == device.back_buffer.Data)
		return &vr_gl.eye_target.target;
#endif
	for (entry = *render_target_bucket(data); entry; entry = entry->next_in_bucket)
	{
		/* (in a VR pass, the screen's targets of its own pass: the eyes' have
		a layer for each eye, the HUD's and the scope's one, each at its
		pass's scale) */
		if (entry->target.data == data && !entry->target.depth && (!best || entry->last_rendered > best->last_rendered)
#ifdef HALO_VR
			&& (vr_gl.pass == HALO_VR_PASS_NONE || entry->kind != _render_target_screen ||
				((entry->target.layers > 1) == (vr_gl.pass == HALO_VR_PASS_EYES) &&
					entry->target.scale[0] == screen_scale[0] && entry->target.scale[1] == screen_scale[1]))
#endif
			)
			best = entry;
	}
	return best ? &best->target : NULL;
}

/* the layers the next framebuffer_find attaches: 1, 2 or 4 (multiview,
from layer framebuffer_base), or -1 or -2 for the first or second layer of
2-layer textures alone */
static int framebuffer_views = 1;
static int framebuffer_base = 0;

/* the framebuffer of these textures, or with renderbuffers, of these
multisampled renderbuffers (render_target_multisample) */
static GLuint framebuffer_find(GLuint color, GLuint depth, BOOL renderbuffers)
{
	struct framebuffer_entry *entry, **link;
	GLenum draw_buffer = color ? GL_COLOR_ATTACHMENT0 : GL_NONE;

	/* (found, it moves to the front: each draw asks again for the one
	the draw before it did) */
	for (link = &framebuffers; (entry = *link) != NULL; link = &entry->next)
	{
		if (entry->color == color && entry->depth == depth && entry->renderbuffers == renderbuffers &&
			entry->views == framebuffer_views && entry->base == framebuffer_base)
		{
			*link = entry->next;
			entry->next = framebuffers;
			framebuffers = entry;
			return entry->framebuffer;
		}
	}
	entry = calloc(1, sizeof(*entry));
	entry->color = color;
	entry->depth = depth;
	entry->renderbuffers = renderbuffers;
	entry->views = framebuffer_views;
	entry->base = framebuffer_base;
	glGenFramebuffers(1, &entry->framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, entry->framebuffer);
	if (renderbuffers)
	{
		if (color)
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
		if (depth)
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
	}
#ifdef HALO_VR
	else if (framebuffer_views > 1)
	{
		/* both eyes' layers, each draw drawing into both */
		if (color)
			glFramebufferTextureMultiviewOVR(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, color, 0, framebuffer_base,
				framebuffer_views);
		if (depth)
			glFramebufferTextureMultiviewOVR(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, depth, 0, framebuffer_base,
				framebuffer_views);
	}
	else if (framebuffer_views < 0)
	{
		/* one eye's layer */
		if (color)
			glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, color, 0, -framebuffer_views - 1);
		if (depth)
			glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, depth, 0, -framebuffer_views - 1);
	}
#endif
	else
	{
		if (color)
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
		if (depth)
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
	}
	glDrawBuffers(1, &draw_buffer);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		platform_log("framebuffer %u/%u%s is incomplete", color, depth, renderbuffers ? " (multisampled)" : "");
	xgpu_gl_state_invalidate();
	entry->next = framebuffers;
	framebuffers = entry;
	return entry->framebuffer;
}

static GLuint framebuffer_get(GLuint color, GLuint depth)
{
	return framebuffer_find(color, depth, FALSE);
}

/* ---------- multisampling

With display.anti_aliasing's multisampling, the back buffer and its depth
buffer are drawn into multisampled renderbuffers, and so is any target drawn
together with one of them (the mirror's view goes to the secondary target
with the back buffer's depth buffer): a framebuffer's attachments are all
multisampled or none is, so that of a target's texture and its renderbuffer
only one has pixels drawn since the other had them. The renderbuffer's
pixels are resolved into the texture before anything reads the texture (a
draw's textures, the display blit), and the texture's are put into the
renderbuffer when it is made, or made again for other samples. Nothing
samples a depth buffer as a texture (the game's copy of the depth buffer's
memory is a target of its own), so a depth buffer is resolved only when it
stops being multisampled. */

/* the samples a pixel of the bound targets: their renderbuffers' (or 1) */
static int target_samples = 1;

/* the multisampled pixels of a target drawn into since into its texture */
static void render_target_resolve(struct xgpu_render_target *target)
{
	GLuint read, draw;

	if (!target->unresolved)
		return;
	target->unresolved = FALSE;
	/* (both found first: making a framebuffer binds it) */
	read = target->depth ? framebuffer_find(0, target->multisample, TRUE) :
		framebuffer_find(target->multisample, 0, TRUE);
	draw = target->depth ? framebuffer_get(0, target->texture) : framebuffer_get(target->texture, 0);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
	glDisable(GL_SCISSOR_TEST);
	glBlitFramebuffer(0, 0, (GLint)target->gl_width, (GLint)target->gl_height,
		0, 0, (GLint)target->gl_width, (GLint)target->gl_height,
		target->depth ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : GL_COLOR_BUFFER_BIT, GL_NEAREST);
	xgpu_gl_state_invalidate();
}

/* a target's renderbuffer with these samples a pixel (0: none, its storage
a pixel, the renderbuffer kept for the framebuffers made of it): its pixels
resolved into its texture first, and the texture's put into the new
storage */
static void render_target_multisample(struct xgpu_render_target *target, int samples)
{
	GLuint draw;

	if (target->samples == samples)
		return;
	render_target_resolve(target);
	if (!target->multisample)
		glGenRenderbuffers(1, &target->multisample);
	glBindRenderbuffer(GL_RENDERBUFFER, target->multisample);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, target->depth ? GL_DEPTH24_STENCIL8 : GL_RGBA8,
		samples ? (GLsizei)target->gl_width : 1, samples ? (GLsizei)target->gl_height : 1);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	target->samples = samples;
	if (!samples)
		return;
	draw = target->depth ? framebuffer_find(0, target->multisample, TRUE) :
		framebuffer_find(target->multisample, 0, TRUE);
#ifdef HALO_GLES
	/* (ES blits into no multisampled framebuffer: it is cleared instead. A
	change of the setting is taken up between frames, and the frame clears
	the screen's targets before it draws) */
	glBindFramebuffer(GL_FRAMEBUFFER, draw);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_TRUE);
	glStencilMask(0xff);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClearDepth(1.0f);
	glClearStencil(0);
	glClear(target->depth ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : GL_COLOR_BUFFER_BIT);
#else
	{
		GLuint read = target->depth ? framebuffer_get(0, target->texture) : framebuffer_get(target->texture, 0);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
		glDisable(GL_SCISSOR_TEST);
		glBlitFramebuffer(0, 0, (GLint)target->gl_width, (GLint)target->gl_height,
			0, 0, (GLint)target->gl_width, (GLint)target->gl_height,
			target->depth ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
#endif
	xgpu_gl_state_invalidate();
}

/* counts draws and clears into render targets (xgpu_render_target.written) */
static unsigned long render_target_write_serial;

/* the pixels per unit of the bound targets (render_target_get) */
static float target_scale[2] = { 1.0f, 1.0f };
#ifdef HALO_VR
/* the bound targets' rows: the eye pass draws upside down (vr_flip_rows) */
static unsigned long target_height;

/* The eye pass draws upside down (its vertex shaders flip y, nv2a_vsh.c),
as OpenXR's GL images have their first row at the bottom where the game's
targets have it at the top: the rows of its rectangles count from the
other end, and its layered targets are sampled upside down (nv2a_psh.c). */
static void vr_flip_rows(GLint *y, GLint height)
{
	if (vr_gl.multiview || vr_gl.left_eye || vr_gl.right_eye)
		*y = (GLint)target_height - *y - height;
}

/* Foveated (xgpu.h), each view's magnification and offset of its clip
positions (x, y, as the eye pass draws them, upside down): the whole eyes'
the identity; an inset's, its part of the eye at the eye image's pixels.
A target's pixel q (from its top left, at the eye pass's scale) is the eye
image's q s (s: its pixels over the layer's), the inset's q s - c (c: the
inset's top left, vr_gl.fovea_corners), so that the inset is drawn exactly
on the eye image's pixels wherever it is (as the gaze moves, nothing in it
crawls). For the viewport v (x, y its first row, width, height) of a
target h high (the clip origin upper left: rows from the first),
	x' = s x + (s - 1 + 2 ((s - 1) v.x - c.x) / v.width) w,
	y' = s y + (1 - s - 2 ((1 - s) (h - v.y) + c.y) / v.height) w.
A viewport not the whole target would clip the inset's view to its own
rectangle (the eye pass's always is: the log says if one is not). */
static void vr_fovea_views(void)
{
	const GLint *viewport = gl_state.viewport;
	float views[4][4];
	int view;

	if (!vr_gl.foveated || viewport[2] <= 0 || viewport[3] <= 0)
		return;
	if (viewport[0] != 0 || viewport[1] != 0 || viewport[3] != (GLint)target_height)
	{
		static BOOL said;

		if (!said)
			platform_log("vr: foveated: a draw into part of its target (%d %d %d %d of %lu rows): its insets are clipped",
				(int)viewport[0], (int)viewport[1], (int)viewport[2], (int)viewport[3], target_height);
		said = TRUE;
	}
	for (view = 0; view < 4; view++)
	{
		float sx = (float)vr_gl.eye_width / (float)vr_gl.fovea_width;
		float sy = (float)vr_gl.eye_height / (float)vr_gl.fovea_height;
		const int *corner = vr_gl.fovea_corners[view >> 1];

		if (!(view & 1))
		{
			views[view][0] = views[view][1] = 1.0f;
			views[view][2] = views[view][3] = 0.0f;
			continue;
		}
		views[view][0] = sx;
		views[view][1] = sy;
		views[view][2] = sx - 1.0f + 2.0f * ((sx - 1.0f) * (float)viewport[0] - (float)corner[0]) / (float)viewport[2];
		views[view][3] = 1.0f - sy - 2.0f * ((1.0f - sy) * ((float)target_height - (float)viewport[1]) +
			(float)corner[1]) / (float)viewport[3];
	}
	if (memcmp(views, vr_gl.fovea_view, sizeof(views)))
	{
		memcpy(vr_gl.fovea_view, views, sizeof(views));
		vr_gl.fovea_serial++;
	}
}
#endif

/* the pixel edge of a coordinate in a target's units, at its scale:
floorf's, without its call (on 32-bit x86 it saves and restores the FPU's
rounding, several times a draw) */
static GLint scaled_pixel(float coordinate, float scale)
{
	float value = coordinate * scale + 0.5f;
	GLint pixel = (GLint)value;

	/* (the conversion is toward zero: a negative value with a fraction
	rounds down one more) */
	if ((float)pixel > value)
		pixel--;
	return pixel;
}

/* ... in the bound targets' units */
static GLint target_pixel(float coordinate, int axis)
{
	return scaled_pixel(coordinate, target_scale[axis]);
}

/* binds the framebuffer for the current targets; returns FALSE if there is
nothing to draw into */
#ifdef HALO_VR
static void vr_frame_resume(void);
#endif

#ifdef HALO_VR
static GLuint vr_group_target(const struct render_target_entry *hud, int group);
#endif

static BOOL bind_targets(BOOL *has_depth)
{
	struct render_target_entry *color, *depth;
	GLuint color_texture;
	int samples;

#ifdef HALO_VR
	vr_frame_resume();
#endif
	color = render_target_get(device.render_target);
	depth = render_target_get(device.depth_stencil);

	if (depth && !depth->target.depth)
		depth = NULL;
	if (!color && !depth)
		return FALSE;
	if (color)
	{
		color->last_rendered = device.frame + 1;
		color->target.written = ++render_target_write_serial;
	}
	color_texture = color ? color->target.texture : 0;
	/* viewports and clears are in the targets' units (render_target_get) */
	target_scale[0] = color ? color->target.scale[0] : depth->target.scale[0];
	target_scale[1] = color ? color->target.scale[1] : depth->target.scale[1];
#ifdef HALO_VR
	framebuffer_views = color ? color->target.layers : depth->target.layers;
	if (color && depth && color->target.layers != depth->target.layers)
		depth = NULL;
	vr_gl.left_eye = framebuffer_views > 1 && (vr_gl.visibility_test || vr_gl.eye_alone == 0);
	vr_gl.right_eye = framebuffer_views > 1 && !vr_gl.left_eye && vr_gl.eye_alone == 1;
	vr_gl.multiview = framebuffer_views > 1 && !vr_gl.left_eye && !vr_gl.right_eye;
	if (framebuffer_views == 4 && (vr_gl.left_eye || vr_gl.right_eye))
	{
		/* foveated: that eye's two layers (xgpu.h) */
		framebuffer_base = vr_gl.right_eye ? 2 : 0;
		framebuffer_views = 2;
	}
	else if (vr_gl.left_eye)
	{
		framebuffer_views = -1;
	}
	else if (vr_gl.right_eye)
	{
		framebuffer_views = -2;
	}
	if (vr_gl.multiview || vr_gl.left_eye || vr_gl.right_eye)
		vr_gl.eyes_drawn = TRUE;
	if (vr_gl.pass == HALO_VR_PASS_HUD && color && color->target.data == device.back_buffer.Data)
	{
		vr_gl.hud_drawn = TRUE;
		/* a group of the HUD's into its own target, for its panel */
		if (vr_gl.hud_panels && vr_gl.hud_group > HALO_VR_HUD_GROUP_CENTRE)
			color_texture = vr_group_target(color, vr_gl.hud_group);
	}
	target_height = color ? color->target.gl_height : depth->target.gl_height;
#endif
	/* with multisampling, multisampled where either is a screen buffer or
	is multisampled already */
	samples = anti_aliasing() == _anti_aliasing_msaa ? anti_aliasing_samples : 0;
	if (samples && !((color && (color->screen_buffer || color->target.samples)) ||
		(depth && (depth->screen_buffer || depth->target.samples))))
	{
		samples = 0;
	}
#ifdef HALO_VR
	/* (not in the VR mode's passes: the eyes' targets have a layer for each
	eye, and the HUD and its groups are layers of their own) */
	if (vr_gl.pass != HALO_VR_PASS_NONE)
		samples = 0;
#endif
	if (color)
		render_target_multisample(&color->target, samples);
	if (depth)
		render_target_multisample(&depth->target, samples);
	if (samples)
	{
		state_framebuffer(framebuffer_find(color ? color->target.multisample : 0,
			depth ? depth->target.multisample : 0, TRUE));
		if (color)
			color->target.unresolved = TRUE;
		if (depth)
			depth->target.unresolved = TRUE;
		target_samples = samples;
	}
	else
	{
		state_framebuffer(framebuffer_get(color_texture, depth ? depth->target.texture : 0));
		target_samples = 1;
	}
#ifdef HALO_VR
	framebuffer_views = 1;
	framebuffer_base = 0;
#endif
	*has_depth = depth != NULL;
	return TRUE;
}

/* ---------- device creation */

#ifdef XGPU_SAMPLE_COUNTERS
/* the visibility tests' counters and their snapshots (with
SAMPLE_COUNTING) */
static void sample_counters_create(void)
{
	int ring;

	if (!SAMPLE_COUNTING || device.visibility_counters)
		return;
	glGenBuffers(1, &device.visibility_counters);
	glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, device.visibility_counters);
	glBufferData(GL_ATOMIC_COUNTER_BUFFER, VISIBILITY_TEST_SLOTS * sizeof(GLuint), NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, 0);
	glGenBuffers(STREAM_BUFFER_RING, device.counter_snapshots);
	for (ring = 0; ring < STREAM_BUFFER_RING; ring++)
	{
		glBindBuffer(GL_COPY_WRITE_BUFFER, device.counter_snapshots[ring]);
		glBufferData(GL_COPY_WRITE_BUFFER, VISIBILITY_TEST_SLOTS * sizeof(GLuint), NULL, GL_STREAM_READ);
	}
	glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
	xgpu_gl_state_invalidate();
}
#endif

#ifdef HALO_VR
static void vr_gl_initialize(void);
#endif

static void gl_initialize(void)
{
	GLint major = 0, minor = 0;
	int index;

	glGetIntegerv(GL_MAJOR_VERSION, &major);
	glGetIntegerv(GL_MINOR_VERSION, &minor);
#ifdef HALO_GLES
	{
		BOOL es32 = major > 3 || (major == 3 && minor >= 2);
		const char *base_vertex_source = es32 ? "core" : "none";

		/* clip control is emulated in the vertex shader (nv2a_vsh.c) */
		xgpu_capabilities.copy_image = es32 || host_gl_has_extension("GL_EXT_copy_image") ||
			host_gl_has_extension("GL_OES_copy_image");
		xgpu_capabilities.border_clamp = es32 || host_gl_has_extension("GL_EXT_texture_border_clamp") ||
			host_gl_has_extension("GL_OES_texture_border_clamp");
		xgpu_capabilities.anisotropy = host_gl_has_extension("GL_EXT_texture_filter_anisotropic");
		/* ANGLE and many Android drivers report ES 3.0 or 3.1 and offer
		glDrawElementsBaseVertex as an extension under another name; the renderer
		calls the pointer it has, so it takes the extension's when the core one is
		not there. One that cannot be loaded leaves the draws without a base vertex */
		xgpu_capabilities.base_vertex = es32 && halo_glDrawElementsBaseVertex != NULL;
		if (!xgpu_capabilities.base_vertex)
		{
			if (host_gl_has_extension("GL_EXT_draw_elements_base_vertex") && halo_glDrawElementsBaseVertexEXT)
			{
				halo_glDrawElementsBaseVertex = halo_glDrawElementsBaseVertexEXT;
				xgpu_capabilities.base_vertex = TRUE;
				base_vertex_source = "GL_EXT_draw_elements_base_vertex";
			}
			else if (host_gl_has_extension("GL_OES_draw_elements_base_vertex") && halo_glDrawElementsBaseVertexOES)
			{
				halo_glDrawElementsBaseVertex = halo_glDrawElementsBaseVertexOES;
				xgpu_capabilities.base_vertex = TRUE;
				base_vertex_source = "GL_OES_draw_elements_base_vertex";
			}
		}
		xgpu_capabilities.shading_language = major > 3 || (major == 3 && minor >= 1) ? "310 es" : "300 es";
		if (major > 3 || (major == 3 && minor >= 1))
		{
			GLint counters = 0;

			glGetIntegerv(GL_MAX_FRAGMENT_ATOMIC_COUNTERS, &counters);
			xgpu_capabilities.atomic_counters = counters > 0;
		}
		xgpu_capabilities.s3tc = host_gl_has_extension("GL_EXT_texture_compression_s3tc") ||
			(host_gl_has_extension("GL_EXT_texture_compression_dxt1") &&
			host_gl_has_extension("GL_ANGLE_texture_compression_dxt3") &&
			host_gl_has_extension("GL_ANGLE_texture_compression_dxt5"));
		platform_log("OpenGL ES %d.%d: copy image %d, border clamp %d, anisotropy %d, S3TC %d, sample counting %d, "
			"base vertex %d (%s)",
			(int)major, (int)minor, xgpu_capabilities.copy_image, xgpu_capabilities.border_clamp,
			xgpu_capabilities.anisotropy, xgpu_capabilities.s3tc, xgpu_capabilities.atomic_counters,
			xgpu_capabilities.base_vertex, base_vertex_source);
	}
#else
	if (config_boolean("debug.gl_debug"))
	{
		glEnable(GL_DEBUG_OUTPUT);
		glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		glDebugMessageCallback(gl_debug_callback, NULL);
	}
	glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
	glEnable(GL_PROGRAM_POINT_SIZE);
#endif
	glGenVertexArrays(1, &device.vertex_array);
	glBindVertexArray(device.vertex_array);
#ifdef XGPU_STREAM_RING
	{
		int ring;

		glGenBuffers(STREAM_BUFFER_RING, device.stream_buffers);
		glGenBuffers(STREAM_BUFFER_RING, device.index_buffers);
		for (ring = 0; ring < STREAM_BUFFER_RING; ring++)
		{
			glBindBuffer(GL_ARRAY_BUFFER, device.stream_buffers[ring]);
			glBufferData(GL_ARRAY_BUFFER, STREAM_BUFFER_SIZE, NULL, GL_STREAM_DRAW);
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, device.index_buffers[ring]);
			glBufferData(GL_ELEMENT_ARRAY_BUFFER, INDEX_BUFFER_SIZE, NULL, GL_STREAM_DRAW);
		}
		device.stream_buffer = device.stream_buffers[0];
		device.index_buffer = device.index_buffers[0];
	}
#endif
#ifndef XGPU_STREAM_RING
	glGenBuffers(1, &device.stream_buffer);
	glBindBuffer(GL_ARRAY_BUFFER, device.stream_buffer);
	glBufferData(GL_ARRAY_BUFFER, STREAM_BUFFER_SIZE, NULL, GL_STREAM_DRAW);
	glGenBuffers(1, &device.index_buffer);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, device.index_buffer);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, INDEX_BUFFER_SIZE, NULL, GL_STREAM_DRAW);
#endif
	glGenSamplers(D3DTSS_MAXSTAGES, device.samplers);
	glGenQueries(VISIBILITY_TEST_SLOTS, device.queries);
	glGenQueries(1, &device.scratch_query);
#ifndef HALO_GLES
	glGenBuffers(1, &device.visibility_results_buffer);
	glBindBuffer(GL_QUERY_BUFFER, device.visibility_results_buffer);
	glBufferStorage(GL_QUERY_BUFFER, VISIBILITY_TEST_SLOTS * sizeof(GLuint), NULL,
		GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
#ifdef HALO_ARM64_GUEST
	/* the mapping would be a host address: the host keeps it and copies the
	results into guest memory after each frame (D3DDevice_Present) */
	if (host_gl_map_results(device.visibility_results_buffer, VISIBILITY_TEST_SLOTS * sizeof(GLuint)))
		device.visibility_results = device.visibility_results_copy;
#else
	device.visibility_results = glMapBufferRange(GL_QUERY_BUFFER, 0, VISIBILITY_TEST_SLOTS * sizeof(GLuint),
		GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
#endif
	if (device.visibility_results)
	{
		glGenQueries(VISIBILITY_TEST_SLOTS, device.visibility_batches[0].queries);
		glGenQueries(VISIBILITY_TEST_SLOTS, device.visibility_batches[1].queries);
	}
	else
		platform_log("cannot map the visibility test results; tests wait for the GPU");
	/* (unbound: a query read into a bound query buffer takes its pointer
	for an offset into it) */
	glBindBuffer(GL_QUERY_BUFFER, 0);
	{
		long every = config_integer("debug.gpu_flush_draws");
		const char *renderer = (const char *)glGetString(GL_RENDERER);

		if (every < 0)
			every = renderer && strstr(renderer, "Mesa Intel") ? 3 : 0;
		if (every > 0)
		{
			device.flush_every = (unsigned long)every;
			platform_log("GPU: a pipeline flush every %ld draws", every);
		}
	}
#endif
#ifdef HALO_GLES
	sample_counters_create();
#endif
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		device.attributes[index][3] = 1.0f;
		glVertexAttrib4fv(index, device.attributes[index]);
	}
	memory_watch_initialize();
	debug_settings.skip_vertex_shaders = config_string("debug.gpu_skip_vertex_shaders");
	debug_settings.dump_shaders = *config_string("debug.gpu_dump_shaders") ?
		config_string("debug.gpu_dump_shaders") : NULL;
	debug_settings.statistics = config_boolean("debug.gpu_stats");
	{
		GLint renderbuffer_size = 0;

		glGetIntegerv(GL_MAX_SAMPLES, &anti_aliasing_maximum_samples);
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum_target_size);
		glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &renderbuffer_size);
		if (renderbuffer_size < maximum_target_size)
			maximum_target_size = renderbuffer_size;
	}
#ifdef HALO_VR
	vr_gl_initialize();
#endif
	xgpu_gl_state_invalidate();
	device.gl_ready = TRUE;
	if (anti_aliasing_value < 0)
		anti_aliasing_read();
	else
		anti_aliasing_prepare();
}

/* what display.anti_aliasing's value needs of the GL context, once there is
one: multisampling's samples, at most the GPU's, and the passes' programs,
built as the value is chosen rather than in the middle of a frame (SMAA's
are large) */
static void anti_aliasing_prepare(void)
{
	int mode;

	if (!device.gl_ready || anti_aliasing_value < 0)
		return;
	mode = anti_aliasing_values[anti_aliasing_value].mode;
	anti_aliasing_samples = anti_aliasing_values[anti_aliasing_value].samples;
	if (anti_aliasing_samples > anti_aliasing_maximum_samples)
	{
		platform_log("anti-aliasing: the GPU has at most %d samples a pixel", (int)anti_aliasing_maximum_samples);
		anti_aliasing_samples = anti_aliasing_maximum_samples < 2 ? 0 : anti_aliasing_maximum_samples;
	}
	if ((mode == _anti_aliasing_fxaa || mode == _anti_aliasing_smaa) && !xgpu_post_prepare(mode == _anti_aliasing_smaa))
		platform_log("anti-aliasing: its programs do not build, so the 3D view is not antialiased");
	xgpu_gl_state_invalidate();
}

Direct3D *WINAPI Direct3DCreate8(UINT sdk_version)
{
	(void)sdk_version;
	return (Direct3D *)1;
}

void WINAPI Direct3D_SetPushBufferSize(DWORD push_buffer_size, DWORD segment_count)
{
	(void)push_buffer_size;
	(void)segment_count;
}

/* each vertex constant register's serial is the value constants_serial took
when the register last changed; a program's registers are current up to
the serial it recorded when it last uploaded them. The serials are 64-bit:
the count rises with every register a draw changes (a skinned model changes
up to 132), and 32 bits wrapped within minutes at a high frame rate, after
which every program's next draw found none of its registers changed and
drew with what it last uploaded (another object's node matrices: vertices
flung across the screen for a frame). */
static unsigned long long constant_serials[XGPU_VERTEX_CONSTANT_COUNT];
static unsigned long long constants_serial;
/* the register each of the latest serials changed, so a program that is
only a little behind finds its changed registers without a full scan */
#define CONSTANT_LOG_SIZE 1024
static unsigned char constant_log[CONSTANT_LOG_SIZE];
/* the registers changed since the last upload (to any program): the
smallest and largest, and the serial that upload was current to. A program
current to that serial needs them, and nothing in the log. The serial is
64-bit as the others are (cut to 32, it matched a program left at an old
serial, which then took only the checkpoint's registers). */
static unsigned long long constants_checkpoint_serial;
static unsigned long constants_checkpoint_first = XGPU_VERTEX_CONSTANT_COUNT, constants_checkpoint_last;

static void constants_store(unsigned long first, const void *data, unsigned long count)
{
	const float (*values)[4] = data;
	unsigned long index;

	for (index = 0; index < count; index++)
	{
		if (memcmp(device.constants[first + index], values[index], sizeof(device.constants[0])))
		{
			memcpy(device.constants[first + index], values[index], sizeof(device.constants[0]));
			constant_serials[first + index] = ++constants_serial;
			constant_log[constants_serial % CONSTANT_LOG_SIZE] = (unsigned char)(first + index);
			if (constants_checkpoint_first > first + index)
				constants_checkpoint_first = first + index;
			if (constants_checkpoint_last < first + index)
				constants_checkpoint_last = first + index;
		}
	}
}

static void viewport_update_constants(void)
{
	/* Direct3D's reserved constants c[-38] and c[-37] map clip space to
	the screen; zscale is the depth buffer's range */
	float zscale = 16777215.0f;
	unsigned long width, height;
	BOOL depth;

	if (device.depth_stencil)
	{
		struct xgpu_texture_description description;

		xgpu_texture_describe(device.depth_stencil->Format, device.depth_stencil->Size, &description);
		if (description.format == D3DFMT_D16 || description.format == D3DFMT_LIN_D16 ||
			description.format == D3DFMT_F16 || description.format == D3DFMT_LIN_F16)
		{
			zscale = 65535.0f;
		}
	}
	(void)width; (void)height; (void)depth;
	device.viewport_scale[0] = device.viewport.Width * 0.5f;
	device.viewport_scale[1] = -(float)device.viewport.Height * 0.5f;
	device.viewport_scale[2] = zscale * (device.viewport.MaxZ - device.viewport.MinZ);
	device.viewport_scale[3] = 0.0f;
	device.viewport_offset[0] = device.viewport.X + device.viewport.Width * 0.5f;
	device.viewport_offset[1] = device.viewport.Y + device.viewport.Height * 0.5f;
	device.viewport_offset[2] = zscale * device.viewport.MinZ;
	device.viewport_offset[3] = 0.0f;
	if (!(device.shader_constant_mode & D3DSCM_NORESERVEDCONSTANTS))
	{
		constants_store(XGPU_VERTEX_CONSTANT_BIAS - 38, device.viewport_scale, 1);
		constants_store(XGPU_VERTEX_CONSTANT_BIAS - 37, device.viewport_offset, 1);
	}
}

HRESULT WINAPI Direct3D_CreateDevice(UINT adapter, D3DDEVTYPE device_type, void *unused, DWORD behavior_flags,
	D3DPRESENT_PARAMETERS *presentation_parameters, D3DDevice **returned_device)
{
	unsigned long width, height;
	int index;

	(void)adapter;
	(void)device_type;
	(void)unused;
	(void)behavior_flags;
	if (!device.created)
	{
		memset(&device, 0, sizeof(device));
		if (presentation_parameters)
			device.presentation = *presentation_parameters;
		width = device.presentation.BackBufferWidth ? device.presentation.BackBufferWidth : 640;
		height = device.presentation.BackBufferHeight ? device.presentation.BackBufferHeight : 480;
#ifdef HALO_ANDROID
		d3d8_surface_initialize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, width, height);
		d3d8_surface_initialize(&device.depth_buffer, D3DFMT_LIN_D24S8, width, height);
#else
		/* room for the widest screen, which F11 can switch to (the screen's
		width, above) */
		d3d8_surface_initialize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, SCREEN_MAXIMUM_WIDTH, height);
		d3d8_surface_initialize(&device.depth_buffer, D3DFMT_LIN_D24S8, SCREEN_MAXIMUM_WIDTH, height);
		d3d8_surface_resize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, width, height);
		d3d8_surface_resize(&device.depth_buffer, D3DFMT_LIN_D24S8, width, height);
#endif
		device.render_target = &device.back_buffer;
		device.depth_stencil = &device.depth_buffer;
		for (index = 0; index < D3DTS_MAX; index++)
		{
			device.transforms[index]._11 = 1.0f;
			device.transforms[index]._22 = 1.0f;
			device.transforms[index]._33 = 1.0f;
			device.transforms[index]._44 = 1.0f;
		}
		device.viewport.Width = width;
		device.viewport.Height = height;
		device.viewport.MaxZ = 1.0f;
		device.next_vertex_shader_id = 1;
		D3D__RenderState[D3DRS_ZENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZWRITEENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
		D3D__RenderState[D3DRS_COLORWRITEENABLE] = D3DCOLORWRITEENABLE_ALL;
		D3D__RenderState[D3DRS_SRCBLEND] = D3DBLEND_ONE;
		D3D__RenderState[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
		D3D__RenderState[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
		D3D__RenderState[D3DRS_CULLMODE] = D3DCULL_CCW;
		D3D__RenderState[D3DRS_FRONTFACE] = D3DFRONT_CW;
		D3D__RenderState[D3DRS_FILLMODE] = D3DFILL_SOLID;
		D3D__RenderState[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
		D3D__RenderState[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
		D3D__RenderState[D3DRS_STENCILMASK] = 0xff;
		D3D__RenderState[D3DRS_STENCILWRITEMASK] = 0xff;
		D3D__RenderState[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
		D3D__RenderState[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
		D3D__RenderState[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
		for (index = 0; index < D3DTSS_MAXSTAGES; index++)
		{
			D3D__TextureState[index][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSW] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_MAGFILTER] = D3DTEXF_POINT;
			D3D__TextureState[index][D3DTSS_MINFILTER] = D3DTEXF_POINT;
			D3D__TextureState[index][D3DTSS_MAXANISOTROPY] = 1;
		}
		viewport_update_constants();

		if (!config_boolean("debug.null_renderer") && platform_video_initialize(width, height))
			gl_initialize();
		else
			platform_log("Direct3D: running without a window (nothing is displayed)");
		device.created = TRUE;
	}
	*returned_device = device_pointer();
	return S_OK;
}

/* ---------- the menus' pointer */

/* a point in the window, as SDL reports it, in the menus' coordinates: the
inverse of the letterboxed display blit at presentation, the screen's
width and the menus' centering (halo_screen_ui_offset); or, not centered,
the screen's (the game's drawing: the scoreboard's); x and y are -1 if there
is no back buffer or window yet */
static void ui_point_from_window_on(float window_x, float window_y, int centered, short *x, short *y)
{
	struct render_target_entry *back_buffer = render_target_get(&device.back_buffer);
	int window_width, window_height, pixel_width, pixel_height, width, height, left, top;
	float screen_x, screen_y;

	*x = *y = -1;
	if (!back_buffer)
		return;
	platform_video_window_size(&window_width, &window_height);
	platform_video_drawable_size(&pixel_width, &pixel_height);
	if (window_width <= 0 || window_height <= 0)
		return;
	width = pixel_width;
	height = (int)((long)pixel_width * back_buffer->target.gl_height / back_buffer->target.gl_width);
	if (height > pixel_height)
	{
		height = pixel_height;
		width = (int)((long)pixel_height * back_buffer->target.gl_width / back_buffer->target.gl_height);
	}
	left = (pixel_width - width) / 2;
	top = (pixel_height - height) / 2;
	screen_x = (window_x * pixel_width / window_width - left) * (float)back_buffer->target.width / (float)width;
	screen_y = (window_y * pixel_height / window_height - top) * (float)back_buffer->target.height / (float)height;
	*x = (short)floorf(screen_x - (centered ? (float)(halo_screen_width() - 640) / 2.0f : 0.0f));
	*y = (short)floorf(screen_y);
}

static void ui_point_from_window(float window_x, float window_y, short *x, short *y)
{
	ui_point_from_window_on(window_x, window_y, TRUE, x, y);
}

#ifdef HALO_ANDROID
int halo_scoreboard_pointer_update(int offered, struct halo_ui_pointer *pointer)
{
	(void)offered;
	(void)pointer;
	return -1;
}
#else
int halo_scoreboard_pointer_update(int offered, struct halo_ui_pointer *pointer)
{
	struct platform_ui_pointer state;

	memset(pointer, 0, sizeof(*pointer));
	if (!platform_scoreboard_pointer(offered != 0, &state) || !device.gl_ready)
		return 0;
	ui_point_from_window_on(state.x, state.y, FALSE, &pointer->x, &pointer->y);
	ui_point_from_window_on(state.click_x, state.click_y, FALSE, &pointer->click_x, &pointer->click_y);
	pointer->moved = state.moved != FALSE;
	pointer->left_clicks = (unsigned char)(state.left_clicks < 255 ? state.left_clicks : 255);
	return 1;
}
#endif

int halo_ui_pointer_update(int menus_active, struct halo_ui_pointer *pointer)
{
	struct platform_ui_pointer state;

	platform_menus_set_active(menus_active != 0);
	platform_ui_pointer_set_active(menus_active != 0);
#ifdef HALO_VR
	halo_vr_menus(menus_active != 0);
#endif
	if (!menus_active || !device.gl_ready || !platform_ui_pointer_read(&state))
		return 0;
	memset(pointer, 0, sizeof(*pointer));
	ui_point_from_window(state.x, state.y, &pointer->x, &pointer->y);
	ui_point_from_window(state.click_x, state.click_y, &pointer->click_x, &pointer->click_y);
	pointer->moved = state.moved != FALSE;
	pointer->left_clicks = (unsigned char)(state.left_clicks < 255 ? state.left_clicks : 255);
	pointer->right_clicks = (unsigned char)(state.right_clicks < 255 ? state.right_clicks : 255);
	pointer->wheel_steps = (signed char)(state.wheel_steps < -8 ? -8 : state.wheel_steps > 8 ? 8 : state.wheel_steps);
	pointer->touch = state.touch != FALSE;
	pointer->downs = (unsigned char)(state.downs < 255 ? state.downs : 255);
	ui_point_from_window(state.down_x, state.down_y, &pointer->down_x, &pointer->down_y);
	return 1;
}

/* takes up the display's shape and resolution, or the window's, if they
have changed; between frames, since the game's layout and the targets must
agree for a whole frame. Returns the width the game draws. */
long halo_screen_commit(void)
{
	long width;
	float scale[2];

	/* (display.anti_aliasing, as Settings or config.toml has it now:
	supersampling changes the scale below) */
	anti_aliasing_read();
	/* display.shadow_resolution, if it has changed: the maps' entries at
	the old scale are freed (render_targets_evict), new ones made at the new */
	if (shadow_scale && shadow_scale_read_at != config_changes())
	{
		long shadow = shadow_scale_choose();

		shadow_scale_read_at = config_changes();
		if (shadow != shadow_scale)
		{
			platform_log("shadow maps: %ldx%ld", SHADOW_MAP_SIZE * shadow, SHADOW_MAP_SIZE * shadow);
			shadow_scale = shadow;
			render_targets_evict();
		}
	}
	if (!screen_width)
		return halo_screen_width();
	screen_mode_choose(&width, scale);
	if (width != screen_width || scale[0] != screen_scale[0] || scale[1] != screen_scale[1])
	{
		platform_log("screen: %ldx%d drawn at %.0fx%.0f", width, SCREEN_HEIGHT,
			width * scale[0], SCREEN_HEIGHT * scale[1]);
		screen_width = width;
		screen_scale[0] = scale[0];
		screen_scale[1] = scale[1];
#ifndef HALO_ANDROID
		if (device.created)
		{
			device.presentation.BackBufferWidth = (UINT)width;
			d3d8_surface_resize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, (unsigned long)width, SCREEN_HEIGHT);
			d3d8_surface_resize(&device.depth_buffer, D3DFMT_LIN_D24S8, (unsigned long)width, SCREEN_HEIGHT);
		}
#endif
		/* (the entries of the old size and scale, which nothing finds now) */
		render_targets_evict();
	}
	return screen_width;
}

ULONG WINAPI D3DDevice_Release(void)
{
	return 1;
}

void WINAPI D3DDevice_GetDeviceCaps(D3DCAPS8 *caps)
{
	memset(caps, 0, sizeof(*caps));
	caps->DeviceType = D3DDEVTYPE_HAL;
	caps->MaxTextureWidth = 4096;
	caps->MaxTextureHeight = 4096;
	caps->MaxVolumeExtent = 512;
	caps->MaxTextureRepeat = 8192;
	caps->MaxTextureAspectRatio = 4096;
	caps->MaxAnisotropy = 4;
	caps->MaxTextureBlendStages = 4;
	caps->MaxSimultaneousTextures = 4;
	caps->MaxActiveLights = 8;
	caps->MaxVertexBlendMatrices = 4;
	caps->MaxPointSize = 64.0f;
	caps->MaxPrimitiveCount = 0xfffff;
	caps->MaxVertexIndex = 0xffff;
	caps->MaxStreams = 16;
	caps->MaxStreamStride = 255;
	caps->VertexShaderVersion = D3DVS_VERSION(1, 1);
	caps->MaxVertexShaderConst = 192;
	caps->PixelShaderVersion = D3DPS_VERSION(1, 1);
	caps->MaxPixelShaderValue = 1.0f;
}

void WINAPI D3DDevice_GetBackBuffer(INT back_buffer, D3DBACKBUFFER_TYPE type, D3DSurface **result)
{
	(void)back_buffer;
	(void)type;
	/* like Direct3D, the caller gets a reference it must release */
	device.back_buffer.Common++;
	*result = &device.back_buffer;
}

HRESULT WINAPI D3DDevice_GetDepthStencilSurface(D3DSurface **result)
{
	*result = device.depth_stencil;
	if (!*result)
		return D3DERR_NOTFOUND;
	(*result)->Common++;
	return S_OK;
}

static BOOL trace_frame(void);

void WINAPI D3DDevice_SetRenderTarget(D3DSurface *render_target, D3DSurface *depth_stencil)
{
	if (trace_frame())
		platform_log("set render target %08lx depth %08lx", render_target ? (unsigned long)render_target->Data : 0,
			depth_stencil ? (unsigned long)depth_stencil->Data : 0);
	stats.target_changes++;
	if (render_target)
		device.render_target = render_target;
	device.depth_stencil = depth_stencil;
	/* like Direct3D, reset the viewport to the whole new target */
	if (device.render_target)
	{
		unsigned long width, height;
		BOOL depth;

		surface_dimensions(device.render_target, &width, &height, &depth);
		device.viewport.X = 0;
		device.viewport.Y = 0;
		device.viewport.Width = width;
		device.viewport.Height = height;
		device.viewport.MinZ = 0.0f;
		device.viewport.MaxZ = 1.0f;
	}
	viewport_update_constants();
}

void WINAPI D3DDevice_SetViewport(CONST D3DVIEWPORT8 *viewport)
{
	device.viewport = *viewport;
	viewport_update_constants();
}

void WINAPI D3DDevice_SetTransform(D3DTRANSFORMSTATETYPE state, CONST D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		device.transforms[state] = *matrix;
}

void WINAPI D3DDevice_GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		*matrix = device.transforms[state];
}

void WINAPI D3DDevice_SetFlickerFilter(DWORD filter) { (void)filter; }
void WINAPI D3DDevice_SetSoftDisplayFilter(BOOL enable) { (void)enable; }

void WINAPI D3DDevice_SetShaderConstantMode(D3DSHADERCONSTANTMODE mode)
{
	device.shader_constant_mode = mode;
	viewport_update_constants();
}

/* ---------- GPU synchronisation: GL keeps its own ordering */

BOOL WINAPI D3DDevice_IsBusy(void)
{
	return FALSE;
}

void WINAPI D3DDevice_KickPushBuffer(void)
{
	if (device.gl_ready)
		glFlush();
}

void WINAPI D3DDevice_InsertCallback(D3DCALLBACKTYPE type, D3DCALLBACK callback, DWORD context)
{
	(void)type;
	/* the "GPU" reaches the callback immediately */
	if (callback)
		callback(context);
}

/* ---------- visibility (occlusion) tests */

void WINAPI D3DDevice_BeginVisibilityTest(void)
{
	if (!device.gl_ready || device.visibility_test_active)
		return;
	/* the query object is chosen when the test ends; use a scratch one */
	device.visibility_test_active = TRUE;
#ifdef HALO_VR
	vr_gl.visibility_test = vr_gl.pass == HALO_VR_PASS_EYES && !SAMPLE_COUNTING;
	if (vr_gl.visibility_test)
	{
		BOOL has_depth;

		/* the query begins with the left eye's layer bound, outside any
		multiview pass (xgpu.h, XGPU_MULTIVIEW_LEFT_EYE) */
		bind_targets(&has_depth);
	}
#endif
#ifdef XGPU_SAMPLE_COUNTERS
	if (SAMPLE_COUNTING)
	{
		const GLuint zero = 0;

		device.counter_next = (device.counter_next + 1) % VISIBILITY_TEST_SLOTS;
		device.counter_active = device.counter_next;
		glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, device.visibility_counters);
		host_gl_buffer_write(GL_ATOMIC_COUNTER_BUFFER, (unsigned int)(device.counter_active * sizeof(GLuint)),
			sizeof(zero), &zero);
		glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, 0);
		return;
	}
#else
	if (device.visibility_results)
	{
		unsigned long count = device.visibility_batches[device.visibility_batch].count;

		/* the batch's next query, or (with the batch full) a scratch one
		whose count is dropped */
		device.active_query = count < VISIBILITY_TEST_SLOTS ?
			device.visibility_batches[device.visibility_batch].queries[count] : device.scratch_query;
		glBeginQuery(VISIBILITY_QUERY, device.active_query);
		return;
	}
#endif
	glBeginQuery(VISIBILITY_QUERY, device.scratch_query);
}

HRESULT WINAPI D3DDevice_EndVisibilityTest(DWORD index)
{
	GLuint scratch;

	if (!device.gl_ready || !device.visibility_test_active)
		return S_OK;
	device.visibility_test_active = FALSE;
#ifdef HALO_VR
	vr_gl.visibility_test = FALSE;
#endif
	index %= VISIBILITY_TEST_SLOTS;
#ifdef XGPU_SAMPLE_COUNTERS
	if (SAMPLE_COUNTING)
	{
		unsigned long *count = &device.ring_test_count[device.buffer_ring];

		/* (a slot tested twice in a frame is listed twice: the later
		counter, resolved after, wins) */
		if (*count < VISIBILITY_TEST_SLOTS)
		{
			device.ring_tests[device.buffer_ring][*count][0] = (unsigned short)index;
			device.ring_tests[device.buffer_ring][*count][1] = (unsigned short)device.counter_active;
			(*count)++;
		}
		device.query_pending[index] = TRUE;
#ifdef HALO_VR
		/* (a count of the game's pixels, as below: drawn into both eyes,
		the test counts each pixel twice) */
		device.query_area[index] = target_scale[0] * target_scale[1] * (vr_gl.multiview ? 2.0f : 1.0f);
#endif
		return S_OK;
	}
#endif
	glEndQuery(VISIBILITY_QUERY);
	/* the target's samples to a game pixel (its pixels, by its samples a
	pixel with multisampling): the result is a count of the game's pixels
	(visibility_unscaled), which the game divides by its own test's area
	(lens flares, rasterizer_lights.c), a split-screen window's or the
	screen's alike */
	device.query_area[index] = target_scale[0] * target_scale[1] * (float)target_samples;
#ifndef HALO_GLES
	if (device.visibility_results)
	{
		/* the GPU copies the count into the slot at the next frame's end
		(visibility_copy_batch) */
		unsigned long *count = &device.visibility_batches[device.visibility_batch].count;

		if (*count < VISIBILITY_TEST_SLOTS)
		{
			device.visibility_batches[device.visibility_batch].slots[(*count)++] = (unsigned short)index;
			device.query_pending[index] = TRUE;
		}
		return S_OK;
	}
#endif
	/* swap the scratch query into the requested slot */
	scratch = device.scratch_query;
	device.scratch_query = device.queries[index];
	device.queries[index] = scratch;
	device.query_pending[index] = TRUE;
	device.visibility_unread[index] = TRUE;
	return S_OK;
}

#ifndef HALO_GLES
/* a count of pixels in the game's pixels */
static GLuint visibility_unscaled(GLuint samples, DWORD index)
{
	float area = device.query_area[index];

	return area > 1.0f ? (GLuint)(samples / area + 0.5f) : samples;
}

#endif
HRESULT WINAPI D3DDevice_GetVisibilityTestResult(DWORD index, UINT *result, ULONGLONG *time_stamp)
{
	GLuint available = 0, samples = 0;

	if (time_stamp)
		*time_stamp = 0;
	index %= VISIBILITY_TEST_SLOTS;
	if (!device.gl_ready || !device.query_pending[index])
	{
		if (result)
			*result = 0;
		return S_OK;
	}
#ifdef XGPU_SAMPLE_COUNTERS
	if (SAMPLE_COUNTING)
	{
		/* the latest count the GPU has finished (counter_snapshots) */
		if (result)
#ifdef HALO_VR
			*result = visibility_unscaled(device.visibility_latest[index], index);
#else
			*result = device.visibility_latest[index];
#endif
		return S_OK;
	}
#endif
#ifndef HALO_GLES
	if (device.visibility_results)
	{
		/* the latest count the GPU has written: from this test, or while
		the GPU is still behind, from the slot's earlier ones */
		if (result)
			*result = visibility_unscaled(device.visibility_results[index], index);
		return S_OK;
	}
#endif
	/* the latest count known: from this test, or while the GPU is still
	behind, from the slot's earlier ones */
	if (device.visibility_unread[index])
	{
		glGetQueryObjectuiv(device.queries[index], GL_QUERY_RESULT_AVAILABLE, &available);
		if (available)
		{
			glGetQueryObjectuiv(device.queries[index], GL_QUERY_RESULT, &samples);
#ifdef HALO_GLES
			/* ES only says whether any sample passed. The game divides the
			count by the test's area (lens flare brightness,
			rasterizer_lights.c): report more than any test covers, well
			below what would overflow there. */
			if (samples)
				samples = VISIBILITY_ALL_SAMPLES;
#else
			samples = visibility_unscaled(samples, index);
#endif
			device.visibility_known[index] = samples;
			device.visibility_unread[index] = FALSE;
		}
	}
	if (result)
		*result = device.visibility_known[index];
	return S_OK;
}

/* ---------- render and texture stage state */

void D3DFASTCALL D3DDevice_SetRenderState_Simple(DWORD method, DWORD value)
{
	/* callers also store the value in D3D__RenderState themselves */
	(void)method;
	(void)value;
}

void D3DFASTCALL D3DDevice_SetRenderState_Deferred(D3DRENDERSTATETYPE state, DWORD value)
{
	if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value);

void WINAPI D3DDevice_SetRenderStateNotInline(D3DRENDERSTATETYPE state, DWORD value)
{
	if (state == D3DRS_ZBIAS)
		D3DDevice_SetRenderState_ZBias(value);
	else if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

/* As the Xbox's D3D8 does it: a z bias is a polygon offset of -bias depth
units plus -bias/4 times the polygon's depth slope, enabled for every fill
mode. Without the slope term, decals (biased by 8) fight with the surface
under them wherever it is seen at an angle. */
void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value)
{
	float offset = -(float)value;
	float slope = offset * 0.25f;
	DWORD enable = value != 0;

	memcpy(&D3D__RenderState[D3DRS_POLYGONOFFSETZSLOPESCALE], &slope, sizeof(slope));
	memcpy(&D3D__RenderState[D3DRS_POLYGONOFFSETZOFFSET], &offset, sizeof(offset));
	D3D__RenderState[D3DRS_POINTOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_WIREFRAMEOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_SOLIDOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_ZBIAS] = value;
}

#define COMPLEX_RENDER_STATE(name, state) \
	void WINAPI D3DDevice_SetRenderState_##name(DWORD value) { D3D__RenderState[state] = value; }

COMPLEX_RENDER_STATE(PSTextureModes, D3DRS_PSTEXTUREMODES)
COMPLEX_RENDER_STATE(VertexBlend, D3DRS_VERTEXBLEND)
COMPLEX_RENDER_STATE(FogColor, D3DRS_FOGCOLOR)
COMPLEX_RENDER_STATE(FillMode, D3DRS_FILLMODE)
COMPLEX_RENDER_STATE(BackFillMode, D3DRS_BACKFILLMODE)
COMPLEX_RENDER_STATE(TwoSidedLighting, D3DRS_TWOSIDEDLIGHTING)
COMPLEX_RENDER_STATE(NormalizeNormals, D3DRS_NORMALIZENORMALS)
COMPLEX_RENDER_STATE(ZEnable, D3DRS_ZENABLE)
COMPLEX_RENDER_STATE(StencilEnable, D3DRS_STENCILENABLE)
COMPLEX_RENDER_STATE(StencilFail, D3DRS_STENCILFAIL)
COMPLEX_RENDER_STATE(FrontFace, D3DRS_FRONTFACE)
COMPLEX_RENDER_STATE(CullMode, D3DRS_CULLMODE)
COMPLEX_RENDER_STATE(TextureFactor, D3DRS_TEXTUREFACTOR)
COMPLEX_RENDER_STATE(LogicOp, D3DRS_LOGICOP)
COMPLEX_RENDER_STATE(EdgeAntiAlias, D3DRS_EDGEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleAntiAlias, D3DRS_MULTISAMPLEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleMask, D3DRS_MULTISAMPLEMASK)
COMPLEX_RENDER_STATE(MultiSampleType, D3DRS_MULTISAMPLETYPE)
COMPLEX_RENDER_STATE(ShadowFunc, D3DRS_SHADOWFUNC)
COMPLEX_RENDER_STATE(LineWidth, D3DRS_LINEWIDTH)
COMPLEX_RENDER_STATE(Dxt1NoiseEnable, D3DRS_DXT1NOISEENABLE)
COMPLEX_RENDER_STATE(YuvEnable, D3DRS_YUVENABLE)
COMPLEX_RENDER_STATE(OcclusionCullEnable, D3DRS_OCCLUSIONCULLENABLE)
COMPLEX_RENDER_STATE(StencilCullEnable, D3DRS_STENCILCULLENABLE)
COMPLEX_RENDER_STATE(RopZCmpAlwaysRead, D3DRS_ROPZCMPALWAYSREAD)
COMPLEX_RENDER_STATE(RopZRead, D3DRS_ROPZREAD)
COMPLEX_RENDER_STATE(DoNotCullUncompressed, D3DRS_DONOTCULLUNCOMPRESSED)

void D3DFASTCALL D3DDevice_SetTextureState_Deferred(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTextureState_TexCoordIndex(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_TEXCOORDINDEX] = value;
}

void WINAPI D3DDevice_SetTextureState_BorderColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_BORDERCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_ColorKeyColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_COLORKEYCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_BumpEnv(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTexture(DWORD stage, D3DBaseTexture *texture)
{
	if (stage < D3DTSS_MAXSTAGES)
		device.textures[stage] = texture;
}

void WINAPI D3DDevice_SetPalette(DWORD stage, D3DPalette *palette)
{
	if (stage < D3DTSS_MAXSTAGES)
		device.palettes[stage] = palette;
}

void WINAPI D3DDevice_SetPixelShaderProgram(D3DPIXELSHADERDEF *definition)
{
	/* the definition's members are the pixel shader render states */
	if (!definition)
		return;
	memcpy(&D3D__RenderState[D3DRS_PSALPHAINPUTS0], definition->PSAlphaInputs, sizeof(definition->PSAlphaInputs));
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSABCD] = definition->PSFinalCombinerInputsABCD;
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSEFG] = definition->PSFinalCombinerInputsEFG;
	memcpy(&D3D__RenderState[D3DRS_PSCONSTANT0_0], definition->PSConstant0, sizeof(definition->PSConstant0));
	memcpy(&D3D__RenderState[D3DRS_PSCONSTANT1_0], definition->PSConstant1, sizeof(definition->PSConstant1));
	memcpy(&D3D__RenderState[D3DRS_PSALPHAOUTPUTS0], definition->PSAlphaOutputs, sizeof(definition->PSAlphaOutputs));
	memcpy(&D3D__RenderState[D3DRS_PSRGBINPUTS0], definition->PSRGBInputs, sizeof(definition->PSRGBInputs));
	D3D__RenderState[D3DRS_PSCOMPAREMODE] = definition->PSCompareMode;
	D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0] = definition->PSFinalCombinerConstant0;
	D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1] = definition->PSFinalCombinerConstant1;
	memcpy(&D3D__RenderState[D3DRS_PSRGBOUTPUTS0], definition->PSRGBOutputs, sizeof(definition->PSRGBOutputs));
	D3D__RenderState[D3DRS_PSCOMBINERCOUNT] = definition->PSCombinerCount;
	D3D__RenderState[D3DRS_PSTEXTUREMODES] = definition->PSTextureModes;
	D3D__RenderState[D3DRS_PSDOTMAPPING] = definition->PSDotMapping;
	D3D__RenderState[D3DRS_PSINPUTTEXTURE] = definition->PSInputTexture;
}

/* ---------- vertex shaders */

static unsigned long vertex_type_bytes(unsigned long type)
{
	switch (type)
	{
	case D3DVSDT_FLOAT1: return 4;
	case D3DVSDT_FLOAT2: return 8;
	case D3DVSDT_FLOAT3: return 12;
	case D3DVSDT_FLOAT4: return 16;
	case D3DVSDT_D3DCOLOR: return 4;
	case D3DVSDT_SHORT1: return 2;
	case D3DVSDT_SHORT2: return 4;
	case D3DVSDT_SHORT3: return 6;
	case D3DVSDT_SHORT4: return 8;
	case D3DVSDT_NORMSHORT1: return 2;
	case D3DVSDT_NORMSHORT2: return 4;
	case D3DVSDT_NORMSHORT3: return 6;
	case D3DVSDT_NORMSHORT4: return 8;
	case D3DVSDT_NORMPACKED3: return 4;
	case D3DVSDT_PBYTE1: return 1;
	case D3DVSDT_PBYTE2: return 2;
	case D3DVSDT_PBYTE3: return 3;
	case D3DVSDT_PBYTE4: return 4;
	case D3DVSDT_FLOAT2H: return 12;
	default: return 0;
	}
}

static void parse_declaration(struct vertex_shader_object *object, const DWORD *declaration)
{
	unsigned long stream = 0;
	unsigned long offsets[16] = { 0 };

	for (; declaration && *declaration != D3DVSD_END(); declaration++)
	{
		DWORD token = *declaration;
		unsigned long token_type = (token & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT;

		switch (token_type)
		{
		case D3DVSD_TOKEN_STREAM:
			stream = token & D3DVSD_STREAMNUMBERMASK;
			break;
		case D3DVSD_TOKEN_STREAMDATA:
			if (token & D3DVSD_DATALOADTYPEMASK)
			{
				/* skip: the count is in dwords, or in bytes with bit 27 */
				unsigned long count = (token & D3DVSD_SKIPCOUNTMASK) >> D3DVSD_SKIPCOUNTSHIFT;

				offsets[stream] += (token & 0x08000000) ? count : count * 4;
			}
			else if (object->element_count < XGPU_VERTEX_ATTRIBUTE_COUNT)
			{
				struct vertex_element *element = &object->elements[object->element_count++];

				element->reg = (unsigned char)(token & D3DVSD_VERTEXREGMASK);
				element->stream = (unsigned char)stream;
				element->type = (unsigned char)((token & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT);
				element->bytes = (unsigned char)vertex_type_bytes(element->type);
				element->offset = (unsigned short)offsets[stream];
				offsets[stream] += element->bytes;
				if (element->type == D3DVSDT_NORMPACKED3)
					object->packed_mask |= 1UL << element->reg;
			}
			break;
		case D3DVSD_TOKEN_CONSTMEM:
			declaration += ((token & D3DVSD_CONSTCOUNTMASK) >> D3DVSD_CONSTCOUNTSHIFT) * 4;
			break;
		case D3DVSD_TOKEN_EXT:
			declaration += (token & D3DVSD_EXTCOUNTMASK) >> D3DVSD_EXTCOUNTSHIFT;
			break;
		default:
			break;
		}
	}
}

HRESULT WINAPI D3DDevice_CreateVertexShader(CONST DWORD *declaration, CONST DWORD *function, DWORD *handle, DWORD usage)
{
	struct vertex_shader_object *object = calloc(1, sizeof(*object));

	(void)usage;
	if (!object)
		return E_OUTOFMEMORY;
	object->signature = VERTEX_SHADER_SIGNATURE;
	object->id = device.next_vertex_shader_id++;
	if (function)
	{
		/* header: program type in the low word, instruction count in the high */
		object->instruction_count = function[0] >> 16;
		object->instructions = malloc(object->instruction_count * 4 * sizeof(DWORD));
		memcpy(object->instructions, function + 1, object->instruction_count * 4 * sizeof(DWORD));
	}
	parse_declaration(object, declaration);
	/* odd values are FVF codes; programmable shader handles are even */
	*handle = (DWORD)object;
	return S_OK;
}

static struct vertex_shader_object *vertex_shader_from_handle(DWORD handle)
{
	struct vertex_shader_object *object = (struct vertex_shader_object *)handle;

	if (!handle || (handle & 1) || object->signature != VERTEX_SHADER_SIGNATURE)
		return NULL;
	return object;
}

/* the game names its model lighting programs as it creates them
(rasterizer_xbox_vertex_shaders_initialize.c), so that their draws can be lit
for each pixel (display.per_pixel_lighting); one whose lighting is not as
the pixel shader computes it stays lit for each vertex */
void halo_vertex_shader_lighting(unsigned long handle)
{
	struct vertex_shader_object *object = vertex_shader_from_handle((DWORD)handle);

	if (!object || !object->instructions)
		return;
	if (!nv2a_vertex_shader_lighting(object->instructions, object->instruction_count, &object->lighting))
	{
		memset(&object->lighting, 0, sizeof(object->lighting));
		platform_log("GPU: vertex shader %lu is not lit as the pixel shader would light it: lit for each vertex",
			object->id);
	}
}

void WINAPI D3DDevice_DeleteVertexShader(DWORD handle)
{
	/* programs stay cached; the object is small */
	(void)handle;
}

void WINAPI D3DDevice_SetVertexShader(DWORD handle)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	if (object)
	{
		device.vertex_shader = object;
		device.program_address = 0;
		device.program_slots[0] = object;
	}
}

void WINAPI D3DDevice_LoadVertexShader(DWORD handle, DWORD address)
{
	if (address < VERTEX_PROGRAM_SLOTS)
		device.program_slots[address] = vertex_shader_from_handle(handle);
}

void WINAPI D3DDevice_SelectVertexShader(DWORD handle, DWORD address)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	if (object)
		device.vertex_shader = object;
	if (address < VERTEX_PROGRAM_SLOTS)
		device.program_address = address;
}

void WINAPI D3DDevice_GetVertexShaderSize(DWORD handle, UINT *size)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	*size = object ? object->instruction_count : 0;
}

void WINAPI D3DDevice_SetVertexShaderConstant(INT reg, CONST void *constant_data, DWORD constant_count)
{
	long first = reg + XGPU_VERTEX_CONSTANT_BIAS;

	if (first < 0 || first >= XGPU_VERTEX_CONSTANT_COUNT)
		return;
	if (first + (long)constant_count > XGPU_VERTEX_CONSTANT_COUNT)
		constant_count = XGPU_VERTEX_CONSTANT_COUNT - first;
	constants_store((unsigned long)first, constant_data, constant_count);
}

/* the program that runs: the one loaded at the selected address, else the
current shader's own */
static struct vertex_shader_object *current_program(void)
{
	struct vertex_shader_object *program = device.program_slots[device.program_address];

	return program ? program : device.vertex_shader;
}

/* ---------- program cache */

/* size is a multiple of 4 */
static unsigned long hash_words(const void *data, unsigned long size)
{
	const DWORD *words = data;
	unsigned long hash = 2166136261UL;

	for (size /= 4; size; size--)
		hash = (hash ^ *words++) * 16777619UL;
	return hash;
}

/* size is a multiple of 4. Every draw compares its pixel shader key and its
uniforms' inputs (about 300 bytes each) with recent ones: the loop has no
early exit, so that the compiler compares several words at once, where the
C library's memcmp (the Windows build's) compares them one at a time */
static BOOL words_equal(const void *a, const void *b, unsigned long size)
{
	const DWORD *words_a = a;
	const DWORD *words_b = b;
	DWORD difference = 0;
	unsigned long index;

	for (index = 0; index < size / 4; index++)
		difference |= words_a[index] ^ words_b[index];
	return difference == 0;
}

/* lighting: for the shader that hands the lighting's normal and position on
(vertex_shader_object lit_shader: a model lighting program's own, or a
structure's lightmap pass's, XGPU_ENVIRONMENT_LIGHTING), else NULL;
multiview: XGPU_MULTIVIEW_*, the VR mode's eyes */
static GLuint vertex_shader_get(struct vertex_shader_object *program, BOOL immediate,
	const struct nv2a_vertex_lighting *lighting, int multiview)
{
	BOOL lit = lighting != NULL;
	int variant = (immediate ? 1 : 0) + multiview * 2;
	GLuint *shader = lit ? &program->lit_shader[variant] : &program->shader[variant];
	unsigned char tried = (unsigned char)(1 << (variant + (lit ? 2 : 0)));

	if (!*shader && !(program->shaders_tried & tried))
	{
		char *source = nv2a_vertex_shader_to_glsl(program->instructions, program->instruction_count,
			immediate ? 0 : device.vertex_shader->packed_mask, lighting, multiview);

		program->shaders_tried |= tried;
		*shader = xgpu_compile_shader(GL_VERTEX_SHADER, source, "vertex");
		if (multiview)
			program->projects = strstr(source, "clip_captured = true") != NULL;
		if (debug_settings.dump_shaders)
		{
			char path[512];
			FILE *file;

			snprintf(path, sizeof(path), "%s/vs%03lu_%d%s.glsl", debug_settings.dump_shaders, program->id, variant,
				lit ? "_lit" : "");
			if ((file = fopen(path, "w")) != NULL)
			{
				fputs(source, file);
				fclose(file);
			}
		}
		free(source);
	}
	return *shader;
}

typedef char pixel_shader_key_size_assert[sizeof(struct nv2a_pixel_shader_key) % 4 == 0 ? 1 : -1];

static GLuint fragment_shader_get(const struct nv2a_pixel_shader_key *key)
{
	/* consecutive draws mostly use one of a few pixel shaders (an object's
	parts take turns) */
#define RECENT_FRAGMENT_COUNT 4
	static struct fragment_entry *recent[RECENT_FRAGMENT_COUNT];
	static unsigned long recent_next;
	unsigned long hash, index;
	struct fragment_entry **bucket;
	struct fragment_entry *entry;
	char *source;

	for (index = 0; index < RECENT_FRAGMENT_COUNT; index++)
	{
		if (recent[index] && words_equal(&recent[index]->key, key, sizeof(*key)))
			return recent[index]->shader;
	}
	hash = hash_words(key, sizeof(*key));
	bucket = &fragment_buckets[hash % FRAGMENT_BUCKETS];
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->hash == hash && words_equal(&entry->key, key, sizeof(*key)))
		{
			recent[recent_next++ % RECENT_FRAGMENT_COUNT] = entry;
			return entry->shader;
		}
	}
	entry = calloc(1, sizeof(*entry));
	entry->hash = hash;
	entry->key = *key;
	source = nv2a_pixel_shader_to_glsl(key);
	entry->shader = xgpu_compile_shader(GL_FRAGMENT_SHADER, source, "pixel");
	if (debug_settings.dump_shaders)
	{
		char path[512];
		FILE *file;

		snprintf(path, sizeof(path), "%s/ps_%08lx.glsl", debug_settings.dump_shaders, hash);
		if ((file = fopen(path, "w")) != NULL)
		{
			fputs(source, file);
			fclose(file);
		}
	}
	free(source);
	entry->next = *bucket;
	*bucket = entry;
	recent[recent_next++ % RECENT_FRAGMENT_COUNT] = entry;
	return entry->shader;
}

static struct program_entry *program_get(GLuint vertex_shader, GLuint fragment_shader)
{
	static struct program_entry *last;
	unsigned long hash = (vertex_shader * 2654435761UL) ^ fragment_shader;
	struct program_entry **bucket = &program_buckets[hash % PROGRAM_BUCKETS];
	struct program_entry *entry;
	int stage;

	if (last && last->vertex_shader == vertex_shader && last->fragment_shader == fragment_shader)
		return last;
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->vertex_shader == vertex_shader && entry->fragment_shader == fragment_shader)
		{
			if (!entry->program)
				return NULL;
			last = entry;
			return entry;
		}
	}
	entry = calloc(1, sizeof(*entry));
	entry->vertex_shader = vertex_shader;
	entry->fragment_shader = fragment_shader;
	memset(&entry->uniforms, 0xff, sizeof(entry->uniforms));
	entry->next = *bucket;
	*bucket = entry;
	if (!vertex_shader || !fragment_shader)
		return NULL;

	entry->program = xgpu_link_program(vertex_shader, fragment_shader, "shader");
	if (!entry->program)
		return NULL;
	state_program(entry->program);
	entry->constants = glGetUniformLocation(entry->program, "c");
	entry->constant_count = XGPU_VERTEX_CONSTANT_COUNT;
	if (entry->constants >= 0)
	{
		unsigned long index;

		/* c[i] is usually at c's location plus i, and the compiler may
		drop registers past the last one the program reads */
		entry->constants_consecutive = TRUE;
		for (index = 1; index < XGPU_VERTEX_CONSTANT_COUNT; index++)
		{
			char name[16];
			GLint location;

			snprintf(name, sizeof(name), "c[%lu]", index);
			location = glGetUniformLocation(entry->program, name);
			if (location < 0)
			{
				entry->constant_count = index;
				break;
			}
			if (location != entry->constants + (GLint)index)
			{
				entry->constants_consecutive = FALSE;
				entry->constant_count = XGPU_VERTEX_CONSTANT_COUNT;
				break;
			}
		}
	}
	entry->viewport_scale = glGetUniformLocation(entry->program, "viewport_scale");
	entry->viewport_offset = glGetUniformLocation(entry->program, "viewport_offset");
	entry->point_size = glGetUniformLocation(entry->program, "point_size");
	entry->ps_c0 = glGetUniformLocation(entry->program, "ps_c0");
	entry->ps_c1 = glGetUniformLocation(entry->program, "ps_c1");
	entry->ps_final_c0 = glGetUniformLocation(entry->program, "ps_final_c0");
	entry->ps_final_c1 = glGetUniformLocation(entry->program, "ps_final_c1");
	entry->fog_color = glGetUniformLocation(entry->program, "fog_color");
	entry->fog_parameters = glGetUniformLocation(entry->program, "fog_parameters");
	entry->alpha_reference = glGetUniformLocation(entry->program, "alpha_reference");
	entry->bump_matrix = glGetUniformLocation(entry->program, "bump_matrix");
	entry->bump_luminance = glGetUniformLocation(entry->program, "bump_luminance");
	entry->texture_scale = glGetUniformLocation(entry->program, "texture_scale");
	entry->texture_lod_bias = glGetUniformLocation(entry->program, "texture_lod_bias");
	entry->screen_offset = glGetUniformLocation(entry->program, "screen_offset");
	entry->blend_constant = glGetUniformLocation(entry->program, "blend_constant");
	entry->model_lights = glGetUniformLocation(entry->program, "model_lights");
	entry->eye_correction = glGetUniformLocation(entry->program, "eye_correction");
	entry->screen_correction = glGetUniformLocation(entry->program, "screen_correction");
	entry->screen_points = glGetUniformLocation(entry->program, "screen_points");
	entry->eye_serial = (unsigned long)-1;
	entry->fovea_view = glGetUniformLocation(entry->program, "fovea_view");
	entry->fovea_sample = glGetUniformLocation(entry->program, "fovea_sample");
	entry->fovea_serial = (unsigned long)-1;
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		char name[8];

		snprintf(name, sizeof(name), "tex%d", stage);
		glUniform1i(glGetUniformLocation(entry->program, name), stage);
	}
	last = entry;
	return entry;
}

/* ---------- per-draw state */

static unsigned long stage_texture_mode(int stage)
{
	return (D3D__RenderState[D3DRS_PSTEXTUREMODES] >> (5 * stage)) & 0x1f;
}

static GLenum address_mode(DWORD mode)
{
	switch (mode)
	{
	case D3DTADDRESS_MIRROR: return GL_MIRRORED_REPEAT;
	case D3DTADDRESS_CLAMP: return GL_CLAMP_TO_EDGE;
#ifdef HALO_GLES
	case D3DTADDRESS_BORDER: return xgpu_capabilities.border_clamp ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE;
#else
	case D3DTADDRESS_BORDER: return GL_CLAMP_TO_BORDER;
#endif
	case D3DTADDRESS_CLAMPTOEDGE: return GL_CLAMP_TO_EDGE;
	default: return GL_REPEAT;
	}
}

/* ---------- sampler objects

A sampler object for each sampler state the game uses, made once: a draw
binds the one its state needs, rather than changing a sampler's parameters,
which costs a GL call each, and on Zink a new Vulkan sampler. The game uses
a few dozen states. */

#define SAMPLER_STATE_WORDS 11
#define SAMPLER_CACHE_SIZE 512

static struct
{
	DWORD inputs[SAMPLER_STATE_WORDS];
	GLuint sampler;
} sampler_cache[SAMPLER_CACHE_SIZE];
static unsigned long sampler_cache_count;
/* the VR mode samples sRGB images without decoding them (vr_gl_initialize) */
static BOOL samplers_skip_srgb_decode;

/* a sampler's parameters from its state (configure_sampler's inputs) */
static void sampler_parameters(GLuint sampler, const DWORD *inputs)
{
	DWORD min_filter = inputs[0], mip_filter = inputs[1], mag_filter = inputs[2];
	DWORD lod_bias = inputs[6], maximum_mip_level = inputs[7], anisotropy = inputs[8];
	GLenum minification;
	float border[4];

	if (min_filter == D3DTEXF_POINT)
		minification = mip_filter == D3DTEXF_NONE ? GL_NEAREST :
			mip_filter == D3DTEXF_POINT ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_LINEAR;
	else
		minification = mip_filter == D3DTEXF_NONE ? GL_LINEAR :
			mip_filter == D3DTEXF_POINT ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
	glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, (GLint)minification);
	glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, mag_filter == D3DTEXF_POINT ? GL_NEAREST : GL_LINEAR);
	glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, (GLint)address_mode(inputs[3]));
	glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, (GLint)address_mode(inputs[4]));
	glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, (GLint)address_mode(inputs[5]));
#ifdef HALO_GLES
	/* ES has no sampler LOD bias; the pixel shader applies it
	(texture_lod_bias) */
	(void)lod_bias;
	glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, (float)maximum_mip_level);
	if (xgpu_capabilities.anisotropy)
		glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT,
			(min_filter == D3DTEXF_ANISOTROPIC && anisotropy > 1) ? (float)anisotropy : 1.0f);
	if (xgpu_capabilities.border_clamp)
	{
		color_to_vec4(inputs[9], border);
		glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, border);
	}
#else
	glSamplerParameterf(sampler, GL_TEXTURE_LOD_BIAS, dword_to_float(lod_bias));
	glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, (float)maximum_mip_level);
	glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY,
		(min_filter == D3DTEXF_ANISOTROPIC && anisotropy > 1) ? (float)anisotropy : 1.0f);
	color_to_vec4(inputs[9], border);
	glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, border);
#endif
	if (samplers_skip_srgb_decode)
		glSamplerParameteri(sampler, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
}

/* the sampler object of a sampler state, made the first time; with the
cache full (never seen), the stage's own sampler set to it */
static GLuint sampler_get(int stage, const DWORD *inputs)
{
	unsigned long hash = 2166136261UL, index, probe;
	GLuint sampler;

	for (index = 0; index < SAMPLER_STATE_WORDS; index++)
		hash = (hash ^ inputs[index]) * 16777619UL;
	for (probe = 0; probe < SAMPLER_CACHE_SIZE; probe++)
	{
		index = (hash + probe) % SAMPLER_CACHE_SIZE;
		if (!sampler_cache[index].sampler)
			break;
		if (!memcmp(sampler_cache[index].inputs, inputs, sizeof(sampler_cache[index].inputs)))
			return sampler_cache[index].sampler;
	}
	if (probe == SAMPLER_CACHE_SIZE || sampler_cache_count >= SAMPLER_CACHE_SIZE * 3 / 4)
	{
		sampler_parameters(device.samplers[stage], inputs);
		return device.samplers[stage];
	}
	glGenSamplers(1, &sampler);
	sampler_parameters(sampler, inputs);
	memcpy(sampler_cache[index].inputs, inputs, sizeof(sampler_cache[index].inputs));
	sampler_cache[index].sampler = sampler;
	sampler_cache_count++;
	return sampler;
}

/* display.anisotropic_filtering: the most texels along a pixel's footprint
that a texture with mip levels is sampled with where the game filters it
linearly, 0 or 1 for the game's own filtering. The Xbox game asks for none
(it never sets D3DTEXF_ANISOTROPIC or D3DTSS_MAXANISOTROPY), so the floors,
walls and terrain seen at a grazing angle blur toward their smaller mip
levels. At most the GPU's most (16 on the desktop's), and none on GLES
without GL_EXT_texture_filter_anisotropic. */
static long anisotropic_level = -1;
static unsigned long anisotropic_level_read_at;

static DWORD anisotropic_filtering(void)
{
	if (anisotropic_level < 0 || anisotropic_level_read_at != config_changes())
	{
		long level = config_integer("display.anisotropic_filtering");
		GLint maximum = 1;

		anisotropic_level_read_at = config_changes();
#ifdef HALO_GLES
		if (xgpu_capabilities.anisotropy)
			glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maximum);
#else
		glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maximum);
#endif
		if (level > (long)maximum)
			level = (long)maximum;
		if (level < 1)
			level = 1;
		if (level != anisotropic_level)
			platform_log("anisotropic filtering: %ldx (the GPU's most %dx)", level, (int)maximum);
		anisotropic_level = level;
	}
	return (DWORD)anisotropic_level;
}

/* hires: a high-res HUD texture (hud_hires.h), drawn smaller than it is, so
filtered and from its mip levels whatever the game asks: the HUD's meters are
point sampled for one player, to keep the Xbox bitmaps' texels sharp.
rendered: a render target the game draws into */
static void configure_sampler(int stage, BOOL mipmapped, BOOL hires, BOOL rendered)
{
	/* the texture stage state each stage's sampler was last chosen by */
	static DWORD configured[D3DTSS_MAXSTAGES][SAMPLER_STATE_WORDS];
	static GLuint configured_sampler[D3DTSS_MAXSTAGES];
	DWORD *state = D3D__TextureState[stage];
	DWORD inputs[SAMPLER_STATE_WORDS];

	inputs[0] = hires ? D3DTEXF_LINEAR : state[D3DTSS_MINFILTER];
	inputs[1] = hires ? D3DTEXF_LINEAR : mipmapped ? state[D3DTSS_MIPFILTER] : D3DTEXF_NONE;
	inputs[2] = hires ? D3DTEXF_LINEAR : state[D3DTSS_MAGFILTER];
	inputs[3] = state[D3DTSS_ADDRESSU];
	inputs[4] = state[D3DTSS_ADDRESSV];
	inputs[5] = state[D3DTSS_ADDRESSW];
	inputs[6] = hires ? 0 : state[D3DTSS_MIPMAPLODBIAS];
	inputs[7] = hires ? 0 : state[D3DTSS_MAXMIPLEVEL];
	inputs[8] = state[D3DTSS_MAXANISOTROPY];
	inputs[9] = state[D3DTSS_BORDERCOLOR];
	inputs[10] = hires;
	/* (display.anisotropic_filtering, over the game's linear filtering of
	the maps' textures with mip levels; not of what the game renders, the
	water's ripples, whose finer levels seen far off are noise) */
	if (!hires && mipmapped && !rendered && inputs[1] != D3DTEXF_NONE &&
		(inputs[0] == D3DTEXF_LINEAR || inputs[0] == D3DTEXF_ANISOTROPIC) && anisotropic_filtering() > 1)
	{
		inputs[0] = D3DTEXF_ANISOTROPIC;
		if (inputs[8] < anisotropic_filtering())
			inputs[8] = anisotropic_filtering();
	}
	if (!configured_sampler[stage] || memcmp(configured[stage], inputs, sizeof(inputs)))
	{
		memcpy(configured[stage], inputs, sizeof(inputs));
		configured_sampler[stage] = sampler_get(stage, inputs);
	}
	state_sampler(stage, configured_sampler[stage]);
}


/* ---------- render targets sampled with their mip chain

The game renders some textures one mip level at a time, each level being a
surface of its own (the water's ripple map). Sampling such a texture needs
every level in one GL texture, so the levels' render targets are copied into
a mipmapped composite. Each draw of the water binds it, some maps (a30) more
than once a frame, so the copy (and the mipmaps of the levels the game did not
render) is redone only once a level's target has been drawn into since the
last one. */

#define MIP_COMPOSITE_LEVELS 16

struct mip_composite
{
	struct mip_composite *next;
	unsigned long data, width, height, levels;
	GLuint texture;
	/* the levels last copied, and each one's target's texture and written
	serial then */
	unsigned long rendered_levels;
	GLuint level_sources[MIP_COMPOSITE_LEVELS];
	unsigned long level_written[MIP_COMPOSITE_LEVELS];
};

static struct mip_composite *mip_composites;

/* an entry render_target_get no longer finds: the screen's targets at
another scale or screen width than now, the shadow maps at another
display.shadow_resolution */
static BOOL render_target_stale(const struct render_target_entry *entry)
{
	switch (entry->kind)
	{
	case _render_target_screen:
		return entry->target.scale[0] != screen_scale[0] || entry->target.scale[1] != screen_scale[1] ||
			(entry->target.height == SCREEN_HEIGHT && entry->target.width != (unsigned long)halo_screen_width());
	case _render_target_shadow_map:
		return entry->target.scale[0] != (float)halo_shadow_map_scale();
	default:
		return FALSE;
	}
}

/* frees the framebuffers made of a texture, or with renderbuffers, of a
multisampled renderbuffer, which is going */
static void framebuffers_forget(GLuint attachment, BOOL renderbuffers)
{
	struct framebuffer_entry *entry, **link = &framebuffers;

	while ((entry = *link) != NULL)
	{
		if (entry->renderbuffers == renderbuffers && (entry->color == attachment || entry->depth == attachment))
		{
			*link = entry->next;
			glDeleteFramebuffers(1, &entry->framebuffer);
			free(entry);
		}
		else
		{
			link = &entry->next;
		}
	}
}

/* frees an entry taken out of render_targets: its GL objects, the
framebuffers made of them, and what remembered them */
static void render_target_delete(struct render_target_entry *entry)
{
	struct render_target_entry **link;
	struct mip_composite *composite;

	for (link = render_target_bucket(entry->target.data); *link != entry; link = &(*link)->next_in_bucket)
		;
	*link = entry->next_in_bucket;
	framebuffers_forget(entry->target.texture, FALSE);
	if (entry->target.multisample)
	{
		framebuffers_forget(entry->target.multisample, TRUE);
		glDeleteRenderbuffers(1, &entry->target.multisample);
	}
	/* (a composite that copied from the texture copies again, rather than
	take a new texture with the same name for it) */
	for (composite = mip_composites; composite; composite = composite->next)
	{
		unsigned long level;

		for (level = 0; level < composite->rendered_levels; level++)
		{
			if (composite->level_sources[level] == entry->target.texture)
			{
				composite->rendered_levels = 0;
				break;
			}
		}
	}
	xgpu_gl_texture_deleted(entry->target.texture);
	glDeleteTextures(1, &entry->target.texture);
	free(entry);
}

/* (between frames, when the screen's or the shadow maps' scale has
changed: halo_screen_commit) frees the entries render_target_get no longer
finds, which a frame at the old scale left */
static void render_targets_evict(void)
{
	struct render_target_entry *entry, **link = &render_targets;
	BOOL any = FALSE;

	while ((entry = *link) != NULL)
	{
		if (render_target_stale(entry))
		{
			*link = entry->next;
			render_target_delete(entry);
			any = TRUE;
		}
		else
		{
			link = &entry->next;
		}
	}
	if (any)
	{
		memset(recent_render_targets, 0, sizeof(recent_render_targets));
		xgpu_gl_state_invalidate();
	}
}

#ifdef HALO_GLES
static GLuint framebuffer_get(GLuint color, GLuint depth);

/* glCopyImageSubData for ES 3.0/3.1 contexts without the extension */
static void copy_level_by_blit(GLuint source, GLuint destination, GLint level, GLsizei width, GLsizei height)
{
	static GLuint draw_framebuffer;

	if (!draw_framebuffer)
		glGenFramebuffers(1, &draw_framebuffer);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer_get(source, 0));
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_framebuffer);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, destination, level);
	glDisable(GL_SCISSOR_TEST);
	glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	/* the blit bypasses the cached state, so the next draw must re-apply it */
	xgpu_gl_state_invalidate();
}
#endif

static GLuint mip_composite_get(const struct xgpu_texture_description *description, unsigned long data)
{
	struct mip_composite *composite;
	struct xgpu_render_target *targets[MIP_COMPOSITE_LEVELS];
	unsigned long level, rendered_levels = 0;
	BOOL changed;

	for (composite = mip_composites; composite; composite = composite->next)
	{
		if (composite->data == data && composite->width == description->width &&
			composite->height == description->height && composite->levels == description->levels)
		{
			break;
		}
	}
	if (!composite)
	{
		composite = calloc(1, sizeof(*composite));
		composite->data = data;
		composite->width = description->width;
		composite->height = description->height;
		composite->levels = description->levels;
		glGenTextures(1, &composite->texture);
		glBindTexture(GL_TEXTURE_2D, composite->texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)description->levels - 1);
		for (level = 0; level < description->levels; level++)
		{
			GLsizei width = (GLsizei)(description->width >> level ? description->width >> level : 1);
			GLsizei height = (GLsizei)(description->height >> level ? description->height >> level : 1);

			glTexImage2D(GL_TEXTURE_2D, (GLint)level, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
		}
		composite->rendered_levels = ~0UL;
		composite->next = mip_composites;
		mip_composites = composite;
	}
	for (level = 0; level < description->levels && level < MIP_COMPOSITE_LEVELS; level++)
	{
		unsigned long width = description->width >> level ? description->width >> level : 1;
		unsigned long height = description->height >> level ? description->height >> level : 1;
		struct xgpu_render_target *target =
			xgpu_render_target_find(data + xgpu_texture_level_offset(description, level));

		if (!target || target->width != width || target->height != height ||
			target->gl_width != width || target->gl_height != height)
			break;
		targets[level] = target;
		rendered_levels++;
	}
	changed = rendered_levels != composite->rendered_levels;
	for (level = 0; level < rendered_levels && !changed; level++)
	{
		changed = targets[level]->texture != composite->level_sources[level] ||
			targets[level]->written != composite->level_written[level];
	}
	if (!changed)
		return composite->texture;
	composite->rendered_levels = rendered_levels;
	for (level = 0; level < rendered_levels; level++)
	{
		struct xgpu_render_target *target = targets[level];
		GLsizei width = (GLsizei)target->width, height = (GLsizei)target->height;

		composite->level_sources[level] = target->texture;
		composite->level_written[level] = target->written;
		render_target_resolve(target);
#ifdef HALO_GLES
		if (!xgpu_capabilities.copy_image)
		{
			copy_level_by_blit(target->texture, composite->texture, (GLint)level, width, height);
		}
		else
#endif
		glCopyImageSubData(target->texture, GL_TEXTURE_2D, 0, 0, 0, 0,
			composite->texture, GL_TEXTURE_2D, (GLint)level, 0, 0, 0, width, height, 1);
	}
	glBindTexture(GL_TEXTURE_2D, composite->texture);
	/* levels the game did not render come from the ones it did */
	if (rendered_levels < description->levels)
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, rendered_levels ? (GLint)rendered_levels - 1 : 0);
		glGenerateMipmap(GL_TEXTURE_2D);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	}
	xgpu_gl_state_invalidate();
	return composite->texture;
}

#ifdef HALO_WEB
/* WebGL refuses a draw that samples a texture of the framebuffer it draws
into (a feedback loop), which desktop GL and ES drivers draw, sampling the
pixels as they were: a copy of the target is sampled instead, made again
only once the target has been drawn into since */
static GLuint feedback_copy(struct xgpu_render_target *target)
{
	static GLuint copy;
	static GLsizei copy_width, copy_height;
	static unsigned long copied_texture, copied_written;

	if (target->depth)
		return target->texture;
	if (!copy || copy_width != (GLsizei)target->gl_width || copy_height != (GLsizei)target->gl_height)
	{
		if (!copy)
			glGenTextures(1, &copy);
		copy_width = (GLsizei)target->gl_width;
		copy_height = (GLsizei)target->gl_height;
		glBindTexture(GL_TEXTURE_2D, copy);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, copy_width, copy_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		xgpu_gl_state_invalidate();
		copied_texture = 0;
	}
	if (copied_texture != target->texture || copied_written != target->written)
	{
		copy_level_by_blit(target->texture, copy, 0, copy_width, copy_height);
		copied_texture = target->texture;
		copied_written = target->written;
	}
	return copy;
}
#endif

static void bind_textures(struct nv2a_pixel_shader_key *key, float texture_scale[4][4])
{
	/* Bind only after resolving every stage, since texture uploads can
	overwrite the active unit's binding. */
	GLenum gl_targets[D3DTSS_MAXSTAGES];
	GLuint gl_textures[D3DTSS_MAXSTAGES];
	int stage;

	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		D3DBaseTexture *texture = device.textures[stage];
		unsigned long mode = stage_texture_mode(stage);

		texture_scale[stage][0] = texture_scale[stage][1] = 1.0f;
		texture_scale[stage][2] = texture_scale[stage][3] = 1.0f;
		if (!texture || !texture->Data || mode == 0 || mode == 0x04 || mode == 0x05 || mode == 0x11)
		{
			gl_targets[stage] = GL_TEXTURE_2D;
			gl_textures[stage] = 0;
			key->sampler_type[stage] = mode == 0x11 ? _xgpu_sampler_2d : _xgpu_sampler_none;
			continue;
		}
		{
			struct xgpu_render_target *target = xgpu_render_target_find(texture->Data);
			struct xgpu_texture_description description;
			GLenum gl_target;
			GLuint gl_texture;

			if (target)
			{
				/* (multisampled: its pixels drawn since, resolved) */
				render_target_resolve(target);
				xgpu_texture_describe(texture->Format, texture->Size, &description);
				gl_texture = target->texture;
				gl_target = GL_TEXTURE_2D;
				if (description.linear)
				{
					texture_scale[stage][0] = 1.0f / (float)target->width;
					texture_scale[stage][1] = 1.0f / (float)target->height;
				}
				if (!description.linear && !description.cube_map && description.levels > 1 &&
					target->width == description.width && target->height == description.height &&
					target->layers == 1)
					gl_texture = mip_composite_get(&description, texture->Data);
				else
					description.levels = 1;
#ifdef HALO_VR
				if (target->layers > 1)
					gl_target = GL_TEXTURE_2D_ARRAY;
#endif
#ifdef HALO_WEB
				{
					struct render_target_entry *drawn = render_target_get(device.render_target);

					if (drawn && gl_texture == drawn->target.texture)
						gl_texture = feedback_copy(target);
				}
#endif
			}
			else
			{
				const D3DCOLOR *palette = device.palettes[stage] && device.palettes[stage]->Data ?
					(const D3DCOLOR *)PLATFORM_PHYSICAL_TO_VIRTUAL(device.palettes[stage]->Data) : NULL;

				gl_texture = xgpu_texture_get((const DWORD *)texture, palette, &gl_target, &description);
				if (description.linear)
				{
					texture_scale[stage][0] = 1.0f / (float)description.width;
					texture_scale[stage][1] = 1.0f / (float)description.height;
				}
			}
			gl_targets[stage] = gl_target;
			gl_textures[stage] = gl_texture;
			configure_sampler(stage, description.levels > 1, description.hires, target != NULL);
			if (stage == 0)
			{
				key->coverage_alpha = description.hires_coverage != FALSE;
				key->point_threshold = description.hires_point_threshold != FALSE;
			}
			key->sampler_type[stage] = gl_target == GL_TEXTURE_CUBE_MAP ? _xgpu_sampler_cube :
				gl_target == GL_TEXTURE_3D ? _xgpu_sampler_3d :
				gl_target == GL_TEXTURE_2D_ARRAY ? _xgpu_sampler_2d_layered : _xgpu_sampler_2d;
		}
	}
#ifdef HALO_GLES
	/* (a stage the pixel shader does not sample keeps what it had: unbinding
	it, to bind it again at the next draw that has it, is two calls) */
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		if (gl_textures[stage] || key->sampler_type[stage] != _xgpu_sampler_none)
			state_texture(stage, gl_targets[stage], gl_textures[stage]);
	}
#else
	{
		/* the units whose texture changes, bound in one call (GL 4.4's
		multi-bind) rather than selecting and binding each unit; binding no
		texture unbinds all of the unit's targets */
		int first = -1, last = -1;

		for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
		{
			if (gl_state.textures[stage][texture_slot(gl_targets[stage])] != gl_textures[stage])
			{
				if (first < 0)
					first = stage;
				last = stage;
			}
		}
		if (first >= 0)
		{
			glBindTextures((GLuint)first, (GLsizei)(last - first + 1), &gl_textures[first]);
			for (stage = first; stage <= last; stage++)
			{
				if (gl_textures[stage])
					gl_state.textures[stage][texture_slot(gl_targets[stage])] = gl_textures[stage];
				else
					memset(gl_state.textures[stage], 0, sizeof(gl_state.textures[stage]));
			}
		}
	}
#endif
}

static GLenum stencil_operation(DWORD operation)
{
	/* Xbox stencil operations are the GL enumerants, plus 0 for ZERO */
	return operation ? (GLenum)operation : GL_ZERO;
}

static GLenum blend_equation(DWORD operation)
{
	switch (operation)
	{
	case D3DBLENDOP_SUBTRACT: return GL_FUNC_SUBTRACT;
	case D3DBLENDOP_REVSUBTRACT:
	case D3DBLENDOP_REVSUBTRACTSIGNED: return GL_FUNC_REVERSE_SUBTRACT;
	case D3DBLENDOP_MIN: return GL_MIN;
	case D3DBLENDOP_MAX: return GL_MAX;
	default: return GL_FUNC_ADD;
	}
}

static void apply_raster_state(BOOL has_depth)
{
	DWORD *rs = D3D__RenderState;
	DWORD write = rs[D3DRS_COLORWRITEENABLE];
	GLint viewport[4];
	GLint scissor[4];
	float depth_range[2];
	unsigned char color_mask;
	BOOL depth_test = has_depth && rs[D3DRS_ZENABLE];

	viewport[0] = target_pixel((float)device.viewport.X, 0);
	viewport[1] = target_pixel((float)device.viewport.Y, 1);
	viewport[2] = target_pixel((float)(device.viewport.X + device.viewport.Width), 0) - viewport[0];
	viewport[3] = target_pixel((float)(device.viewport.Y + device.viewport.Height), 1) - viewport[1];
#ifdef HALO_VR
	vr_flip_rows(&viewport[1], viewport[3]);
#endif
	if (memcmp(gl_state.viewport, viewport, sizeof(viewport)))
	{
		memcpy(gl_state.viewport, viewport, sizeof(viewport));
		glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	}
	/* the game never issues a scissor rectangle, and the NV2A scissor register
	defaults to the viewport, so fragment clipping follows the viewport: this is
	what keeps a split-screen window's geometry from bleeding across the divider */
	/* (glScissor takes the corner and the size, as glViewport does) */
	memcpy(scissor, viewport, sizeof(scissor));
	if (memcmp(gl_state.scissor, scissor, sizeof(scissor)))
	{
		memcpy(gl_state.scissor, scissor, sizeof(scissor));
		glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
	}
	state_enable(&gl_state.scissor_test, GL_SCISSOR_TEST, scissor[2] > 0 && scissor[3] > 0);
	depth_range[0] = device.viewport.MinZ;
	depth_range[1] = device.viewport.MaxZ;
	if (memcmp(gl_state.depth_range, depth_range, sizeof(depth_range)))
	{
		memcpy(gl_state.depth_range, depth_range, sizeof(depth_range));
		glDepthRange(depth_range[0], depth_range[1]);
	}

	state_enable(&gl_state.depth_test, GL_DEPTH_TEST, depth_test);
	if (depth_test)
	{
		GLenum function = rs[D3DRS_ZFUNC] ? (GLenum)rs[D3DRS_ZFUNC] : GL_NEVER;

		if (gl_state.depth_function != function)
		{
			gl_state.depth_function = function;
			glDepthFunc(function);
		}
	}
	{
		unsigned char mask = depth_test && rs[D3DRS_ZWRITEENABLE] ? 1 : 0;

		if (gl_state.depth_mask != mask)
		{
			gl_state.depth_mask = mask;
			glDepthMask(mask ? GL_TRUE : GL_FALSE);
		}
	}

	state_enable(&gl_state.stencil_test, GL_STENCIL_TEST, has_depth && rs[D3DRS_STENCILENABLE]);
	if (has_depth && rs[D3DRS_STENCILENABLE])
	{
		GLenum function = rs[D3DRS_STENCILFUNC] ? (GLenum)rs[D3DRS_STENCILFUNC] : GL_NEVER;
		GLenum operations[3];

		if (gl_state.stencil_function != function || gl_state.stencil_reference != (GLint)rs[D3DRS_STENCILREF] ||
			gl_state.stencil_value_mask != rs[D3DRS_STENCILMASK])
		{
			gl_state.stencil_function = function;
			gl_state.stencil_reference = (GLint)rs[D3DRS_STENCILREF];
			gl_state.stencil_value_mask = rs[D3DRS_STENCILMASK];
			glStencilFunc(function, (GLint)rs[D3DRS_STENCILREF], rs[D3DRS_STENCILMASK]);
		}
		operations[0] = stencil_operation(rs[D3DRS_STENCILFAIL]);
		operations[1] = stencil_operation(rs[D3DRS_STENCILZFAIL]);
		operations[2] = stencil_operation(rs[D3DRS_STENCILPASS]);
		if (memcmp(gl_state.stencil_operations, operations, sizeof(operations)))
		{
			memcpy(gl_state.stencil_operations, operations, sizeof(operations));
			glStencilOp(operations[0], operations[1], operations[2]);
		}
		if (gl_state.stencil_write_mask != rs[D3DRS_STENCILWRITEMASK])
		{
			gl_state.stencil_write_mask = rs[D3DRS_STENCILWRITEMASK];
			glStencilMask(rs[D3DRS_STENCILWRITEMASK]);
		}
	}

	state_enable(&gl_state.blend, GL_BLEND, rs[D3DRS_ALPHABLENDENABLE] != 0);
	if (rs[D3DRS_ALPHABLENDENABLE])
	{
		GLenum equation = blend_equation(rs[D3DRS_BLENDOP]);
		float blend_color[4];

		/* (a constant factor the pixel shader applies: source_blend_in_shader) */
		GLenum source = source_blend_in_shader() ? GL_ONE : (GLenum)rs[D3DRS_SRCBLEND];

		if (gl_state.blend_source != source ||
			gl_state.blend_destination != (GLenum)rs[D3DRS_DESTBLEND])
		{
			gl_state.blend_source = source;
			gl_state.blend_destination = (GLenum)rs[D3DRS_DESTBLEND];
			glBlendFunc(gl_state.blend_source, gl_state.blend_destination);
		}
		if (gl_state.blend_equation != equation)
		{
			gl_state.blend_equation = equation;
			glBlendEquation(equation);
		}
		color_to_vec4(rs[D3DRS_BLENDCOLOR], blend_color);
		if (memcmp(gl_state.blend_color, blend_color, sizeof(blend_color)))
		{
			memcpy(gl_state.blend_color, blend_color, sizeof(blend_color));
			glBlendColor(blend_color[0], blend_color[1], blend_color[2], blend_color[3]);
		}
	}
	color_mask = (unsigned char)(((write & D3DCOLORWRITEENABLE_RED) ? 1 : 0) | ((write & D3DCOLORWRITEENABLE_GREEN) ? 2 : 0) |
		((write & D3DCOLORWRITEENABLE_BLUE) ? 4 : 0) | ((write & D3DCOLORWRITEENABLE_ALPHA) ? 8 : 0));
	if (gl_state.color_mask != color_mask)
	{
		gl_state.color_mask = color_mask;
		glColorMask((color_mask & 1) != 0, (color_mask & 2) != 0, (color_mask & 4) != 0, (color_mask & 8) != 0);
	}

	/* the cull mode names the winding to discard; FRONTFACE names the
	front winding */
	state_enable(&gl_state.cull_face, GL_CULL_FACE, rs[D3DRS_CULLMODE] != D3DCULL_NONE);
	if (rs[D3DRS_CULLMODE] != D3DCULL_NONE)
	{
#ifdef HALO_GLES
		/* the vertex shader flips y in clip space, which (unlike desktop
		GL's upper-left clip origin) also flips the winding */
		GLenum front_face = rs[D3DRS_FRONTFACE] == D3DFRONT_CCW ? GL_CW : GL_CCW;
#else
		GLenum front_face = rs[D3DRS_FRONTFACE] == D3DFRONT_CCW ? GL_CCW : GL_CW;
#endif
#ifdef HALO_VR
		/* (as does the eye pass's) */
		if (vr_gl.multiview || vr_gl.left_eye || vr_gl.right_eye)
			front_face = front_face == GL_CW ? GL_CCW : GL_CW;
		/* (and a mirrored model's) */
		if (vr_gl.mirrored)
			front_face = front_face == GL_CW ? GL_CCW : GL_CW;
#endif
		GLenum cull_mode = rs[D3DRS_CULLMODE] == rs[D3DRS_FRONTFACE] ? GL_FRONT : GL_BACK;

		if (gl_state.front_face != front_face)
		{
			gl_state.front_face = front_face;
			glFrontFace(front_face);
		}
		if (gl_state.cull_mode != cull_mode)
		{
			gl_state.cull_mode = cull_mode;
			glCullFace(cull_mode);
		}
	}
#ifndef HALO_GLES
	/* ES draws filled polygons only (wireframe is a debug mode) */
	{
		GLenum polygon_mode = rs[D3DRS_FILLMODE] == D3DFILL_WIREFRAME ? GL_LINE :
			rs[D3DRS_FILLMODE] == D3DFILL_POINT ? GL_POINT : GL_FILL;

		if (gl_state.polygon_mode != polygon_mode)
		{
			gl_state.polygon_mode = polygon_mode;
			glPolygonMode(GL_FRONT_AND_BACK, polygon_mode);
		}
	}
#endif

	/* D3DRS_ZBIAS is expressed in these states (D3DDevice_SetRenderState_ZBias) */
	state_enable(&gl_state.offset_fill, GL_POLYGON_OFFSET_FILL, rs[D3DRS_SOLIDOFFSETENABLE] != 0);
#ifndef HALO_GLES
	state_enable(&gl_state.offset_line, GL_POLYGON_OFFSET_LINE, rs[D3DRS_SOLIDOFFSETENABLE] != 0);
#endif
	if (rs[D3DRS_SOLIDOFFSETENABLE])
	{
		float offset[2];

		offset[0] = dword_to_float(rs[D3DRS_POLYGONOFFSETZSLOPESCALE]);
		offset[1] = dword_to_float(rs[D3DRS_POLYGONOFFSETZOFFSET]);
		if (memcmp(gl_state.polygon_offset, offset, sizeof(offset)))
		{
			memcpy(gl_state.polygon_offset, offset, sizeof(offset));
			glPolygonOffset(offset[0], offset[1]);
		}
	}
}

#ifdef HALO_GLES
/* ES has no debug callback in 3.0; debug.gl_debug polls glGetError around
each draw instead, reporting each distinct error a few times */
static void gl_check_errors(const char *where)
{
	static int enabled = -1;
	static unsigned long reports;
	GLenum error;

	if (enabled < 0)
		enabled = config_boolean("debug.gl_debug");
	if (!enabled)
		return;
	while ((error = glGetError()) != GL_NO_ERROR)
	{
		if (reports++ < 200)
			platform_log("GL error %04x at %s (frame %lu)", (unsigned)error, where, device.frame);
	}
}
#else
#define gl_check_errors(where) ((void)0)
#endif

/* the uniforms of the latest draws, converted from these inputs; the serial
counts the conversions */
#define DRAW_UNIFORM_INPUT_COUNT (4 + 4 + 16 + 1 + 16 + 2 + 4 + 1 + 1 + 7 * D3DTSS_MAXSTAGES)

static DWORD draw_uniform_inputs[DRAW_UNIFORM_INPUT_COUNT];
static struct draw_uniforms draw_uniforms;
static unsigned long draw_uniforms_serial;

/* sets a program's uniform unless it already holds value */
static void uniform_vec4(GLint location, float *shadow, const float *value, int count)
{
	if (location < 0 || !memcmp(shadow, value, (size_t)count * 4 * sizeof(float)))
		return;
	memcpy(shadow, value, (size_t)count * 4 * sizeof(float));
	glUniform4fv(location, count, value);
}

static void uniform_float(GLint location, float *shadow, float value)
{
	if (location < 0 || !memcmp(shadow, &value, sizeof(value)))
		return;
	*shadow = value;
	glUniform1f(location, value);
}

/* Intel's graphics with Mesa's driver can hang the GPU in a long run of
draws with no pipeline flush between them, which the game's effects make
(hundreds of small draws in a row): the command streamer stops at a draw,
and the reset that follows takes the desktop's other programs with it.
Intel's workaround for a hang of this kind on their DG2 graphics
(Wa_16014538804) is a flush at least every 3 draws, which Mesa does not
apply to the others. A memory barrier is one (and only that: nothing
here writes images). */
static void draw_flush(void)
{
#ifndef HALO_GLES
	if (device.flush_every && ++device.flush_draws >= device.flush_every)
	{
		device.flush_draws = 0;
		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	}
#endif
}

/* ---------- enhanced dynamic lights

display.dynamic_lights "enhanced" (xgpu.h; the game's side is
port/linux/game/dynamic_lights.c): each frame the game hands over its
dynamic lights (halo_dynamic_lights_set), placed where the drawn objects are
(between ticks), and the structure's lightmap pass and the models lit for
each pixel add them for each pixel (nv2a_psh.c), in place of the game's own
passes over the structure for each light and its two lights for each
object's vertices. A grid about the camera says which lights reach each of
its cells (xgpu.h), so a pixel weighs only the lights near it: a frame's
lights cost what lights each pixel, not their number. A plasma bolt's light
(a projectile with no model) also has a glow drawn about it
(halo_dynamic_lights_halos_draw).
"classic", the default: none of this, the game's own lighting. Desktop
OpenGL only (layout bindings: GLSL 4.20). */
#ifndef HALO_GLES
static struct
{
	unsigned long read_at;
	BOOL enhanced;
	/* the look: [0] the lights' brightness, the core (1 / (1 + core x^2)),
	the spill past white, the models' wrap; [1] the glow's size (of the
	light's radius), brightness, softness against what it meets (world
	units), its least size (debug.dynamic_lights_look; nv2a_psh.c) */
	float look[2][4];
	/* the lights this frame (0: none: drawn as classic) */
	int count;
	/* the game's lightmap pass is being drawn (halo_dynamic_lights_environment) */
	BOOL environment;
	BOOL failed;
	GLuint block, cells;
	struct xgpu_dynamic_lights_block data;
	GLuint mask[XGPU_DYNAMIC_LIGHTS_GRID_Z][XGPU_DYNAMIC_LIGHTS_GRID_Y][XGPU_DYNAMIC_LIGHTS_GRID_X][4];
	/* the glows: each one's middle and size, its color (the display's) */
	int halo_count;
	float halos[XGPU_DYNAMIC_LIGHTS_MAXIMUM * 2][4];
	float camera_right[3], camera_up[3];
	/* the world's projection (c[-96] to c[-93]), the screen conversion (c[-38],
	c[-37]) and the viewport's, as the structure's lightmap pass began (the
	window's 3D view: later draws, and the sky before, set others) */
	float projection[6][4];
	float viewport_scale[4], viewport_offset[4];
	/* their programs: drawn into 1, 2 and 4 layers */
	GLuint halo_programs[3];
	BOOL halo_failed[3];
	GLuint depth_sampler;
} dynamic_lights;

static void dynamic_lights_read_settings(void)
{
	static const float defaults[2][4] = { { 1.7f, 12.0f, 0.18f, 0.25f }, { 0.22f, 1.2f, 0.12f, 0.035f } };
	const char *look;

	if (dynamic_lights.read_at == config_changes() && dynamic_lights.read_at)
		return;
	dynamic_lights.read_at = config_changes();
	dynamic_lights.enhanced = !strcmp(config_string("display.dynamic_lights"), "enhanced");
	memcpy(dynamic_lights.look, defaults, sizeof(defaults));
	look = config_string("debug.dynamic_lights_look");
	if (look && *look)
	{
		float *values = &dynamic_lights.look[0][0];
		int index;

		for (index = 0; index < 8 && *look; index++)
		{
			char *end;
			float value = (float)strtod(look, &end);

			if (end == look)
				break;
			values[index] = value;
			look = end;
		}
	}
}

/* whether the game is to light with the enhanced dynamic lights (it then
hands them over, and leaves the structure to them) */
int halo_dynamic_lights_enhanced(void)
{
	dynamic_lights_read_settings();
	return dynamic_lights.enhanced && !dynamic_lights.failed && device.gl_ready;
}

/* a grid cell's index along an axis for an offset from the middle (xgpu.h) */
static int dynamic_lights_cell(float offset, float scale, int count)
{
	float cell = log2f(1.0f + fabsf(offset) * (1.0f / XGPU_DYNAMIC_LIGHTS_CELL)) * scale;
	int index = (int)floorf(offset < 0.0f ? (float)(count / 2) - cell : (float)(count / 2) + cell);

	return index < 0 ? 0 : index >= count ? count - 1 : index;
}

/* the frame's lights (the game's dynamic_lights.c): where they are, as the
objects are drawn; the camera's position and axes (the glows face it) */
void halo_dynamic_lights_set(const struct halo_dynamic_light *lights, int count, const float camera[3],
	const float right[3], const float up[3])
{
	static const int sizes[3] = { XGPU_DYNAMIC_LIGHTS_GRID_X, XGPU_DYNAMIC_LIGHTS_GRID_Y, XGPU_DYNAMIC_LIGHTS_GRID_Z };
	float scales[3];
	int index, axis;

	dynamic_lights.count = 0;
	dynamic_lights.halo_count = 0;
	if (!halo_dynamic_lights_enhanced())
		return;
	if (count > XGPU_DYNAMIC_LIGHTS_MAXIMUM)
		count = XGPU_DYNAMIC_LIGHTS_MAXIMUM;
	if (!dynamic_lights.block)
	{
		glGenBuffers(1, &dynamic_lights.block);
		glBindBuffer(GL_UNIFORM_BUFFER, dynamic_lights.block);
		glBufferData(GL_UNIFORM_BUFFER, sizeof(dynamic_lights.data), NULL, GL_DYNAMIC_DRAW);
		glGenTextures(1, &dynamic_lights.cells);
		glActiveTexture(GL_TEXTURE0 + XGPU_DYNAMIC_LIGHTS_UNIT);
		glBindTexture(GL_TEXTURE_3D, dynamic_lights.cells);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 0);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA32UI, XGPU_DYNAMIC_LIGHTS_GRID_X, XGPU_DYNAMIC_LIGHTS_GRID_Y,
			XGPU_DYNAMIC_LIGHTS_GRID_Z, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, NULL);
		glBindBufferBase(GL_UNIFORM_BUFFER, XGPU_DYNAMIC_LIGHTS_BINDING, dynamic_lights.block);
		xgpu_gl_state_invalidate();
		if (glGetError() != GL_NO_ERROR)
		{
			platform_log("GPU: the enhanced dynamic lights' buffers could not be made: classic lights");
			dynamic_lights.failed = TRUE;
			return;
		}
	}
	for (axis = 0; axis < 3; axis++)
	{
		scales[axis] = (float)(sizes[axis] / 2) / log2f(1.0f + XGPU_DYNAMIC_LIGHTS_REACH / XGPU_DYNAMIC_LIGHTS_CELL);
		dynamic_lights.data.grid[0][axis] = camera[axis];
		dynamic_lights.data.grid[1][axis] = scales[axis];
		dynamic_lights.camera_right[axis] = right[axis];
		dynamic_lights.camera_up[axis] = up[axis];
	}
	dynamic_lights.data.grid[0][3] = (float)count;
	dynamic_lights.data.grid[1][3] = 1.0f / XGPU_DYNAMIC_LIGHTS_CELL;
	memcpy(dynamic_lights.data.grid[2], dynamic_lights.look[0], sizeof(dynamic_lights.look[0]));
	memset(dynamic_lights.mask, 0, sizeof(dynamic_lights.mask));
	for (index = 0; index < count; index++)
	{
		const struct halo_dynamic_light *light = &lights[index];
		int first[3], last[3], x, y, z;
		float brightness = dynamic_lights.look[0][0];

		if (!(light->radius > 0.0f))
			continue;
		dynamic_lights.data.position[index][0] = light->position[0];
		dynamic_lights.data.position[index][1] = light->position[1];
		dynamic_lights.data.position[index][2] = light->position[2];
		dynamic_lights.data.position[index][3] = (light->flags & HALO_DYNAMIC_LIGHT_MODELS_ONLY ? -1.0f : 1.0f) /
			light->radius;
		for (axis = 0; axis < 3; axis++)
		{
			/* (linear: the display's gamma out) */
			dynamic_lights.data.color[index][axis] = powf(light->color[axis], 2.2f) * brightness;
			dynamic_lights.data.cone[index][axis] = light->axis[axis];
			first[axis] = dynamic_lights_cell(light->position[axis] - light->radius - camera[axis], scales[axis],
				sizes[axis]);
			last[axis] = dynamic_lights_cell(light->position[axis] + light->radius - camera[axis], scales[axis],
				sizes[axis]);
		}
		dynamic_lights.data.color[index][3] = light->cone_offset;
		dynamic_lights.data.cone[index][3] = light->cone_scale;
		for (z = first[2]; z <= last[2]; z++)
			for (y = first[1]; y <= last[1]; y++)
				for (x = first[0]; x <= last[0]; x++)
					dynamic_lights.mask[z][y][x][index >> 5] |= 1u << (index & 31);
		if ((light->flags & HALO_DYNAMIC_LIGHT_HALO) && dynamic_lights.halo_count < XGPU_DYNAMIC_LIGHTS_MAXIMUM)
		{
			float *halo = dynamic_lights.halos[dynamic_lights.halo_count * 2];
			float size = light->radius * dynamic_lights.look[1][0];

			halo[0] = light->position[0];
			halo[1] = light->position[1];
			halo[2] = light->position[2];
			halo[3] = size > dynamic_lights.look[1][3] ? size : dynamic_lights.look[1][3];
			halo[4] = light->color[0] * dynamic_lights.look[1][1];
			halo[5] = light->color[1] * dynamic_lights.look[1][1];
			halo[6] = light->color[2] * dynamic_lights.look[1][1];
			halo[7] = 0.0f;
			dynamic_lights.halo_count++;
		}
	}
	if (!count)
		return;
	glBindBuffer(GL_UNIFORM_BUFFER, dynamic_lights.block);
	glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(dynamic_lights.data), &dynamic_lights.data);
	glBindBufferBase(GL_UNIFORM_BUFFER, XGPU_DYNAMIC_LIGHTS_BINDING, dynamic_lights.block);
	glActiveTexture(GL_TEXTURE0 + XGPU_DYNAMIC_LIGHTS_UNIT);
	glBindTexture(GL_TEXTURE_3D, dynamic_lights.cells);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, XGPU_DYNAMIC_LIGHTS_GRID_X, XGPU_DYNAMIC_LIGHTS_GRID_Y,
		XGPU_DYNAMIC_LIGHTS_GRID_Z, GL_RGBA_INTEGER, GL_UNSIGNED_INT, dynamic_lights.mask);
	glActiveTexture(GL_TEXTURE0);
	xgpu_gl_state_invalidate();
	dynamic_lights.count = count;
}

/* the game draws the structure's lightmap pass (rasterizer_xbox_environment.c);
as it begins, the world's projection is the 3D view's (the glows' then) */
void halo_dynamic_lights_environment(int drawing)
{
	dynamic_lights.environment = drawing != 0;
	if (drawing)
	{
		memcpy(dynamic_lights.projection[0], device.constants[0], 4 * sizeof(dynamic_lights.projection[0]));
		memcpy(dynamic_lights.projection[4], device.constants[XGPU_VERTEX_CONSTANT_BIAS - 38], sizeof(dynamic_lights.projection[0]));
		memcpy(dynamic_lights.projection[5], device.constants[XGPU_VERTEX_CONSTANT_BIAS - 37], sizeof(dynamic_lights.projection[0]));
		memcpy(dynamic_lights.viewport_scale, device.viewport_scale, sizeof(dynamic_lights.viewport_scale));
		memcpy(dynamic_lights.viewport_offset, device.viewport_offset, sizeof(dynamic_lights.viewport_offset));
	}
}

/* the frame's draws are lit by the enhanced dynamic lights */
static BOOL dynamic_lights_active(void)
{
	return dynamic_lights.count > 0 && dynamic_lights.enhanced;
}

/* a glow's program, drawn into this many layers (1: the screen's target, 2
the VR mode's eyes, 4 its eyes foveated): the camera-facing quad of each
light about its middle, projected as the game's vertex programs project
(nv2a_vsh.c's conversion, each eye's correction, the inset's view), its
light round and soft, fading out as it nears what is behind it and gone
behind what is before it, against the depth the scene was drawn with
(sampled: the target is drawn into without its depth) */
static GLuint dynamic_lights_halo_program(int layers)
{
	struct xgpu_text vertex = { 0 }, fragment = { 0 };
	int slot = layers == 1 ? 0 : layers == 2 ? 1 : 2;
	GLuint vertex_shader, fragment_shader;

	if (dynamic_lights.halo_programs[slot] || dynamic_lights.halo_failed[slot])
		return dynamic_lights.halo_programs[slot];
	dynamic_lights.halo_failed[slot] = TRUE;
	xgpu_text_append(&vertex, "#version 450 core\n");
	xgpu_text_append(&fragment, "#version 450 core\n");
	if (layers > 1)
	{
		xgpu_text_append(&vertex, "#extension GL_OVR_multiview2 : require\nlayout(num_views = %d) in;\n"
			"#define VIEW int(gl_ViewID_OVR)\n#define EYE (VIEW%s)\nuniform mat4 eye_correction[2];\n%s",
			layers, layers == 4 ? " >> 1" : "", layers == 4 ? "uniform vec4 fovea_view[4];\n" : "");
		xgpu_text_append(&fragment, "#extension GL_OVR_multiview2 : require\n"
			"layout(binding = 0) uniform highp sampler2DArray scene_depth;\n");
	}
	else
	{
		xgpu_text_append(&fragment, "layout(binding = 0) uniform highp sampler2D scene_depth;\n");
	}
	xgpu_text_append(&vertex,
		"uniform vec4 c[6];\n"
		"uniform vec4 viewport_scale;\n"
		"uniform vec4 viewport_offset;\n"
		"uniform float screen_offset;\n"
		"uniform vec4 halos[%d];\n"
		"uniform vec4 camera_axes[2];\n"
		"out vec2 corner;\n"
		"out vec3 color;\n"
		"out float depth;\n"
		"const vec2 corners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0),\n"
		"\tvec2(1.0, 1.0), vec2(-1.0, 1.0));\n"
		"void main()\n"
		"{\n"
		"\tint halo = gl_VertexID / 6;\n"
		"\tvec4 middle = halos[halo * 2];\n"
		"\tcorner = corners[gl_VertexID - halo * 6];\n"
		"\tcolor = halos[halo * 2 + 1].rgb;\n"
		"\tvec3 world = middle.xyz + (camera_axes[0].xyz * corner.x + camera_axes[1].xyz * corner.y) * middle.w;\n"
		"\tvec4 clip = vec4(dot(world, c[0].xyz) + c[0].w, dot(world, c[1].xyz) + c[1].w,\n"
		"\t\tdot(world, c[2].xyz) + c[2].w, dot(world, c[3].xyz) + c[3].w);\n"
		"\tdepth = clip.w;\n"
		"%s"
		"\tvec3 scale = vec3(viewport_scale.x != 0.0 ? viewport_scale.x : 1.0,\n"
		"\t\tviewport_scale.y != 0.0 ? viewport_scale.y : 1.0, viewport_scale.z != 0.0 ? viewport_scale.z : 1.0);\n"
		"\tgl_Position = vec4((clip.xyz * c[4].xyz + (c[5].xyz + vec3(0.5 + screen_offset, 0.5, 0.0) -\n"
		"\t\tviewport_offset.xyz) * clip.w) / scale, clip.w);\n"
		"%s%s"
		"}\n",
		XGPU_DYNAMIC_LIGHTS_MAXIMUM * 2,
		layers > 1 ? "\tclip = eye_correction[EYE] * clip;\n" : "",
		layers > 1 ? "\tgl_Position.y = -gl_Position.y;\n" : "",
		layers == 4 ? "\tgl_Position.xy = gl_Position.xy * fovea_view[VIEW].xy + fovea_view[VIEW].zw * gl_Position.w;\n" :
			"");
	xgpu_text_append(&fragment,
		/* the depth buffer's value for a distance from the camera: alpha +
		beta / distance (beta less than 0: nearer, less); and the softness,
		world units */
		"uniform vec4 depth_parameters;\n"
		"in vec2 corner;\n"
		"in vec3 color;\n"
		"in float depth;\n"
		"layout(location = 0) out vec4 fragment_color;\n"
		"void main()\n"
		"{\n"
		"\tfloat r2 = dot(corner, corner);\n"
		"\tif (r2 >= 1.0)\n"
		"\t\tdiscard;\n"
		"\tfloat scene = texelFetch(scene_depth, %s, 0).r;\n"
		"\tfloat away = scene - depth_parameters.x;\n"
		"\tfloat distance = away * depth_parameters.y > 0.0 ? depth_parameters.y / away : 1.0e9;\n"
		"\tfloat fade = scene >= 1.0 ? 1.0 : clamp((distance - depth) / depth_parameters.z, 0.0, 1.0);\n"
		/* a wide soft glow, and a small hot core */
		"\tfloat glow = (exp(-3.0 * r2) - 0.0498) * 0.6 + exp(-24.0 * r2) * 0.8;\n"
		"\tfragment_color = vec4(color * (glow * fade * fade), 0.0);\n"
		"}\n",
		layers > 1 ? "ivec3(ivec2(gl_FragCoord.xy), int(gl_ViewID_OVR))" : "ivec2(gl_FragCoord.xy)");
	vertex_shader = xgpu_compile_shader(GL_VERTEX_SHADER, vertex.buffer, "dynamic light glow vertex");
	fragment_shader = xgpu_compile_shader(GL_FRAGMENT_SHADER, fragment.buffer, "dynamic light glow pixel");
	free(vertex.buffer);
	free(fragment.buffer);
	if (vertex_shader && fragment_shader)
		dynamic_lights.halo_programs[slot] = xgpu_link_program(vertex_shader, fragment_shader, "dynamic light glow");
	if (dynamic_lights.halo_programs[slot])
		dynamic_lights.halo_failed[slot] = FALSE;
	return dynamic_lights.halo_programs[slot];
}

/* the glows of the frame's bolts' lights, into the 3D view, after its
transparent geometry (source/render/render.c): added to what is there */
void halo_dynamic_lights_halos_draw(void)
{
	struct render_target_entry *color, *depth;
	GLuint program, framebuffer;
	int layers;
	float c[6][4], depth_parameters[4];

	if (!dynamic_lights_active() || !dynamic_lights.halo_count || !device.gl_ready)
		return;
#ifdef HALO_VR
	vr_frame_resume();
	/* (the eyes' pass, or the flat screen's; not the HUD's, nor a scope's) */
	if (vr_gl.pass != HALO_VR_PASS_NONE && vr_gl.pass != HALO_VR_PASS_EYES)
		return;
#endif
	color = render_target_get(device.render_target);
	depth = render_target_get(device.depth_stencil);
	if (!color || !depth || !depth->target.depth || color->target.layers != depth->target.layers)
		return;
	layers = (int)color->target.layers;
	program = dynamic_lights_halo_program(layers);
	if (!program)
		return;
	/* the depth the scene was drawn with, in its texture */
	if (depth->target.samples)
		render_target_resolve(&depth->target);
#ifdef HALO_VR
	if (layers > 1)
		vr_fovea_views();
	framebuffer_views = layers;
	framebuffer_base = 0;
#endif
	if (color->target.samples)
	{
		framebuffer = framebuffer_find(color->target.multisample, 0, TRUE);
		color->target.unresolved = TRUE;
	}
	else
	{
		framebuffer = framebuffer_get(color->target.texture, 0);
	}
#ifdef HALO_VR
	framebuffer_views = 1;
#endif
	/* the world's projection and the screen conversion, as the lights were
	set; the depth for a distance, from two points ahead */
	memcpy(c, dynamic_lights.projection, sizeof(c));
	{
		float forward[3], z[2], w[2], a, b, scale_z, near_z, far_z;
		int point, axis;

		/* (the camera's forward: the projection's w row) */
		for (axis = 0; axis < 3; axis++)
			forward[axis] = c[3][axis];
		for (point = 0; point < 2; point++)
		{
			float t = point ? 64.0f : 1.0f;
			float length = sqrtf(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
			float p[3];

			for (axis = 0; axis < 3; axis++)
				p[axis] = dynamic_lights.data.grid[0][axis] + forward[axis] / (length > 0.0f ? length : 1.0f) * t;
			z[point] = c[2][0] * p[0] + c[2][1] * p[1] + c[2][2] * p[2] + c[2][3];
			w[point] = c[3][0] * p[0] + c[3][1] * p[1] + c[3][2] * p[2] + c[3][3];
		}
		b = (z[0] / w[0] - z[1] / w[1]) / (1.0f / w[0] - 1.0f / w[1]);
		a = z[0] / w[0] - b / w[0];
		scale_z = dynamic_lights.viewport_scale[2] != 0.0f ? dynamic_lights.viewport_scale[2] : 1.0f;
		near_z = gl_state.depth_range[0] >= 0.0f && gl_state.depth_range[0] <= 1.0f ? gl_state.depth_range[0] : 0.0f;
		far_z = gl_state.depth_range[1] >= 0.0f && gl_state.depth_range[1] <= 1.0f ? gl_state.depth_range[1] : 1.0f;
		depth_parameters[0] = near_z + (far_z - near_z) * (a * c[4][2] + c[5][2] - dynamic_lights.viewport_offset[2]) /
			scale_z;
		depth_parameters[1] = (far_z - near_z) * b * c[4][2] / scale_z;
		depth_parameters[2] = dynamic_lights.look[1][2];
		depth_parameters[3] = 0.0f;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glUseProgram(program);
	glUniform4fv(glGetUniformLocation(program, "c"), 6, c[0]);
	glUniform4fv(glGetUniformLocation(program, "viewport_scale"), 1, dynamic_lights.viewport_scale);
	glUniform4fv(glGetUniformLocation(program, "viewport_offset"), 1, dynamic_lights.viewport_offset);
	glUniform1f(glGetUniformLocation(program, "screen_offset"), (float)UI_OFFSET);
	glUniform4fv(glGetUniformLocation(program, "halos"), dynamic_lights.halo_count * 2, dynamic_lights.halos[0]);
	{
		float axes[2][4];

		memcpy(axes[0], dynamic_lights.camera_right, sizeof(dynamic_lights.camera_right));
		memcpy(axes[1], dynamic_lights.camera_up, sizeof(dynamic_lights.camera_up));
		axes[0][3] = axes[1][3] = 0.0f;
		glUniform4fv(glGetUniformLocation(program, "camera_axes"), 2, axes[0]);
	}
	glUniform4fv(glGetUniformLocation(program, "depth_parameters"), 1, depth_parameters);
#ifdef HALO_VR
	if (layers > 1)
	{
		glUniformMatrix4fv(glGetUniformLocation(program, "eye_correction"), 2, GL_TRUE, &vr_gl.eye_transforms[0][0]);
		if (layers == 4)
			glUniform4fv(glGetUniformLocation(program, "fovea_view"), 4, &vr_gl.fovea_view[0][0]);
	}
#endif
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D, depth->target.texture);
	/* (its own sampler: no comparison, as a shadow map's has, nor filter) */
	if (!dynamic_lights.depth_sampler)
	{
		glGenSamplers(1, &dynamic_lights.depth_sampler);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_COMPARE_MODE, GL_NONE);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	glBindSampler(0, dynamic_lights.depth_sampler);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glDepthMask(GL_FALSE);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_ONE, GL_ONE);
	glBindVertexArray(device.vertex_array);
	glDrawArrays(GL_TRIANGLES, 0, dynamic_lights.halo_count * 6);
	xgpu_gl_state_invalidate();
}
#else
int halo_dynamic_lights_enhanced(void)
{
	return FALSE;
}

void halo_dynamic_lights_set(const struct halo_dynamic_light *lights, int count, const float camera[3],
	const float right[3], const float up[3])
{
	(void)lights;
	(void)count;
	(void)camera;
	(void)right;
	(void)up;
}

void halo_dynamic_lights_environment(int drawing)
{
	(void)drawing;
}

void halo_dynamic_lights_halos_draw(void)
{
}
#endif

/* display.per_pixel_lighting: the model lighting programs' draws are lit for
each pixel (nv2a_psh.c model_lighting), from shaders of their own; read
again when a setting changes. The enhanced dynamic lights light models for
each pixel too, whatever it says. */
static BOOL per_pixel_lighting(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static BOOL enabled;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		enabled = config_boolean("display.per_pixel_lighting") != 0;
	}
	return enabled || halo_dynamic_lights_enhanced();
}

/* the vertex constant register of each of the per-pixel lighting's
(XGPU_MODEL_LIGHT_COUNT) */
static unsigned long model_light_register(int light)
{
	return (unsigned long)(XGPU_VERTEX_CONSTANT_BIAS + (light ? -80 + light : -82));
}

static struct program_entry *prepare_draw(BOOL immediate)
{
	struct vertex_shader_object *program = current_program();
	struct nv2a_pixel_shader_key key;
	struct program_entry *entry;
	struct draw_uniforms uniforms;
	BOOL has_depth = FALSE;
	int stage;

	if (!device.gl_ready || !program || !device.vertex_shader || !program->instructions)
	{
		stats.skipped_no_program++;
		return NULL;
	}
	{
		const char *skip = debug_settings.skip_vertex_shaders;

		while (skip && *skip)
		{
			if ((unsigned long)atol(skip) == program->id)
				return NULL;
			skip = strchr(skip, ',');
			if (skip)
				skip++;
		}
	}
	memset(&key, 0, sizeof(key));
	memcpy(key.combiner_state, D3D__RenderState, sizeof(key.combiner_state));
	/* constants are uniforms, not part of the program */
	memset(&key.combiner_state[D3DRS_PSCONSTANT0_0], 0, 16 * sizeof(DWORD));
	key.combiner_state[D3DRS_PSFINALCOMBINERCONSTANT0] = 0;
	key.combiner_state[D3DRS_PSFINALCOMBINERCONSTANT1] = 0;
	key.texture_modes = D3D__RenderState[D3DRS_PSTEXTUREMODES];
	/* the textures before the targets: a render target the draw samples
	has its multisampled pixels resolved by a blit (render_target_resolve),
	which binds framebuffers of its own, and the back buffer can be both
	sampled and drawn into */
	bind_textures(&key, uniforms.texture_scale);
	if (!bind_targets(&has_depth))
	{
		stats.skipped_no_target++;
		return NULL;
	}
	apply_raster_state(has_depth);
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		key.alpha_kill[stage] = D3D__TextureState[stage][D3DTSS_ALPHAKILL] == D3DTALPHAKILL_ENABLE;
		key.color_sign[stage] = (unsigned char)((D3D__TextureState[stage][D3DTSS_COLORSIGN] >> 28) & 0xf);
	}
	/* (only with the meter's blend: hud_hires.h, nv2a_pixel_shader_key) */
	key.coverage_alpha = key.coverage_alpha && D3D__RenderState[D3DRS_ALPHABLENDENABLE] &&
		D3D__RenderState[D3DRS_SRCBLEND] == D3DBLEND_CONSTANTCOLOR &&
		D3D__RenderState[D3DRS_DESTBLEND] == D3DBLEND_SRCALPHA;
	key.point_threshold = key.point_threshold && key.coverage_alpha;
	key.alpha_test_function = D3D__RenderState[D3DRS_ALPHATESTENABLE] ? D3D__RenderState[D3DRS_ALPHAFUNC] : 0;
#ifndef HALO_GLES
	/* (gl_SampleMask: ES has it only from 3.2) */
	if (target_samples > 1 && key.alpha_test_function && !D3D__RenderState[D3DRS_ALPHABLENDENABLE])
		key.alpha_test_samples = (unsigned char)target_samples;
#endif
	key.fog_enable = D3D__RenderState[D3DRS_FOGENABLE] != 0;
	key.source_blend = source_blend_in_shader();
	key.fog_table_mode = (unsigned char)D3D__RenderState[D3DRS_FOGTABLEMODE];
#ifdef XGPU_SAMPLE_COUNTERS
	key.count_samples = device.visibility_test_active && SAMPLE_COUNTING;
#endif
#ifdef HALO_VR
	key.multiview = vr_gl.multiview ? XGPU_MULTIVIEW_EYES : vr_gl.left_eye ? XGPU_MULTIVIEW_LEFT_EYE :
		vr_gl.right_eye ? XGPU_MULTIVIEW_RIGHT_EYE : XGPU_MULTIVIEW_NONE;
#endif
	entry = NULL;
	if (program->lighting.lights && !program->lighting_failed && per_pixel_lighting())
	{
		key.per_pixel_lighting = (unsigned char)program->lighting.lights;
#ifndef HALO_GLES
		/* (the enhanced dynamic lights in the two point lights' place) */
		if (dynamic_lights_active() && program->lighting.world_found)
			key.dynamic_lights = XGPU_DYNAMIC_LIGHTS_MODEL;
#endif
		entry = program_get(vertex_shader_get(program, immediate, &program->lighting, key.multiview),
			fragment_shader_get(&key));
		if (!entry)
		{
			/* drawn as the vertex shader lights it instead (not at all,
			were it to fail too) */
			platform_log("GPU: vertex shader %lu cannot be lit for each pixel here (refer to the shader log above): "
				"lit for each vertex", program->id);
			program->lighting_failed = TRUE;
			key.per_pixel_lighting = 0;
			key.dynamic_lights = 0;
		}
	}
#ifndef HALO_GLES
	else if (dynamic_lights.environment && dynamic_lights_active() && !immediate && !program->lighting_failed &&
		(device.vertex_shader->packed_mask & 0xe) == 0xe)
	{
		/* a structure's lightmap pass: its normal, binormal and tangent are
		packed (rasterizer_xbox_vertex_shaders_initialize.c's declaration) */
		static const struct nv2a_vertex_lighting environment = { XGPU_ENVIRONMENT_LIGHTING };

		key.dynamic_lights = XGPU_DYNAMIC_LIGHTS_ENVIRONMENT;
		entry = program_get(vertex_shader_get(program, immediate, &environment, key.multiview),
			fragment_shader_get(&key));
		if (!entry)
		{
			platform_log("GPU: vertex shader %lu cannot be lit by the dynamic lights (refer to the shader log "
				"above): lit by the game's own passes", program->id);
			program->lighting_failed = TRUE;
			key.dynamic_lights = 0;
		}
	}
#endif
	if (!entry)
		entry = program_get(vertex_shader_get(program, immediate, NULL, key.multiview), fragment_shader_get(&key));
#ifdef HALO_VR
	if (vr_gl.multiview && vr_gl.statistics && !program->projects)
	{
		/* debug.gpu_stats: the eye pass's draws in screen space, the same in
		both eyes (each program once, and the game's function that drew) */
		static unsigned long reported[64];
		static int reported_count;
		int index;

		for (index = 0; index < reported_count && reported[index] != program->id; index++)
			;
		if (index == reported_count && reported_count < 64)
		{
			reported[reported_count++] = program->id;
			platform_log("vr: screen-space draw in the eye pass: vertex shader %lu, drawn from %p", program->id,
				__builtin_return_address(1));
		}
	}
#endif
	if (!entry)
	{
		stats.skipped_link++;
		gl_check_errors("program");
		return NULL;
	}
	gl_check_errors("state");
	if (immediate)
		stats.immediate_draws++;
	else
		stats.draws++;
#ifdef HALO_WEB
	device.draw_serial++;
#endif
	draw_flush();
	state_program(entry->program);
	if (key.source_blend && entry->blend_constant >= 0)
	{
		float blend_constant[4];

		color_to_vec4(D3D__RenderState[D3DRS_BLENDCOLOR], blend_constant);
		if (memcmp(entry->blend_constant_value, blend_constant, sizeof(blend_constant)))
		{
			memcpy(entry->blend_constant_value, blend_constant, sizeof(blend_constant));
			glUniform4fv(entry->blend_constant, 1, blend_constant);
		}
	}
#ifdef XGPU_SAMPLE_COUNTERS
	if (key.count_samples)
		glBindBufferRange(GL_ATOMIC_COUNTER_BUFFER, 0, device.visibility_counters,
			(GLintptr)(device.counter_active * sizeof(GLuint)), sizeof(GLuint));
#endif
#ifdef HALO_VR
	if ((entry->eye_correction >= 0 || entry->screen_correction >= 0) && entry->eye_serial != vr_gl.eye_serial)
	{
		/* (a program that never projects has no eye_correction) */
		if (entry->eye_correction >= 0)
			glUniformMatrix4fv(entry->eye_correction, 2, GL_TRUE, &vr_gl.eye_transforms[0][0]);
		if (entry->screen_correction >= 0)
			glUniform4fv(entry->screen_correction, 2, &vr_gl.screen_corrections[0][0]);
		if (entry->screen_points >= 0)
			glUniform1f(entry->screen_points, vr_gl.screen_points ? 1.0f : 0.0f);
		entry->eye_serial = vr_gl.eye_serial;
	}
	if (entry->fovea_view >= 0 || entry->fovea_sample >= 0)
	{
		vr_fovea_views();
		if (entry->fovea_serial != vr_gl.fovea_serial)
		{
			if (entry->fovea_view >= 0)
				glUniform4fv(entry->fovea_view, 4, &vr_gl.fovea_view[0][0]);
			if (entry->fovea_sample >= 0)
				glUniform4fv(entry->fovea_sample, 4, &vr_gl.fovea_sample[0][0]);
			entry->fovea_serial = vr_gl.fovea_serial;
		}
	}
#endif

	if (entry->constants >= 0 && entry->constants_serial != constants_serial)
	{
		unsigned long first = entry->constant_count, last = 0, index;

		if (entry->constants_serial == constants_checkpoint_serial &&
			constants_checkpoint_last < entry->constant_count)
		{
			if (constants_checkpoint_first <= constants_checkpoint_last)
			{
				first = constants_checkpoint_first;
				last = constants_checkpoint_last;
			}
		}
		else if (constants_serial - entry->constants_serial <= XGPU_VERTEX_CONSTANT_COUNT)
		{
			unsigned long long serial;

			for (serial = entry->constants_serial + 1; serial <= constants_serial; serial++)
			{
				index = constant_log[serial % CONSTANT_LOG_SIZE];
				if (index >= entry->constant_count)
					continue;
				if (first > index)
					first = index;
				if (last < index)
					last = index;
			}
		}
		else
		{
			for (index = 0; index < entry->constant_count; index++)
			{
				if (constant_serials[index] > entry->constants_serial)
				{
					if (first > index)
						first = index;
					last = index;
				}
			}
		}
		if (first < entry->constant_count)
		{
			if (entry->constants_consecutive)
				glUniform4fv(entry->constants + (GLint)first, (GLsizei)(last - first + 1), device.constants[first]);
			else
				glUniform4fv(entry->constants, XGPU_VERTEX_CONSTANT_COUNT, &device.constants[0][0]);
		}
		entry->constants_serial = constants_serial;
		constants_checkpoint_serial = constants_serial;
		constants_checkpoint_first = XGPU_VERTEX_CONSTANT_COUNT;
		constants_checkpoint_last = 0;
	}
	/* the lights of a draw lit for each pixel, from the same registers, when
	any of them changed since the program last had them */
	if (entry->model_lights >= 0 && entry->model_lights_serial != constants_serial)
	{
		int light;

		for (light = 0; light < XGPU_MODEL_LIGHT_COUNT; light++)
		{
			if (constant_serials[model_light_register(light)] > entry->model_lights_serial)
				break;
		}
		if (light < XGPU_MODEL_LIGHT_COUNT)
		{
			float lights[XGPU_MODEL_LIGHT_COUNT][4];

			for (light = 0; light < XGPU_MODEL_LIGHT_COUNT; light++)
				memcpy(lights[light], device.constants[model_light_register(light)], sizeof(lights[0]));
			glUniform4fv(entry->model_lights, XGPU_MODEL_LIGHT_COUNT, lights[0]);
		}
		entry->model_lights_serial = constants_serial;
	}

	/* the state the other uniforms come from: most draws share it with the
	draw before them, and so share its uniforms */
	{
		DWORD inputs[DRAW_UNIFORM_INPUT_COUNT];
		unsigned long count = 0;

		memcpy(&inputs[count], device.viewport_scale, sizeof(device.viewport_scale));
		count += 4;
		memcpy(&inputs[count], device.viewport_offset, sizeof(device.viewport_offset));
		count += 4;
		memcpy(&inputs[count], uniforms.texture_scale, sizeof(uniforms.texture_scale));
		count += 16;
		inputs[count++] = D3D__RenderState[D3DRS_POINTSIZE];
		for (stage = 0; stage < 8; stage++)
		{
			inputs[count++] = D3D__RenderState[D3DRS_PSCONSTANT0_0 + stage];
			inputs[count++] = D3D__RenderState[D3DRS_PSCONSTANT1_0 + stage];
		}
		inputs[count++] = D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0];
		inputs[count++] = D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1];
		inputs[count++] = D3D__RenderState[D3DRS_FOGCOLOR];
		inputs[count++] = D3D__RenderState[D3DRS_FOGSTART];
		inputs[count++] = D3D__RenderState[D3DRS_FOGEND];
		inputs[count++] = D3D__RenderState[D3DRS_FOGDENSITY];
		inputs[count++] = D3D__RenderState[D3DRS_ALPHAREF];
		inputs[count++] = (DWORD)UI_OFFSET;
		for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
		{
			DWORD *state = D3D__TextureState[stage];

			inputs[count++] = state[D3DTSS_BUMPENVMAT00];
			inputs[count++] = state[D3DTSS_BUMPENVMAT01];
			inputs[count++] = state[D3DTSS_BUMPENVMAT10];
			inputs[count++] = state[D3DTSS_BUMPENVMAT11];
			inputs[count++] = state[D3DTSS_BUMPENVLSCALE];
			inputs[count++] = state[D3DTSS_BUMPENVLOFFSET];
			inputs[count++] = state[D3DTSS_MIPMAPLODBIAS];
		}
		if (!draw_uniforms_serial || !words_equal(inputs, draw_uniform_inputs, sizeof(inputs)))
		{
			struct draw_uniforms *converted = &draw_uniforms;

			memcpy(draw_uniform_inputs, inputs, sizeof(inputs));
			draw_uniforms_serial++;
			memcpy(converted->viewport_scale, device.viewport_scale, sizeof(converted->viewport_scale));
			memcpy(converted->viewport_offset, device.viewport_offset, sizeof(converted->viewport_offset));
			memcpy(converted->texture_scale, uniforms.texture_scale, sizeof(converted->texture_scale));
			converted->point_size = D3D__RenderState[D3DRS_POINTSIZE] ?
				dword_to_float(D3D__RenderState[D3DRS_POINTSIZE]) : 1.0f;
			for (stage = 0; stage < 8; stage++)
			{
				color_to_vec4(D3D__RenderState[D3DRS_PSCONSTANT0_0 + stage], converted->ps_c0[stage]);
				color_to_vec4(D3D__RenderState[D3DRS_PSCONSTANT1_0 + stage], converted->ps_c1[stage]);
			}
			color_to_vec4(D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0], converted->ps_final_c0);
			color_to_vec4(D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1], converted->ps_final_c1);
			color_to_vec4(D3D__RenderState[D3DRS_FOGCOLOR], converted->fog_color);
			converted->fog_parameters[0] = dword_to_float(D3D__RenderState[D3DRS_FOGSTART]);
			converted->fog_parameters[1] = dword_to_float(D3D__RenderState[D3DRS_FOGEND]);
			converted->fog_parameters[2] = dword_to_float(D3D__RenderState[D3DRS_FOGDENSITY]);
			converted->fog_parameters[3] = 0.0f;
			converted->alpha_reference = (float)(D3D__RenderState[D3DRS_ALPHAREF] & 0xff);
			for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
			{
				DWORD *state = D3D__TextureState[stage];

				converted->bump_matrix[stage][0] = dword_to_float(state[D3DTSS_BUMPENVMAT00]);
				converted->bump_matrix[stage][1] = dword_to_float(state[D3DTSS_BUMPENVMAT01]);
				converted->bump_matrix[stage][2] = dword_to_float(state[D3DTSS_BUMPENVMAT10]);
				converted->bump_matrix[stage][3] = dword_to_float(state[D3DTSS_BUMPENVMAT11]);
				converted->bump_luminance[stage][0] = dword_to_float(state[D3DTSS_BUMPENVLSCALE]);
				converted->bump_luminance[stage][1] = dword_to_float(state[D3DTSS_BUMPENVLOFFSET]);
				converted->bump_luminance[stage][2] = converted->bump_luminance[stage][3] = 0.0f;
				converted->texture_lod_bias[stage] = dword_to_float(state[D3DTSS_MIPMAPLODBIAS]);
			}
			converted->screen_offset = (float)UI_OFFSET;
		}
	}
	/* and a program that has had them since needs none of them */
	if (entry->uniforms_serial == draw_uniforms_serial)
		return entry;
	entry->uniforms_serial = draw_uniforms_serial;
	uniform_vec4(entry->viewport_scale, entry->uniforms.viewport_scale, draw_uniforms.viewport_scale, 1);
	uniform_vec4(entry->viewport_offset, entry->uniforms.viewport_offset, draw_uniforms.viewport_offset, 1);
	uniform_float(entry->point_size, &entry->uniforms.point_size, draw_uniforms.point_size);
	uniform_vec4(entry->ps_c0, entry->uniforms.ps_c0[0], draw_uniforms.ps_c0[0], 8);
	uniform_vec4(entry->ps_c1, entry->uniforms.ps_c1[0], draw_uniforms.ps_c1[0], 8);
	uniform_vec4(entry->ps_final_c0, entry->uniforms.ps_final_c0, draw_uniforms.ps_final_c0, 1);
	uniform_vec4(entry->ps_final_c1, entry->uniforms.ps_final_c1, draw_uniforms.ps_final_c1, 1);
	uniform_vec4(entry->fog_color, entry->uniforms.fog_color, draw_uniforms.fog_color, 1);
	uniform_vec4(entry->fog_parameters, entry->uniforms.fog_parameters, draw_uniforms.fog_parameters, 1);
	uniform_float(entry->alpha_reference, &entry->uniforms.alpha_reference, draw_uniforms.alpha_reference);
	uniform_vec4(entry->bump_matrix, entry->uniforms.bump_matrix[0], draw_uniforms.bump_matrix[0], 4);
	uniform_vec4(entry->bump_luminance, entry->uniforms.bump_luminance[0], draw_uniforms.bump_luminance[0], 4);
	uniform_vec4(entry->texture_scale, entry->uniforms.texture_scale[0], draw_uniforms.texture_scale[0], 4);
	uniform_float(entry->screen_offset, &entry->uniforms.screen_offset, draw_uniforms.screen_offset);
	uniform_vec4(entry->texture_lod_bias, entry->uniforms.texture_lod_bias, draw_uniforms.texture_lod_bias, 1);
	return entry;
}

/* ---------- tracing (debug.gpu_trace_frame) */

static BOOL trace_frame(void)
{
	static long frame = -2;

	if (frame == -2)
		frame = config_integer("debug.gpu_trace_frame");
	return frame >= 0 && device.frame == (unsigned long)frame;
}

static void trace_draw(const char *kind, D3DPRIMITIVETYPE type, unsigned long count, const float *first_vertex)
{
	struct vertex_shader_object *program = current_program();
	DWORD *rs = D3D__RenderState;

	if (!trace_frame())
		return;
	platform_log("%s type %d count %lu vs %lu (decl %lu) vp %lu,%lu %lux%lu z%.2f-%.2f zen %lu zw %lu zf %lx blend %lu %lx/%lx cull %lx cw %08lx tm %05lx cc %lx fin %08lx/%08lx at %lu/%lx",
		kind, type, count, program ? program->id : 0, device.vertex_shader ? device.vertex_shader->id : 0,
		device.viewport.X, device.viewport.Y, device.viewport.Width, device.viewport.Height,
		device.viewport.MinZ, device.viewport.MaxZ, rs[D3DRS_ZENABLE], rs[D3DRS_ZWRITEENABLE], rs[D3DRS_ZFUNC],
		rs[D3DRS_ALPHABLENDENABLE], rs[D3DRS_SRCBLEND], rs[D3DRS_DESTBLEND], rs[D3DRS_CULLMODE],
		rs[D3DRS_COLORWRITEENABLE], rs[D3DRS_PSTEXTUREMODES], rs[D3DRS_PSCOMBINERCOUNT],
		rs[D3DRS_PSFINALCOMBINERINPUTSABCD], rs[D3DRS_PSFINALCOMBINERINPUTSEFG],
		rs[D3DRS_ALPHATESTENABLE], rs[D3DRS_ALPHAFUNC]);
	{
		int stage;

		for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
		{
			D3DBaseTexture *texture = device.textures[stage];
			struct xgpu_texture_description description;

			if (!texture || !((D3D__RenderState[D3DRS_PSTEXTUREMODES] >> (5 * stage)) & 0x1f))
				continue;
			xgpu_texture_describe(texture->Format, texture->Size, &description);
			platform_log("    t%d: data %08lx format %08lx size %08lx -> fmt %02lx %lux%lux%lu levels %lu linear %d cube %d rt %d min %lu mip %lu bias %g maxmip %lu",
				stage, texture->Data, texture->Format, texture->Size, description.format, description.width,
				description.height, description.depth, description.levels, description.linear, description.cube_map,
				xgpu_render_target_find(texture->Data) != NULL, D3D__TextureState[stage][D3DTSS_MINFILTER],
				D3D__TextureState[stage][D3DTSS_MIPFILTER], dword_to_float(D3D__TextureState[stage][D3DTSS_MIPMAPLODBIAS]),
				D3D__TextureState[stage][D3DTSS_MAXMIPLEVEL]);
		}
	}
	platform_log("    offset enable %lu slope %g offset %g zbias %ld stencil %lu func %lx ref %lx mask %lx write %lx ops %lx/%lx/%lx",
		rs[D3DRS_SOLIDOFFSETENABLE], dword_to_float(rs[D3DRS_POLYGONOFFSETZSLOPESCALE]),
		dword_to_float(rs[D3DRS_POLYGONOFFSETZOFFSET]), (long)rs[D3DRS_ZBIAS], rs[D3DRS_STENCILENABLE],
		rs[D3DRS_STENCILFUNC], rs[D3DRS_STENCILREF], rs[D3DRS_STENCILMASK], rs[D3DRS_STENCILWRITEMASK],
		rs[D3DRS_STENCILFAIL], rs[D3DRS_STENCILZFAIL], rs[D3DRS_STENCILPASS]);
	if (config_boolean("debug.gpu_trace_constants"))
	{
		int constant;

		for (constant = 0; constant < XGPU_VERTEX_CONSTANT_COUNT; constant++)
		{
			const float *value = device.constants[constant];

			if (value[0] || value[1] || value[2] || value[3])
				platform_log("    c[%d] = %g %g %g %g", constant, value[0], value[1], value[2], value[3]);
		}
	}
	if (device.vertex_shader)
	{
		unsigned long index;

		for (index = 0; index < device.vertex_shader->element_count; index++)
		{
			const struct vertex_element *element = &device.vertex_shader->elements[index];

			platform_log("    decl v%lu: stream %lu offset %lu type %02lx", (unsigned long)element->reg,
				(unsigned long)element->stream, (unsigned long)element->offset, (unsigned long)element->type);
		}
		for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
		{
			const float *value = device.attributes[index];

			if (value[0] || value[1] || value[2] || value[3] != 1.0f)
				platform_log("    current v%lu = %g %g %g %g", index, value[0], value[1], value[2], value[3]);
		}
	}
	if (first_vertex)
	{
		int reg;

		for (reg = 0; reg < XGPU_VERTEX_ATTRIBUTE_COUNT; reg++)
		{
			const float *v = first_vertex + reg * 4;

			if (v[0] || v[1] || v[2] || v[3] != 1.0f)
				platform_log("    v%d = %g %g %g %g", reg, v[0], v[1], v[2], v[3]);
		}
	}
}


/* Mesa's GL thread queues a glBufferSubData of up to 8 KB; a larger one
first waits for everything queued before it to have run. The same bytes in
pieces are queued (all but the largest, as a map loads, which would be
thousands; Android's GL has no such thread). */
#define BUFFER_UPLOAD_PIECE 4096
#define BUFFER_UPLOAD_PIECES_MAXIMUM 16

static void buffer_upload(GLenum target, unsigned long offset, unsigned long size, const void *data)
{
#ifndef HALO_ANDROID
	if (size <= BUFFER_UPLOAD_PIECE * BUFFER_UPLOAD_PIECES_MAXIMUM)
	{
		const unsigned char *bytes = data;

		while (size)
		{
			unsigned long piece = size < BUFFER_UPLOAD_PIECE ? size : BUFFER_UPLOAD_PIECE;

			glBufferSubData(target, (GLintptr)offset, (GLsizeiptr)piece, bytes);
			offset += piece;
			bytes += piece;
			size -= piece;
		}
		return;
	}
#endif
	glBufferSubData(target, (GLintptr)offset, (GLsizeiptr)size, data);
}

/* ---------- the contiguous window in GL buffers

Vertex and index buffers live in the Xbox's contiguous memory, where most
never change once loaded. The mirror keeps a copy of that memory in GL
buffers (one per segment, created when first needed) and uploads a page
only when it is first drawn from or after the game has written it: pages
are write-protected once uploaded, as cached textures are (memory_watch.c).
Pages the game rewrites frame after frame (dynamic vertices) would fault on
every write; after a few such rewrites a page counts as volatile for a
while, and draws that use it stream their data as before. */

#define MIRROR_SEGMENT_SIZE 0x400000UL
#define MIRROR_SEGMENT_COUNT (PLATFORM_CONTIGUOUS_SIZE / MIRROR_SEGMENT_SIZE)
#define MIRROR_PAGE_SIZE 0x1000UL
#define MIRROR_PAGE_COUNT (PLATFORM_CONTIGUOUS_SIZE / MIRROR_PAGE_SIZE)
/* rewrites no more than this many frames apart ... */
#define MIRROR_REWRITE_FRAMES 2
/* ... this many times in a row make a page volatile ... */
#define MIRROR_VOLATILE_REWRITES 4
/* ... for this many frames */
#define MIRROR_VOLATILE_FRAMES 600

enum
{
	_mirror_page_absent,
	_mirror_page_present,
	_mirror_page_volatile
};

static struct
{
	GLuint buffers[MIRROR_SEGMENT_COUNT];
	unsigned char state[MIRROR_PAGE_COUNT];
	unsigned char rewrites[MIRROR_PAGE_COUNT];
	/* the page's memory_watch generation when it was uploaded */
	unsigned long generation[MIRROR_PAGE_COUNT];
	unsigned long rewritten_frame[MIRROR_PAGE_COUNT];
#ifdef XGPU_STREAM_RING
	/* device.frame at the segment's latest glBufferSubData */
	unsigned long subdata_frame[MIRROR_SEGMENT_COUNT];
#endif
} mirror;

/* uploads the pages of [first, last) that are absent or stale; FALSE if one
of them turns out to be volatile */
static BOOL mirror_refresh(unsigned long first, unsigned long last)
{
	unsigned long page, run;
	BOOL volatile_page = FALSE;
	unsigned char stale[256];
	unsigned long count = last - first;

	if (count > sizeof(stale))
	{
		/* a range this long is refreshed in pieces */
		for (page = first; page < last; page += sizeof(stale))
		{
			if (!mirror_refresh(page, page + sizeof(stale) < last ? page + sizeof(stale) : last))
				return FALSE;
		}
		return TRUE;
	}
	for (page = first; page < last; page++)
	{
		BOOL written = mirror.state[page] == _mirror_page_present &&
			memory_watch_generation(PLATFORM_CONTIGUOUS_BASE + page * MIRROR_PAGE_SIZE, MIRROR_PAGE_SIZE) >
			mirror.generation[page];

		stale[page - first] = mirror.state[page] != _mirror_page_present || written;
		if (written)
		{
			if (device.frame - mirror.rewritten_frame[page] <= MIRROR_REWRITE_FRAMES)
				mirror.rewrites[page]++;
			else
				mirror.rewrites[page] = 1;
			mirror.rewritten_frame[page] = device.frame;
			if (mirror.rewrites[page] >= MIRROR_VOLATILE_REWRITES)
			{
				mirror.state[page] = _mirror_page_volatile;
				volatile_page = TRUE;
			}
		}
	}
	if (volatile_page)
		return FALSE;
	for (page = first; page < last; page = run)
	{
		unsigned long segment = page * MIRROR_PAGE_SIZE / MIRROR_SEGMENT_SIZE;
		unsigned long address, size;
		/* no queued draw can read pages uploaded for the first time */
		BOOL unused = TRUE;

		if (!stale[page - first])
		{
			run = page + 1;
			continue;
		}
		for (run = page; run < last && stale[run - first]; run++)
		{
			if (mirror.state[run] != _mirror_page_present)
			{
				mirror.rewrites[run] = 0;
				mirror.rewritten_frame[run] = device.frame;
			}
			else
			{
				unused = FALSE;
			}
		}
		address = PLATFORM_CONTIGUOUS_BASE + page * MIRROR_PAGE_SIZE;
		size = (run - page) * MIRROR_PAGE_SIZE;
		/* protect first, so a write racing with the upload is noticed */
		memory_watch_protect(address, size);
		for (; page < run; page++)
		{
			mirror.generation[page] = memory_watch_generation(PLATFORM_CONTIGUOUS_BASE + page * MIRROR_PAGE_SIZE,
				MIRROR_PAGE_SIZE);
			mirror.state[page] = _mirror_page_present;
		}
		if (!mirror.buffers[segment])
		{
			glGenBuffers(1, &mirror.buffers[segment]);
			glBindBuffer(GL_COPY_WRITE_BUFFER, mirror.buffers[segment]);
			glBufferData(GL_COPY_WRITE_BUFFER, MIRROR_SEGMENT_SIZE, NULL, GL_DYNAMIC_DRAW);
#ifdef XGPU_STREAM_RING
			/* (no glBufferSubData yet: as if long enough ago) */
			mirror.subdata_frame[segment] = device.frame - STREAM_BUFFER_RING;
#endif
		}
		glBindBuffer(GL_COPY_WRITE_BUFFER, mirror.buffers[segment]);
#ifdef XGPU_STREAM_RING
		/* Mali copies the whole buffer for a glBufferSubData that queued
		draws might read (see STREAM_BUFFER_RING); unused pages can be
		written without waiting for them. Not while such a copy may be
		pending: it gives the buffer new storage at once but fills it on
		the GPU's timeline, over what was written into it meanwhile (a level
		loaded after another drew some of the earlier level's vertices).
		The GPU is done with a frame's work STREAM_BUFFER_RING frames on */
		if (unused && device.frame - mirror.subdata_frame[segment] >= STREAM_BUFFER_RING)
		{
			host_gl_buffer_write(GL_COPY_WRITE_BUFFER,
				(unsigned int)(address - PLATFORM_CONTIGUOUS_BASE - segment * MIRROR_SEGMENT_SIZE),
				(unsigned int)size, (const void *)address);
			continue;
		}
		mirror.subdata_frame[segment] = device.frame;
#else
		(void)unused;
#endif
		buffer_upload(GL_COPY_WRITE_BUFFER, address - PLATFORM_CONTIGUOUS_BASE - segment * MIRROR_SEGMENT_SIZE, size,
			(const void *)address);
	}
	return TRUE;
}

/* makes [address, address + size) current in the mirror, giving the buffer
that holds it, the range's offset in that buffer and the newest upload
generation of its pages (which changes whenever its contents do); FALSE if
the range is outside the window, spans two segments or is volatile */
static BOOL mirror_range(unsigned long address, unsigned long size, GLuint *buffer, unsigned long *offset,
	unsigned long *generation)
{
	unsigned long start = address - PLATFORM_CONTIGUOUS_BASE;
	unsigned long segment, first, last, page, oldest = ~0UL, newest = 0;
	BOOL present = TRUE;

	if (!size || address < PLATFORM_CONTIGUOUS_BASE || start + size > PLATFORM_CONTIGUOUS_SIZE)
		return FALSE;
	segment = start / MIRROR_SEGMENT_SIZE;
	if ((start + size - 1) / MIRROR_SEGMENT_SIZE != segment)
		return FALSE;
	first = start / MIRROR_PAGE_SIZE;
	last = (start + size - 1) / MIRROR_PAGE_SIZE + 1;
	for (page = first; page < last; page++)
	{
		if (mirror.state[page] == _mirror_page_volatile)
		{
			if (device.frame - mirror.rewritten_frame[page] < MIRROR_VOLATILE_FRAMES)
				return FALSE;
			mirror.state[page] = _mirror_page_absent;
		}
		if (mirror.state[page] != _mirror_page_present)
		{
			present = FALSE;
		}
		else
		{
			if (mirror.generation[page] < oldest)
				oldest = mirror.generation[page];
			if (mirror.generation[page] > newest)
				newest = mirror.generation[page];
		}
	}
	if (!present || memory_watch_generation(address, size) > oldest)
	{
		if (!mirror_refresh(first, last))
			return FALSE;
		for (newest = 0, page = first; page < last; page++)
		{
			if (mirror.generation[page] > newest)
				newest = mirror.generation[page];
		}
	}
	*buffer = mirror.buffers[segment];
	*offset = start - segment * MIRROR_SEGMENT_SIZE;
	if (generation)
		*generation = newest;
	stats.mirrored_bytes += size;
	return TRUE;
}

/* the smallest and largest index of an index range the mirror holds: the
same ranges are drawn frame after frame */
#define INDEX_RANGE_SLOTS 4096

static struct
{
	unsigned long address;
	unsigned long count;
	unsigned long generation;
	WORD minimum;
	WORD maximum;
} index_ranges[INDEX_RANGE_SLOTS];

static void index_extent(const WORD *indices, unsigned long count, unsigned long generation, BOOL cached,
	unsigned long *minimum, unsigned long *maximum)
{
	unsigned long slot = (((unsigned long)indices >> 1) ^ (count * 2654435761UL)) % INDEX_RANGE_SLOTS;
	unsigned long index, low = 0xffff, high = 0;

	if (cached && index_ranges[slot].address == (unsigned long)indices && index_ranges[slot].count == count &&
		index_ranges[slot].generation == generation)
	{
		*minimum = index_ranges[slot].minimum;
		*maximum = index_ranges[slot].maximum;
		return;
	}
	for (index = 0; index < count; index++)
	{
		if (indices[index] < low)
			low = indices[index];
		if (indices[index] > high)
			high = indices[index];
	}
	if (cached)
	{
		index_ranges[slot].address = (unsigned long)indices;
		index_ranges[slot].count = count;
		index_ranges[slot].generation = generation;
		index_ranges[slot].minimum = (WORD)low;
		index_ranges[slot].maximum = (WORD)high;
	}
	*minimum = low;
	*maximum = high;
}

/* ---------- vertex data */

#ifdef HALO_WEB
/* port: WebGL on Metal (Chrome's and Safari's ANGLE) waits for the GPU to
finish everything queued before it writes a buffer that a queued draw reads
(without its shadow buffers, which Chrome leaves off): a glBufferSubData
into the stream buffer after each draw was a wait for the GPU each draw,
some 2000 a frame, the frame rate a few frames each second. So each draw's
uploads go into a buffer of their own that no queued draw reads, taken from
pools of a few sizes, one set for each frame of the ring: a buffer is
written again only STREAM_BUFFER_RING frames on, when the GPU is done with
it. */
#define WEB_CHUNK_SHIFT 12 /* 4 KB */
#define WEB_CHUNK_CLASSES 14 /* to 32 MB */

static struct web_chunks
{
	GLuint *buffers;
	unsigned long count;
	unsigned long capacity;
	unsigned long next;
} web_chunks[2][STREAM_BUFFER_RING][WEB_CHUNK_CLASSES];

/* a buffer of at least size bytes for this frame's vertices (index 0) or
indices (index 1), bound, and its size */
static GLuint web_chunk_get(int indices, unsigned long size, unsigned long *capacity)
{
	struct web_chunks *chunks;
	int chunk_class = 0;
	GLuint buffer;

	while (chunk_class < WEB_CHUNK_CLASSES - 1 && (1UL << (WEB_CHUNK_SHIFT + chunk_class)) < size)
		chunk_class++;
	chunks = &web_chunks[indices][device.buffer_ring][chunk_class];
	*capacity = 1UL << (WEB_CHUNK_SHIFT + chunk_class);
	if (chunks->next == chunks->count)
	{
		if (chunks->count == chunks->capacity)
		{
			unsigned long grown_capacity = chunks->capacity ? chunks->capacity * 2 : 64;
			GLuint *grown = realloc(chunks->buffers, grown_capacity * sizeof(*grown));

			if (!grown)
			{
				/* (out of memory: the frame's own buffer, written as before) */
				*capacity = indices ? INDEX_BUFFER_SIZE : STREAM_BUFFER_SIZE;
				return indices ? device.index_buffers[device.buffer_ring] : device.stream_buffers[device.buffer_ring];
			}
			chunks->buffers = grown;
			chunks->capacity = grown_capacity;
		}
		glGenBuffers(1, &chunks->buffers[chunks->count]);
		buffer = chunks->buffers[chunks->count++];
		if (indices)
			state_element_array_buffer(buffer);
		else
			state_array_buffer(buffer);
		glBufferData(indices ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER, (GLsizeiptr)*capacity, NULL, GL_STREAM_DRAW);
	}
	return chunks->buffers[chunks->next++];
}

/* the ring's next frame: its buffers are free again */
static void web_chunks_begin_frame(void)
{
	int indices, chunk_class;

	for (indices = 0; indices < 2; indices++)
	{
		for (chunk_class = 0; chunk_class < WEB_CHUNK_CLASSES; chunk_class++)
			web_chunks[indices][device.buffer_ring][chunk_class].next = 0;
	}
	device.stream_capacity = 0;
	device.index_capacity = 0;
}
#endif

/* makes room for size bytes of uploads, orphaning the stream buffer if it
is full. A draw reserves room for all of its streams at once: orphaning
between two of them would leave the attributes already pointed at the
buffer reading its new, empty storage. */
static void stream_reserve(unsigned long size)
{
#ifdef HALO_WEB
	/* (a new buffer when a draw since the latest write may read this one) */
	if (size && (device.stream_offset + size > device.stream_capacity ||
		(device.stream_offset && device.stream_serial != device.draw_serial)))
	{
		device.stream_buffer = web_chunk_get(0, size, &device.stream_capacity);
		device.stream_offset = 0;
	}
#else
	if (device.stream_offset + size > STREAM_BUFFER_SIZE)
	{
		/* orphan the buffer and start again */
		state_array_buffer(device.stream_buffer);
		glBufferData(GL_ARRAY_BUFFER, STREAM_BUFFER_SIZE, NULL, GL_STREAM_DRAW);
		device.stream_offset = 0;
	}
#endif
}

static unsigned long stream_upload(const void *data, unsigned long size);

#ifdef HALO_GLES
/* stream_upload, at a multiple of alignment into the buffer */
static unsigned long stream_upload_aligned(const void *data, unsigned long size, unsigned long alignment)
{
	stream_reserve(((size + 15) & ~15UL) + alignment);
	device.stream_offset += (alignment - device.stream_offset % alignment) % alignment;
	return stream_upload(data, size);
}
#endif

static unsigned long stream_upload(const void *data, unsigned long size)
{
	unsigned long offset;
	/* (as index_upload: the vertices' own bytes, in room rounded up to 16) */
	unsigned long length = size;

	size = (size + 15) & ~15UL;
	stream_reserve(size);
	offset = device.stream_offset;
	state_array_buffer(device.stream_buffer);
#ifdef XGPU_STREAM_RING
	host_gl_buffer_write(GL_ARRAY_BUFFER, (unsigned int)offset, (unsigned int)length, data);
#else
	buffer_upload(GL_ARRAY_BUFFER, offset, length, data);
#endif
#ifdef HALO_WEB
	device.stream_serial = device.draw_serial;
#endif
	device.stream_offset += size;
	return offset;
}

#ifdef HALO_GLES
/* stream_upload, with the D3DCOLOR elements of the stream turned from BGRA
into the RGBA byte order ES reads */
static unsigned long stream_upload_swizzled(const struct vertex_shader_object *declaration, unsigned long stream,
	const unsigned char *data, unsigned long size, unsigned long stride)
{
	static unsigned char *scratch;
	static unsigned long scratch_size;
	unsigned long offsets[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned long count = 0, index, vertex;

	for (index = 0; index < declaration->element_count; index++)
	{
		const struct vertex_element *element = &declaration->elements[index];

		if (element->stream == stream && element->type == D3DVSDT_D3DCOLOR)
			offsets[count++] = element->offset;
	}
	if (!count || !stride)
		return stream_upload(data, size);
	if (scratch_size < size)
	{
		free(scratch);
		scratch_size = size + 65536;
		scratch = malloc(scratch_size);
	}
	memcpy(scratch, data, size);
	for (vertex = 0; vertex + stride <= size; vertex += stride)
	{
		for (index = 0; index < count; index++)
		{
			unsigned char *color = scratch + vertex + offsets[index];
			unsigned char blue = color[0];

			color[0] = color[2];
			color[2] = blue;
		}
	}
	return stream_upload(scratch, size);
}
#endif

static unsigned long index_upload(const void *data, unsigned long size)
{
	unsigned long offset;
	/* (the indices' own bytes are written; the room they take is rounded up
	to 16, for the next ones' alignment: rounding what was read too read
	past the caller's indices) */
	unsigned long length = size;

	size = (size + 15) & ~15UL;
#ifdef HALO_WEB
	/* (as stream_reserve) */
	if (device.index_offset + size > device.index_capacity ||
		(device.index_offset && device.index_serial != device.draw_serial))
	{
		device.index_buffer = web_chunk_get(1, size, &device.index_capacity);
		device.index_offset = 0;
	}
	state_element_array_buffer(device.index_buffer);
#else
	state_element_array_buffer(device.index_buffer);
	if (device.index_offset + size > INDEX_BUFFER_SIZE)
	{
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, INDEX_BUFFER_SIZE, NULL, GL_STREAM_DRAW);
		device.index_offset = 0;
	}
#endif
	offset = device.index_offset;
#ifdef XGPU_STREAM_RING
	host_gl_buffer_write(GL_ELEMENT_ARRAY_BUFFER, (unsigned int)offset, (unsigned int)length, data);
#else
	buffer_upload(GL_ELEMENT_ARRAY_BUFFER, offset, length, data);
#endif
#ifdef HALO_WEB
	device.index_serial = device.draw_serial;
#endif
	device.index_offset += size;
	return offset;
}

static void attribute_format(const struct vertex_element *element, GLint *size, GLenum *type, GLboolean *normalized)
{
	*normalized = GL_FALSE;
	switch (element->type)
	{
	case D3DVSDT_FLOAT1: *size = 1; *type = GL_FLOAT; break;
	case D3DVSDT_FLOAT2: *size = 2; *type = GL_FLOAT; break;
	case D3DVSDT_FLOAT3: case D3DVSDT_FLOAT2H: *size = 3; *type = GL_FLOAT; break;
	case D3DVSDT_FLOAT4: *size = 4; *type = GL_FLOAT; break;
#ifdef HALO_GLES
	/* ES has no BGRA attributes: stream_upload_swizzled swaps the bytes */
	case D3DVSDT_D3DCOLOR: *size = 4; *type = GL_UNSIGNED_BYTE; *normalized = GL_TRUE; break;
#else
	case D3DVSDT_D3DCOLOR: *size = GL_BGRA; *type = GL_UNSIGNED_BYTE; *normalized = GL_TRUE; break;
#endif
	case D3DVSDT_SHORT1: *size = 1; *type = GL_SHORT; break;
	case D3DVSDT_SHORT2: *size = 2; *type = GL_SHORT; break;
	case D3DVSDT_SHORT3: *size = 3; *type = GL_SHORT; break;
	case D3DVSDT_SHORT4: *size = 4; *type = GL_SHORT; break;
	case D3DVSDT_NORMSHORT1: *size = 1; *type = GL_SHORT; *normalized = GL_TRUE; break;
	case D3DVSDT_NORMSHORT2: *size = 2; *type = GL_SHORT; *normalized = GL_TRUE; break;
	case D3DVSDT_NORMSHORT3: *size = 3; *type = GL_SHORT; *normalized = GL_TRUE; break;
	case D3DVSDT_NORMSHORT4: *size = 4; *type = GL_SHORT; *normalized = GL_TRUE; break;
	case D3DVSDT_PBYTE1: *size = 1; *type = GL_UNSIGNED_BYTE; *normalized = GL_TRUE; break;
	case D3DVSDT_PBYTE2: *size = 2; *type = GL_UNSIGNED_BYTE; *normalized = GL_TRUE; break;
	case D3DVSDT_PBYTE3: *size = 3; *type = GL_UNSIGNED_BYTE; *normalized = GL_TRUE; break;
	case D3DVSDT_PBYTE4: *size = 4; *type = GL_UNSIGNED_BYTE; *normalized = GL_TRUE; break;
	default: *size = 4; *type = GL_FLOAT; break;
	}
}

/* upload vertices [first, first + count) of every stream the declaration
uses and point the attributes at them; attribute data then starts at
vertex 0 of the uploaded range */
#ifdef HALO_GLES
/* ES has no BGRA attributes, so a stream with colours is swizzled as it is
uploaded (stream_upload_swizzled) and cannot come from the mirror */
static BOOL stream_has_colors(const struct vertex_shader_object *declaration, unsigned long stream)
{
	unsigned long index;

	for (index = 0; index < declaration->element_count; index++)
	{
		if (declaration->elements[index].stream == stream && declaration->elements[index].type == D3DVSDT_D3DCOLOR)
			return TRUE;
	}
	return FALSE;
}
#endif

#ifdef HALO_WEB
/* Each draw's attributes would be pointed again (a call into the browser
for each) whenever it reads its streams from another place than the draw
before it, which is nearly always. The draws whose streams all come from
the mirror read the same places frame after frame (a model's part, the
structure's), so each such set of places has a vertex array of its own,
made the first time, and the draw binds it: one call. Other draws use the
device's own vertex array, pointed as before. A vertex array holds the
element array buffer's binding too, kept here for each. Past
VERTEX_ARRAY_CACHE_MAXIMUM, they are all deleted and made again. */
#define VERTEX_ARRAY_CACHE_SLOTS 8192
#define VERTEX_ARRAY_CACHE_MAXIMUM 4096

struct vertex_array_cache_entry
{
	const struct vertex_shader_object *declaration;
	unsigned long streams;
	GLuint buffers[16];
	unsigned long offsets[16];
	unsigned long strides[16];
	GLuint vertex_array;
	/* its element array buffer binding, when known: kept in the epoch */
	GLuint element_array_buffer;
	unsigned long element_epoch;
};

static struct
{
	struct vertex_array_cache_entry *slots[VERTEX_ARRAY_CACHE_SLOTS];
	unsigned long count;
	/* the vertex array bound: an entry, or NULL for the device's own */
	struct vertex_array_cache_entry *bound;
	BOOL bound_known;
	GLuint device_element_array_buffer;
	/* the entries' element array buffer bindings are known only from the
	latest invalidation on (xgpu_gl_state_invalidate) */
	unsigned long element_epoch;
} vertex_array_cache;

static void vertex_array_cache_invalidate(void)
{
	vertex_array_cache.bound_known = FALSE;
	vertex_array_cache.device_element_array_buffer = (GLuint)-1;
	vertex_array_cache.element_epoch++;
}

/* binds the entry's vertex array (NULL: the device's own) */
static void vertex_array_cache_bind(struct vertex_array_cache_entry *entry)
{
	if (vertex_array_cache.bound_known && vertex_array_cache.bound == entry)
		return;
	if (vertex_array_cache.bound_known)
	{
		if (vertex_array_cache.bound)
		{
			vertex_array_cache.bound->element_array_buffer = gl_state.element_array_buffer;
			vertex_array_cache.bound->element_epoch = vertex_array_cache.element_epoch;
		}
		else
			vertex_array_cache.device_element_array_buffer = gl_state.element_array_buffer;
	}
	else
	{
		/* (what was bound to the device's own is not known) */
		vertex_array_cache.device_element_array_buffer = (GLuint)-1;
	}
	glBindVertexArray(entry ? entry->vertex_array : device.vertex_array);
	vertex_array_cache.bound = entry;
	vertex_array_cache.bound_known = TRUE;
	gl_state.element_array_buffer = !entry ? vertex_array_cache.device_element_array_buffer :
		entry->element_epoch == vertex_array_cache.element_epoch ? entry->element_array_buffer : (GLuint)-1;
}

static void vertex_array_cache_clear(void)
{
	unsigned long slot;

	vertex_array_cache_bind(NULL);
	for (slot = 0; slot < VERTEX_ARRAY_CACHE_SLOTS; slot++)
	{
		struct vertex_array_cache_entry *entry = vertex_array_cache.slots[slot];

		if (entry)
		{
			glDeleteVertexArrays(1, &entry->vertex_array);
			free(entry);
			vertex_array_cache.slots[slot] = NULL;
		}
	}
	vertex_array_cache.count = 0;
}

/* binds the vertex array of the declaration reading its streams (streams,
a bit each) from buffers at offsets; FALSE if there is none and none can be
made */
static BOOL vertex_array_cache_use(const struct vertex_shader_object *declaration, unsigned long streams,
	const GLuint *buffers, const unsigned long *offsets)
{
	unsigned long hash = 2166136261UL ^ (unsigned long)declaration, stream, slot, probe, index;
	struct vertex_array_cache_entry *entry = NULL;

	for (stream = 0; stream < 16; stream++)
	{
		if (streams & (1UL << stream))
		{
			hash = (hash ^ buffers[stream]) * 16777619UL;
			hash = (hash ^ offsets[stream]) * 16777619UL;
			hash = (hash ^ device.streams[stream].stride) * 16777619UL;
		}
	}
	hash = (hash ^ streams) * 16777619UL;
	slot = hash % VERTEX_ARRAY_CACHE_SLOTS;
	for (probe = 0; probe < VERTEX_ARRAY_CACHE_SLOTS; probe++, slot = (slot + 1) % VERTEX_ARRAY_CACHE_SLOTS)
	{
		struct vertex_array_cache_entry *candidate = vertex_array_cache.slots[slot];

		if (!candidate)
			break;
		if (candidate->declaration != declaration || candidate->streams != streams)
			continue;
		for (stream = 0; stream < 16; stream++)
		{
			if ((streams & (1UL << stream)) && (candidate->buffers[stream] != buffers[stream] ||
				candidate->offsets[stream] != offsets[stream] || candidate->strides[stream] != device.streams[stream].stride))
			{
				break;
			}
		}
		if (stream == 16)
		{
			entry = candidate;
			break;
		}
	}
	if (!entry)
	{
		if (vertex_array_cache.count >= VERTEX_ARRAY_CACHE_MAXIMUM)
		{
			vertex_array_cache_clear();
			return vertex_array_cache_use(declaration, streams, buffers, offsets);
		}
		entry = calloc(1, sizeof(*entry));
		if (!entry)
			return FALSE;
		entry->declaration = declaration;
		entry->streams = streams;
		for (stream = 0; stream < 16; stream++)
		{
			if (streams & (1UL << stream))
			{
				entry->buffers[stream] = buffers[stream];
				entry->offsets[stream] = offsets[stream];
				entry->strides[stream] = device.streams[stream].stride;
			}
		}
		glGenVertexArrays(1, &entry->vertex_array);
		entry->element_array_buffer = 0;
		entry->element_epoch = vertex_array_cache.element_epoch;
		vertex_array_cache.slots[slot] = entry;
		vertex_array_cache.count++;
		vertex_array_cache_bind(entry);
		for (index = 0; index < declaration->element_count; index++)
		{
			const struct vertex_element *element = &declaration->elements[index];
			GLint size;
			GLenum type;
			GLboolean normalized;

			stream = element->stream;
			if (!(streams & (1UL << stream)) || element->type == D3DVSDT_NONE)
				continue;
			state_array_buffer(buffers[stream]);
			glEnableVertexAttribArray(element->reg);
			if (element->type == D3DVSDT_NORMPACKED3)
			{
				glVertexAttribIPointer(element->reg, 1, GL_UNSIGNED_INT, (GLsizei)device.streams[stream].stride,
					(const void *)(offsets[stream] + element->offset));
			}
			else
			{
				attribute_format(element, &size, &type, &normalized);
				glVertexAttribPointer(element->reg, size, type, normalized, (GLsizei)device.streams[stream].stride,
					(const void *)(offsets[stream] + element->offset));
			}
		}
		return TRUE;
	}
	vertex_array_cache_bind(entry);
	return TRUE;
}
#endif

static void setup_streams(unsigned long first, unsigned long count)
{
	struct vertex_shader_object *declaration = device.vertex_shader;
	GLuint stream_buffers[16];
	unsigned long stream_offsets[16];
	BOOL placed[16] = { FALSE };
	BOOL enabled[XGPU_VERTEX_ATTRIBUTE_COUNT] = { FALSE };
	unsigned long index, total = 0;
#ifndef HALO_GLES
	struct vertex_layout layout;
	unsigned long streams_used = 0;

	/* (zeroed: layouts are compared and hashed whole) */
	memset(&layout, 0, sizeof(layout));
#endif

	/* the mirror first; then one reservation for everything streamed */
	for (index = 0; index < declaration->element_count; index++)
	{
		const struct vertex_element *element = &declaration->elements[index];
		unsigned long stream = element->stream;
		unsigned long stride = device.streams[stream].stride;
		unsigned long bytes = stride ? stride * count : 64;
		unsigned long base;

		if (!device.streams[stream].data || element->type == D3DVSDT_NONE || placed[stream])
			continue;
		placed[stream] = TRUE;
		stream_buffers[stream] = 0;
		base = (unsigned long)PLATFORM_PHYSICAL_TO_VIRTUAL(device.streams[stream].data) + first * stride;
#ifdef HALO_GLES
		if (!stream_has_colors(declaration, stream))
#endif
		if (mirror_range(base, bytes, &stream_buffers[stream], &stream_offsets[stream], NULL))
			continue;
		stream_buffers[stream] = 0;
		total += (bytes + 15) & ~15UL;
	}
#ifdef HALO_WEB
	{
		/* every stream from the mirror: a vertex array of their own
		(vertex_array_cache) */
		unsigned long streams = 0;

		for (index = 0; index < 16; index++)
		{
			if (placed[index])
				streams |= 1UL << index;
		}
		if (!total && streams && vertex_array_cache_use(declaration, streams, stream_buffers, stream_offsets))
		{
			for (index = 0; index < declaration->element_count; index++)
			{
				const struct vertex_element *element = &declaration->elements[index];

				if (device.streams[element->stream].data && element->type != D3DVSDT_NONE)
					enabled[element->reg] = TRUE;
			}
			for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
			{
				if (!enabled[index])
					state_attribute_constant(index, declaration->packed_mask & (1UL << index) ? NULL : device.attributes[index]);
			}
			return;
		}
		vertex_array_cache_bind(NULL);
	}
#endif
	stream_reserve(total);
	for (index = 0; index < declaration->element_count; index++)
	{
		const struct vertex_element *element = &declaration->elements[index];
		unsigned long stream = element->stream;
		unsigned long stride = device.streams[stream].stride;
		GLint size;
		GLenum type;
		GLboolean normalized;

		if (!device.streams[stream].data || element->type == D3DVSDT_NONE)
			continue;
		if (!stream_buffers[stream])
		{
			const unsigned char *base = PLATFORM_PHYSICAL_TO_VIRTUAL(device.streams[stream].data);
			unsigned long bytes = stride ? stride * count : 64;

#ifdef HALO_GLES
			stream_offsets[stream] = stream_upload_swizzled(declaration, stream, base + first * stride, bytes, stride);
#else
			stream_offsets[stream] = stream_upload(base + first * stride, bytes);
#endif
			stream_buffers[stream] = device.stream_buffer;
			stats.streamed_bytes += bytes;
		}
#ifdef HALO_GLES
		if (element->type == D3DVSDT_NORMPACKED3)
		{
			state_attribute_stream(element->reg, (GLuint)stream, stream_buffers[stream], 1, GL_UNSIGNED_INT, GL_FALSE,
				TRUE, (GLsizei)stride, stream_offsets[stream], element->offset);
		}
		else
		{
			attribute_format(element, &size, &type, &normalized);
			state_attribute_stream(element->reg, (GLuint)stream, stream_buffers[stream], size, type, normalized,
				FALSE, (GLsizei)stride, stream_offsets[stream], element->offset);
		}
#else
		if (element->type == D3DVSDT_NORMPACKED3)
		{
			layout_attribute(&layout, element->reg, (GLuint)stream, 1, GL_UNSIGNED_INT, GL_FALSE, TRUE, element->offset);
		}
		else
		{
			attribute_format(element, &size, &type, &normalized);
			layout_attribute(&layout, element->reg, (GLuint)stream, size, type, normalized, FALSE, element->offset);
		}
		streams_used |= 1UL << stream;
#endif
		enabled[element->reg] = TRUE;
	}
#ifndef HALO_GLES
	/* (the layout follows from the declaration and the streams it has, so
	the same two have the same vertex array: no need to look it up) */
	if (!declaration->vertex_array || declaration->vertex_array_streams != streams_used)
	{
		declaration->vertex_array = vertex_array_get(&layout);
		declaration->vertex_array_streams = streams_used;
	}
	state_vertex_array(declaration->vertex_array);
	for (index = 0; index < 16; index++)
	{
		if (streams_used & (1UL << index))
			state_vertex_buffer((GLuint)index, stream_buffers[index], stream_offsets[index],
				(GLsizei)device.streams[index].stride);
	}
#endif
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		if (!enabled[index])
			state_attribute_value(index, declaration->packed_mask & (1UL << index) ? NULL : device.attributes[index]);
	}
}

static GLenum primitive_mode(D3DPRIMITIVETYPE type)
{
	switch (type)
	{
	case D3DPT_POINTLIST: return GL_POINTS;
	case D3DPT_LINELIST: return GL_LINES;
	case D3DPT_LINELOOP: return GL_LINE_LOOP;
	case D3DPT_LINESTRIP: return GL_LINE_STRIP;
	case D3DPT_TRIANGLESTRIP:
	case D3DPT_QUADSTRIP: return GL_TRIANGLE_STRIP;
	case D3DPT_TRIANGLEFAN:
	case D3DPT_POLYGON: return GL_TRIANGLE_FAN;
	default: return GL_TRIANGLES;
	}
}

/* quads become two triangles each */
static WORD *quad_indices(const WORD *indices, unsigned long count, unsigned long *out_count)
{
	unsigned long quads = count / 4;
	WORD *result = malloc(quads * 6 * sizeof(WORD) + 2);
	unsigned long quad;

	for (quad = 0; quad < quads; quad++)
	{
		WORD v0 = indices ? indices[quad * 4] : (WORD)(quad * 4);
		WORD v1 = indices ? indices[quad * 4 + 1] : (WORD)(quad * 4 + 1);
		WORD v2 = indices ? indices[quad * 4 + 2] : (WORD)(quad * 4 + 2);
		WORD v3 = indices ? indices[quad * 4 + 3] : (WORD)(quad * 4 + 3);

		result[quad * 6 + 0] = v0;
		result[quad * 6 + 1] = v1;
		result[quad * 6 + 2] = v2;
		result[quad * 6 + 3] = v0;
		result[quad * 6 + 4] = v2;
		result[quad * 6 + 5] = v3;
	}
	*out_count = quads * 6;
	return result;
}

void WINAPI D3DDevice_SetStreamSource(UINT stream_number, D3DVertexBuffer *stream_data, UINT stride)
{
	if (stream_number >= 16)
		return;
	device.streams[stream_number].data = stream_data ? stream_data->Data : 0;
	device.streams[stream_number].stride = stride;
}

void WINAPI D3DDevice_SetIndices(D3DIndexBuffer *index_data, UINT base_vertex_index)
{
	device.base_vertex_index = base_vertex_index;
	D3D__IndexData = index_data ? (WORD *)index_data->Data : NULL;
}

void WINAPI D3DDevice_DrawVertices(D3DPRIMITIVETYPE primitive_type, UINT start_vertex, UINT vertex_count)
{
	if (!vertex_count || !prepare_draw(FALSE))
		return;
	trace_draw("draw", primitive_type, vertex_count, NULL);
	setup_streams(start_vertex, vertex_count);
	if (primitive_type == D3DPT_QUADLIST)
	{
		unsigned long count;
		WORD *indices = quad_indices(NULL, vertex_count, &count);

		glDrawElements(GL_TRIANGLES, (GLsizei)count, GL_UNSIGNED_SHORT,
			(const void *)index_upload(indices, count * sizeof(WORD)));
		free(indices);
	}
	else
	{
		glDrawArrays(primitive_mode(primitive_type), 0, (GLsizei)vertex_count);
	}
	gl_check_errors("draw");
}

#ifdef HALO_WEB
/* WebGL has no base vertex, so the indices of a draw are rebased (less their
smallest) and uploaded with it, a call into the browser each and a check of
their range by it; and the game's index buffers are ordinary memory, which
the mirror does not hold. An index array drawn twice the same is kept
instead, rebased, in a buffer of its own: a draw of it compares its indices
with a copy and draws from the buffer. Arrays that change (the structure's
visible triangles, made each frame) are never the same twice, and are
uploaded as before. When the buffer is full, everything kept is forgotten. */
#define INDEX_CACHE_BUFFER_SIZE (8 * 1024 * 1024)
#define INDEX_CACHE_SLOTS 8192
#define INDEX_CACHE_PROBES 4

struct index_cache_entry
{
	const WORD *address;
	unsigned long count;
	/* the indices as last drawn from address, and room for them */
	WORD *copy;
	unsigned long capacity;
	WORD minimum, maximum;
	/* in the buffer (when kept is the epoch), or not yet: seen once */
	unsigned long offset;
	unsigned long kept;
};

static struct
{
	GLuint buffer;
	unsigned long used;
	/* entries kept in an earlier epoch are not in the buffer */
	unsigned long epoch;
	struct index_cache_entry entries[INDEX_CACHE_SLOTS];
} index_cache = { 0, 0, 1 };

/* the buffer offset of the rebased indices kept for the array, with their
extent; FALSE if it is not kept (yet) */
static BOOL index_cache_get(const WORD *indices, unsigned long count, unsigned long *minimum, unsigned long *maximum,
	unsigned long *offset)
{
	unsigned long slot = (((unsigned long)indices >> 1) ^ (count * 2654435761UL)) % INDEX_CACHE_SLOTS;
	unsigned long probe, size = count * sizeof(WORD), index;
	struct index_cache_entry *entry = NULL, *oldest = NULL;
	WORD *rebased;

	for (probe = 0; probe < INDEX_CACHE_PROBES; probe++)
	{
		struct index_cache_entry *candidate = &index_cache.entries[(slot + probe) % INDEX_CACHE_SLOTS];

		if (candidate->address == indices && candidate->count == count)
		{
			entry = candidate;
			break;
		}
		if (!oldest || candidate->kept < oldest->kept)
			oldest = candidate;
	}
	if (entry && entry->copy && !memcmp(entry->copy, indices, size))
	{
		if (entry->kept == index_cache.epoch)
		{
			*minimum = entry->minimum;
			*maximum = entry->maximum;
			*offset = entry->offset;
			return TRUE;
		}
		/* the second time the same: kept from now on */
		if (index_cache.used + ((size + 15) & ~15UL) > INDEX_CACHE_BUFFER_SIZE)
		{
			index_cache.epoch++;
			index_cache.used = 0;
		}
		if (!index_cache.buffer)
		{
			glGenBuffers(1, &index_cache.buffer);
			state_element_array_buffer(index_cache.buffer);
			glBufferData(GL_ELEMENT_ARRAY_BUFFER, INDEX_CACHE_BUFFER_SIZE, NULL, GL_STATIC_DRAW);
		}
		rebased = malloc(size + 2);
		if (!rebased)
			return FALSE;
		for (index = 0; index < count; index++)
			rebased[index] = (WORD)(indices[index] - entry->minimum);
		state_element_array_buffer(index_cache.buffer);
		host_gl_buffer_write(GL_ELEMENT_ARRAY_BUFFER, (unsigned int)index_cache.used, (unsigned int)size, rebased);
		free(rebased);
		entry->offset = index_cache.used;
		entry->kept = index_cache.epoch;
		index_cache.used += (size + 15) & ~15UL;
		*minimum = entry->minimum;
		*maximum = entry->maximum;
		*offset = entry->offset;
		return TRUE;
	}
	/* seen for the first time, or changed: remembered */
	if (!entry)
		entry = oldest;
	if (entry->capacity < count)
	{
		WORD *grown = realloc(entry->copy, size);

		if (!grown)
		{
			entry->address = NULL;
			return FALSE;
		}
		entry->copy = grown;
		entry->capacity = count;
	}
	memcpy(entry->copy, indices, size);
	entry->address = indices;
	entry->count = count;
	entry->kept = 0;
	index_extent(indices, count, 0, FALSE, minimum, maximum);
	entry->minimum = (WORD)*minimum;
	entry->maximum = (WORD)*maximum;
	return FALSE;
}
#endif

void WINAPI D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE primitive_type, UINT vertex_count, CONST WORD *index_data)
{
	unsigned long minimum, maximum, index, count, generation = 0, index_offset = 0;
	WORD *indices = NULL;
	const WORD *source = index_data;
	GLuint index_buffer = 0;
	BOOL mirrored;

	if (!vertex_count || !index_data || !prepare_draw(FALSE))
		return;
	/* quads are drawn as triangles, from indices made for the draw */
	mirrored = primitive_type != D3DPT_QUADLIST &&
#ifdef HALO_GLES
		xgpu_capabilities.base_vertex &&
#endif
		mirror_range((unsigned long)index_data, vertex_count * sizeof(WORD), &index_buffer, &index_offset, &generation);
#ifdef HALO_WEB
	if (!mirrored && primitive_type != D3DPT_QUADLIST &&
		index_cache_get(index_data, vertex_count, &minimum, &maximum, &index_offset))
	{
		trace_draw("indexed", primitive_type, vertex_count, NULL);
		setup_streams(device.base_vertex_index + minimum, maximum - minimum + 1);
		state_element_array_buffer(index_cache.buffer);
		glDrawElements(primitive_mode(primitive_type), (GLsizei)vertex_count, GL_UNSIGNED_SHORT, (const void *)index_offset);
		return;
	}
#endif
	index_extent(index_data, vertex_count, generation, mirrored, &minimum, &maximum);
	trace_draw("indexed", primitive_type, vertex_count, NULL);
	/* (the streams from the base vertex on: index i is vertex base + i) */
	setup_streams(device.base_vertex_index + minimum, maximum - minimum + 1);
	if (mirrored)
	{
		/* the attributes start at vertex minimum */
		state_element_array_buffer(index_buffer);
		glDrawElementsBaseVertex(primitive_mode(primitive_type), (GLsizei)vertex_count, GL_UNSIGNED_SHORT,
			(const void *)index_offset, -(GLint)minimum);
		return;
	}
	stats.streamed_bytes += vertex_count * sizeof(WORD);
	count = vertex_count;
	if (primitive_type == D3DPT_QUADLIST)
	{
		indices = quad_indices(index_data, vertex_count, &count);
		source = indices;
	}
#ifdef HALO_GLES
	if (!xgpu_capabilities.base_vertex)
	{
		/* the indices are copied anyway: rebase them */
		WORD *rebased = malloc(count * sizeof(WORD) + 2);

		for (index = 0; index < count; index++)
			rebased[index] = (WORD)(source[index] - minimum);
		glDrawElements(primitive_mode(primitive_type), (GLsizei)count, GL_UNSIGNED_SHORT,
			(const void *)index_upload(rebased, count * sizeof(WORD)));
		free(rebased);
		free(indices);
		return;
	}
#endif
	(void)index;
	glDrawElementsBaseVertex(primitive_mode(primitive_type), (GLsizei)count, GL_UNSIGNED_SHORT,
		(const void *)index_upload(source, count * sizeof(WORD)), -(GLint)minimum);
	free(indices);
}

/* ---------- immediate mode */

void WINAPI D3DDevice_Begin(D3DPRIMITIVETYPE primitive_type)
{
	device.immediate_active = TRUE;
	device.immediate_type = primitive_type;
	device.immediate_count = 0;
	device.immediate_written = 0;
}

static void immediate_emit(void)
{
	unsigned long floats = XGPU_VERTEX_ATTRIBUTE_COUNT * 4;

	if (device.immediate_count == device.immediate_capacity)
	{
		unsigned long capacity = device.immediate_capacity ? device.immediate_capacity * 2 : 256;
		float *vertices = realloc(device.immediate_vertices, capacity * floats * sizeof(float));

		/* (out of memory: the vertex is dropped) */
		if (!vertices)
			return;
		device.immediate_vertices = vertices;
		device.immediate_capacity = capacity;
	}
	memcpy(device.immediate_vertices + device.immediate_count * floats, device.attributes, floats * sizeof(float));
	device.immediate_count++;
}

void WINAPI D3DDevice_End(void)
{
	unsigned long stride = XGPU_VERTEX_ATTRIBUTE_COUNT * 4 * sizeof(float);
	unsigned long offset, index, count = device.immediate_count;
	D3DPRIMITIVETYPE type = device.immediate_type;
	/* (the vertices' place in the streaming buffer, where the attributes'
	pointers start at its beginning) */
	unsigned long first_vertex = 0;

	device.immediate_active = FALSE;
	if (!count || !prepare_draw(TRUE))
		return;
	trace_draw("immediate", type, count, device.immediate_vertices);
#ifdef HALO_WEB
	vertex_array_cache_bind(NULL);
#endif
	{
		/* Only the registers written since Begin are uploaded, one after
		another in each vertex; the others are constants (most draws write 3
		of the 16, and a vertex of all of them is 256 bytes). On ES the
		vertices start a whole number of vertices into the streaming buffer
		and are drawn from there (first_vertex), so that draws of the same
		registers keep the attributes' pointers: each pointer set is a call
		into the driver, or the browser. All sixteen would be 256 bytes, more
		than WebGL's largest stride (255): each attribute then goes in an
		array of its own there. */
		static float *packed;
		static unsigned long packed_capacity;
		unsigned long attribute_size = 4 * sizeof(float);
		unsigned long registers[XGPU_VERTEX_ATTRIBUTE_COUNT];
		unsigned long register_count = 0, vertex_size, vertex, slot;

		for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
		{
			if (device.immediate_written & (1UL << index))
				registers[register_count++] = index;
		}
		if (count > packed_capacity)
		{
			float *grown = realloc(packed, count * stride);

			/* (out of memory: the draw is dropped) */
			if (!grown)
				return;
			packed = grown;
			packed_capacity = count;
		}
		if (register_count < XGPU_VERTEX_ATTRIBUTE_COUNT)
		{
			vertex_size = register_count * attribute_size;
			for (vertex = 0; vertex < count; vertex++)
			{
				for (slot = 0; slot < register_count; slot++)
				{
					memcpy(packed + (vertex * register_count + slot) * 4,
						device.immediate_vertices + (vertex * XGPU_VERTEX_ATTRIBUTE_COUNT + registers[slot]) * 4,
						attribute_size);
				}
			}
#ifdef HALO_GLES
			offset = stream_upload_aligned(packed, count * vertex_size, vertex_size);
			first_vertex = offset / vertex_size;
			for (slot = 0; slot < register_count; slot++)
			{
				state_attribute_stream(registers[slot], 0, device.stream_buffer, 4, GL_FLOAT, GL_FALSE, FALSE,
					(GLsizei)vertex_size, 0, slot * attribute_size);
			}
#else
			offset = stream_upload(packed, count * vertex_size);
			{
				/* (the vertex array of the last few sets of registers) */
				static struct
				{
					unsigned long written;
					struct vertex_array_entry *entry;
				} arrays[8];
				static unsigned long next_array;
				struct vertex_array_entry *entry = NULL;

				for (slot = 0; slot < 8; slot++)
				{
					if (arrays[slot].entry && arrays[slot].written == device.immediate_written)
						entry = arrays[slot].entry;
				}
				if (!entry)
				{
					struct vertex_layout layout;

					memset(&layout, 0, sizeof(layout));
					for (slot = 0; slot < register_count; slot++)
						layout_attribute(&layout, registers[slot], 0, 4, GL_FLOAT, GL_FALSE, FALSE, slot * attribute_size);
					entry = vertex_array_get(&layout);
					arrays[next_array % 8].written = device.immediate_written;
					arrays[next_array % 8].entry = entry;
					next_array++;
				}
				state_vertex_array(entry);
				state_vertex_buffer(0, device.stream_buffer, offset, (GLsizei)vertex_size);
			}
#endif
			for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
			{
				if (!(device.immediate_written & (1UL << index)))
					state_attribute_value(index, device.attributes[index]);
			}
		}
		else
		{
#ifdef HALO_WEB
			for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
			{
				for (vertex = 0; vertex < count; vertex++)
				{
					memcpy(packed + (index * count + vertex) * 4,
						device.immediate_vertices + (vertex * XGPU_VERTEX_ATTRIBUTE_COUNT + index) * 4, attribute_size);
				}
			}
			offset = stream_upload(packed, count * stride);
			for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
			{
				state_attribute_stream(index, 0, device.stream_buffer, 4, GL_FLOAT, GL_FALSE, FALSE,
					(GLsizei)attribute_size, offset, index * count * attribute_size);
			}
#elif defined(HALO_GLES)
			offset = stream_upload(device.immediate_vertices, count * stride);
			for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
			{
				state_attribute_stream(index, 0, device.stream_buffer, 4, GL_FLOAT, GL_FALSE, FALSE, (GLsizei)stride,
					offset, index * 4 * sizeof(float));
			}
#else
			offset = stream_upload(device.immediate_vertices, count * stride);
			{
				/* every attribute four floats, one after another */
				static struct vertex_array_entry *immediate_array;

				if (!immediate_array)
				{
					struct vertex_layout layout;

					memset(&layout, 0, sizeof(layout));
					for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
						layout_attribute(&layout, index, 0, 4, GL_FLOAT, GL_FALSE, FALSE, index * 4 * sizeof(float));
					immediate_array = vertex_array_get(&layout);
				}
				state_vertex_array(immediate_array);
				state_vertex_buffer(0, device.stream_buffer, offset, (GLsizei)stride);
			}
#endif
		}
	}
	if (type == D3DPT_QUADLIST)
	{
		unsigned long index_count;
		WORD *indices = quad_indices(NULL, count, &index_count);

		if (first_vertex)
		{
			/* (the indices from the first vertex on, past what 16 bits hold) */
			GLuint *placed = indices ? malloc(index_count * sizeof(GLuint)) : NULL;

			if (placed)
			{
				for (index = 0; index < index_count; index++)
					placed[index] = (GLuint)(first_vertex + indices[index]);
				glDrawElements(GL_TRIANGLES, (GLsizei)index_count, GL_UNSIGNED_INT,
					(const void *)index_upload(placed, index_count * sizeof(GLuint)));
				free(placed);
			}
		}
		else
		{
			glDrawElements(GL_TRIANGLES, (GLsizei)index_count, GL_UNSIGNED_SHORT,
				(const void *)index_upload(indices, index_count * sizeof(WORD)));
		}
		free(indices);
	}
	else
	{
		glDrawArrays(primitive_mode(type), (GLint)first_vertex, (GLsizei)count);
	}
	gl_check_errors("immediate draw");
}

static void set_attribute(INT reg, float a, float b, float c, float d)
{
	BOOL emit = FALSE;

	if (reg == D3DVSDE_VERTEX)
	{
		reg = 0;
		emit = TRUE;
	}
	if (reg < 0 || reg >= XGPU_VERTEX_ATTRIBUTE_COUNT)
		return;
	device.attributes[reg][0] = a;
	device.attributes[reg][1] = b;
	device.attributes[reg][2] = c;
	device.attributes[reg][3] = d;
	if (device.immediate_active)
		device.immediate_written |= 1UL << reg;
	/* like the hardware, writing register 0 completes a vertex */
	if (device.immediate_active && (emit || reg == 0))
		immediate_emit();
}

void WINAPI D3DDevice_SetVertexData2f(INT reg, FLOAT a, FLOAT b)
{
	set_attribute(reg, a, b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4f(INT reg, FLOAT a, FLOAT b, FLOAT c, FLOAT d)
{
	set_attribute(reg, a, b, c, d);
}

void WINAPI D3DDevice_SetVertexData2s(INT reg, SHORT a, SHORT b)
{
	set_attribute(reg, (float)a, (float)b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4ub(INT reg, BYTE a, BYTE b, BYTE c, BYTE d)
{
	set_attribute(reg, a / 255.0f, b / 255.0f, c / 255.0f, d / 255.0f);
}

void WINAPI D3DDevice_SetVertexDataColor(INT reg, D3DCOLOR color)
{
	float value[4];

	color_to_vec4(color, value);
	set_attribute(reg, value[0], value[1], value[2], value[3]);
}

/* ---------- clearing */

/* the state a clear changed, no longer what the shadows say: the masks it
cleared through, the scissor (left disabled) and its rectangle. The rest
of the state is as it was, so the next draw sets only these again (all of
it again, at every clear, was hundreds of calls a frame) */
static void clear_state_forget(void)
{
	gl_state.color_mask = 0xff;
	gl_state.depth_mask = 0xff;
	gl_state.stencil_write_mask = (GLuint)-1;
	gl_state.scissor_test = 0;
	memset(gl_state.scissor, 0xff, sizeof(gl_state.scissor));
}

void WINAPI D3DDevice_Clear(DWORD count, CONST D3DRECT *rectangles, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	float rgba[4];
	GLbitfield mask = 0;
	BOOL has_depth = FALSE;
	DWORD index;

	if (!device.gl_ready || !bind_targets(&has_depth))
		return;
	if (trace_frame())
		platform_log("clear flags %lx color %08lx z %g count %lu target %08lx depth %08lx", (unsigned long)flags,
			(unsigned long)color, z, (unsigned long)count,
			device.render_target ? (unsigned long)device.render_target->Data : 0,
			device.depth_stencil ? (unsigned long)device.depth_stencil->Data : 0);
	stats.clears++;
	color_to_vec4(color, rgba);
	if (flags & D3DCLEAR_TARGET)
	{
		/* the Xbox clears the channels named (D3DCLEAR_TARGET_R, _G, _B, _A):
		the fog screen clears only alpha, leaving the picture under the fog */
		glColorMask((flags & D3DCLEAR_TARGET_R) != 0, (flags & D3DCLEAR_TARGET_G) != 0,
			(flags & D3DCLEAR_TARGET_B) != 0, (flags & D3DCLEAR_TARGET_A) != 0);
		glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
		mask |= GL_COLOR_BUFFER_BIT;
	}
	if (has_depth && (flags & D3DCLEAR_ZBUFFER))
	{
		glDepthMask(GL_TRUE);
		glClearDepth(z);
		mask |= GL_DEPTH_BUFFER_BIT;
	}
	if (has_depth && (flags & D3DCLEAR_STENCIL))
	{
		glStencilMask(0xff);
		glClearStencil((GLint)stencil);
		mask |= GL_STENCIL_BUFFER_BIT;
	}
	if (!mask)
		return;
	if (!count || !rectangles)
	{
		/* the NV2A clips a viewport-less clear to the viewport, which is what
		keeps a split-screen window's clear from wiping the other window */
		GLint x0 = target_pixel((float)device.viewport.X, 0);
		GLint y0 = target_pixel((float)device.viewport.Y, 1);
		GLint height = target_pixel((float)(device.viewport.Y + device.viewport.Height), 1) - y0;

#ifdef HALO_VR
		vr_flip_rows(&y0, height);
#endif
		glEnable(GL_SCISSOR_TEST);
		glScissor(x0, y0, target_pixel((float)(device.viewport.X + device.viewport.Width), 0) - x0, height);
		glClear(mask);
		glDisable(GL_SCISSOR_TEST);
		clear_state_forget();
		return;
	}
	glEnable(GL_SCISSOR_TEST);
	for (index = 0; index < count; index++)
	{
		INT left = rectangles[index].x1 > device.viewport.X ? rectangles[index].x1 : device.viewport.X;
		INT top = rectangles[index].y1 > device.viewport.Y ? rectangles[index].y1 : device.viewport.Y;
		INT right = rectangles[index].x2 < device.viewport.X + device.viewport.Width ?
			rectangles[index].x2 : device.viewport.X + device.viewport.Width;
		INT bottom = rectangles[index].y2 < device.viewport.Y + device.viewport.Height ?
			rectangles[index].y2 : device.viewport.Y + device.viewport.Height;
		GLint x0, y0;

		if (left >= right || top >= bottom)
			continue;
		x0 = target_pixel((float)(left + UI_OFFSET), 0);
		y0 = target_pixel((float)top, 1);
		{
			GLint height = target_pixel((float)bottom, 1) - y0;

#ifdef HALO_VR
			vr_flip_rows(&y0, height);
#endif
			glScissor(x0, y0, target_pixel((float)(right + UI_OFFSET), 0) - x0, height);
		}
		glClear(mask);
	}
	glDisable(GL_SCISSOR_TEST);
	clear_state_forget();
}

/* ---------- the anti-aliasing passes */

/* display.anti_aliasing's pass over a window's 3D view, before the HUD and
menus are drawn over it (source/render/render.c); the window's bounds in the
game's units of the screen */
void halo_screen_anti_alias(short x0, short y0, short x1, short y1)
{
	struct render_target_entry *target;
	GLint corners[4];
	int mode = anti_aliasing();

	if (!device.gl_ready || (mode != _anti_aliasing_fxaa && mode != _anti_aliasing_smaa))
		return;
	/* (the primary target's view: the back buffer's) */
	target = render_target_get(&device.back_buffer);
	if (!target)
		return;
	/* (no longer multisampled, if it was before the setting changed) */
	render_target_multisample(&target->target, 0);
	corners[0] = scaled_pixel(x0, target->target.scale[0]);
	corners[1] = scaled_pixel(y0, target->target.scale[1]);
	corners[2] = scaled_pixel(x1, target->target.scale[0]);
	corners[3] = scaled_pixel(y1, target->target.scale[1]);
	xgpu_post_anti_alias(mode == _anti_aliasing_smaa, framebuffer_get(target->target.texture, 0),
		target->target.gl_width, target->target.gl_height, corners);
	glBindVertexArray(device.vertex_array);
	xgpu_gl_state_invalidate();
}

/* ---------- presentation */

#ifndef HALO_ARM64_GUEST
/* the screenshot key's PNG (controls.screenshot; the Android guest has none
of the SDL calls it takes) */
static void write_key_screenshot(struct render_target_entry *target)
{
	char directory[512], path[640], timestamp[32], filename[64];
	SDL_Time now;
	SDL_DateTime date;
	SDL_Surface *surface = NULL;
	unsigned int collision;
	unsigned long width = target->target.gl_width, height = target->target.gl_height;
	GLint framebuffer, draw_framebuffer, pack_buffer, alignment, row_length, skip_rows, skip_pixels;
	GLenum error;
	unsigned long row, column;

	if (snprintf(directory, sizeof(directory), "%s/screenshots", platform_data_root()) >= (int)sizeof(directory))
	{
		SDL_SetError("Screenshot directory path is too long");
		goto failed;
	}
	if (!SDL_CreateDirectory(directory) || !SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &date, true))
		goto failed;
	snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d_%02d.%02d.%02d",
		date.year, date.month, date.day, date.hour, date.minute, date.second);
	for (collision = 0; collision < 1000; collision++)
	{
		if (collision)
			snprintf(filename, sizeof(filename), "%s_%u.png", timestamp, collision + 1);
		else
			snprintf(filename, sizeof(filename), "%s.png", timestamp);
		snprintf(path, sizeof(path), "%s/%s", directory, filename);
		if (!SDL_GetPathInfo(path, NULL))
			break;
	}
	if (collision == 1000)
	{
		SDL_SetError("Too many screenshots with the same timestamp");
		goto failed;
	}
	if (!width || !height || width > INT_MAX / 4 || height > INT_MAX)
	{
		SDL_SetError("Invalid screenshot dimensions");
		goto failed;
	}
	surface = SDL_CreateSurface((int)width, (int)height, SDL_PIXELFORMAT_BGRA32);
	if (!surface)
		goto failed;

	/* Read the render target before the display blit's vertical flip. Its
	row zero is the top of the image. Preserve the caller's readback state. */
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &framebuffer);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer);
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
	glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
	glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
	glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
	glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer_get(target->target.texture, 0));
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glPixelStorei(GL_PACK_ROW_LENGTH, surface->pitch / 4);
	glPixelStorei(GL_PACK_SKIP_ROWS, 0);
	glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
	glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_BGRA, GL_UNSIGNED_BYTE, surface->pixels);
	error = glGetError();
	glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_framebuffer);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buffer);
	glPixelStorei(GL_PACK_ALIGNMENT, alignment);
	glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
	glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows);
	glPixelStorei(GL_PACK_SKIP_PIXELS, skip_pixels);
	if (error != GL_NO_ERROR)
	{
		SDL_SetError("OpenGL screenshot readback failed (%04x)", (unsigned)error);
		goto failed;
	}
	/* Destination alpha is scratch data in the game, not transparency. */
	for (row = 0; row < height; row++)
	{
		unsigned char *pixels = (unsigned char *)surface->pixels + (size_t)row * surface->pitch;

		for (column = 0; column < width; column++)
			pixels[column * 4 + 3] = 0xff;
	}
	if (!SDL_SavePNG(surface, path))
		goto failed;
	SDL_DestroySurface(surface);
	console_printf(FALSE, "Saved screenshot as %s", filename);
	return;

failed:
	console_printf(FALSE, "Screenshot failed: %.180s", SDL_GetError());
	if (surface)
		SDL_DestroySurface(surface);
}

#endif
/* writes BGRA pixels as a BMP named <name><frame>.bmp in the screenshot
folder: rows from the top (as the game's targets hold them), or from the
bottom (as the VR mode's eye images do) */
static void write_bmp(const char *name, const unsigned char *pixels, unsigned long width, unsigned long height,
	BOOL bottom_up)
{
	const char *directory = config_string("debug.screenshot_directory");
	unsigned char header[54] = { 'B', 'M' };
	unsigned long image_size = width * height * 4;
	unsigned long row;
	char path[512];
	FILE *file;

	snprintf(path, sizeof(path), "%s/%s%05lu.bmp", directory, name, device.frame);
	file = fopen(path, "wb");
	if (!file)
		return;
	*(unsigned int *)(header + 2) = (unsigned int)(54 + image_size);
	*(unsigned int *)(header + 10) = 54;
	*(unsigned int *)(header + 14) = 40;
	*(int *)(header + 18) = (int)width;
	*(int *)(header + 22) = bottom_up ? (int)height : -(int)height;
	*(unsigned short *)(header + 26) = 1;
	*(unsigned short *)(header + 28) = 32;
	*(unsigned int *)(header + 34) = (unsigned int)image_size;
	fwrite(header, 1, sizeof(header), file);
	for (row = 0; row < height; row++)
		fwrite(pixels + row * width * 4, 1, width * 4, file);
	fclose(file);
}

/* the target's pixels, read into a malloc'd BGRA buffer; layer is the
array layer of a layered target */
static unsigned char *read_target(GLuint texture, int layer, unsigned long width, unsigned long height)
{
	unsigned char *pixels = malloc(width * height * 4);
	unsigned long index;

	if (layer < 0)
	{
		glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer_get(texture, 0));
	}
	else
	{
#ifdef HALO_VR
		static GLuint framebuffer;

		if (!framebuffer)
			glGenFramebuffers(1, &framebuffer);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
		glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0, layer);
#endif
	}
	glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	xgpu_gl_state_invalidate();
#ifdef HALO_GLES
	for (index = 0; index < width * height; index++)
	{
		unsigned char red = pixels[index * 4];

		pixels[index * 4] = pixels[index * 4 + 2];
		pixels[index * 4 + 2] = red;
	}
#else
	(void)index;
#endif
	return pixels;
}

static void write_screenshot(struct render_target_entry *target)
{
	unsigned long width = target->target.gl_width, height = target->target.gl_height;
	unsigned char *pixels;
	unsigned long index;

	if (!*config_string("debug.screenshot_directory"))
		return;
	pixels = read_target(target->target.texture, -1, width, height);
	/* the display ignores destination alpha, which the game uses as scratch;
	image viewers would show it as transparency */
	for (index = 0; index < width * height; index++)
		pixels[index * 4 + 3] = 0xff;
	write_bmp("frame", pixels, width, height, FALSE);
	free(pixels);
}

#ifndef HALO_GLES
/* the GPU copies the counts of the previous frame's tests into their slots
(given by name: Mesa's GL thread waits for everything before a
glGetQueryObjectuiv, even one into a bound buffer). A copy right after each
test stopped the GPU until that test's draws were through, hundreds of times
a frame in The Library; a frame later the draws are long done, so the copies
do not wait. */
static void visibility_copy_batch(void)
{
	unsigned long i;

	if (!device.visibility_results)
		return;
	/* this frame's tests wait for the next present */
	device.visibility_batch ^= 1;
	for (i = 0; i < device.visibility_batches[device.visibility_batch].count; i++)
	{
		glGetQueryBufferObjectuiv(device.visibility_batches[device.visibility_batch].queries[i],
			device.visibility_results_buffer, GL_QUERY_RESULT,
			(GLintptr)(device.visibility_batches[device.visibility_batch].slots[i] * sizeof(GLuint)));
	}
	device.visibility_batches[device.visibility_batch].count = 0;
}

#endif
#ifdef HALO_VR
/* ---------- the VR mode's passes */

#define GL_TEXTURE_SRGB_DECODE_EXT 0x8A48
#define GL_DECODE_EXT 0x8A49
#define GL_SKIP_DECODE_EXT 0x8A4A

int host_gl_has_extension(const char *name);

/* ---------- foveation (vr.foveation; xgpu.h, xgpu_foveated)

The eye pass draws, in the same draws, four views at a fraction of each
eye's image's size (vr.foveation_inset, by default half across and up):
for each eye the whole of it (its periphery's detail), and the inset about
where the eyes look (the gaze, XR_EXT_eye_gaze_interaction, smoothed; or the
middle of the view) at the eye image's own resolution, as many pixels. At
the default, half of the eyes' pixels are drawn (as many as one eye's), and
the game's work is the same: no draw is made twice, each vertex is moved to
four views as it was to two (GL_OVR_multiview2, which also gives the inset
views of what is outside them for nothing: the GPU clips it). The present
makes the eye images from them (vr_fovea_composite): the inset where it is,
the periphery about it, magnified (bilinear), and a wide blend between
them (FOVEA_BLEND of the inset's side) whose ends are smoothed, so that no
edge shows where the detail changes; the rest of the present (the tint,
the rim, the visor) draws into the eye images at their full resolution.

Zink (the Steam Frame's OpenGL, on Turnip) has no GL extension for the
GPU's own variable rate shading (VK_KHR_fragment_shading_rate) nor for a
fragment density map (Qualcomm's GL_QCOM_texture_foveated is its
proprietary driver's), and OpenXR's foveation (XR_FB_foveation and its
kin) is applied by the runtime to Vulkan swapchains (or GL ES ones with
Qualcomm's extension): for this OpenGL game, the views are what lowers the
periphery's resolution. The log says which of those the driver has. */

/* the GL extensions that foveate, for the log */
static void vr_fovea_extensions(void)
{
	static const char *const names[] =
	{
		"GL_EXT_fragment_shading_rate", "GL_EXT_fragment_shading_rate_attachment", "GL_NV_shading_rate_image",
		"GL_QCOM_texture_foveated", "GL_QCOM_texture_foveated2", "GL_QCOM_framebuffer_foveated",
	};
	char text[256] = "";
	size_t at = 0, index;
	GLint views = 0;

	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++)
	{
		if (host_gl_has_extension(names[index]) && at < sizeof(text))
			at += (size_t)snprintf(text + at, sizeof(text) - at, "%s%s", at ? ", " : "", names[index]);
	}
	glGetIntegerv(GL_MAX_VIEWS_OVR, &views);
	platform_log("vr: the driver's shading rate and foveation extensions: %s; GL_OVR_multiview2 up to %d views",
		at ? text : "none", (int)views);
}

/* the eye pass's back buffer: the four layers (foveated), or the eyes'
swapchain image */
static void vr_eye_target(BOOL foveated)
{
	struct xgpu_render_target *target = &vr_gl.eye_target.target;
	int width = foveated ? vr_gl.fovea_width : vr_gl.eye_width;
	int height = foveated ? vr_gl.fovea_height : vr_gl.eye_height;

	target->texture = foveated ? vr_gl.fovea_texture : vr_gl.eye_image;
	target->scale[0] = (float)width / 640.0f;
	target->scale[1] = (float)height / (float)SCREEN_HEIGHT;
	target->gl_width = (unsigned long)width;
	target->gl_height = (unsigned long)height;
	target->layers = foveated ? 4 : 2;
}

static void vr_fovea_initialize(void)
{
	float inset = halo_vr_foveation_inset();
	GLint views = 0;

	vr_fovea_extensions();
	if (inset <= 0.0f)
	{
		platform_log("vr: foveation off (vr.foveation)");
		return;
	}
	glGetIntegerv(GL_MAX_VIEWS_OVR, &views);
	if (views < 4 || !SAMPLE_COUNTING)
	{
		/* (four views; the visibility tests counted by the pixel shaders,
		as a multiview draw cannot be queried) */
		platform_log("vr: no foveation: the driver draws %d views at once%s", (int)views,
			SAMPLE_COUNTING ? "" : ", and has no atomic counters");
		return;
	}
	vr_gl.fovea_width = (int)((float)vr_gl.eye_width * inset + 0.5f) & ~1;
	vr_gl.fovea_height = (int)((float)vr_gl.eye_height * inset + 0.5f) & ~1;
	glGenTextures(1, &vr_gl.fovea_texture);
	glBindTexture(GL_TEXTURE_2D_ARRAY, vr_gl.fovea_texture);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, vr_gl.fovea_width, vr_gl.fovea_height, 4, 0, GL_BGRA,
		GL_UNSIGNED_BYTE, NULL);
	xgpu_gl_state_invalidate();
	vr_gl.foveated = TRUE;
	xgpu_foveated = 1;
	vr_gl.fovea_show = config_boolean("debug.vr_foveation_show");
	vr_gl.fovea_corners[0][0] = vr_gl.fovea_corners[1][0] = (vr_gl.eye_width - vr_gl.fovea_width) / 2;
	vr_gl.fovea_corners[0][1] = vr_gl.fovea_corners[1][1] = (vr_gl.eye_height - vr_gl.fovea_height) / 2;
	platform_log("vr: foveation by %s: full detail in %dx%d of each eye's %dx%d, %.0f%% about it: "
		"4 views of %dx%d, %.0f%% of the eyes' pixels", halo_vr_foveation_name(), vr_gl.fovea_width,
		vr_gl.fovea_height, vr_gl.eye_width, vr_gl.eye_height, inset * 100.0f, vr_gl.fovea_width, vr_gl.fovea_height,
		100.0f * 2.0f * (float)vr_gl.fovea_width * (float)vr_gl.fovea_height /
			((float)vr_gl.eye_width * (float)vr_gl.eye_height));
}

/* this frame's insets, where halo_vr_foveation says, on whole pixels of
the eye images, within them */
static void vr_fovea_place(void)
{
	float centres[2][2], samples[4][4];
	int eye;

	if (!halo_vr_foveation(centres))
		centres[0][0] = centres[0][1] = centres[1][0] = centres[1][1] = 0.5f;
	for (eye = 0; eye < 2; eye++)
	{
		int x = (int)floorf(centres[eye][0] * (float)vr_gl.eye_width - (float)vr_gl.fovea_width * 0.5f + 0.5f);
		int y = (int)floorf(centres[eye][1] * (float)vr_gl.eye_height - (float)vr_gl.fovea_height * 0.5f + 0.5f);

		vr_gl.fovea_corners[eye][0] = x < 0 ? 0 : x > vr_gl.eye_width - vr_gl.fovea_width ?
			vr_gl.eye_width - vr_gl.fovea_width : x;
		vr_gl.fovea_corners[eye][1] = y < 0 ? 0 : y > vr_gl.eye_height - vr_gl.fovea_height ?
			vr_gl.eye_height - vr_gl.fovea_height : y;
		samples[eye * 2][0] = samples[eye * 2][1] = 1.0f;
		samples[eye * 2][2] = samples[eye * 2][3] = 0.0f;
		samples[eye * 2 + 1][0] = (float)vr_gl.eye_width / (float)vr_gl.fovea_width;
		samples[eye * 2 + 1][1] = (float)vr_gl.eye_height / (float)vr_gl.fovea_height;
		samples[eye * 2 + 1][2] = -(float)vr_gl.fovea_corners[eye][0];
		samples[eye * 2 + 1][3] = -(float)vr_gl.fovea_corners[eye][1];
	}
	if (memcmp(samples, vr_gl.fovea_sample, sizeof(samples)))
	{
		memcpy(vr_gl.fovea_sample, samples, sizeof(samples));
		vr_gl.fovea_serial++;
	}
	/* (the views' anew, for the next draw's viewport) */
	memset(vr_gl.fovea_view, 0, sizeof(vr_gl.fovea_view));
}

/* the eye images from the four layers: in each pixel the periphery's
(bilinear), and the inset's (its own pixel) blended over it within the
inset, fully FOVEA_BLEND of its side in from its edges (not those at the
image's own edges), the blend's corners round; then the eye pass's back
buffer is the eye images again, for the rest of the present */
static void vr_fovea_composite(void)
{
	static GLuint program;
	static BOOL failed;
	static GLint inset_location, open_location, image_location, show_location;
	static const char vertex_source[] =
		"#version 450 core\n"
		"#extension GL_OVR_multiview2 : require\n"
		"layout(num_views = 2) in;\n"
		"void main()\n"
		"{\n"
		"	vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
		"	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	static const char fragment_source[] =
		"#version 450 core\n"
		"#extension GL_OVR_multiview2 : require\n"
		"uniform sampler2DArray fovea;\n"
		/* each eye's inset: its top left (the image's pixels from its top
		left) and size; 1 where its left, top, right or bottom edge is the
		image's */
		"uniform vec4 inset[2];\n"
		"uniform vec4 open[2];\n"
		/* the image's size, the blend's width (pixels) */
		"uniform vec4 image;\n"
		"uniform float show;\n"
		"out vec4 colour;\n"
		"void main()\n"
		"{\n"
		"	int eye = int(gl_ViewID_OVR);\n"
		"	vec2 p = gl_FragCoord.xy;\n"
		"	vec2 point = vec2(p.x, image.y - p.y);\n"
		"	vec4 r = inset[eye];\n"
		"	vec4 d = vec4(point - r.xy, r.xy + r.zw - point) + open[eye] * 1.0e6;\n"
		"	vec2 e = clamp(1.0 - min(d.xy, d.zw) / image.z, 0.0, 1.0);\n"
		"	float w = 1.0 - smoothstep(0.0, 1.0, length(e));\n"
		"	colour = texture(fovea, vec3(p / image.xy, float(2 * eye)));\n"
		"	if (w > 0.0)\n"
		"	{\n"
		"		ivec2 texel = ivec2(p) - ivec2(int(r.x), int(image.y - r.y - r.w));\n"
		"		colour = mix(colour, texelFetch(fovea, ivec3(texel, 2 * eye + 1), 0), w);\n"
		"	}\n"
		"	colour.gb *= mix(1.0, mix(0.55, 1.0, w), show);\n"
		"}\n";
	float insets[2][4], open[2][4], image[4] = { 0.0f };
	int eye;

	if (!vr_gl.foveated)
		return;
	if (failed || !vr_gl.eyes_drawn || !vr_gl.eye_image)
	{
		vr_eye_target(FALSE);
		return;
	}
	if (!program)
	{
		GLuint vertex = xgpu_compile_shader(GL_VERTEX_SHADER, vertex_source, "VR foveation vertex");
		GLuint fragment = xgpu_compile_shader(GL_FRAGMENT_SHADER, fragment_source, "VR foveation pixel");
		GLint status = 0;

		program = glCreateProgram();
		glAttachShader(program, vertex);
		glAttachShader(program, fragment);
		glLinkProgram(program);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		if (!vertex || !fragment || !status)
		{
			platform_log("vr: cannot make the foveation's program; the eyes show nothing");
			failed = TRUE;
			vr_eye_target(FALSE);
			return;
		}
		glUseProgram(program);
		glUniform1i(glGetUniformLocation(program, "fovea"), 0);
		inset_location = glGetUniformLocation(program, "inset");
		open_location = glGetUniformLocation(program, "open");
		image_location = glGetUniformLocation(program, "image");
		show_location = glGetUniformLocation(program, "show");
	}
	for (eye = 0; eye < 2; eye++)
	{
		const int *corner = vr_gl.fovea_corners[eye];

		insets[eye][0] = (float)corner[0];
		insets[eye][1] = (float)corner[1];
		insets[eye][2] = (float)vr_gl.fovea_width;
		insets[eye][3] = (float)vr_gl.fovea_height;
		open[eye][0] = corner[0] <= 0 ? 1.0f : 0.0f;
		open[eye][1] = corner[1] <= 0 ? 1.0f : 0.0f;
		open[eye][2] = corner[0] + vr_gl.fovea_width >= vr_gl.eye_width ? 1.0f : 0.0f;
		open[eye][3] = corner[1] + vr_gl.fovea_height >= vr_gl.eye_height ? 1.0f : 0.0f;
	}
	image[0] = (float)vr_gl.eye_width;
	image[1] = (float)vr_gl.eye_height;
	image[2] = FOVEA_BLEND * (float)(vr_gl.fovea_width < vr_gl.fovea_height ? vr_gl.fovea_width : vr_gl.fovea_height);
	framebuffer_views = 2;
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_get(vr_gl.eye_image, 0));
	framebuffer_views = 1;
	glViewport(0, 0, vr_gl.eye_width, vr_gl.eye_height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glUseProgram(program);
	glUniform4fv(inset_location, 2, &insets[0][0]);
	glUniform4fv(open_location, 2, &open[0][0]);
	glUniform4fv(image_location, 1, image);
	glUniform1f(show_location, vr_gl.fovea_show ? 1.0f : 0.0f);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D_ARRAY, vr_gl.fovea_texture);
	/* (its own sampler: no comparison, as a shadow map's has, nor filter) */
	if (!dynamic_lights.depth_sampler)
	{
		glGenSamplers(1, &dynamic_lights.depth_sampler);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_COMPARE_MODE, GL_NONE);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	glBindSampler(0, dynamic_lights.depth_sampler);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	xgpu_gl_state_invalidate();
	vr_eye_target(FALSE);
}

static void vr_gl_initialize(void)
{
	int stage;

	if (!halo_vr_enabled())
		return;
	if (!host_gl_has_extension("GL_OVR_multiview2") || !glFramebufferTextureMultiviewOVR)
	{
		platform_log("vr: the driver has no GL_OVR_multiview2; playing flat");
		return;
	}
	if (!halo_vr_initialize())
		return;
	halo_vr_eye_size(&vr_gl.eye_width, &vr_gl.eye_height);
	halo_vr_hud_size(&vr_gl.hud_width, &vr_gl.hud_height);
	vr_gl.scope_size = halo_vr_scope_size();
	vr_gl.scope_scale = (float)vr_gl.scope_size / (float)SCREEN_HEIGHT;
	vr_gl.statistics = config_boolean("debug.gpu_stats");
	vr_gl.gpu_time = vr_gl.statistics && config_boolean("debug.vr_gpu_time");
	/* the eyes' images are sRGB (the only colour formats SteamVR offers),
	holding the game's values as they are: sampling them must not decode
	them, and the samplers override the textures' own setting */
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
		glSamplerParameteri(device.samplers[stage], GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
	samplers_skip_srgb_decode = TRUE;
	for (stage = 0; stage < SAMPLER_CACHE_SIZE; stage++)
	{
		if (sampler_cache[stage].sampler)
			glSamplerParameteri(sampler_cache[stage].sampler, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
	}
	vr_gl.eye_target.target.width = 640;
	vr_gl.eye_target.target.height = SCREEN_HEIGHT;
	vr_gl.eye_target.target.scale[0] = (float)vr_gl.eye_width / 640.0f;
	vr_gl.eye_target.target.scale[1] = (float)vr_gl.eye_height / (float)SCREEN_HEIGHT;
	vr_gl.eye_target.target.gl_width = (unsigned long)vr_gl.eye_width;
	vr_gl.eye_target.target.gl_height = (unsigned long)vr_gl.eye_height;
	vr_gl.eye_target.target.layers = 2;
	memset(vr_gl.eye_transforms, 0, sizeof(vr_gl.eye_transforms));
	for (stage = 0; stage < 4; stage++)
	{
		vr_gl.eye_transforms[0][stage * 5] = 1.0f;
		vr_gl.eye_transforms[1][stage * 5] = 1.0f;
	}
	vr_gl.screen_corrections[0][0] = vr_gl.screen_corrections[0][1] = 1.0f;
	vr_gl.screen_corrections[1][0] = vr_gl.screen_corrections[1][1] = 1.0f;
	vr_gl.eye_alone = -1;
	{
		GLint counters = 0;

		/* the eye pass's visibility tests are counted by the pixel shaders
		(XGPU_SAMPLE_COUNTERS) */
		glGetIntegerv(GL_MAX_FRAGMENT_ATOMIC_COUNTERS, &counters);
		SAMPLE_COUNTING = counters > 0;
		sample_counters_create();
		if (!SAMPLE_COUNTING)
			platform_log("vr: no atomic counters: the visibility tests are drawn in the left eye alone");
	}
	vr_fovea_initialize();
	vr_gl.ready = TRUE;
	/* the screen becomes the menus' 640x480 at the HUD's scale (the device
	is still being made: halo_screen_commit would leave its surfaces) */
	halo_screen_commit();
	device.presentation.BackBufferWidth = 640;
	d3d8_surface_resize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, 640, SCREEN_HEIGHT);
	d3d8_surface_resize(&device.depth_buffer, D3DFMT_LIN_D24S8, 640, SCREEN_HEIGHT);
	if (halo_vr_frame_active())
		halo_vr_pass(HALO_VR_PASS_HUD);
}

static BOOL vr_screen_mode(long *width, float scale[2])
{
	if (!vr_gl.ready)
		return FALSE;
	*width = 640;
	scale[0] = (float)vr_gl.hud_width / 640.0f;
	scale[1] = (float)vr_gl.hud_height / (float)SCREEN_HEIGHT;
	return TRUE;
}

void halo_vr_pass(int pass)
{
	if (!vr_gl.ready)
		return;
	if (pass != HALO_VR_PASS_NONE && !halo_vr_frame_active())
		pass = HALO_VR_PASS_NONE;
	if (pass == HALO_VR_PASS_EYES)
	{
		GLuint image = halo_vr_image(VR_SWAPCHAIN_EYES);

		if (image)
		{
			if (vr_gl.eye_image != image)
			{
				glBindTexture(GL_TEXTURE_2D_ARRAY, image);
				glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
			}
			vr_gl.eye_image = image;
			vr_gl.eye_target.target.data = device.back_buffer.Data;
			/* (foveated, its four layers, made into the image at the
			present) */
			vr_eye_target(vr_gl.foveated);
			if (vr_gl.foveated)
				vr_fovea_place();
			vr_gl.eye_target.last_rendered = device.frame + 1;
			screen_scale[0] = vr_gl.eye_target.target.scale[0];
			screen_scale[1] = vr_gl.eye_target.target.scale[1];
		}
		else
		{
			pass = HALO_VR_PASS_HUD;
		}
	}
	if (pass == HALO_VR_PASS_SCOPE && vr_gl.scope_size <= 0)
		pass = HALO_VR_PASS_HUD;
	if (pass == HALO_VR_PASS_SCOPE)
	{
		/* (its targets, at their own scale, are the game's ordinary 1-layer
		ones, apart from the HUD's: xgpu_render_target_find) */
		screen_scale[0] = screen_scale[1] = vr_gl.scope_scale;
		vr_gl.scope_drawn = TRUE;
	}
	else if (pass != HALO_VR_PASS_EYES)
	{
		screen_scale[0] = (float)vr_gl.hud_width / 640.0f;
		screen_scale[1] = (float)vr_gl.hud_height / (float)SCREEN_HEIGHT;
	}
	if (pass == HALO_VR_PASS_EYES)
		vr_timing(1);
	else if (pass == HALO_VR_PASS_HUD && vr_gl.pass == HALO_VR_PASS_EYES)
		vr_timing(2);
	vr_gl.pass = pass;
	vr_gl.multiview = FALSE;
	vr_gl.left_eye = FALSE;
	vr_gl.right_eye = FALSE;
	vr_gl.eye_alone = -1;
	/* (depth clamping: the sky's alone, halo_vr_sky) */
	glDisable(GL_DEPTH_CLAMP);
	if (pass == HALO_VR_PASS_HUD && !vr_gl.hud_drawn)
	{
		/* the frame's HUD starts transparent */
		struct render_target_entry *hud = render_target_get(&device.back_buffer);

		if (hud)
		{
			glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_get(hud->target.texture, 0));
			glDisable(GL_SCISSOR_TEST);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
			glClear(GL_COLOR_BUFFER_BIT);
			hud->last_rendered = device.frame + 1;
			vr_gl.hud_drawn = TRUE;
		}
		/* the HUD's groups at the frame's start: the centre's, and the
		other groups' targets drawn into as they are first drawn
		(vr_group_target) while the HUD has panels */
		{
			struct vr_visor_panel panels[VR_PANEL_COUNT];

			vr_gl.hud_panels = vr_visor_panels(panels) != 0;
			vr_gl.groups_drawn = 0;
			halo_vr_hud_group(HALO_VR_HUD_GROUP_CENTRE);
		}
	}
	xgpu_gl_state_invalidate();
}

void halo_vr_set_eye_transforms(const float transforms[2][16])
{
	if (memcmp(vr_gl.eye_transforms, transforms, sizeof(vr_gl.eye_transforms)))
	{
		memcpy(vr_gl.eye_transforms, transforms, sizeof(vr_gl.eye_transforms));
		vr_gl.eye_serial++;
	}
}

/* The sky is drawn about the camera at a thousandth of its size, its far
side near the far plane: in the eye pass (whose view and projection are the
union of both eyes', and moved by the head) parts of it were clipped, some
frames, leaving the clear colour where they belonged. Drawn without the
depth test, it is drawn with depth clamping instead of the near and far
planes' clipping. The rest of the eye pass is clipped as the flat screen
is. */
void halo_vr_sky(int sky)
{
	if (!vr_gl.ready)
		return;
	if (sky && vr_gl.pass == HALO_VR_PASS_EYES)
		glEnable(GL_DEPTH_CLAMP);
	else
		glDisable(GL_DEPTH_CLAMP);
}

/* the text the eye pass draws (the players' names, the waypoints' distances:
vr_render.c's marks over the world) is rasterized for the HUD's pixels, as large as it is
drawn there (text_hires.c): its glyphs are for one scale at a time, and the
eyes' would start them over twice a frame; 0 in the other passes */
static float vr_text_pixel_scale(void)
{
	return vr_gl.pass == HALO_VR_PASS_EYES && vr_gl.hud_height > 0 ? (float)vr_gl.hud_height / (float)SCREEN_HEIGHT : 0.0f;
}

/* whether the draws now are the eyes' 3D view (display.model_detail "auto") */
static int vr_drawing_eyes(void)
{
	return vr_gl.pass == HALO_VR_PASS_EYES;
}

/* the draws from now on into that eye's layer alone (0 left, 1 right), or
both (-1): the crosshairs, each eye's in a draw of its own */
void halo_vr_draw_eye(int eye)
{
	vr_gl.eye_alone = eye;
}

/* the model whose nodes' matrices are set now (rasterizer_set_model_skinning)
is mirrored, its axes left-handed (a left-hander's first-person weapon and
hands, vr.dominant_hand): what was its front is its back, until the next
model's, or the model's end */
void halo_vr_mirrored_draws(int mirrored)
{
	vr_gl.mirrored = mirrored != 0;
}

int halo_vr_mirrored(void)
{
	return vr_gl.mirrored;
}

void halo_vr_set_screen_corrections(const float corrections[2][4])
{
	static const float none[2][4] = { { 1.0f, 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f, 0.0f } };

	if (memcmp(vr_gl.screen_corrections, corrections, sizeof(vr_gl.screen_corrections)))
	{
		memcpy(vr_gl.screen_corrections, corrections, sizeof(vr_gl.screen_corrections));
		/* (identity: none of the game's points) */
		vr_gl.screen_points = memcmp(corrections, none, sizeof(none)) != 0;
		vr_gl.eye_serial++;
	}
}

/* debug.screenshot_every in VR: both eyes side by side (eyes<frame>.bmp),
the HUD over grey (hud<frame>.bmp), and both eyes as the headset shows them,
the layers over them as the frame submitted them (view<frame>.bmp: the
compositor's work, done here, roughly, for a picture) */
static void vr_write_screenshots(struct render_target_entry *hud, GLuint hud_image)
{
	unsigned long width = (unsigned long)vr_gl.eye_width, height = (unsigned long)vr_gl.eye_height;
	unsigned long row, index;
	struct vr_host_layers layers;
	struct vr_host_pose head;

	if (!*config_string("debug.screenshot_directory"))
		return;
	if (vr_gl.eyes_drawn && vr_gl.eye_target.target.texture)
	{
		unsigned char *left = read_target(vr_gl.eye_target.target.texture, 0, width, height);
		unsigned char *right = read_target(vr_gl.eye_target.target.texture, 1, width, height);
		unsigned char *both = malloc(width * 2 * height * 4);

		for (row = 0; row < height; row++)
		{
			memcpy(both + row * width * 8, left + row * width * 4, width * 4);
			memcpy(both + row * width * 8 + width * 4, right + row * width * 4, width * 4);
		}
		for (index = 0; index < width * 2 * height; index++)
			both[index * 4 + 3] = 0xff;
		write_bmp("eyes", both, width * 2, height, TRUE);
		if (halo_vr_submitted(&layers, &head) && layers.projection)
		{
			unsigned char *eyes[2];
			unsigned char *hud_pixels = layers.hud && hud_image ?
				read_target(hud_image, -1, (unsigned long)vr_gl.hud_width, (unsigned long)vr_gl.hud_height) : NULL;
			unsigned char *scope_pixels = layers.scope && vr_gl.scope_image ?
				read_target(vr_gl.scope_image, -1, (unsigned long)vr_gl.scope_size, (unsigned long)vr_gl.scope_size) :
				NULL;
			unsigned char *panel_pixels[VR_PANEL_COUNT];
			int panel_width[VR_PANEL_COUNT], panel_height[VR_PANEL_COUNT], panel;

			for (panel = 0; panel < VR_PANEL_COUNT; panel++)
			{
				halo_vr_panel_size(panel, &panel_width[panel], &panel_height[panel]);
				panel_pixels[panel] = layers.panels[panel].shown && vr_gl.panel_images[panel] ?
					read_target(vr_gl.panel_images[panel], -1, (unsigned long)panel_width[panel],
						(unsigned long)panel_height[panel]) : NULL;
			}
			eyes[0] = left;
			eyes[1] = right;
			vr_visor_composite(eyes, (int)width, (int)height, &layers, &head, hud_pixels, vr_gl.hud_width,
				vr_gl.hud_height, panel_pixels, panel_width,
				panel_height, scope_pixels, vr_gl.scope_size);
			for (panel = 0; panel < VR_PANEL_COUNT; panel++)
				free(panel_pixels[panel]);
			for (row = 0; row < height; row++)
			{
				memcpy(both + row * width * 8, left + row * width * 4, width * 4);
				memcpy(both + row * width * 8 + width * 4, right + row * width * 4, width * 4);
			}
			for (index = 0; index < width * 2 * height; index++)
				both[index * 4 + 3] = 0xff;
			write_bmp("view", both, width * 2, height, TRUE);
			/* (the scope's image alone, as it is submitted: over grey where
			it is transparent) */
			if (scope_pixels)
			{
				for (index = 0; index < (unsigned long)(vr_gl.scope_size * vr_gl.scope_size); index++)
				{
					unsigned char *pixel = scope_pixels + index * 4;
					int channel;

					for (channel = 0; channel < 3; channel++)
					{
						int value = pixel[channel] + (255 - pixel[3]) * 96 / 255;

						pixel[channel] = (unsigned char)(value > 255 ? 255 : value);
					}
					pixel[3] = 0xff;
				}
				write_bmp("scope", scope_pixels, (unsigned long)vr_gl.scope_size, (unsigned long)vr_gl.scope_size, TRUE);
			}
			free(hud_pixels);
			free(scope_pixels);
		}
		free(left);
		free(right);
		free(both);
	}
	if (vr_gl.hud_drawn && hud)
	{
		unsigned char *pixels = read_target(hud->target.texture, -1, hud->target.gl_width, hud->target.gl_height);

		for (index = 0; index < hud->target.gl_width * hud->target.gl_height; index++)
		{
			unsigned char *pixel = pixels + index * 4;
			int alpha = pixel[3], channel;

			/* (as the compositor shows it: premultiplied, over grey) */
			for (channel = 0; channel < 3; channel++)
			{
				int value = pixel[channel] + (255 - alpha) * 96 / 255;

				pixel[channel] = (unsigned char)(value > 255 ? 255 : value);
			}
			pixel[3] = 0xff;
		}
		write_bmp("hud", pixels, hud->target.gl_width, hud->target.gl_height, FALSE);
		free(pixels);
	}
}

/* a vec3 as a vec4 uniform (the guest's GL has glUniform4fv) */
static void vr_uniform3(GLint location, const float value[3])
{
	float v[4];

	v[0] = value[0];
	v[1] = value[1];
	v[2] = value[2];
	v[3] = 0.0f;
	glUniform4fv(location, 1, v);
}

/* ---------- the HUD's groups (vr.h, vr_visor.c's panels) */

void halo_vr_hud_group(int group)
{
	vr_gl.hud_group_mode = group;
	if (group >= 0 && group < HALO_VR_HUD_GROUPS)
		vr_gl.hud_group = group;
	else
		vr_gl.hud_group = HALO_VR_HUD_GROUP_CENTRE;
}

void halo_vr_hud_anchor(int corner)
{
	/* (hud_definitions.h's hud_anchor: top left, top right, bottom left,
	bottom right, centre) */
	static const int groups[5] =
	{
		HALO_VR_HUD_GROUP_WEAPON, HALO_VR_HUD_GROUP_STATUS, HALO_VR_HUD_GROUP_TRACKER, HALO_VR_HUD_GROUP_CENTRE,
		HALO_VR_HUD_GROUP_CENTRE,
	};

	if (vr_gl.hud_group_mode == HALO_VR_HUD_BY_ANCHOR && corner >= 0 && corner < 5)
		vr_gl.hud_group = groups[corner];
}

/* a group's target, as large as the HUD's, cleared as it is first drawn
into in the frame */
static GLuint vr_group_target(const struct render_target_entry *hud, int group)
{
	GLuint *texture = &vr_gl.group_textures[group];

	if (vr_gl.group_width != hud->target.gl_width || vr_gl.group_height != hud->target.gl_height)
	{
		int index;

		for (index = 0; index < HALO_VR_HUD_GROUPS; index++)
		{
			if (vr_gl.group_textures[index])
				glDeleteTextures(1, &vr_gl.group_textures[index]);
			vr_gl.group_textures[index] = 0;
		}
		vr_gl.group_width = hud->target.gl_width;
		vr_gl.group_height = hud->target.gl_height;
		vr_gl.groups_drawn = 0;
	}
	if (!*texture)
	{
		glGenTextures(1, texture);
		glBindTexture(GL_TEXTURE_2D, *texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)vr_gl.group_width, (GLsizei)vr_gl.group_height, 0, GL_BGRA,
			GL_UNSIGNED_BYTE, NULL);
		xgpu_gl_state_invalidate();
	}
	if (!(vr_gl.groups_drawn & (1u << group)))
	{
		/* (transparent, as the HUD's: halo_vr_pass) */
		glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_get(*texture, 0));
		glDisable(GL_SCISSOR_TEST);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		xgpu_gl_state_invalidate();
		vr_gl.groups_drawn |= 1u << group;
	}
	return *texture;
}

/* The world's light behind the HUD (vr.hud_react, vr_visor.c): a small map
of it in tangents from the middle of the head, worked out from the eyes'
images as drawn (with the visor's glass), each of its pixels the light
(linear) of what both eyes see in its direction, sixteen samples across its
footprint in each. It keeps its light from frame to frame, taking in a
little of each (the look's light_blend), so that what is behind the HUD
brightens and darkens it smoothly, never with each frame's detail. Read by
the HUD's copy, before which it is made. 0 if there is none. */
#define VR_LIGHT_WIDTH 64
#define VR_LIGHT_HEIGHT 52
static GLuint vr_light_map(const struct vr_visor_hud_image *look)
{
	static GLuint program, sampler, texture;
	static BOOL failed, filled;
	static GLint rows_location, eye_fov_location, reach_location, blend_location;
	static const char vertex_source[] =
		"#version 450 core\n"
		"void main()\n"
		"{\n"
		"	vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
		"	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	static const char fragment_source[] =
		"#version 450 core\n"
		"uniform sampler2DArray eyes;\n"
		/* (the map's size, each eye's transform from the head (rows) and
		field of view, as the visor's, and where the map reaches) */
		"uniform vec2 size;\n"
		"uniform vec4 rows[6];\n"
		"uniform vec4 eye_fov[2];\n"
		"uniform vec4 reach;\n"
		"uniform float blend;\n"
		"out vec4 colour;\n"
		"void main()\n"
		"{\n"
		"	float light = 0.0, samples = 0.0;\n"
		"	for (int i = 0; i < 16; i++)\n"
		"	{\n"
		"		vec2 q = (gl_FragCoord.xy + (vec2(float(i & 3), float(i >> 2)) - 1.5) * 0.25) / size;\n"
		"		vec3 d = vec3(mix(reach.x, reach.y, q.x), mix(reach.w, reach.z, q.y), -1.0);\n"
		"		for (int eye = 0; eye < 2; eye++)\n"
		"		{\n"
		/* (in the eye's view, its image's first row its bottom) */
		"			vec3 e = vec3(dot(rows[eye * 3].xyz, d), dot(rows[eye * 3 + 1].xyz, d), dot(rows[eye * 3 + 2].xyz, d));\n"
		"			vec4 f = eye_fov[eye];\n"
		"			vec2 t = e.xy / max(-e.z, 1e-4);\n"
		"			vec2 uv = (t - f.xw) / (f.yz - f.xw);\n"
		"			if (e.z < -1e-3 && uv.x > 0.0 && uv.y > 0.0 && uv.x < 1.0 && uv.y < 1.0)\n"
		"			{\n"
		"				light += dot(texture(eyes, vec3(uv, float(eye))).rgb, vec3(0.2126, 0.7152, 0.0722));\n"
		"				samples += 1.0;\n"
		"			}\n"
		"		}\n"
		"	}\n"
		/* (nothing seen there: what it had) */
		"	colour = vec4(samples > 0.0 ? light / samples : 0.0, 0.0, 0.0, samples > 0.0 ? blend : 0.0);\n"
		"}\n";
	float rows[6][4];
	int eye, row;

	if (failed || look->react <= 0.0f || !look->curve)
		return 0;
	if (!vr_gl.eyes_drawn || !vr_gl.eye_target.target.texture)
		return filled ? texture : 0;
	if (!program)
	{
		GLuint vertex = xgpu_compile_shader(GL_VERTEX_SHADER, vertex_source, "VR light vertex");
		GLuint fragment = xgpu_compile_shader(GL_FRAGMENT_SHADER, fragment_source, "VR light pixel");
		GLint status = 0;

		program = glCreateProgram();
		glAttachShader(program, vertex);
		glAttachShader(program, fragment);
		glLinkProgram(program);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		if (!vertex || !fragment || !status)
		{
			platform_log("vr: cannot make the light map's program; the HUD does not react to the light");
			failed = TRUE;
			return 0;
		}
		glUseProgram(program);
		glUniform1i(glGetUniformLocation(program, "eyes"), 0);
		glUniform2f(glGetUniformLocation(program, "size"), (GLfloat)VR_LIGHT_WIDTH, (GLfloat)VR_LIGHT_HEIGHT);
		rows_location = glGetUniformLocation(program, "rows");
		eye_fov_location = glGetUniformLocation(program, "eye_fov");
		reach_location = glGetUniformLocation(program, "reach");
		blend_location = glGetUniformLocation(program, "blend");
		/* (the eyes' sRGB decoded, for their light: the renderer's own
		samplers skip it) */
		glGenSamplers(1, &sampler);
		glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glSamplerParameteri(sampler, GL_TEXTURE_SRGB_DECODE_EXT, GL_DECODE_EXT);
		glGenTextures(1, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, VR_LIGHT_WIDTH, VR_LIGHT_HEIGHT, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
		xgpu_gl_state_invalidate();
	}
	for (eye = 0; eye < 2; eye++)
	{
		for (row = 0; row < 3; row++)
			memcpy(rows[eye * 3 + row], look->eye[eye][row], sizeof(rows[0]));
	}
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_get(texture, 0));
	glViewport(0, 0, VR_LIGHT_WIDTH, VR_LIGHT_HEIGHT);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	/* (taking in the frame's light: all of it the first time) */
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(program);
	glUniform4fv(rows_location, 6, rows[0]);
	glUniform4fv(eye_fov_location, 2, look->eye_fov[0]);
	glUniform4fv(reach_location, 1, look->light_reach);
	glUniform1f(blend_location, filled ? look->light_blend : 1.0f);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D_ARRAY, vr_gl.eye_target.target.texture);
	glBindSampler(0, sampler);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	/* (its own sampler: no comparison, as a shadow map's has, nor filter) */
	if (!dynamic_lights.depth_sampler)
	{
		glGenSamplers(1, &dynamic_lights.depth_sampler);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_COMPARE_MODE, GL_NONE);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	glBindSampler(0, dynamic_lights.depth_sampler);
	glDisable(GL_BLEND);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	xgpu_gl_state_invalidate();
	filled = TRUE;
	return texture;
}

/* the HUD's (or the menus') target into its swapchain image, upside down
for OpenXR's GL images. The game's draws leave the target's alpha as their
scratch, not as coverage (the Xbox's display ignored it), and the layer
would show nothing where it is 0: the target starts black, so a pixel's
brightness is its coverage, and the copy makes that its alpha (the colour
is then premultiplied, as the compositor's default blending expects).

On the visor (vr_visor.c) the copy curves it about the eyes: each of the
layer's pixels is a direction from the head, whose angles across and up are
where it is in the HUD (a cylinder layer is in angles across already). A
panel's pixels are directions through its plane, which faces the head from
where its region of the HUD is, tilted as it is; its region alone is shown,
turned and bent along the visor's curve as vr_visor.c has it (its wrap).
The HUD's light glows about it on the glass (a soft ring of its own
colour), and it flickers as the shields break. As light on the glass
(vr.hud_glass) it covers less of the world behind it than its brightness
(the world shows through its light), the glass's gold is a little in it
and more in its bloom, a wider ring about the glow's, and the glass's
second surface reflects it faintly, blurred, a little below. It reacts to
the world (vr.hud_react) as a helmet's display would: before bright light
the glass darkens behind it, softly about each element, and it brightens a
little, so that it reads on a sunlit beach; in the dark it dims. The light
behind it is the world's light map's (vr_light_map), in the direction of
each pixel. As the shields are hit its projection is disturbed for a
moment: bands of it shifted across and dimmed, a new pattern every few
frames, as the hit fades; and its bloom pulses red with the low shields'
warning. FALSE if the copy's program cannot be made. */
static BOOL vr_copy_hud_image(GLuint source, GLuint image, int width, int height,
	const struct vr_visor_hud_image *look, const struct vr_visor_panel *panel, GLuint light)
{
	static GLuint program, sampler, light_sampler;
	static BOOL failed;
	static GLint size_location, curve_location, angles_location, extent_location, opacity_location, glow_location;
	static GLint glass_location, react_location, reach_location, hit_location, warning_location;
	static GLint centre_location, right_location, up_location, region_location, wrap_location;
	static const char vertex_source[] =
		"#version 450 core\n"
		"void main()\n"
		"{\n"
		"	vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
		"	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	static const char fragment_source[] =
		"#version 450 core\n"
		"uniform sampler2D hud_texture;\n"
		"uniform vec2 size;\n"
		"uniform float curve;\n"
		"uniform vec2 half_angles;\n"
		"uniform vec2 extent;\n"
		"uniform float opacity;\n"
		"uniform float glow;\n"
		"uniform float glass;\n"
		/* (the world's light: how much the HUD reacts to it, and where its
		map reaches, in tangents from the middle of the head; the hit's
		disturbance and its pattern; the warning's pulse) */
		"uniform sampler2D light_map;\n"
		"uniform float react;\n"
		"uniform vec4 reach;\n"
		"uniform vec2 hit;\n"
		"uniform float warning;\n"
		/* (a quad's plane: its middle and axes from the head, at a unit's
		distance, and the HUD's region it shows) */
		"uniform vec4 centre;\n"
		"uniform vec4 right_axis;\n"
		"uniform vec4 up_axis;\n"
		"uniform vec4 region;\n"
		/* (its turn across and up along the visor's curve, and its bend) */
		"uniform vec4 wrap;\n"
		"out vec4 colour;\n"
		"void main()\n"
		"{\n"
		/* (row 0 of the image is the target's last) */
		"	vec2 p = gl_FragCoord.xy / size;\n"
		"	vec2 uv = vec2(p.x, 1.0 - p.y);\n"
		/* (the pixel's direction from the head, for the world's light) */
		"	vec3 d = vec3(0.0, 0.0, -1.0);\n"
		"	if (curve > 0.5)\n"
		"	{\n"
		/* the direction's angles across and up: through a quad's plane, or
		a cylinder's angle around and tangent up */
		"		vec2 s = (p * 2.0 - 1.0) * extent;\n"
		"		vec2 angles;\n"
		"		if (curve > 1.5)\n"
		"		{\n"
		"			angles = vec2(s.x, atan(s.y));\n"
		"			d = vec3(sin(s.x), s.y, -cos(s.x));\n"
		"		}\n"
		"		else\n"
		"		{\n"
		"			d = normalize(centre.xyz + right_axis.xyz * s.x + up_axis.xyz * s.y);\n"
		"			angles = vec2(atan(d.x, -d.z), atan(d.y, length(d.xz)));\n"
		"			angles.x -= wrap.x;\n"
		"			angles.y += wrap.z * angles.x * angles.x - wrap.y;\n"
		"		}\n"
		"		uv = vec2(0.5, 0.5) + vec2(0.5, -0.5) * angles / half_angles;\n"
		"		if (uv.x < region.x || uv.y < region.y || uv.x > region.z || uv.y > region.w)\n"
		"		{\n"
		"			colour = vec4(0.0);\n"
		"			return;\n"
		"		}\n"
		"	}\n"
		"	vec2 texel = 1.0 / vec2(textureSize(hud_texture, 0));\n"
		/* a hit: bands of the projection shifted across, each its own way,
		some dimmed */
		"	float dim = 1.0;\n"
		"	if (hit.x > 0.0)\n"
		"	{\n"
		"		float band = floor(uv.y * 48.0);\n"
		"		float h = fract(sin(band * 12.9898 + hit.y * 78.233) * 43758.5453);\n"
		"		float k = fract(sin(band * 39.3468 + hit.y * 11.135) * 24634.6345);\n"
		"		uv.x += (h - 0.5) * 0.008 * hit.x * step(0.4, k);\n"
		"		dim = 1.0 - hit.x * (0.1 + 0.2 * step(0.75, k));\n"
		"	}\n"
		"	vec4 c = texture(hud_texture, uv);\n"
		"	vec4 result = vec4(c.rgb, max(c.r, max(c.g, c.b)));\n"
		"	float coverage = result.a;\n"
		"	vec3 ring = vec3(0.0);\n"
		"	if (glow > 0.0 || react > 0.0)\n"
		"	{\n"
		"		for (int i = 0; i < 8; i++)\n"
		"		{\n"
		"			float a = float(i) * 0.785398 + 0.392699;\n"
		"			ring += texture(hud_texture, uv + vec2(cos(a), sin(a)) * texel * 5.0).rgb;\n"
		"		}\n"
		"	}\n"
		/* (the visor's gold, as the glass has it) */
		"	const vec3 gold = vec3(1.0, 0.80, 0.52);\n"
		"	if (glass > 0.0)\n"
		"	{\n"
		/* light on the glass: a little of its gold, and the world seen
		through it (its alpha, what it covers, less than its light) */
		"		result.rgb *= mix(vec3(1.0), gold, 0.12 * glass);\n"
		"		result.a *= 1.0 - 0.4 * glass;\n"
		"	}\n"
		"	if (glow > 0.0)\n"
		"	{\n"
		"		vec3 light = ring * glow * 0.11, wide = vec3(0.0);\n"
		"		for (int i = 0; i < 8 && glass > 0.0; i++)\n"
		"		{\n"
		"			float a = float(i) * 0.785398 + 0.785398;\n"
		"			wide += texture(hud_texture, uv + vec2(cos(a), sin(a)) * texel * 14.0).rgb;\n"
		"		}\n"
		/* (the bloom, wider, the glass's gold in it) */
		"		light += wide * glow * glass * 0.09 * mix(vec3(1.0), gold, 0.5);\n"
		/* (the warning's red in it, pulsing) */
		"		light = mix(light, vec3(1.0, 0.18, 0.10) * max(light.r, max(light.g, light.b)), 0.6 * warning);\n"
		"		result.rgb += light * (1.0 - result.a);\n"
		"		result.a += max(light.r, max(light.g, light.b)) * (1.0 - result.a);\n"
		"	}\n"
		"	if (glass > 0.0)\n"
		"	{\n"
		/* its reflection in the glass's other surface: faint, blurred, a
		little below (light alone: it covers nothing) */
		"		vec2 ghost = uv - vec2(0.0, 18.0) * texel;\n"
		"		vec3 reflection = texture(hud_texture, ghost + vec2(2.5, 1.5) * texel).rgb +\n"
		"			texture(hud_texture, ghost + vec2(-2.5, -1.5) * texel).rgb +\n"
		"			texture(hud_texture, ghost + vec2(1.5, -2.5) * texel).rgb +\n"
		"			texture(hud_texture, ghost + vec2(-1.5, 2.5) * texel).rgb;\n"
		"		if (curve > 0.5 && (ghost.y < region.y || ghost.y > region.w))\n"
		"			reflection = vec3(0.0);\n"
		"		result.rgb += reflection * 0.25 * 0.10 * glass * gold;\n"
		"	}\n"
		"	if (react > 0.0 && curve > 0.5)\n"
		"	{\n"
		/* the world's light behind it: before bright light the glass
		darkens behind it, softly about each element (its own light and
		its ring's), and it brightens a little; in the dark it dims */
		"		vec2 t = d.xy / max(-d.z, 1e-3);\n"
		"		float world = texture(light_map, (t - reach.xw) / (reach.yz - reach.xw)).r;\n"
		"		float bright = react * smoothstep(0.06, 0.40, world);\n"
		"		float dark = react * (1.0 - smoothstep(0.004, 0.05, world));\n"
		"		float near = clamp(2.5 * coverage + 0.2 * max(ring.r, max(ring.g, ring.b)), 0.0, 1.0);\n"
		"		result.rgb = min(result.rgb * (1.0 + 0.3 * bright), vec3(1.0));\n"
		"		result.a += 0.5 * bright * near * (1.0 - result.a);\n"
		"		result *= 1.0 - 0.3 * dark;\n"
		"	}\n"
		"	colour = result * opacity * dim;\n"
		"}\n";
	static const float none[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

	if (failed)
		return FALSE;
	if (!program)
	{
		static const float transparent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		GLuint vertex = xgpu_compile_shader(GL_VERTEX_SHADER, vertex_source, "VR HUD vertex");
		GLuint fragment = xgpu_compile_shader(GL_FRAGMENT_SHADER, fragment_source, "VR HUD pixel");
		GLint status = 0;

		program = glCreateProgram();
		glAttachShader(program, vertex);
		glAttachShader(program, fragment);
		glLinkProgram(program);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		if (!vertex || !fragment || !status)
		{
			platform_log("vr: cannot make the HUD copy's program; no HUD is shown");
			failed = TRUE;
			return FALSE;
		}
		glUseProgram(program);
		glUniform1i(glGetUniformLocation(program, "hud_texture"), 0);
		size_location = glGetUniformLocation(program, "size");
		curve_location = glGetUniformLocation(program, "curve");
		angles_location = glGetUniformLocation(program, "half_angles");
		extent_location = glGetUniformLocation(program, "extent");
		opacity_location = glGetUniformLocation(program, "opacity");
		glow_location = glGetUniformLocation(program, "glow");
		glass_location = glGetUniformLocation(program, "glass");
		react_location = glGetUniformLocation(program, "react");
		reach_location = glGetUniformLocation(program, "reach");
		hit_location = glGetUniformLocation(program, "hit");
		warning_location = glGetUniformLocation(program, "warning");
		glUniform1i(glGetUniformLocation(program, "light_map"), 1);
		centre_location = glGetUniformLocation(program, "centre");
		right_location = glGetUniformLocation(program, "right_axis");
		up_location = glGetUniformLocation(program, "up_axis");
		region_location = glGetUniformLocation(program, "region");
		wrap_location = glGetUniformLocation(program, "wrap");
		/* (nothing past the HUD's edges, where the curve and the glow reach) */
		glGenSamplers(1, &sampler);
		glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
		glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, transparent);
		/* (the light map: its edges' light beyond them) */
		glGenSamplers(1, &light_sampler);
		glSamplerParameteri(light_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glSamplerParameteri(light_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glSamplerParameteri(light_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glSamplerParameteri(light_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_get(image, 0));
	glViewport(0, 0, width, height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glUseProgram(program);
	glUniform2f(size_location, (GLfloat)width, (GLfloat)height);
	glUniform2f(angles_location, look->half_angles[0], look->half_angles[1]);
	glUniform1f(opacity_location, look->opacity);
	glUniform1f(glow_location, look->curve ? look->glow : 0.0f);
	glUniform1f(glass_location, look->curve ? look->glass : 0.0f);
	glUniform1f(react_location, look->curve && light ? look->react : 0.0f);
	glUniform4fv(reach_location, 1, look->light_reach);
	glUniform2f(hit_location, look->curve ? look->hit : 0.0f, look->hit_pattern);
	glUniform1f(warning_location, look->curve ? look->warning : 0.0f);
	if (panel)
	{
		glUniform1f(curve_location, 1.0f);
		glUniform2f(extent_location, panel->half_size[0], panel->half_size[1]);
		vr_uniform3(centre_location, panel->centre);
		vr_uniform3(right_location, panel->right);
		vr_uniform3(up_location, panel->up);
		glUniform4fv(region_location, 1, panel->region);
		vr_uniform3(wrap_location, panel->wrap);
	}
	else
	{
		static const float ahead[3] = { 0.0f, 0.0f, -1.0f }, across[3] = { 1.0f, 0.0f, 0.0f }, up[3] = { 0.0f, 1.0f, 0.0f };
		static const float straight[3] = { 0.0f, 0.0f, 0.0f };

		glUniform1f(curve_location, (GLfloat)look->curve);
		glUniform2f(extent_location, look->extent[0], look->extent[1]);
		vr_uniform3(centre_location, ahead);
		vr_uniform3(right_location, across);
		vr_uniform3(up_location, up);
		glUniform4fv(region_location, 1, none);
		vr_uniform3(wrap_location, straight);
	}
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, light);
	glBindSampler(1, light_sampler);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, source);
	glBindSampler(0, sampler);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	/* (its own sampler: no comparison, as a shadow map's has, nor filter) */
	if (!dynamic_lights.depth_sampler)
	{
		glGenSamplers(1, &dynamic_lights.depth_sampler);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_COMPARE_MODE, GL_NONE);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	glBindSampler(0, dynamic_lights.depth_sampler);
	glBindSampler(1, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return TRUE;
}

static BOOL vr_copy_hud(struct render_target_entry *hud, GLuint image)
{
	struct vr_visor_hud_image look;
	struct vr_visor_panel panels[VR_PANEL_COUNT];
	int panel;

	GLuint light;

	vr_visor_hud_image(&look);
	light = vr_light_map(&look);
	if (!vr_copy_hud_image(hud->target.texture, image, vr_gl.hud_width, vr_gl.hud_height, &look, NULL, light))
		return FALSE;
	/* the panels of the groups drawn */
	if (vr_gl.hud_panels && vr_visor_panels(panels))
	{
		for (panel = 0; panel < VR_PANEL_COUNT; panel++)
		{
			int group = panels[panel].group, width, height;
			GLuint panel_image;

			halo_vr_panel_size(panel, &width, &height);
			if (!(vr_gl.groups_drawn & (1u << group)) || !vr_gl.group_textures[group] || width <= 0 ||
				!(panel_image = halo_vr_image(VR_SWAPCHAIN_PANEL + panel)))
			{
				continue;
			}
			if (vr_copy_hud_image(vr_gl.group_textures[group], panel_image, width, height, &look, &panels[panel],
				light))
				vr_gl.panel_images[panel] = panel_image;
		}
	}
	return TRUE;
}

/* The scope's view (the scope pass, vr.scopes: port/linux/game/
vr_scope.c) into its image, upside down for OpenXR's GL images: the middle
480x480 of the 640x480 screen, where the pass drew it, as the lens, round,
the image transparent about it (premultiplied, as the HUD's). As a scope's
eyepiece does, the view darkens to black at the lens's edge (its field
stop), so that the lens's rim reads as the scope's, and the reticle is
the scope's, over it: a fine cross through the middle, thick posts from
the edge toward it (a duplex, which the eye finds at once), and a faint
lit dot at the middle, which shows against what is dark. The zoom is
shown small in the lens (about as tall as the eye sees it, whatever the
lens's size: vr_scope.c), below and right of the middle between the posts,
as the game's HUD labels it ("2x", "10x"; vr.c's halo_vr_scope_zoom), as
a scope's display shows it, in the HUD's blue with a shadow: the flat game's zoom readout,
in the HUD's layer, would float beside the lens (hud_weapon.c leaves it
out while a scope is raised).

The smart-link's window (vr.scope_view = "smartlink") is the weapon's sight
fed into the HUD: the image's middle rows are its quad (vr.c's
halo_vr_scope_window), the window within it and a soft glow of the HUD's
blue about it, nothing past that. The view in the window is the feed's: a
faint blue tint, fine scan lines and its edges darkened inside; a thin line
of the HUD's blue about it, heavier at the corners; the sight over it in
the HUD's blue: a fine crosshair across the window with a gap about the
aim, range ticks along it (across and down), a ring and a faint lit dot at
the aim; and a readout along its bottom edge (clear of the HUD's panels),
on a shadow: the zoom ("2X", "10X"), the range to what the aim meets
("RANGE 015.2"; "---.-" past the reticle's farthest) and the rounds loaded
and carried ("AMMO 4/24"), in the 3x5 cells of the zoom's readout. As it
opens (vr_scope.c grows it and pushes its view in), a bright line sweeps
down it and interference (bands offset, a little noise) settles, the sight
and the readout (typed) last; a change of the zoom's level flickers it
briefly; and as it closes it switches off as a CRT does, collapsing to a
bright line through the aim and that to a point. FALSE if the copy's
program cannot be made. */
#define SCOPE_TEXT_LENGTH 28
static BOOL vr_copy_scope(GLuint source, GLuint image, int size)
{
	static GLuint program, sampler;
	static BOOL failed;
	static GLint size_location, zoom_location, zoom_cell_location, smartlink_location, window_location,
		height_location, text_location, text_cell_location, animation_location;
	float zoom_height, reticle[2], range, animation[4] = { 0.0f };
	int zoom = (int)(halo_vr_scope_zoom(&zoom_height) + 0.5f);
	int smartlink = halo_vr_scope_smartlink(reticle);
	int rounds_loaded, rounds_carried, index;
	struct halo_vr_scope_window window;
	GLint text[SCOPE_TEXT_LENGTH];
	char line[SCOPE_TEXT_LENGTH + 1], range_text[16], rounds_text[16];
	static const char vertex_source[] =
		"#version 450 core\n"
		"void main()\n"
		"{\n"
		"	vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
		"	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	static const char fragment_source[] =
		"#version 450 core\n"
		"uniform sampler2D scope_texture;\n"
		"uniform float size;\n"
		/* (the zoom, rounded: up to 99; 0, none; and its cells' size,
		pixels) */
		"uniform int zoom;\n"
		"uniform float zoom_cell;\n"
		/* (the smart-link's window: its half width and half height, of the
		image's half width; the quad's height of its width; the readouts'
		glyphs, -1 past their ends, and their cells' size, pixels) */
		"uniform int smartlink;\n"
		"uniform vec2 window;\n"
		"uniform float height;\n"
		"uniform int text[28];\n"
		"uniform float text_cell;\n"
		/* (its animation: the seconds since it began to open, how far it
		has closed, the seconds since its level changed) */
		"uniform vec4 animation;\n"
		"out vec4 colour;\n"
		"float hash(vec2 v)\n"
		"{\n"
		"	return fract(sin(dot(v, vec2(12.9898, 78.233))) * 43758.5453);\n"
		"}\n"
		/* the readouts' glyphs, 3x5 cells, a bit each, from the top row's
		left: the digits, then X, R, A, N, G, E, M, O, /, ., - and a space */
		"const int glyphs[22] = int[22](31599, 11415, 29671, 29647, 23497, 31183, 31215, 29257, 31727,\n"
		"	31695, 2728, 27565, 11245, 27501, 14699, 31143, 24557, 11114, 4772, 2, 448, 0);\n"
		/* whether the zoom's readout lights the cell q (cells from its top
		left, down the display) */
		"float readout(vec2 q)\n"
		"{\n"
		"	if (q.x < 0.0 || q.y < 0.0 || q.y >= 5.0)\n"
		"		return 0.0;\n"
		"	ivec2 cell = ivec2(floor(q));\n"
		"	int count = zoom >= 10 ? 3 : 2, glyph = cell.x / 4, column = cell.x - glyph * 4;\n"
		"	if (glyph >= count || column > 2)\n"
		"		return 0.0;\n"
		"	int code = glyph == count - 1 ? glyphs[10] : glyphs[count == 3 && glyph == 0 ? zoom / 10 : zoom % 10];\n"
		"	return float((code >> (14 - cell.y * 3 - column)) & 1);\n"
		"}\n"
		/* whether the window's readout lights the cell q (as many of its
		glyphs as are typed) */
		"float line_lit(vec2 q, float typed)\n"
		"{\n"
		"	if (q.x < 0.0 || q.y < 0.0 || q.y >= 5.0)\n"
		"		return 0.0;\n"
		"	ivec2 cell = ivec2(floor(q));\n"
		"	int glyph = cell.x / 4, column = cell.x - glyph * 4;\n"
		"	if (glyph >= 28 || float(glyph) >= typed || column > 2 || text[glyph] < 0)\n"
		"		return 0.0;\n"
		"	return float((glyphs[text[glyph]] >> (14 - cell.y * 3 - column)) & 1);\n"
		"}\n"
		/* (how long it is, in glyphs) */
		"int line_length()\n"
		"{\n"
		"	int n = 0;\n"
		"	for (int i = 0; i < 28; i++)\n"
		"		n += text[i] >= 0 ? 1 : 0;\n"
		"	return n;\n"
		"}\n"
		/* (how much a line half w wide lights, d from it: pixels) */
		"float stroke(float d, float w)\n"
		"{\n"
		"	return clamp(w + 0.5 - abs(d), 0.0, 1.0);\n"
		"}\n"
		"void main()\n"
		"{\n"
		/* (pixels from the middle, and the lens's radius in them; row 0
		of the image is the target's last) */
		"	vec2 p = gl_FragCoord.xy - vec2(size * 0.5);\n"
		"	float radius = size * 0.5;\n"
		"	float r = length(p) / radius;\n"
		"	vec2 uv = vec2(0.125 + 0.75 * gl_FragCoord.x / size, 1.0 - gl_FragCoord.y / size);\n"
		"	vec3 c = texture(scope_texture, uv).rgb;\n"
		"	vec3 hud = vec3(0.45, 0.72, 1.0);\n"
		"	float pixel = max(size / 480.0, 1.0);\n"
		"	float alpha;\n"
		"	if (smartlink != 0)\n"
		"	{\n"
		/* the window, w its half extents (pixels); t how far it has opened
		(its size and magnification follow it: vr_scope.c) */
		"		vec2 w = window * radius;\n"
		"		vec2 a = abs(p);\n"
		"		float t = animation.x;\n"
		"		float thin = max(0.6 * size / 1024.0, 0.5);\n"
		"		vec3 white = mix(hud, vec3(1.0), 0.6);\n"
		/* switching off, as a CRT does: the feed collapses to a bright line
		through the aim (the image squashed into it), then the line to a
		point, and is gone; brighter only as it is thin */
		"		if (animation.y > 0.0)\n"
		"		{\n"
		"			float squash = smoothstep(0.0, 0.6, animation.y);\n"
		"			float shrink = smoothstep(0.6, 1.0, animation.y);\n"
		"			float hh = max(w.y * (1.0 - squash), thin), hw = w.x * (1.0 - shrink);\n"
		"			float e = clamp(min(hw - a.x, hh - a.y) / thin + 1.0, 0.0, 1.0);\n"
		"			vec2 s = vec2(p.x, clamp(p.y * w.y / hh, -w.y, w.y)) + vec2(size * 0.5);\n"
		"			c = texture(scope_texture, vec2(0.125 + 0.75 * s.x / size, 1.0 - s.y / size)).rgb;\n"
		"			c = mix(c * vec3(0.92, 1.0, 1.08), white, pow(squash, 3.0) * 0.9);\n"
		"			colour = vec4(c * e, e);\n"
		"			return;\n"
		"		}\n"
		/* connecting, as it opens: a bright line sweeps down it and
		interference (bands offset, a little noise) settles, as the zoom's
		change of level flickers; the sight and the readouts (typed) last */
		"		float corners = 1.0;\n"
		"		vec2 wf = w;\n"
		"		float scan = t;\n"
		"		float scan_y = w.y - 2.0 * w.y * scan;\n"
		"		float noise = max(1.0 - t, 0.6 * (1.0 - clamp(animation.z / 0.12, 0.0, 1.0)));\n"
		"		float tick_of = floor(t * 12.0) + 7.0 * floor(animation.z * 60.0);\n"
		"		float band = hash(vec2(floor((p.y + w.y) / (0.09 * w.y)), tick_of));\n"
		"		float offset = noise * step(0.7, band) * (band - 0.85) * 0.15 * w.x;\n"
		"		c = texture(scope_texture, vec2(0.125 + 0.75 * (gl_FragCoord.x + offset) / size, uv.y)).rgb;\n"
		"		c += noise * 0.08 * (hash(gl_FragCoord.xy + vec2(tick_of)) - 0.5);\n"
		"		float inside = min(wf.x - a.x, wf.y - a.y);\n"
		/* the feed: tinted, scan lines (about 180 down the window), the
		edges darkened inside */
		"		c *= vec3(0.92, 1.0, 1.08);\n"
		"		c *= 1.0 - 0.06 * (0.5 + 0.5 * sin(gl_FragCoord.y * 3.14159 * 180.0 / w.y));\n"
		"		c *= 0.55 + 0.45 * pow(clamp(inside / (0.12 * w.x), 0.0, 1.0), 0.6);\n"
		/* the crosshair across the window, a gap about the aim; range ticks
		along it (across both ways, down below the aim), every 0.075 of the
		half width, every other longer; the ring and the dot at the aim */
		"		float gap = 0.035 * w.x;\n"
		"		float text_top = -w.y + 0.03 * w.x + 5.0 * text_cell;\n"
		"		float sight = max(stroke(p.y, thin) * step(gap, a.x),\n"
		"			stroke(p.x, thin) * step(gap, a.y) * step(text_top + 2.0 * text_cell, p.y));\n"
		"		float spacing = 0.075 * w.x;\n"
		"		float k = floor(a.x / spacing + 0.5);\n"
		"		float tick = k >= 1.0 ? stroke(a.x - k * spacing, thin) *\n"
		"			step(a.y, (mod(k, 2.0) < 0.5 ? 0.018 : 0.010) * w.x) : 0.0;\n"
		"		float kd = floor(-p.y / spacing + 0.5);\n"
		"		tick = max(tick, kd >= 1.0 ? stroke(-p.y - kd * spacing, thin) *\n"
		"			step(a.x, (mod(kd, 2.0) < 0.5 ? 0.018 : 0.010) * w.x) : 0.0);\n"
		"		float ring = stroke(length(p) - 0.018 * w.x, 1.4 * thin);\n"
		"		float last = clamp((t - 0.75) / 0.25, 0.0, 1.0);\n"
		"		sight = max(max(sight, tick), ring) * step(0.0, inside - 0.02 * w.x) * last;\n"
		"		c = mix(c, vec3(0.0), sight * 0.35);\n"
		"		c = mix(c, hud, sight * 0.85);\n"
		"		float dot_light = clamp(1.6 * pixel + 0.5 - length(p), 0.0, 1.0) * last;\n"
		"		c = mix(c, vec3(1.0, 0.35, 0.15), dot_light * 0.8);\n"
		/* the readout, one line about the middle along the bottom edge
		inside (clear of the HUD's panels above), on its shadow */
		"		int count = line_length();\n"
		"		vec2 q = vec2(p.x / text_cell + float(count * 4 - 1) * 0.5, (text_top - p.y) / text_cell);\n"
		"		float typed = floor(last * float(count) + 0.5);\n"
		"		c = mix(c, vec3(0.0), line_lit(q - vec2(0.5, 0.5), typed) * 0.7 * step(0.0, inside));\n"
		"		c = mix(c, hud, line_lit(q, typed) * 0.95);\n"
		/* the feed; the sweep's line over it */
		"		alpha = inside > 0.0 ? 1.0 : 0.0;\n"
		"		c *= alpha;\n"
		"		float sweep = scan > 0.0 && scan < 1.0 && inside > 0.0 ?\n"
		"			max(stroke(p.y - scan_y, thin * 1.5), 0.5 * exp(-abs(p.y - scan_y) / (0.012 * w.y))) : 0.0;\n"
		"		c = mix(c, white, sweep);\n"
		"		alpha = max(alpha, sweep);\n"
		/* its line, heavier at the corners; the glow outside it */
		"		float corner = step(wf.x - 0.06 * w.x, a.x) * step(wf.y - 0.06 * w.x, a.y);\n"
		"		float edge = corners > 0.0 ?\n"
		"			max(stroke(inside, thin * 1.5) * 0.6, stroke(inside + thin * 1.5, thin * 3.0) * corner) : 0.0;\n"
		"		float glow = inside < 0.0 ? 0.35 * corners * exp(inside / (0.012 * w.x)) : 0.0;\n"
		"		c = mix(c, hud, edge);\n"
		"		alpha = max(alpha, edge);\n"
		"		c += hud * glow * (1.0 - alpha);\n"
		"		alpha = max(alpha, glow * 0.6);\n"
		"		colour = vec4(c, alpha);\n"
		"		return;\n"
		"	}\n"
		"	c *= 1.0 - smoothstep(0.86, 0.97, r);\n"
		/* the reticle: lines' half widths (pixels), the posts from 0.5 of
		the radius out */
		"	vec2 a = abs(p);\n"
		"	float fine = clamp(0.6 * pixel + 0.5 - min(a.x, a.y), 0.0, 1.0);\n"
		"	float post = clamp(2.5 * pixel + 0.5 - min(a.x, a.y), 0.0, 1.0) *\n"
		"		step(0.5 * radius, max(a.x, a.y));\n"
		"	c = mix(c, vec3(0.0), max(fine * 0.85, post));\n"
		"	float dot_light = clamp(1.6 * pixel + 0.5 - length(p), 0.0, 1.0);\n"
		"	c = mix(c, vec3(1.0, 0.35, 0.15), dot_light * 0.8);\n"
		"	alpha = 1.0 - smoothstep(1.0 - 1.5 / radius, 1.0, r);\n"
		/* the zoom: 5 cells tall, from 0.3 of the radius right of and
		below the middle; its shadow half a cell down and right */
		"	if (zoom > 0)\n"
		"	{\n"
		"		vec2 q = (vec2(p.x, -p.y) - vec2(0.3 * radius)) / zoom_cell;\n"
		"		c = mix(c, vec3(0.0), readout(q - vec2(0.5)) * 0.7);\n"
		"		c = mix(c, hud, readout(q) * 0.9);\n"
		"	}\n"
		"	colour = vec4(c * alpha, alpha);\n"
		"}\n";

	if (failed)
		return FALSE;
	if (!program)
	{
		GLuint vertex = xgpu_compile_shader(GL_VERTEX_SHADER, vertex_source, "VR scope vertex");
		GLuint fragment = xgpu_compile_shader(GL_FRAGMENT_SHADER, fragment_source, "VR scope pixel");
		GLint status = 0;

		program = glCreateProgram();
		glAttachShader(program, vertex);
		glAttachShader(program, fragment);
		glLinkProgram(program);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		if (!vertex || !fragment || !status)
		{
			platform_log("vr: cannot make the scope copy's program; no scope is shown");
			failed = TRUE;
			return FALSE;
		}
		glUseProgram(program);
		glUniform1i(glGetUniformLocation(program, "scope_texture"), 0);
		size_location = glGetUniformLocation(program, "size");
		zoom_location = glGetUniformLocation(program, "zoom");
		zoom_cell_location = glGetUniformLocation(program, "zoom_cell");
		smartlink_location = glGetUniformLocation(program, "smartlink");
		window_location = glGetUniformLocation(program, "window");
		height_location = glGetUniformLocation(program, "height");
		text_location = glGetUniformLocation(program, "text");
		text_cell_location = glGetUniformLocation(program, "text_cell");
		animation_location = glGetUniformLocation(program, "animation");
		glGenSamplers(1, &sampler);
		glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	smartlink = smartlink && halo_vr_scope_window(&window);
	/* the window's readout: the zoom, the range, the rounds (as glyphs:
	the copy's table) */
	halo_vr_scope_readout(&range, &rounds_loaded, &rounds_carried);
	if (range > 0.0f && range < 999.9f)
		snprintf(range_text, sizeof(range_text), "%05.1f", range);
	else
		snprintf(range_text, sizeof(range_text), "---.-");
	rounds_text[0] = 0;
	if (rounds_loaded >= 0)
		snprintf(rounds_text, sizeof(rounds_text), "   AMMO %d/%d", rounds_loaded, rounds_carried < 0 ? 0 : rounds_carried);
	snprintf(line, sizeof(line), "%dX   RANGE %s%s", zoom < 0 ? 0 : zoom > 99 ? 99 : zoom, range_text, rounds_text);
	for (index = 0; index < SCOPE_TEXT_LENGTH; index++)
	{
		static const char letters[] = "XRANGEMO/.- ";
		char letter = index < (int)strlen(line) ? line[index] : 0;
		const char *found = letter ? strchr(letters, letter) : NULL;

		text[index] = letter >= '0' && letter <= '9' ? letter - '0' : found ? 10 + (GLint)(found - letters) : -1;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_get(image, 0));
	glViewport(0, 0, size, size);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glUseProgram(program);
	glUniform1f(size_location, (GLfloat)size);
	glUniform1i(zoom_location, zoom < 0 ? 0 : zoom > 99 ? 99 : zoom);
	glUniform1f(zoom_cell_location, fmaxf(floorf((float)size * zoom_height / 5.0f), 1.0f));
	glUniform1i(smartlink_location, smartlink);
	glUniform2f(window_location, smartlink ? window.window[0] : 0.0f,
		smartlink ? window.window[1] * window.half[1] / window.half[0] : 0.0f);
	glUniform1f(height_location, smartlink ? window.half[1] / window.half[0] : 1.0f);
	glUniform1iv(text_location, SCOPE_TEXT_LENGTH, text);
	halo_vr_scope_animation(&animation[0], &animation[1], &animation[2]);
	glUniform4fv(animation_location, 1, animation);
	/* (the readout about 1.8% of the window's width tall) */
	glUniform1f(text_cell_location, smartlink ? fmaxf(floorf((float)size * window.window[0] * 0.0036f + 0.5f), 1.0f) :
		1.0f);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, source);
	glBindSampler(0, sampler);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	/* (its own sampler: no comparison, as a shadow map's has, nor filter) */
	if (!dynamic_lights.depth_sampler)
	{
		glGenSamplers(1, &dynamic_lights.depth_sampler);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_COMPARE_MODE, GL_NONE);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	glBindSampler(0, dynamic_lights.depth_sampler);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return TRUE;
}

/* The visor's glass (vr_visor.c): the helmet about the visor, darkening
the edges of the view; the glass, a faint gold; the warning's glow on its
rim; and the shields' energy across it, the game's own plasma
(rasterizer_xbox_plasma_energy.c): its two noise maps moving through the
glass as through the armour, combined as its pixel shader combines them
(two lookups' differences, squared and sharpened into veins), coloured
facing and grazing, as bright as the game's glow on the armour, and as the
shields recharge, filling the glass to their level, flashing as they are
full. Less in the middle of the glass than at its edges. Its pattern is in
tangents from the middle of the head (what the eyes see of the glass), its
rim a rounded rectangle of their angles; past all that they see, it fades
to nothing (vr_visor.c, visor_extent), so that it never ends in an edge.
Drawn into both eyes' images (multiview), as the rim is, after it: each
pixel where that eye's ray meets the glass's plane, VR_GLASS_DISTANCE before
the head. Not a quad layer of its own, which SteamVR draws with a line
across it this near and this wide (vr_visor.c). The eyes' images are sRGB:
for the glass alone they are blended linearly and written encoded
(GL_FRAMEBUFFER_SRGB), as the compositor blended the layer it was, its light
worked out linearly, premultiplied (the glows' alpha 0: light added). Other
effects on the glass (water, dirt, cracks,
reflections) would be drawn here too, over the glass's tint and under its
energy. */
static void vr_draw_visor(void)
{
	static GLuint program, sampler;
	static BOOL failed;
	static GLint rows_location, eye_fov_location, extent_location, fov_location, clear_location, rim_location;
	static GLint plasma_location, level_location;
	static GLint perpendicular_location, parallel_location, noise_location[2];
	static const char vertex_source[] =
		"#version 450 core\n"
		"#extension GL_OVR_multiview2 : require\n"
		"layout(num_views = 2) in;\n"
		/* each eye's transform from the head (rows) and its field of view
		(tangents: left, right, up, down), as the rim's */
		"uniform vec4 rows[6];\n"
		"uniform vec4 eye_fov[2];\n"
		/* the eye's ray through the pixel, from the head's middle (its
		direction linear across the view), and where the eye is */
		"out vec3 ray;\n"
		"flat out vec3 origin;\n"
		"void main()\n"
		"{\n"
		"	vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)) * 2.0 - 1.0;\n"
		"	int eye = int(gl_ViewID_OVR);\n"
		"	vec4 f = eye_fov[eye];\n"
		"	vec3 x = rows[eye * 3].xyz, y = rows[eye * 3 + 1].xyz, z = rows[eye * 3 + 2].xyz;\n"
		/* (the eye pass's clip space is upside down: its upper left
		origin) */
		"	vec3 local = vec3(mix(f.x, f.y, p.x * 0.5 + 0.5), mix(f.w, f.z, -p.y * 0.5 + 0.5), -1.0);\n"
		"	ray = local.x * x + local.y * y + local.z * z;\n"
		"	origin = -(rows[eye * 3].w * x + rows[eye * 3 + 1].w * y + rows[eye * 3 + 2].w * z);\n"
		"	gl_Position = vec4(p, 0.0, 1.0);\n"
		"}\n";
	static const char fragment_source[] =
		"#version 450 core\n"
		"uniform float distance;\n"
		/* (tangents: left, right, up, down) */
		"uniform vec4 extent;\n"
		"uniform vec4 fov;\n"
		/* (all that the eyes see: past it, to the extent, the glass fades
		out) */
		"uniform vec4 clear;\n"
		/* the glow on the rim (rgb) and the frame's strength (a) */
		"uniform vec4 rim;\n"
		/* the plasma: the game's glow, the recharge, its level and the flash
		as it is full; each noise map's scale, direction over its period
		(xyz) and time (w) */
		"uniform vec4 plasma;\n"
		"uniform float level;\n"
		"uniform vec4 perpendicular;\n"
		"uniform vec4 parallel;\n"
		"uniform vec4 noise[2];\n"
		"uniform vec4 noise_scale;\n"
		"uniform sampler3D noise0;\n"
		"uniform sampler3D noise1;\n"
		"in vec3 ray;\n"
		"flat in vec3 origin;\n"
		"out vec4 colour;\n"
		"vec4 over(vec4 top, vec4 under) { return top + under * (1.0 - top.a); }\n"
		"void main()\n"
		"{\n"
		/* where the eye's ray meets the glass, in tangents from the
		middle of the head (nothing where the ray does not meet it) */
		"	if (ray.z >= -1e-4)\n"
		"		discard;\n"
		"	vec2 t = (origin.xy + ray.xy * ((-distance - origin.z) / ray.z)) / distance;\n"
		"	vec2 edge = atan(vec2(t.x < 0.0 ? -fov.x : fov.y, t.y > 0.0 ? fov.z : -fov.w));\n"
		"	vec2 n = abs(atan(t)) / edge;\n"
		/* (0 in the middle, 1 at the edge of the view: the visor's rounded
		rim) */
		"	float r = pow(pow(n.x, 4.0) + pow(n.y, 4.0), 0.25);\n"
		/* the helmet's shadow on the glass: from the brow and the chin (the
		visor is wide and low), the sides' faint; never more where they
		meet, so no corners are drawn */
		"	float shade = max(t.y > 0.0 ? smoothstep(0.78, 1.06, n.y) : smoothstep(0.84, 1.06, n.y),\n"
		"		0.5 * smoothstep(0.88, 1.06, n.x));\n"
		"	vec4 light = vec4(vec3(0.85, 0.62, 0.25) * 0.035, 0.035) * rim.a * (1.0 - shade);\n"
		"	light = over(vec4(vec3(0.010, 0.011, 0.013), 1.0) * 0.45 * shade * rim.a, light);\n"
		/* the warning's glow: about the rim, fading inward well before the
		HUD's edges */
		"	light.rgb += rim.rgb * smoothstep(0.80, 1.0, r);\n"
		"	if (plasma.x + plasma.y + plasma.z > 0.0)\n"
		"	{\n"
		/* the noise maps move through the glass as through the armour: its
		directions from the eyes, on a shell about the head */
		"		vec3 d = normalize(vec3(t, -1.0)) * 0.25;\n"
		"		vec4 t0 = texture(noise0, d * noise_scale.x + noise[0].xyz * noise[0].w);\n"
		"		vec4 t1 = texture(noise1, d * noise_scale.y + noise[1].xyz * noise[1].w);\n"
		/* the plasma's combiners (rasterizer_xbox_plasma_energy.c's
		pixel shader, as nv2a_psh.c reads it) */
		"		float x = clamp(t0.b + 0.5 - t1.b, -1.0, 1.0);\n"
		"		float y = clamp(t1.a + 0.5 - t0.a, -1.0, 1.0);\n"
		"		float e = clamp(4.0 * (x >= 0.5 ? max(y, 0.0) * max(y, 0.0) : max(x, 0.0) * max(x, 0.0)), -1.0, 1.0);\n"
		"		float g = e >= 0.5 ? (2.0 * max(e, 0.0) - 1.0) * (2.0 * max(e, 0.0) - 1.0) : 0.0;\n"
		/* (facing the eyes in the middle of the glass, more grazing at its
		edges) */
		"		vec3 tint = mix(parallel.rgb, perpendicular.rgb, 1.0 - 0.15 * r * r);\n"
		"		vec3 energy = clamp((e * tint + 0.5 * g) * e * e, 0.0, 1.0);\n"
		/* how much: the armour's glow, and the recharge filling the glass
		from below to its level, brightest at its front; all of it more at
		the edges than in the middle */
		"		float height = 0.5 + 0.5 * (t.y > 0.0 ? n.y : -n.y);\n"
		"		float fill = plasma.y * (0.5 * smoothstep(level + 0.05, level - 0.05, height) +\n"
		"			0.7 * exp(-pow((height - level) / 0.07, 2.0)));\n"
		"		float amount = clamp(plasma.x + fill + 0.3 * plasma.z, 0.0, 1.0) * plasma.w *\n"
		"			mix(0.06, 1.0, smoothstep(0.45, 0.97, r)) * (1.0 - shade * 0.6);\n"
		"		light.rgb += pow(energy * amount, vec3(2.2)) * 1.0;\n"
		"	}\n"
		/* past all that the eyes see, fading to nothing at its reach, all
		of it (light and shade): no edge to see */
		"	vec4 fade = clamp((vec4(t.x, -t.x, -t.y, t.y) + vec4(-extent.x, extent.y, extent.z, -extent.w)) /\n"
		"		(vec4(-extent.x, extent.y, extent.z, -extent.w) - vec4(-clear.x, clear.y, clear.z, -clear.w)), 0.0, 1.0);\n"
		"	fade = fade * fade * (3.0 - 2.0 * fade);\n"
		"	light *= fade.x * fade.y * fade.z * fade.w;\n"
		"	colour = clamp(light, 0.0, 1.0);\n"
		"}\n";
	struct vr_visor_image look;
	GLuint noise_textures[2] = { 0, 0 };
	float rows[6][4];
	int map, eye, row;

	if (failed || !vr_gl.eyes_drawn || !vr_gl.eye_target.target.texture)
		return;
	vr_visor_image(&look);
	if (!look.shown)
		return;
	if (!program)
	{
		GLuint vertex = xgpu_compile_shader(GL_VERTEX_SHADER, vertex_source, "VR visor vertex");
		GLuint fragment = xgpu_compile_shader(GL_FRAGMENT_SHADER, fragment_source, "VR visor pixel");
		GLint status = 0;

		program = glCreateProgram();
		glAttachShader(program, vertex);
		glAttachShader(program, fragment);
		glLinkProgram(program);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		if (!vertex || !fragment || !status)
		{
			platform_log("vr: cannot make the visor's program; no visor is shown");
			failed = TRUE;
			return;
		}
		glUseProgram(program);
		rows_location = glGetUniformLocation(program, "rows");
		eye_fov_location = glGetUniformLocation(program, "eye_fov");
		glUniform1f(glGetUniformLocation(program, "distance"), VR_GLASS_DISTANCE);
		extent_location = glGetUniformLocation(program, "extent");
		fov_location = glGetUniformLocation(program, "fov");
		clear_location = glGetUniformLocation(program, "clear");
		rim_location = glGetUniformLocation(program, "rim");
		plasma_location = glGetUniformLocation(program, "plasma");
		level_location = glGetUniformLocation(program, "level");
		perpendicular_location = glGetUniformLocation(program, "perpendicular");
		parallel_location = glGetUniformLocation(program, "parallel");
		noise_location[0] = glGetUniformLocation(program, "noise");
		noise_location[1] = glGetUniformLocation(program, "noise_scale");
		glUniform1i(glGetUniformLocation(program, "noise0"), 0);
		glUniform1i(glGetUniformLocation(program, "noise1"), 1);
		/* (the noise repeats, as the plasma's draw wraps it) */
		glGenSamplers(1, &sampler);
		glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, GL_REPEAT);
	}
	/* the plasma's noise maps, as the game's draws have them */
	if (look.plasma)
	{
		for (map = 0; map < 2; map++)
		{
			struct xgpu_texture_description description;
			GLenum target = 0;
			GLuint texture = xgpu_texture_get((const DWORD *)look.noise_texture[map], NULL, &target, &description);

			if (target == GL_TEXTURE_3D)
				noise_textures[map] = texture;
		}
		if (!noise_textures[0] || !noise_textures[1])
			look.plasma = 0;
	}
	for (eye = 0; eye < 2; eye++)
	{
		for (row = 0; row < 3; row++)
			memcpy(rows[eye * 3 + row], look.eye[eye][row], sizeof(rows[0]));
	}
	framebuffer_views = 2;
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_get(vr_gl.eye_target.target.texture, 0));
	framebuffer_views = 1;
	glViewport(0, 0, vr_gl.eye_width, vr_gl.eye_height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
	glEnable(GL_FRAMEBUFFER_SRGB);
	glUseProgram(program);
	glUniform4fv(rows_location, 6, rows[0]);
	glUniform4fv(eye_fov_location, 2, look.eye_fov[0]);
	glUniform4fv(extent_location, 1, look.extent);
	glUniform4fv(fov_location, 1, look.fov);
	glUniform4fv(clear_location, 1, look.clear);
	{
		float rim[4], plasma[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, noise[2][4], scale[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

		rim[0] = look.glow[0];
		rim[1] = look.glow[1];
		rim[2] = look.glow[2];
		rim[3] = look.frame;
		if (look.plasma)
		{
			plasma[0] = look.plasma_glow;
			plasma[1] = look.charge;
			plasma[2] = look.full;
			plasma[3] = look.energy;
		}
		for (map = 0; map < 2; map++)
		{
			/* (the game's: its time over the map's period, along its
			direction; the fraction alone, as the noise repeats) */
			float cycles = look.noise_period[map] > 0.0f ? look.time / look.noise_period[map] : 0.0f;

			noise[map][0] = look.noise_direction[map][0];
			noise[map][1] = look.noise_direction[map][1];
			noise[map][2] = look.noise_direction[map][2];
			noise[map][3] = cycles - floorf(cycles);
			scale[map] = look.noise_scale[map];
		}
		glUniform4fv(rim_location, 1, rim);
		glUniform4fv(plasma_location, 1, plasma);
		glUniform1f(level_location, look.charge_level);
		vr_uniform3(perpendicular_location, look.perpendicular);
		vr_uniform3(parallel_location, look.parallel);
		glUniform4fv(noise_location[0], 2, noise[0]);
		glUniform4fv(noise_location[1], 1, scale);
	}
	for (map = 0; map < 2; map++)
	{
		glActiveTexture(GL_TEXTURE0 + map);
		glBindTexture(GL_TEXTURE_3D, noise_textures[map]);
		glBindSampler(map, sampler);
	}
	glDrawArrays(GL_TRIANGLES, 0, 3);
	for (map = 0; map < 2; map++)
	{
		glActiveTexture(GL_TEXTURE0 + map);
		glBindTexture(GL_TEXTURE_3D, 0);
		glBindSampler(map, 0);
	}
	glActiveTexture(GL_TEXTURE0);
	glDisable(GL_FRAMEBUFFER_SRGB);
	glDisable(GL_BLEND);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	xgpu_gl_state_invalidate();
}

/* The helmet's rim (vr_visor.c, vr.helmet_rim): the faceplate's lip about
the visor, drawn into both eyes' images (multiview) where it is before the
head (vr.helmet_rim_depth): the visor's opening, wide and low, its lip a
little in from the edges of what the eyes see on that plane
(vr.helmet_rim_reach), its bevel and frame curving back toward the face
beyond the eyes' views. Only the brow and the chin are there to see, the
sides barely and the corners not at all: no frame closes the view in, and
each eye sees its own side of it only, the brow and the chin with both
eyes, in depth. Its vertices are made here from their index (no buffers):
rings about the opening (the lip fading in over a few degrees, the bevel,
the frame beyond), each a loop of segments. Dark armour, its bevel lighter
than the frame behind it. Nothing of it is ever a line or an edge: it fades
in smoothly from the lip and out again at the frame's far ring (in case a
wide or turned view sees past it), with no highlight along it (a bright
thread along the brow read as a line across the view), and what is behind
an eye is clipped, not thrown across its view. Its depth is the nearest,
for the compositor's reprojection, where it covers the world. */
static void vr_draw_rim(void)
{
	enum { SEGMENTS = 128, RINGS = 4 };
	static GLuint program;
	static BOOL failed;
	static GLint rows_location, fov_location, edges_location, strength_location, pass_location, distance_location;
	static GLint reach_location;
	static const char vertex_source[] =
		"#version 450 core\n"
		"#extension GL_OVR_multiview2 : require\n"
		"layout(num_views = 2) in;\n"
		/* each eye's transform from the head (rows), its field of view
		(tangents: left, right, up, down) */
		"uniform vec4 rows[6];\n"
		"uniform vec4 fov[2];\n"
		/* the edges of the eyes' views on the lip's plane (metres: left,
		right, up, down), that plane's distance before the head */
		"uniform vec4 edges;\n"
		"uniform float distance;\n"
		/* (how far in from the edges its lip begins: a fraction of them) */
		"uniform float reach;\n"
		"out float ring_at;\n"
		"out float ring_shade;\n"
		"out float presence;\n"
		"const int SEGMENTS = 128;\n"
		/* (the rings: out from the opening, toward the face) */
		"const float out_by[4] = float[4](0.0, 0.0, 1.03, 1.7);\n"
		"const float nearer[4] = float[4](1.0, 0.99, 0.9, 0.5);\n"
		"const float shading[4] = float[4](1.7, 1.0, 0.55, 0.3);\n"
		"const ivec2 corners[6] = ivec2[6](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(0, 1), ivec2(1, 0), ivec2(1, 1));\n"
		"void main()\n"
		"{\n"
		"	int band = gl_VertexID / (SEGMENTS * 6);\n"
		"	int rest = gl_VertexID - band * SEGMENTS * 6;\n"
		"	ivec2 corner = corners[rest % 6];\n"
		"	int ring = band + corner.y;\n"
		"	float angle = float(rest / 6 + corner.x) * 6.2831853 / float(SEGMENTS);\n"
		"	vec2 c = vec2(cos(angle), sin(angle));\n"
		/* (the visor's opening, wide and low: its brow and its chin a little
		lower toward the temples) */
		"	vec2 u = sign(c) * pow(abs(c), vec2(0.25));\n"
		"	vec2 point = vec2(u.x * (u.x > 0.0 ? edges.y : -edges.x), u.y * (u.y > 0.0 ? edges.z : -edges.w) *\n"
		"		(1.0 - 0.04 * u.x * u.x));\n"
		/* the brow and the chin, the sides barely, the corners not at all */
		"	presence = 0.85 * pow(abs(c.y), 4.0) + 0.2 * pow(abs(c.x), 8.0);\n"
		"	float by = ring == 0 ? 1.0 - reach : ring == 1 ? 1.0 - 0.36 * reach : out_by[ring];\n"
		"	vec3 v = vec3(point * by, -distance) * nearer[ring];\n"
		"	int eye = int(gl_ViewID_OVR);\n"
		"	vec3 e = vec3(dot(rows[eye * 3], vec4(v, 1.0)), dot(rows[eye * 3 + 1], vec4(v, 1.0)),\n"
		"		dot(rows[eye * 3 + 2], vec4(v, 1.0)));\n"
		"	vec4 f = fov[eye];\n"
		/* (linear in the eye's coordinates, not divided here, so that what
		is behind the eye is clipped, not thrown across the view) */
		"	float w = -e.z;\n"
		"	vec2 clip = vec2((2.0 * e.x - (f.y + f.x) * w) / (f.y - f.x), (2.0 * e.y - (f.z + f.w) * w) / (f.z - f.w));\n"
		/* (the eye pass's clip space is upside down: its upper left origin) */
		"	gl_Position = vec4(clip.x, -clip.y, 0.0, w);\n"
		"	ring_at = float(ring);\n"
		"	ring_shade = shading[ring];\n"
		"}\n";
	static const char fragment_source[] =
		"#version 450 core\n"
		"uniform float strength;\n"
		"uniform float depth_pass;\n"
		"in float ring_at;\n"
		"in float ring_shade;\n"
		"in float presence;\n"
		"out vec4 colour;\n"
		"void main()\n"
		"{\n"
		/* (from nothing at the lip to the bevel's 0.85 and back to nothing at
		the far ring, smoothly: no crease to read as a line) */
		"	float a = 0.85 * smoothstep(0.0, 2.0, ring_at) * (1.0 - smoothstep(2.0, 3.0, ring_at)) * presence * strength;\n"
		"	if (depth_pass > 0.5 && a < 0.5)\n"
		"		discard;\n"
		/* dark armour, olive in the dark */
		"	vec3 armour = vec3(0.085, 0.092, 0.075) * ring_shade;\n"
		"	colour = vec4(armour * a, a);\n"
		"}\n";
	struct vr_visor_rim rim;
	struct render_target_entry *depth = NULL;
	int pass = vr_gl.pass, eye, row;
	float scale[2], rows[6][4];

	if (failed || !vr_gl.eyes_drawn || !vr_gl.eye_target.target.texture || !vr_visor_rim(&rim))
		return;
	if (!program)
	{
		GLuint vertex = xgpu_compile_shader(GL_VERTEX_SHADER, vertex_source, "VR rim vertex");
		GLuint fragment = xgpu_compile_shader(GL_FRAGMENT_SHADER, fragment_source, "VR rim pixel");
		GLint status = 0;

		program = glCreateProgram();
		glAttachShader(program, vertex);
		glAttachShader(program, fragment);
		glLinkProgram(program);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		if (!vertex || !fragment || !status)
		{
			platform_log("vr: cannot make the helmet's rim's program; no rim is drawn");
			failed = TRUE;
			return;
		}
		rows_location = glGetUniformLocation(program, "rows");
		fov_location = glGetUniformLocation(program, "fov");
		edges_location = glGetUniformLocation(program, "edges");
		strength_location = glGetUniformLocation(program, "strength");
		pass_location = glGetUniformLocation(program, "depth_pass");
		glUseProgram(program);
		distance_location = glGetUniformLocation(program, "distance");
		reach_location = glGetUniformLocation(program, "reach");
	}
	/* the eyes' depth (as vr_copy_depth finds it; foveated, none: it is
	not the eye image's) */
	scale[0] = screen_scale[0];
	scale[1] = screen_scale[1];
	vr_gl.pass = HALO_VR_PASS_EYES;
	screen_scale[0] = vr_gl.eye_target.target.scale[0];
	screen_scale[1] = vr_gl.eye_target.target.scale[1];
	depth = vr_gl.foveated ? NULL : render_target_get(&device.depth_buffer);
	vr_gl.pass = pass;
	screen_scale[0] = scale[0];
	screen_scale[1] = scale[1];
	if (depth && depth->target.layers != 2)
		depth = NULL;
	for (eye = 0; eye < 2; eye++)
	{
		for (row = 0; row < 3; row++)
			memcpy(rows[eye * 3 + row], rim.eye[eye][row], sizeof(rows[0]));
	}
	framebuffer_views = 2;
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_get(vr_gl.eye_target.target.texture, depth ? depth->target.texture : 0));
	framebuffer_views = 1;
	glViewport(0, 0, vr_gl.eye_width, vr_gl.eye_height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
	glUseProgram(program);
	glUniform4fv(rows_location, 6, rows[0]);
	glUniform4fv(fov_location, 2, rim.fov[0]);
	glUniform4fv(edges_location, 1, rim.edges);
	glUniform1f(strength_location, rim.strength);
	glUniform1f(distance_location, rim.distance);
	glUniform1f(reach_location, rim.reach);
	glUniform1f(pass_location, 0.0f);
	glDrawArrays(GL_TRIANGLES, 0, (RINGS - 1) * SEGMENTS * 6);
	if (depth)
	{
		/* where it covers, its depth: the nearest */
		glDisable(GL_BLEND);
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_ALWAYS);
		glDepthMask(GL_TRUE);
		glDepthRange(0.0, 1.0);
		glUniform1f(pass_location, 1.0f);
		glDrawArrays(GL_TRIANGLES, 0, (RINGS - 1) * SEGMENTS * 6);
		glDisable(GL_DEPTH_TEST);
	}
	glDisable(GL_BLEND);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	xgpu_gl_state_invalidate();
}

/* the eyes' tint this frame (port/linux/game/vr_seat.c's blinks, vr_death.c's
dimming as the player dies): the colour each eye's image is multiplied by
at the present, all of it, before the helmet's rim. The game's screen flash
covers its viewport only, which the eyes' images reach past at their outer
edges (by the margin of the union of their views), so a flash left those
edges lit. */
static float vr_tint[3] = { 1.0f, 1.0f, 1.0f };

/* the death's feed this frame (port/linux/game/vr_death.c, vr.death_view =
"crt"): how collapsed each eye's view is (to a line through its middle,
then the line to a point), how bright as it collapses, the shields' alarm
(static, red), whether it is the death cam's feed, and its interference;
drawn at the present (vr_draw_feed), then none */
static BOOL vr_feed_set;
static float vr_feed[8];

void halo_vr_eye_feed(float squash, float shrink, float bright, float noise, float red, float look, float interference)
{
	vr_feed_set = TRUE;
	vr_feed[0] = squash;
	vr_feed[1] = shrink;
	vr_feed[2] = bright;
	vr_feed[3] = noise;
	vr_feed[4] = red;
	vr_feed[5] = look;
	vr_feed[6] = interference;
}

void halo_vr_eye_tint(float red, float green, float blue)
{
	vr_tint[0] *= red;
	vr_tint[1] *= green;
	vr_tint[2] *= blue;
}

/* The death's feed (halo_vr_eye_feed) in both eyes' images (multiview),
before the tint and the helmet's rim: each eye's view collapsed about its
middle as a CRT switches off (squashed to a bright line of the HUD's blue,
the line shrunk to a point, the rest dark) or opened so as it switches on;
the shields' alarm's static (bands offset, noise) and red; and the death
cam's feed: a faint blue tint, scan lines, its edges darkened, interference
settling. The images are copied first (the pass reads them), and written
as they are encoded, as the rim is. */
static void vr_draw_feed(void)
{
	static GLuint program, copy, sampler;
	static int copy_width, copy_height;
	static BOOL failed;
	static GLint crt_location, look_location, image_location;
	static const char vertex_source[] =
		"#version 450 core\n"
		"#extension GL_OVR_multiview2 : require\n"
		"layout(num_views = 2) in;\n"
		"void main()\n"
		"{\n"
		"	vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
		"	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	static const char fragment_source[] =
		"#version 450 core\n"
		"#extension GL_OVR_multiview2 : require\n"
		"uniform sampler2DArray eyes;\n"
		/* (squash, shrink, bright, noise; red, the feed's look,
		interference, a seed; the image's size) */
		"uniform vec4 crt;\n"
		"uniform vec4 look;\n"
		"uniform vec2 image;\n"
		"out vec4 colour;\n"
		"float hash(vec2 v)\n"
		"{\n"
		"	return fract(sin(dot(v, vec2(12.9898, 78.233))) * 43758.5453);\n"
		"}\n"
		"void main()\n"
		"{\n"
		"	vec2 p = gl_FragCoord.xy, c = image * 0.5, d = p - c;\n"
		"	float hh = max(c.y * (1.0 - crt.x), 1.5), hw = c.x * (1.0 - crt.y);\n"
		"	float n = max(crt.w, look.z * 0.6);\n"
		"	float band = hash(vec2(floor(p.y / (image.y / 32.0)), look.w));\n"
		"	float offset = n * step(0.65, band) * (band - 0.82) * 0.12 * image.x;\n"
		"	vec2 s = vec2(p.x + offset, c.y + d.y * c.y / hh);\n"
		"	vec3 col = texture(eyes, vec3(s / image, float(gl_ViewID_OVR))).rgb;\n"
		"	col += n * 0.25 * (hash(p + vec2(look.w)) - 0.5);\n"
		"	col = mix(col, vec3(1.0, 0.16, 0.12), look.x * 0.35);\n"
		"	if (look.y > 0.5)\n"
		"	{\n"
		"		col *= vec3(0.92, 1.0, 1.08);\n"
		"		col *= 1.0 - 0.07 * (0.5 + 0.5 * sin(p.y * 3.14159 / 3.0));\n"
		"		col *= 1.0 - 0.35 * clamp((length(d) / c.y - 0.55) / 0.6, 0.0, 1.0);\n"
		"	}\n"
		"	vec3 white = vec3(0.45, 0.72, 1.0) * 0.4 + 0.6;\n"
		"	bool inside = abs(d.y) <= hh && abs(d.x) <= hw;\n"
		"	col = inside ? mix(col, white, crt.z) : vec3(0.0);\n"
		"	float glow = !inside && abs(d.x) <= hw ? exp(-(abs(d.y) - hh) / 6.0) * crt.z * 0.6 : 0.0;\n"
		"	colour = vec4(clamp(col + white * glow, 0.0, 1.0), 1.0);\n"
		"}\n";
	static unsigned long seed;
	GLuint eyes = vr_gl.eye_target.target.texture;

	if (!vr_feed_set)
		return;
	vr_feed_set = FALSE;
	if (failed || !vr_gl.eyes_drawn || !eyes)
		return;
#ifdef HALO_GLES
	if (!xgpu_capabilities.copy_image)
		return;
#endif
	if (!program)
	{
		GLuint vertex = xgpu_compile_shader(GL_VERTEX_SHADER, vertex_source, "VR feed vertex");
		GLuint fragment = xgpu_compile_shader(GL_FRAGMENT_SHADER, fragment_source, "VR feed pixel");
		GLint status = 0;

		program = glCreateProgram();
		glAttachShader(program, vertex);
		glAttachShader(program, fragment);
		glLinkProgram(program);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		if (!vertex || !fragment || !status)
		{
			platform_log("vr: cannot make the death feed's program; the death cam switches without it");
			failed = TRUE;
			return;
		}
		glUseProgram(program);
		glUniform1i(glGetUniformLocation(program, "eyes"), 0);
		crt_location = glGetUniformLocation(program, "crt");
		look_location = glGetUniformLocation(program, "look");
		image_location = glGetUniformLocation(program, "image");
		glGenSamplers(1, &sampler);
		glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	if (!copy || copy_width != vr_gl.eye_width || copy_height != vr_gl.eye_height)
	{
		if (copy)
			glDeleteTextures(1, &copy);
		glGenTextures(1, &copy);
		glBindTexture(GL_TEXTURE_2D_ARRAY, copy);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
		/* (the eyes' encoded values, as plain RGBA: sampled as they are) */
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, vr_gl.eye_width, vr_gl.eye_height, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE,
			NULL);
		copy_width = vr_gl.eye_width;
		copy_height = vr_gl.eye_height;
	}
	glCopyImageSubData(eyes, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, copy, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, vr_gl.eye_width,
		vr_gl.eye_height, 2);
	framebuffer_views = 2;
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_get(eyes, 0));
	framebuffer_views = 1;
	glViewport(0, 0, vr_gl.eye_width, vr_gl.eye_height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_FRAMEBUFFER_SRGB);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
	glUseProgram(program);
	vr_feed[7] = (float)(++seed % 97);
	glUniform4fv(crt_location, 1, &vr_feed[0]);
	glUniform4fv(look_location, 1, &vr_feed[4]);
	glUniform2f(image_location, (float)vr_gl.eye_width, (float)vr_gl.eye_height);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D_ARRAY, copy);
	glBindSampler(0, sampler);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	/* (its own sampler: no comparison, as a shadow map's has, nor filter) */
	if (!dynamic_lights.depth_sampler)
	{
		glGenSamplers(1, &dynamic_lights.depth_sampler);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_COMPARE_MODE, GL_NONE);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	glBindSampler(0, dynamic_lights.depth_sampler);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	xgpu_gl_state_invalidate();
}

/* A feed cockpit's display (vr.scorpion_view = "feed": port/linux/game/
vr_cabin.c, vr.c's halo_vr_cockpit_window) in both eyes' images
(multiview), after the foveation's composite and before the smart-link's
window (which opens within it): the cockpit's opening, which each eye sees
the world through, where each eye's ray meets the curved wall it is in,
given the feed's look as the smart-link's window has it: a faint blue tint,
scan lines, its edges darkened, a thin line of the HUD's blue about it with
heavier corners, and the readout along its bottom (the zoom, the range, the
rounds of the vehicle's weapon and its second's). It washes over the
opening from its top down as the grate closes (wash 0 to 1: a bright line
sweeping down, interference about it, the world through the bars above it
and the feed below; the readout typed last), and back up as it opens. What
is nearer than the opening (the hands) is left as it is. The images are
copied first (the pass reads them), and written as they are encoded. */
#define COCKPIT_TEXT_LENGTH 44
static void vr_draw_cockpit_display(GLuint depth_texture, int depth_layers)
{
	static GLuint program, copy, sampler;
	static int copy_width, copy_height;
	static BOOL failed;
	static GLint centre_location, up_location, forward_location, right_location, fov_location, shape_location,
		image_location, wash_location, text_location, depth_location, layers_location;
	static const char vertex_source[] =
		"#version 450 core\n"
		"#extension GL_OVR_multiview2 : require\n"
		"layout(num_views = 2) in;\n"
		"void main()\n"
		"{\n"
		"	vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
		"	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	static const char fragment_source[] =
		"#version 450 core\n"
		"#extension GL_OVR_multiview2 : require\n"
		"uniform sampler2DArray eyes;\n"
		"uniform sampler2DArray depth_texture;\n"
		/* each eye's: the wall's axis's point, its up, the way to the
		display's middle and its right (its space), its field of view
		(tangents: left, right, up, down); the wall's radius, the display's
		half angle (radians) and half height (metres); the image's size; the
		wash (0 to 1) and a seed; the readout's glyphs (-1 past its end); the
		depth's mapping (0: none) and layers (4: foveated) */
		"uniform vec4 centre[2];\n"
		"uniform vec4 up[2];\n"
		"uniform vec4 forward[2];\n"
		"uniform vec4 right[2];\n"
		"uniform vec4 fov[2];\n"
		"uniform vec4 shape;\n"
		"uniform vec2 image;\n"
		"uniform vec2 wash;\n"
		"uniform int text[44];\n"
		"uniform vec2 depth_map;\n"
		"uniform int layers;\n"
		"out vec4 colour;\n"
		"float hash(vec2 v)\n"
		"{\n"
		"	return fract(sin(dot(v, vec2(12.9898, 78.233))) * 43758.5453);\n"
		"}\n"
		/* (the smart-link's glyphs: 3x5 cells from the top row's left, the
		digits, then X, R, A, N, G, E, M, O, /, ., - and a space) */
		"const int glyphs[22] = int[22](31599, 11415, 29671, 29647, 23497, 31183, 31215, 29257, 31727,\n"
		"	31695, 2728, 27565, 11245, 27501, 14699, 31143, 24557, 11114, 4772, 2, 448, 0);\n"
		"float line_lit(vec2 q, float typed)\n"
		"{\n"
		"	if (q.x < 0.0 || q.y < 0.0 || q.y >= 5.0)\n"
		"		return 0.0;\n"
		"	ivec2 cell = ivec2(floor(q));\n"
		"	int glyph = cell.x / 4, column = cell.x - glyph * 4;\n"
		"	if (glyph >= 44 || float(glyph) >= typed || column > 2 || text[glyph] < 0)\n"
		"		return 0.0;\n"
		"	return float((glyphs[text[glyph]] >> (14 - cell.y * 3 - column)) & 1);\n"
		"}\n"
		"void main()\n"
		"{\n"
		"	int eye = int(gl_ViewID_OVR);\n"
		"	vec2 f = gl_FragCoord.xy / image;\n"
		"	vec4 t = fov[eye];\n"
		"	vec3 ray = vec3(mix(t.x, t.y, f.x), mix(t.w, t.z, f.y), -1.0);\n"
		"	vec3 u = up[eye].xyz, c = centre[eye].xyz;\n"
		/* (where the ray leaves the wall's cylinder: the eye is inside it) */
		"	vec3 rp = ray - dot(ray, u) * u, op = -c - dot(-c, u) * u;\n"
		"	float a = dot(rp, rp), b = 2.0 * dot(op, rp), k = dot(op, op) - shape.x * shape.x;\n"
		"	float disc = b * b - 4.0 * a * k;\n"
		"	if (a < 1.0e-9 || disc < 0.0)\n"
		"		discard;\n"
		"	float s = (-b + sqrt(disc)) / (2.0 * a);\n"
		"	if (s <= 0.0)\n"
		"		discard;\n"
		"	vec3 rel = ray * s - c;\n"
		"	vec2 q = vec2(atan(dot(rel, right[eye].xyz), dot(rel, forward[eye].xyz)) / shape.y, dot(rel, u) / shape.z);\n"
		"	vec2 aq = abs(q);\n"
		/* (its frame's line stands a little outside the opening) */
		"	float px = 1.5 * length(vec2(dFdx(q.y), dFdy(q.y)));\n"
		"	if (aq.x > 1.0 + 6.0 * px * shape.z / (shape.x * shape.y) || aq.y > 1.0 + 6.0 * px)\n"
		"		discard;\n"
		"	if (depth_map.x > 0.0)\n"
		"	{\n"
		"		float z = texture(depth_texture, vec3(f, float(layers == 4 ? 2 * eye : eye))).r;\n"
		"		if (z < depth_map.x * (1.0 - depth_map.y / (0.85 * s)))\n"
		"			discard;\n"
		"	}\n"
		"	vec3 hud = vec3(0.45, 0.72, 1.0), white = mix(hud, vec3(1.0), 0.6);\n"
		/* the wash: its line from the top (q.y 1) down to the bottom (-1);
		washed below it; interference about it */
		"	float line_y = 1.0 - 2.0 * wash.x;\n"
		"	float washed = wash.x >= 1.0 ? 1.0 : smoothstep(line_y + 0.02, line_y - 0.02, q.y);\n"
		"	float near_line = wash.x < 1.0 ? exp(-abs(q.y - line_y) / 0.08) : 0.0;\n"
		"	float band = hash(vec2(floor((q.y + 1.0) * 24.0), wash.y));\n"
		"	float offset = near_line * step(0.6, band) * (band - 0.8) * 0.04 * image.x;\n"
		"	vec3 col = texture(eyes, vec3((gl_FragCoord.xy + vec2(offset, 0.0)) / image, float(eye))).rgb;\n"
		"	vec3 world = texture(eyes, vec3(f, float(eye))).rgb;\n"
		"	col += near_line * 0.15 * (hash(gl_FragCoord.xy + vec2(wash.y)) - 0.5);\n"
		"	float edge = min(1.0 - aq.x * 1.0, 1.0 - aq.y);\n"
		"	vec3 feed = col * vec3(0.92, 1.0, 1.08);\n"
		"	feed *= 1.0 - 0.07 * (0.5 + 0.5 * sin(gl_FragCoord.y * 3.14159 / 3.0));\n"
		"	feed *= 0.6 + 0.4 * smoothstep(0.0, 0.12, edge);\n"
		"	vec3 result = mix(world, feed, washed);\n"
		/* the sweep's line */
		"	float sweep = wash.x > 0.0 && wash.x < 1.0 ? exp(-abs(q.y - line_y) / 0.006) : 0.0;\n"
		"	result = mix(result, white, sweep * 0.9);\n"
		/* the frame: its line, heavier at the corners, as the feed comes */
		"	float outside = max(aq.x - 1.0, aq.y - 1.0);\n"
		"	float frame = (1.0 - smoothstep(0.0, 2.0 * px, abs(outside - 2.0 * px))) * 0.7;\n"
		"	float corner = step(0.93, aq.x) * step(0.85, aq.y);\n"
		"	frame = max(frame, corner * (1.0 - smoothstep(0.0, 3.0 * px, abs(outside - 3.0 * px))));\n"
		"	result = mix(result, hud, frame * clamp(wash.x * 3.0, 0.0, 1.0));\n"
		/* the readout along the bottom, typed last */
		"	float cell = 0.016;\n"
		"	float aspect = shape.z / (shape.x * shape.y);\n"
		"	int count = 0;\n"
		"	for (int i = 0; i < 44; i++)\n"
		"		count += text[i] >= 0 ? 1 : 0;\n"
		"	vec2 g = vec2(q.x / (cell * aspect) + float(count * 4 - 1) * 0.5, (-0.86 - q.y) / cell);\n"
		"	float typed = floor(clamp((wash.x - 0.7) / 0.3, 0.0, 1.0) * float(count) + 0.5);\n"
		"	result = mix(result, vec3(0.0), line_lit(g - vec2(0.5, 0.5), typed) * 0.7 * washed);\n"
		"	result = mix(result, hud, line_lit(g, typed) * 0.95 * washed);\n"
		"	colour = vec4(clamp(result, 0.0, 1.0), 1.0);\n"
		"}\n";
	static unsigned long seed;
	struct halo_vr_cockpit_window window;
	struct halo_vr_cockpit_display display;
	float fov_rows[2][4], centre[2][4], up[2][4], forward[2][4], right[2][4];
	GLint text[COCKPIT_TEXT_LENGTH];
	char line[COCKPIT_TEXT_LENGTH + 1], range_text[16], rounds_text[24], second_text[24];
	GLuint eyes = vr_gl.eye_target.target.texture;
	int eye, index;

	if (failed || !vr_gl.eyes_drawn || !eyes || !halo_vr_cockpit_window(&window, &display) || display.wash <= 0.0f)
		return;
#ifdef HALO_GLES
	if (!xgpu_capabilities.copy_image)
		return;
#endif
	if (!program)
	{
		GLuint vertex = xgpu_compile_shader(GL_VERTEX_SHADER, vertex_source, "VR cockpit display vertex");
		GLuint fragment = xgpu_compile_shader(GL_FRAGMENT_SHADER, fragment_source, "VR cockpit display pixel");
		GLint status = 0;

		program = glCreateProgram();
		glAttachShader(program, vertex);
		glAttachShader(program, fragment);
		glLinkProgram(program);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		if (!vertex || !fragment || !status)
		{
			platform_log("vr: cannot make the cockpit display's program; the opening shows the world as it is");
			failed = TRUE;
			return;
		}
		glUseProgram(program);
		glUniform1i(glGetUniformLocation(program, "eyes"), 0);
		glUniform1i(glGetUniformLocation(program, "depth_texture"), 1);
		centre_location = glGetUniformLocation(program, "centre");
		up_location = glGetUniformLocation(program, "up");
		forward_location = glGetUniformLocation(program, "forward");
		right_location = glGetUniformLocation(program, "right");
		fov_location = glGetUniformLocation(program, "fov");
		shape_location = glGetUniformLocation(program, "shape");
		image_location = glGetUniformLocation(program, "image");
		wash_location = glGetUniformLocation(program, "wash");
		text_location = glGetUniformLocation(program, "text");
		depth_location = glGetUniformLocation(program, "depth_map");
		layers_location = glGetUniformLocation(program, "layers");
		glGenSamplers(1, &sampler);
		glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	if (!copy || copy_width != vr_gl.eye_width || copy_height != vr_gl.eye_height)
	{
		if (copy)
			glDeleteTextures(1, &copy);
		glGenTextures(1, &copy);
		glBindTexture(GL_TEXTURE_2D_ARRAY, copy);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, vr_gl.eye_width, vr_gl.eye_height, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE,
			NULL);
		copy_width = vr_gl.eye_width;
		copy_height = vr_gl.eye_height;
	}
	/* the readout: the zoom (1x: the feed), the range, the rounds (as
	glyphs: the smart-link's table) */
	if (display.range > 0.0f && display.range < 999.9f)
		snprintf(range_text, sizeof(range_text), "%05.1f", display.range);
	else
		snprintf(range_text, sizeof(range_text), "---.-");
	rounds_text[0] = second_text[0] = 0;
	if (display.rounds_loaded >= 0)
		snprintf(rounds_text, sizeof(rounds_text), "   AMMO %d/%d", display.rounds_loaded,
			display.rounds_carried < 0 ? 0 : display.rounds_carried);
	if (display.second_loaded >= 0)
		snprintf(second_text, sizeof(second_text), "   MG %d/%d", display.second_loaded,
			display.second_carried < 0 ? 0 : display.second_carried);
	snprintf(line, sizeof(line), "1X   RANGE %s%s%s", range_text, rounds_text, second_text);
	for (index = 0; index < COCKPIT_TEXT_LENGTH; index++)
	{
		static const char letters[] = "XRANGEMO/.- ";
		char letter = index < (int)strlen(line) ? line[index] : 0;
		const char *found = letter ? strchr(letters, letter) : NULL;

		text[index] = letter >= '0' && letter <= '9' ? letter - '0' : found ? 10 + (GLint)(found - letters) : -1;
	}
	for (eye = 0; eye < 2; eye++)
	{
		memcpy(fov_rows[eye], window.fov[eye], sizeof(fov_rows[eye]));
		memcpy(centre[eye], window.centre[eye], sizeof(window.centre[eye]));
		memcpy(up[eye], window.up[eye], sizeof(window.up[eye]));
		memcpy(forward[eye], window.forward[eye], sizeof(window.forward[eye]));
		memcpy(right[eye], window.right[eye], sizeof(window.right[eye]));
		centre[eye][3] = up[eye][3] = forward[eye][3] = right[eye][3] = 0.0f;
	}
	glCopyImageSubData(eyes, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, copy, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, vr_gl.eye_width,
		vr_gl.eye_height, 2);
	framebuffer_views = 2;
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_get(eyes, 0));
	framebuffer_views = 1;
	glViewport(0, 0, vr_gl.eye_width, vr_gl.eye_height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_FRAMEBUFFER_SRGB);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
	glUseProgram(program);
	glUniform4fv(centre_location, 2, centre[0]);
	glUniform4fv(up_location, 2, up[0]);
	glUniform4fv(forward_location, 2, forward[0]);
	glUniform4fv(right_location, 2, right[0]);
	glUniform4fv(fov_location, 2, fov_rows[0]);
	{
		float shape[4] = { display.radius, display.half_angle, display.half_height, 0.0f };

		glUniform4fv(shape_location, 1, shape);
	}
	glUniform2f(image_location, (float)vr_gl.eye_width, (float)vr_gl.eye_height);
	glUniform2f(wash_location, display.wash, (float)(++seed % 97));
	glUniform1iv(text_location, COCKPIT_TEXT_LENGTH, text);
	/* (the depth of a point d away, as the world's is mapped: far / (far -
	near) (1 - near / d)) */
	glUniform2f(depth_location, depth_texture && window.far_z > window.near_z ?
		window.far_z / (window.far_z - window.near_z) : 0.0f, window.near_z);
	glUniform1i(layers_location, depth_layers);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D_ARRAY, depth_texture);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glBindSampler(1, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D_ARRAY, copy);
	glBindSampler(0, sampler);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindSampler(0, 0);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	xgpu_gl_state_invalidate();
}

/* the comfort vignette (halo_vr_vignette): the eyes' images dimmed toward
the edges of each eye's view, beyond VIGNETTE_CLEAR_DEGREES (less
VIGNETTE_CLOSING_DEGREES at its strongest) from the eye's own middle (where
its view's line is: the fields of view are not even), over
VIGNETTE_FADE_DEGREES, by VIGNETTE_DIMMING at its strongest: a little
narrower and darker the stronger; drawn before the helmet's rim, after the
tint (multiplied too) */
#define VIGNETTE_CLEAR_DEGREES 50.0f
#define VIGNETTE_CLOSING_DEGREES 20.0f
#define VIGNETTE_FADE_DEGREES 22.0f
#define VIGNETTE_DIMMING 0.65f
static void vr_draw_vignette(void)
{
	static GLuint program;
	static BOOL failed;
	static GLint fov_location, clear_location, fade_location, dimming_location;
	static const char vertex_source[] =
		"#version 450 core\n"
		"#extension GL_OVR_multiview2 : require\n"
		"layout(num_views = 2) in;\n"
		"out vec2 at;\n"
		"void main()\n"
		"{\n"
		"	vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
		"	at = p;\n"
		"	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	static const char fragment_source[] =
		"#version 450 core\n"
		"#extension GL_OVR_multiview2 : require\n"
		/* (each eye's tangents: left, right, up, down; the angles, radians) */
		"uniform vec4 fov[2];\n"
		"uniform float clear;\n"
		"uniform float fade;\n"
		"uniform float dimming;\n"
		"in vec2 at;\n"
		"out vec4 colour;\n"
		"void main()\n"
		"{\n"
		"	vec4 f = fov[int(gl_ViewID_OVR)];\n"
		"	vec2 t = vec2(mix(f.x, f.y, at.x), mix(f.w, f.z, at.y));\n"
		"	float angle = atan(length(t));\n"
		"	float m = 1.0 - dimming * smoothstep(clear, clear + fade, angle);\n"
		"	colour = vec4(m, m, m, 1.0);\n"
		"}\n";
	float fov[2][4], strength = halo_vr_vignette(fov);
	float radians = 3.14159265f / 180.0f;

	if (failed || strength <= 0.0f || !vr_gl.eyes_drawn || !vr_gl.eye_target.target.texture)
		return;
	if (!program)
	{
		GLuint vertex = xgpu_compile_shader(GL_VERTEX_SHADER, vertex_source, "VR vignette vertex");
		GLuint fragment = xgpu_compile_shader(GL_FRAGMENT_SHADER, fragment_source, "VR vignette pixel");
		GLint status = 0;

		program = glCreateProgram();
		glAttachShader(program, vertex);
		glAttachShader(program, fragment);
		glLinkProgram(program);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		if (!vertex || !fragment || !status)
		{
			platform_log("vr: cannot make the comfort vignette's program; none is drawn");
			failed = TRUE;
			return;
		}
		fov_location = glGetUniformLocation(program, "fov");
		clear_location = glGetUniformLocation(program, "clear");
		fade_location = glGetUniformLocation(program, "fade");
		dimming_location = glGetUniformLocation(program, "dimming");
	}
	framebuffer_views = 2;
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_get(vr_gl.eye_target.target.texture, 0));
	framebuffer_views = 1;
	glViewport(0, 0, vr_gl.eye_width, vr_gl.eye_height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_ZERO, GL_SRC_COLOR);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
	glUseProgram(program);
	glUniform4fv(fov_location, 2, fov[0]);
	glUniform1f(clear_location, (VIGNETTE_CLEAR_DEGREES - VIGNETTE_CLOSING_DEGREES * strength) * radians);
	glUniform1f(fade_location, VIGNETTE_FADE_DEGREES * radians);
	glUniform1f(dimming_location, VIGNETTE_DIMMING * strength);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glDisable(GL_BLEND);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	xgpu_gl_state_invalidate();
}

static void vr_draw_tint(void)
{
	static GLuint program;
	static BOOL failed;
	static GLint tint_location;
	static const char vertex_source[] =
		"#version 450 core\n"
		"#extension GL_OVR_multiview2 : require\n"
		"layout(num_views = 2) in;\n"
		"void main()\n"
		"{\n"
		"	vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
		"	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	static const char fragment_source[] =
		"#version 450 core\n"
		"uniform vec4 tint;\n"
		"out vec4 colour;\n"
		"void main()\n"
		"{\n"
		"	colour = tint;\n"
		"}\n";
	float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	int channel;

	memcpy(tint, vr_tint, sizeof(vr_tint));
	vr_tint[0] = vr_tint[1] = vr_tint[2] = 1.0f;
	if (failed || !vr_gl.eyes_drawn || !vr_gl.eye_target.target.texture ||
		(tint[0] >= 1.0f && tint[1] >= 1.0f && tint[2] >= 1.0f))
	{
		return;
	}
	if (!program)
	{
		GLuint vertex = xgpu_compile_shader(GL_VERTEX_SHADER, vertex_source, "VR tint vertex");
		GLuint fragment = xgpu_compile_shader(GL_FRAGMENT_SHADER, fragment_source, "VR tint pixel");
		GLint status = 0;

		program = glCreateProgram();
		glAttachShader(program, vertex);
		glAttachShader(program, fragment);
		glLinkProgram(program);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		if (!vertex || !fragment || !status)
		{
			platform_log("vr: cannot make the eyes' tint's program; no blink or tint is drawn");
			failed = TRUE;
			return;
		}
		tint_location = glGetUniformLocation(program, "tint");
	}
	framebuffer_views = 2;
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_get(vr_gl.eye_target.target.texture, 0));
	framebuffer_views = 1;
	glViewport(0, 0, vr_gl.eye_width, vr_gl.eye_height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_ZERO, GL_SRC_COLOR);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
	glUseProgram(program);
	for (channel = 0; channel < 3; channel++)
		tint[channel] = tint[channel] > 0.0f ? tint[channel] : 0.0f;
	glUniform4fv(tint_location, 1, tint);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glDisable(GL_BLEND);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	xgpu_gl_state_invalidate();
}

/* the eyes' depth into its swapchain (XR_KHR_composition_layer_depth),
for the compositor's reprojection: the game's depth buffer has a stencil,
which OpenXR's depth formats have not, so a full-screen draw copies it,
both eyes at once */
static void vr_copy_depth(void)
{
	static GLuint program;
	static const char vertex_source[] =
		"#version 450 core\n"
		"#extension GL_OVR_multiview2 : require\n"
		"layout(num_views = 2) in;\n"
		"out vec2 uv;\n"
		"void main()\n"
		"{\n"
		"	vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
		/* (the upper-left clip origin: row 0 at the top) */
		"	uv = vec2(p.x, 1.0 - p.y);\n"
		"	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	static const char fragment_source[] =
		"#version 450 core\n"
		"#extension GL_OVR_multiview2 : require\n"
		"uniform sampler2DArray depth_texture;\n"
		"in vec2 uv;\n"
		"void main()\n"
		"{\n"
		"	gl_FragDepth = texture(depth_texture, vec3(uv, float(gl_ViewID_OVR))).r;\n"
		"}\n";
	struct render_target_entry *depth;
	GLuint image;
	int pass = vr_gl.pass;
	float scale[2];

	if (!vr_gl.eyes_drawn || !halo_vr_depth_wanted())
		return;
	if (vr_gl.foveated)
	{
		/* (the depth buffer is the four layers', not the eye images') */
		static BOOL said;

		if (!said)
			platform_log("vr: foveated: the eyes' depth is not submitted (vr.depth)");
		said = TRUE;
		return;
	}
	scale[0] = screen_scale[0];
	scale[1] = screen_scale[1];
	vr_gl.pass = HALO_VR_PASS_EYES;
	screen_scale[0] = vr_gl.eye_target.target.scale[0];
	screen_scale[1] = vr_gl.eye_target.target.scale[1];
	depth = render_target_get(&device.depth_buffer);
	vr_gl.pass = pass;
	screen_scale[0] = scale[0];
	screen_scale[1] = scale[1];
	if (!depth || depth->target.layers != 2 || !(image = halo_vr_image(VR_SWAPCHAIN_DEPTH)))
		return;
	if (!program)
	{
		GLuint vertex = xgpu_compile_shader(GL_VERTEX_SHADER, vertex_source, "VR depth vertex");
		GLuint fragment = xgpu_compile_shader(GL_FRAGMENT_SHADER, fragment_source, "VR depth pixel");
		GLint status = 0;

		program = glCreateProgram();
		glAttachShader(program, vertex);
		glAttachShader(program, fragment);
		glLinkProgram(program);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		if (!vertex || !fragment || !status)
		{
			platform_log("vr: cannot make the depth copy's program; no depth is submitted");
			vr_gl.depth_failed = TRUE;
		}
		glUseProgram(program);
		glUniform1i(glGetUniformLocation(program, "depth_texture"), 0);
	}
	if (vr_gl.depth_failed)
		return;
	framebuffer_views = 2;
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_get(0, image));
	framebuffer_views = 1;
	glViewport(0, 0, vr_gl.eye_width, vr_gl.eye_height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_ALWAYS);
	glDepthMask(GL_TRUE);
	glDepthRange(0.0, 1.0);
	glUseProgram(program);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D_ARRAY, depth->target.texture);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	/* (its own sampler: no comparison, as a shadow map's has, nor filter) */
	if (!dynamic_lights.depth_sampler)
	{
		glGenSamplers(1, &dynamic_lights.depth_sampler);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_COMPARE_MODE, GL_NONE);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	glBindSampler(0, dynamic_lights.depth_sampler);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	xgpu_gl_state_invalidate();
	halo_vr_depth_written();
}

/* The smart-link's window (vr_scope.c, vr_copy_scope's image) drawn into
both eyes' images (multiview), where each eye's ray meets its quad (vr.c's
halo_vr_scope_window: at the reticle's depth, so that each eye sees it about
the reticle), after the foveation's composite and before the tint, the
helmet's rim and the visor: over the world and the weapon in hand, under the
HUD's layer. Where the weapon in hand is (the eyes' depth nearer than
SCOPE_GUN_METRES: the first-person weapon and the hands), vr.scope_gun: the
window over it ("hidden"), the weapon a fifth through it ("ghost"), or the
weapon in front of it ("visible"). Blended as the rim is (the eyes' images'
encoded values, premultiplied). */
#define SCOPE_GUN_METRES 1.2f
static void vr_draw_scope_window(GLuint depth_texture, int depth_layers)
{
	static GLuint program, sampler;
	static BOOL failed;
	static GLint centre_location, right_location, up_location, half_location, fov_location, height_location,
		image_location, gun_location, gun_depth_location, layers_location;
	static const char vertex_source[] =
		"#version 450 core\n"
		"#extension GL_OVR_multiview2 : require\n"
		"layout(num_views = 2) in;\n"
		"void main()\n"
		"{\n"
		"	vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
		"	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	static const char fragment_source[] =
		"#version 450 core\n"
		"#extension GL_OVR_multiview2 : require\n"
		"uniform sampler2D window_texture;\n"
		"uniform sampler2DArray depth_texture;\n"
		/* each eye's: the quad's middle, right and up (its space), its
		field of view (tangents: left, right, up, down); the quad's half
		width and height (metres), its height of its width in the image; the
		eye's image's size; how the weapon shows (0 hidden, 1 ghost, 2
		visible), the depth nearer than which it is, the depth's layers (4:
		foveated, the periphery's) */
		"uniform vec4 centre[2];\n"
		"uniform vec4 right[2];\n"
		"uniform vec4 up[2];\n"
		"uniform vec4 fov[2];\n"
		"uniform vec2 half_size;\n"
		"uniform float height;\n"
		"uniform vec2 image;\n"
		"uniform int gun;\n"
		"uniform float gun_depth;\n"
		"uniform int layers;\n"
		"out vec4 colour;\n"
		"void main()\n"
		"{\n"
		"	int eye = int(gl_ViewID_OVR);\n"
		"	vec2 f = gl_FragCoord.xy / image;\n"
		/* (the eye images' rows from the bottom of the view, as OpenXR's GL
		images are: the first row the view's lowest) */
		"	vec4 t = fov[eye];\n"
		"	vec3 ray = vec3(mix(t.x, t.y, f.x), mix(t.w, t.z, f.y), -1.0);\n"
		"	vec3 n = cross(right[eye].xyz, up[eye].xyz);\n"
		"	float facing = dot(ray, n);\n"
		"	if (abs(facing) < 1.0e-6)\n"
		"		discard;\n"
		"	float along = dot(centre[eye].xyz, n) / facing;\n"
		"	if (along <= 0.0)\n"
		"		discard;\n"
		"	vec3 d = ray * along - centre[eye].xyz;\n"
		"	vec2 q = vec2(dot(d, right[eye].xyz) / half_size.x, dot(d, up[eye].xyz) / half_size.y);\n"
		"	if (abs(q.x) > 1.0 || abs(q.y) > 1.0)\n"
		"		discard;\n"
		"	vec4 c = texture(window_texture, vec2(0.5 + 0.5 * q.x, 0.5 + 0.5 * q.y * height));\n"
		"	if (gun != 0)\n"
		"	{\n"
		"		float z = texture(depth_texture, vec3(f, float(layers == 4 ? 2 * eye : eye))).r;\n"
		"		if (z < gun_depth)\n"
		"			c *= gun == 1 ? 0.8 : 0.0;\n"
		"	}\n"
		"	colour = c;\n"
		"}\n";
	struct halo_vr_scope_window window;
	float fov_rows[2][4], centre[2][4], right[2][4], up[2][4];
	int gun = halo_vr_scope_gun(), eye;

	if (failed || !vr_gl.eyes_drawn || !vr_gl.eye_target.target.texture || !vr_gl.scope_window_texture ||
		!halo_vr_scope_window(&window))
	{
		return;
	}
	if (!program)
	{
		GLuint vertex = xgpu_compile_shader(GL_VERTEX_SHADER, vertex_source, "VR scope window vertex");
		GLuint fragment = xgpu_compile_shader(GL_FRAGMENT_SHADER, fragment_source, "VR scope window pixel");
		GLint status = 0;

		program = glCreateProgram();
		glAttachShader(program, vertex);
		glAttachShader(program, fragment);
		glLinkProgram(program);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		if (!vertex || !fragment || !status)
		{
			platform_log("vr: cannot make the scope window's program; no smart-link window is shown");
			failed = TRUE;
			return;
		}
		glUseProgram(program);
		glUniform1i(glGetUniformLocation(program, "window_texture"), 0);
		glUniform1i(glGetUniformLocation(program, "depth_texture"), 1);
		centre_location = glGetUniformLocation(program, "centre");
		right_location = glGetUniformLocation(program, "right");
		up_location = glGetUniformLocation(program, "up");
		fov_location = glGetUniformLocation(program, "fov");
		half_location = glGetUniformLocation(program, "half_size");
		height_location = glGetUniformLocation(program, "height");
		image_location = glGetUniformLocation(program, "image");
		gun_location = glGetUniformLocation(program, "gun");
		gun_depth_location = glGetUniformLocation(program, "gun_depth");
		layers_location = glGetUniformLocation(program, "layers");
		glGenSamplers(1, &sampler);
		glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	if (!depth_texture)
		gun = 0;
	for (eye = 0; eye < 2; eye++)
	{
		/* (left, right, up, down) */
		fov_rows[eye][0] = window.fov[eye][0];
		fov_rows[eye][1] = window.fov[eye][1];
		fov_rows[eye][2] = window.fov[eye][2];
		fov_rows[eye][3] = window.fov[eye][3];
		memcpy(centre[eye], window.centre[eye], sizeof(window.centre[eye]));
		memcpy(right[eye], window.right[eye], sizeof(window.right[eye]));
		memcpy(up[eye], window.up[eye], sizeof(window.up[eye]));
		centre[eye][3] = right[eye][3] = up[eye][3] = 0.0f;
	}
	framebuffer_views = 2;
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_get(vr_gl.eye_target.target.texture, 0));
	framebuffer_views = 1;
	glViewport(0, 0, vr_gl.eye_width, vr_gl.eye_height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
	glUseProgram(program);
	glUniform4fv(centre_location, 2, centre[0]);
	glUniform4fv(right_location, 2, right[0]);
	glUniform4fv(up_location, 2, up[0]);
	glUniform4fv(fov_location, 2, fov_rows[0]);
	glUniform2f(half_location, window.half[0], window.half[1]);
	glUniform1f(height_location, window.half[1] / window.half[0]);
	glUniform2f(image_location, (float)vr_gl.eye_width, (float)vr_gl.eye_height);
	glUniform1i(gun_location, gun);
	/* (the depth of a point SCOPE_GUN_METRES away, as the world's depth is
	mapped: 0 near, 1 far) */
	glUniform1f(gun_depth_location, window.far_z > window.near_z && SCOPE_GUN_METRES > window.near_z ?
		window.far_z / (window.far_z - window.near_z) * (1.0f - window.near_z / SCOPE_GUN_METRES) : 0.0f);
	glUniform1i(layers_location, depth_layers);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D_ARRAY, depth_texture);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glBindSampler(1, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, vr_gl.scope_window_texture);
	glBindSampler(0, sampler);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	/* (its own sampler: no comparison, as a shadow map's has, nor filter) */
	if (!dynamic_lights.depth_sampler)
	{
		glGenSamplers(1, &dynamic_lights.depth_sampler);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_COMPARE_MODE, GL_NONE);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glSamplerParameteri(dynamic_lights.depth_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	glBindSampler(0, dynamic_lights.depth_sampler);
	glDisable(GL_BLEND);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	xgpu_gl_state_invalidate();
}

/* a test's screenshot of this frame (as debug.screenshot_every's, into
debug.screenshot_directory: port/linux/game/vr_death.c's) */
static BOOL vr_screenshot_wanted;

void halo_vr_test_screenshot(void)
{
	vr_screenshot_wanted = TRUE;
}

/* presents a VR frame: the HUD copied into its image, upside down for
OpenXR's GL images (the eyes were drawn so), and the frame ended */
static void vr_present(BOOL screenshot)
{
	struct render_target_entry *hud = NULL, *eye_depth = NULL;
	GLuint hud_image = 0;

	vr_timing(3);
	/* the eyes' depth as drawn (foveated, the four layers'), for the
	smart-link's window */
	if (vr_gl.eyes_drawn)
	{
		int pass = vr_gl.pass;
		float scale[2];

		scale[0] = screen_scale[0];
		scale[1] = screen_scale[1];
		vr_gl.pass = HALO_VR_PASS_EYES;
		screen_scale[0] = vr_gl.eye_target.target.scale[0];
		screen_scale[1] = vr_gl.eye_target.target.scale[1];
		eye_depth = render_target_get(&device.depth_buffer);
		vr_gl.pass = pass;
		screen_scale[0] = scale[0];
		screen_scale[1] = scale[1];
		if (eye_depth && eye_depth->target.layers != (vr_gl.foveated ? 4 : 2))
			eye_depth = NULL;
	}
	/* foveated, the eyes' images made from the inset and the periphery,
	which the rest draws into */
	vr_fovea_composite();
	/* a feed cockpit's display, under the smart-link's window */
	vr_draw_cockpit_display(eye_depth ? eye_depth->target.texture : 0, eye_depth ? eye_depth->target.layers : 0);
	/* the scope's view: the smart-link's window drawn into the eyes, or
	the lens's image for its layer */
	vr_gl.scope_image = 0;
	if (vr_gl.scope_drawn && vr_gl.eyes_drawn)
	{
		struct render_target_entry *scope;
		struct halo_vr_scope_window window;
		int pass = vr_gl.pass;
		float scale[2];
		GLuint image;

		scale[0] = screen_scale[0];
		scale[1] = screen_scale[1];
		vr_gl.pass = HALO_VR_PASS_SCOPE;
		screen_scale[0] = screen_scale[1] = vr_gl.scope_scale;
		scope = render_target_get(&device.back_buffer);
		vr_gl.pass = pass;
		screen_scale[0] = scale[0];
		screen_scale[1] = scale[1];
		if (scope && halo_vr_scope_window(&window))
		{
			if (!vr_gl.scope_window_texture)
			{
				glGenTextures(1, &vr_gl.scope_window_texture);
				glBindTexture(GL_TEXTURE_2D, vr_gl.scope_window_texture);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, vr_gl.scope_size, vr_gl.scope_size, 0, GL_RGBA,
					GL_UNSIGNED_BYTE, NULL);
				xgpu_gl_state_invalidate();
			}
			if (vr_copy_scope(scope->target.texture, vr_gl.scope_window_texture, vr_gl.scope_size))
				vr_draw_scope_window(eye_depth ? eye_depth->target.texture : 0, eye_depth ? eye_depth->target.layers : 0);
		}
		else if (scope && (image = halo_vr_image(VR_SWAPCHAIN_SCOPE)) != 0 &&
			vr_copy_scope(scope->target.texture, image, vr_gl.scope_size))
		{
			vr_gl.scope_image = image;
		}
	}
	/* the eyes' tint (a blink, the dead's), then the helmet's rim and the
	visor's glass in the eyes (not in the menus: vr_visor.c), the rim's depth
	with theirs */
	vr_draw_feed();
	vr_draw_tint();
	vr_draw_vignette();
	vr_draw_rim();
	vr_draw_visor();
	vr_copy_depth();
	memset(vr_gl.panel_images, 0, sizeof(vr_gl.panel_images));
	if (vr_gl.hud_drawn)
	{
		int pass = vr_gl.pass;

		vr_gl.pass = HALO_VR_PASS_HUD;
		screen_scale[0] = (float)vr_gl.hud_width / 640.0f;
		screen_scale[1] = (float)vr_gl.hud_height / (float)SCREEN_HEIGHT;
		hud = render_target_get(&device.back_buffer);
		vr_gl.pass = pass;
	}
	if (hud)
	{
		hud_image = halo_vr_image(VR_SWAPCHAIN_HUD);
		if (!hud_image || !vr_copy_hud(hud, hud_image))
			hud_image = 0;
	}
	vr_gl.pass = HALO_VR_PASS_NONE;
	vr_gl.multiview = FALSE;
	vr_gl.left_eye = FALSE;
	vr_gl.right_eye = FALSE;
	vr_gl.eye_alone = -1;
	xgpu_gl_state_invalidate();
	/* (the GPU starts on the frame while the game updates: halo_vr_present) */
	glFlush();
	if (vr_gl.gpu_time)
	{
		/* debug.vr_gpu_time: the driver submits a frame's work at the flush
		(Zink records it until then), so waiting for it to finish from here
		times the GPU's work on the frame, at the cost of the overlap */
		Uint64 start = SDL_GetTicksNS();
		double milliseconds;

		glFinish();
		milliseconds = (double)(SDL_GetTicksNS() - start) * 1e-6;
		vr_gl.gpu_milliseconds += milliseconds;
		if (milliseconds > vr_gl.gpu_worst)
			vr_gl.gpu_worst = milliseconds;
		vr_gl.gpu_frames++;
	}
	halo_vr_present(vr_gl.eyes_drawn, hud_image != 0, vr_gl.scope_image != 0);
	if (screenshot)
		vr_write_screenshots(hud, hud_image);
	vr_gl.eyes_drawn = FALSE;
	vr_gl.hud_drawn = FALSE;
	vr_gl.scope_drawn = FALSE;
	vr_gl.groups_drawn = 0;
	halo_vr_hud_group(HALO_VR_HUD_GROUP_CENTRE);
	vr_timing(0);
}

/* the next frame begins when the game first draws (vr.c waits for it
then): until the 3D view, everything goes to the HUD */
static void vr_frame_resume(void)
{
	if (vr_gl.ready && vr_gl.pass == HALO_VR_PASS_NONE && halo_vr_frame_active())
		halo_vr_pass(HALO_VR_PASS_HUD);
}
#endif

/* the end of a frame's GL work: the streamed data and visibility results of
the frames the GPU may still be drawing stay as they are */
static void frame_end_buffers(void)
{
#ifdef XGPU_SAMPLE_COUNTERS
	if (SAMPLE_COUNTING)
	{
		/* this frame's counts, for when the GPU is done with it (the
		barrier makes the shaders' counter writes visible to the copy,
		which ES 3.1 does not promise without one) */
		glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
		glBindBuffer(GL_COPY_READ_BUFFER, device.visibility_counters);
		glBindBuffer(GL_COPY_WRITE_BUFFER, device.counter_snapshots[device.buffer_ring]);
		glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0,
			VISIBILITY_TEST_SLOTS * sizeof(GLuint));
		glBindBuffer(GL_COPY_READ_BUFFER, 0);
		glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
	}
#endif
#ifdef XGPU_STREAM_RING
	host_gl_fence_frame((unsigned int)device.buffer_ring);
	device.buffer_ring = (device.buffer_ring + 1) % STREAM_BUFFER_RING;
	host_gl_wait_frame((unsigned int)device.buffer_ring);
#endif
#ifdef XGPU_SAMPLE_COUNTERS
	if (SAMPLE_COUNTING && device.ring_test_count[device.buffer_ring])
	{
		unsigned long ring = device.buffer_ring;
		unsigned long test;

		/* the GPU has passed that frame's fence: its copy is complete,
		and the slot is free for this frame's tests */
		host_gl_read_buffer(device.counter_snapshots[ring], 0, VISIBILITY_TEST_SLOTS * sizeof(GLuint),
			device.counter_values);
		for (test = 0; test < device.ring_test_count[ring]; test++)
		{
			device.visibility_latest[device.ring_tests[ring][test][0]] =
				device.counter_values[device.ring_tests[ring][test][1]];
		}
		device.ring_test_count[ring] = 0;
	}
#endif
#ifdef XGPU_STREAM_RING
	device.stream_buffer = device.stream_buffers[device.buffer_ring];
	device.index_buffer = device.index_buffers[device.buffer_ring];
	device.stream_offset = 0;
	device.index_offset = 0;
#ifdef HALO_WEB
	web_chunks_begin_frame();
#endif
#else
	device.stream_offset = STREAM_BUFFER_SIZE; /* orphan next frame */
	device.index_offset = INDEX_BUFFER_SIZE;
#endif
#if defined(HALO_ARM64_GUEST) && !defined(HALO_GLES)
	if (device.visibility_results)
		host_gl_read_results(device.visibility_results_copy, sizeof(device.visibility_results_copy));
#endif
}

void WINAPI D3DDevice_Present(CONST RECT *source_rectangle, CONST RECT *destination_rectangle,
	void *unused, void *unused2)
{
	static long screenshot_every = -1;

	(void)source_rectangle;
	(void)destination_rectangle;
	(void)unused;
	(void)unused2;
	if (screenshot_every < 0)
		screenshot_every = config_integer("debug.screenshot_every");

#ifdef HALO_VR
	if (device.gl_ready && vr_gl.ready)
	{
		/* (the window stays hidden: the headset paces the frames) */
		vr_present(vr_screenshot_wanted || (screenshot_every > 0 && device.frame % (unsigned long)screenshot_every == 0));
		vr_screenshot_wanted = FALSE;
		xgpu_texture_cache_begin_frame();
		frame_end_buffers();
	}
	else
#endif
	if (device.gl_ready)
	{
		struct render_target_entry *back_buffer = render_target_get(&device.back_buffer);
		int window_width, window_height, width, height, x, y;

		if (trace_frame())
			platform_log("present back buffer %08lx texture %u", (unsigned long)device.back_buffer.Data,
				back_buffer->target.texture);
		render_target_resolve(&back_buffer->target);
#ifndef HALO_GLES
		visibility_copy_batch();
#endif
		if (screenshot_every > 0 && device.frame % (unsigned long)screenshot_every == 0)
			write_screenshot(back_buffer);
#ifndef HALO_ARM64_GUEST
		if (platform_screenshot_take_request())
			write_key_screenshot(back_buffer);
#endif

		platform_video_drawable_size(&window_width, &window_height);
		/* letterbox to the back buffer's aspect ratio */
		width = window_width;
		height = (int)((long)window_width * back_buffer->target.gl_height / back_buffer->target.gl_width);
		if (height > window_height)
		{
			height = window_height;
			width = (int)((long)window_height * back_buffer->target.gl_width / back_buffer->target.gl_height);
		}
		x = (window_width - width) / 2;
		y = (window_height - height) / 2;
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		glDisable(GL_SCISSOR_TEST);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer_get(back_buffer->target.texture, 0));
		/* row 0 of the render target is the top of the picture */
		glBlitFramebuffer(0, 0, (GLint)back_buffer->target.gl_width, (GLint)back_buffer->target.gl_height,
			x, y + height, x + width, y, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		platform_video_swap();
		xgpu_gl_state_invalidate();
		memory_watch_begin_frame();
		xgpu_texture_cache_begin_frame();
		frame_end_buffers();
	}
	device.frame++;
	stats.presents++;
	if (debug_settings.statistics && device.frame % 60 == 0)
	{
		platform_log("frame %lu: %lu draws, %lu immediate, %lu clears, %lu target changes; skipped %lu no program, %lu no target, %lu link; "
			"%lu KB mirrored, %lu KB streamed",
			device.frame, stats.draws / stats.presents, stats.immediate_draws / stats.presents, stats.clears / stats.presents,
			stats.target_changes / stats.presents, stats.skipped_no_program, stats.skipped_no_target, stats.skipped_link,
			stats.mirrored_bytes / stats.presents / 1024, stats.streamed_bytes / stats.presents / 1024);
		memset(&stats, 0, sizeof(stats));
	}
	platform_pump_events();

	pthread_mutex_lock(&vertical_blank_lock);
	/* the Xbox keeps at most two frames queued behind its 60 Hz display;
	with interpolation, frames come at the real display's rate instead,
	paced by vsync (platform_video_swap) */
	if (halo_interpolation_enabled())
	{
		flip_count++;
	}
	else
	{
		while (pending_flips >= 2)
			pthread_cond_wait(&vertical_blank_condition, &vertical_blank_lock);
		pending_flips++;
	}
	pthread_mutex_unlock(&vertical_blank_lock);
}

HRESULT WINAPI D3DDevice_PersistDisplay(void)
{
	return S_OK;
}
