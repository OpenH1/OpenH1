/*
XGPU_GL.C

The renderer's OpenGL backend (xgpu_backend.h): OpenGL 4.5 on the desktop,
OpenGL ES 3.0 to 3.2 on Android and the Linux arm64 build's GLES renderer,
WebGL 2 in the browser. The front end (d3d8_device.c) describes each draw
in no API's terms; this makes the GL calls: a pipeline cache keyed by the
front end's pipeline key (the programs its shaders link to, written as GLSL
from the shader IR: xgpu_glsl.c, and its fixed-function state's GL values),
applied through a cache of the GL state set; render targets' textures and
framebuffers, multisampled storage and its resolves; samplers; the stream
buffers and the mirror's segments; visibility counting; the window's
present; and the VR mode's eye images and the present's composition for
OpenXR.

Conventions:
- Clip space is D3D's (depth 0..1, y down in window space). glClipControl
  (GL_UPPER_LEFT, GL_ZERO_TO_ONE) makes GL agree, so viewports, scissors and
  texture rows line up with D3D's top-left origin; the window blit at
  Present flips the image back for display. OpenGL ES has no clip control:
  its vertex shaders flip y and remap depth (xgpu_glsl.c), which flips the
  triangles' winding too.
- The VR mode's eye pass draws upside down, for OpenXR's GL images, whose
  first row is at the bottom (vr_flip_rows).
*/

#include "xgpu_gl.h"
#include "xgpu_shader.h"
#include "xgpu_backend.h"
#include "sdl_platform.h"
#include "port_config.h"

#ifdef XGPU_GL_HOST
#include "host_load_trace.h"
#define gl_load_trace host_load_trace
#else
#define gl_load_trace(...) ((void)0)
#endif

#include <SDL3/SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* what the backend can do (xgpu_backend.h; gl_initialize) */
struct xgpu_backend_capabilities xgpu_backend_capabilities;

static void color_to_vec4(D3DCOLOR color, float *out)
{
	out[0] = ((color >> 16) & 0xff) / 255.0f;
	out[1] = ((color >> 8) & 0xff) / 255.0f;
	out[2] = (color & 0xff) / 255.0f;
	out[3] = ((color >> 24) & 0xff) / 255.0f;
}

/* ---------- render targets

The front end numbers its targets; each number's storage is here. */

struct gl_target
{
	/* made (xgpu_backend_target_create, or the VR mode's eye target) */
	BOOL made;
	unsigned long gl_width, gl_height;
	BOOL depth;
	int layers;
	GLuint texture;
	/* with multisampling, the multisampled renderbuffer draws go to, its
	samples a pixel (0 when it has none), and whether it has been drawn into
	since the texture last had its pixels (render_target_multisample) */
	GLuint multisample;
	int samples;
	BOOL unresolved;
};

static struct gl_target *gl_targets;
static unsigned long gl_target_capacity;

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

static struct framebuffer_entry *framebuffers;

/* ---------- programs */

struct fragment_entry
{
	struct fragment_entry *next;
	unsigned long hash;
	struct nv2a_pixel_shader_key key;
	GLuint shader;
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
	struct xgpu_draw_uniforms uniforms;
};

#define FRAGMENT_BUCKETS 1024
#define PROGRAM_BUCKETS 1024

static struct fragment_entry *fragment_buckets[FRAGMENT_BUCKETS];
static struct program_entry *program_buckets[PROGRAM_BUCKETS];

/* ---------- the device */

#if defined(HALO_GLES) || defined(XGPU_GL_HOST)
/* Mobile drivers (Mali) keep every orphaned copy of a buffer until the GPU
is done with it, so a large buffer orphaned each frame costs its size per
frame in flight and more. Instead each frame streams into the next of a few
smaller buffers, reusing one only once the GPU has finished the frame that
last used it (host_gl_wait_frame). A busy frame streams about 5 MB of
vertices. The host's desktop renderer streams so too:
on Zink (Turnip), as on Mali, a glBufferSubData for each draw costs much
more than the unsynchronized write the ring allows (host_gl_buffer_write). */
#define XGPU_STREAM_RING
#define STREAM_BUFFER_SIZE (16 * 1024 * 1024)
#define INDEX_BUFFER_SIZE (2 * 1024 * 1024)
#define STREAM_BUFFER_RING 3
#else
#define STREAM_BUFFER_SIZE (32 * 1024 * 1024)
#define INDEX_BUFFER_SIZE (8 * 1024 * 1024)
#endif
#define VISIBILITY_TEST_SLOTS 4096
/* (with sample counters) the counter of a frame's tests past the first
VISIBILITY_TEST_SLOTS */
#define VISIBILITY_SPARE_COUNTER VISIBILITY_TEST_SLOTS
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
	/* (queries read on the CPU) each slot's latest count known, and whether
	its query has yet to be read: the game spins on a result it is told is
	incomplete, so a query is read only once it says it is available, and
	until then the slot's earlier count stands. A frame of The Library's
	lights makes hundreds of tests. */
	GLuint visibility_known[VISIBILITY_TEST_SLOTS];
	BOOL visibility_unread[VISIBILITY_TEST_SLOTS];
#ifdef XGPU_SAMPLE_COUNTERS
	/* with atomic counters: a counter for each of a frame's tests, its
	place in the frame's list (ring_tests), and one more for the tests past
	the list's end, never read (VISIBILITY_SPARE_COUNTER); and a buffer of
	zeros they are set to 0 from */
	GLuint visibility_counters;
	GLuint counter_zeros;
	unsigned long counter_active;
	/* Reading the counters waits for the draws that counted, which stops
	the CPU until the GPU has caught up (the game asks at the start of the
	next frame), halving the frame rate on drivers that queue frames (Zink,
	Turnip). Instead, at the end of each frame the GPU copies them into the
	frame's snapshot buffer of the stream ring, and the frame's tests (each
	a result slot) are listed with it; then it sets them to 0 again for the
	next frame. Once its fence has passed (two frames on, D3DDevice_Present)
	the CPU reads the snapshot into counter_values and gives each listed
	slot its count: a result is the latest count known, as the desktop's
	query buffer gives.

	Only the GPU writes the counters, in its order: a write by the CPU
	(a mapping, GL_MAP_UNSYNCHRONIZED_BIT) while the copies of the frames
	still queued read them would leave what those copies read undefined,
	and every buffer here begins as zeros, not undefined (glBufferData's
	NULL): a count is only ever the count of a test's draws. */
	GLuint counter_snapshots[STREAM_BUFFER_RING];
	unsigned short ring_tests[STREAM_BUFFER_RING][VISIBILITY_TEST_SLOTS];
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
	/* a pipeline flush every flush_every draws (draw_flush), 0 never */
	unsigned long flush_every;
	unsigned long flush_draws;
#endif
	/* the frames presented (xgpu_backend_frame_end) */
	unsigned long frame;
	BOOL gl_ready;
};

static struct gl_device device;

#ifdef HALO_GLES
#define SAMPLE_COUNTING xgpu_capabilities.atomic_counters
#elif defined(XGPU_SAMPLE_COUNTERS)
/* the VR mode's (vr_gl_initialize) */
static BOOL sample_counting;
#define SAMPLE_COUNTING sample_counting
#endif

/* ---------- the VR mode (HALO_VR; vr.h, vr.c): the backend's part

The front end keeps the passes; here are the eyes' images (the OpenXR
swapchain's, drawn into with GL_OVR_multiview2), the foveated layers, the
HUD groups' and panels' images, the eye corrections, and the present's
composition (xgpu_backend_vr_compose). */

/* (xgpu.h; the front end sets it from xgpu_backend_vr_initialize's answer) */
extern int xgpu_foveated;

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

/* Timer queries are optional and never make the render thread wait. */
#define VR_GPU_TIME_FRAMES 32
#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP 0x8E28
#endif
#ifndef GL_QUERY_COUNTER_BITS
#define GL_QUERY_COUNTER_BITS 0x8864
#endif
static void (GLAPIENTRY *vr_query_counter)(GLuint, GLenum);
static void (GLAPIENTRY *vr_query_result)(GLuint, GLenum, GLuint64 *);
static void (GLAPIENTRY *vr_query_bits)(GLenum, GLenum, GLint *);

static struct
{
	BOOL ready;
	/* the pass drawn (xgpu_backend_vr_pass) */
	int pass;
	int eye_width, eye_height;
	int hud_width, hud_height;
	/* the back buffer in the eye pass (its number the front end's): the
	swapchain's image (the HUD pass draws into a target of its own, copied
	into the HUD's image at the present); foveated, the four layers it is
	made from at the present (vr_fovea_composite), the swapchain's image
	after it */
	struct
	{
		xgpu_target number;
		struct gl_target target;
	} eye_target;
	GLuint eye_image;
	GLuint preparation_image;
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
	/* the frame's (xgpu_vr_frame) */
	BOOL eyes_drawn, hud_drawn, scope_drawn, hud_panels;
	struct gl_target *eye_depth;
	GLuint hud_image;
	/* the scope's image (vr.scopes), square, and the image it was copied
	into this frame; the smart-link's window's image (vr_copy_scope's, as
	the scope's), drawn into the eyes' images: not a layer */
	int scope_size;
	GLuint scope_image;
	GLuint scope_window_texture;
	/* the HUD's groups' targets (vr.h's HALO_VR_HUD_GROUP_*), as large as
	the HUD's, and the groups drawn into this frame */
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
	/* the bound targets are the eyes' layers, all or one
	(xgpu_backend_set_targets): drawn upside down */
	BOOL flipped;
	/* debug.gpu_stats: where a frame's time goes (vr_timing) */
	BOOL depth_failed;
	BOOL statistics;
	Uint64 marks[4];
	double spans[3];
	unsigned long timed;
	/* debug.vr_gpu_time: the GPU's time for each frame */
	BOOL gpu_time;
	GLuint gpu_queries[VR_GPU_TIME_FRAMES][2];
	BOOL gpu_query_used[VR_GPU_TIME_FRAMES];
	int gpu_query_bits, gpu_query_current;
	BOOL gpu_query_active, gpu_query_armed;
	double gpu_milliseconds, gpu_worst;
	unsigned long gpu_frames;
} vr_gl;

static void vr_gpu_time_begin(void)
{
	int index;

	if (!vr_gl.gpu_time || !vr_gl.gpu_query_armed || vr_gl.gpu_query_active)
		return;
	vr_gl.gpu_query_armed = FALSE;
	for (index = 0; index < VR_GPU_TIME_FRAMES && vr_gl.gpu_query_used[index]; index++)
		;
	if (index == VR_GPU_TIME_FRAMES)
		return;
	vr_query_counter(vr_gl.gpu_queries[index][0], GL_TIMESTAMP);
	vr_gl.gpu_query_used[index] = TRUE;
	vr_gl.gpu_query_current = index;
	vr_gl.gpu_query_active = TRUE;
}

static void vr_gpu_time_end(void)
{
	vr_gl.gpu_query_armed = FALSE;
	if (!vr_gl.gpu_query_active)
		return;
	vr_query_counter(vr_gl.gpu_queries[vr_gl.gpu_query_current][1], GL_TIMESTAMP);
	vr_gl.gpu_query_active = FALSE;
}

static void vr_gpu_time_read(void)
{
	int index;

	for (index = 0; index < VR_GPU_TIME_FRAMES; index++)
	{
		GLuint available[2] = { 0, 0 };
		GLuint64 start, end, mask;
		double milliseconds;

		if (!vr_gl.gpu_query_used[index])
			continue;
		glGetQueryObjectuiv(vr_gl.gpu_queries[index][0], GL_QUERY_RESULT_AVAILABLE, &available[0]);
		glGetQueryObjectuiv(vr_gl.gpu_queries[index][1], GL_QUERY_RESULT_AVAILABLE, &available[1]);
		if (!available[0] || !available[1])
			continue;
		vr_query_result(vr_gl.gpu_queries[index][0], GL_QUERY_RESULT, &start);
		vr_query_result(vr_gl.gpu_queries[index][1], GL_QUERY_RESULT, &end);
		mask = vr_gl.gpu_query_bits >= 64 ? ~(GLuint64)0 : ((GLuint64)1 << vr_gl.gpu_query_bits) - 1;
		milliseconds = (double)((end - start) & mask) * 1e-6;
		vr_gl.gpu_milliseconds += milliseconds;
		if (milliseconds > vr_gl.gpu_worst)
			vr_gl.gpu_worst = milliseconds;
		vr_gl.gpu_frames++;
		vr_gl.gpu_query_used[index] = FALSE;
	}
}

/* the frame's marks: the last present's end (then the game updates, and
waits for the runtime's frame), the eye pass begun, the HUD pass begun
after it, the present */
static void vr_timing(int mark)
{
	if (!vr_gl.statistics)
		return;
	vr_gl.marks[mark] = SDL_GetTicksNS();
	if (mark == 0)
		vr_gl.gpu_query_armed = vr_gl.gpu_time;
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
				platform_log("vr: elapsed render GPU timeline per frame: %.2f ms, worst %.2f ms (%lu samples, %dx%d eyes%s)",
					vr_gl.gpu_milliseconds / vr_gl.gpu_frames, vr_gl.gpu_worst, vr_gl.gpu_frames,
					vr_gl.eye_width, vr_gl.eye_height,
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

/* a target's storage by its number (grown as the front end numbers more) */
static struct gl_target *gl_target(xgpu_target number)
{
#ifdef HALO_VR
	/* (the eye pass's back buffer: the eyes' images, or the fovea's layers) */
	if (number && number == vr_gl.eye_target.number)
		return &vr_gl.eye_target.target;
#endif
	if (number >= gl_target_capacity)
	{
		unsigned long capacity = gl_target_capacity ? gl_target_capacity : 64;
		struct gl_target *grown;

		while (capacity <= number)
			capacity *= 2;
		grown = realloc(gl_targets, capacity * sizeof(*grown));
		if (!grown)
		{
			platform_log("GPU: out of memory for render target %u", number);
			abort();
		}
		memset(grown + gl_target_capacity, 0, (capacity - gl_target_capacity) * sizeof(*grown));
		gl_targets = grown;
		gl_target_capacity = capacity;
	}
	return &gl_targets[number];
}

/* ---------- debugging settings, read once (gl_initialize) */

static struct
{
	/* debug.gpu_dump_shaders: the GLSL of each shader made, into this
	folder */
	const char *dump_shaders;
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

/* ---------- GL helpers */

/* KHR_parallel_shader_compile makes completion queryable without asking for
the blocking compile/link status. It is a driver hint, not a deadline promise. */
static void gl_compilation_wait(GLuint object, int program)
{
#ifdef XGPU_GL_HOST
	static int supported = -1;
	GLint completed = GL_FALSE;
	Uint64 started, maximum = 0;
	unsigned polls = 0;
	int trace = host_load_trace_enabled();
	void host_render_stream_loading_service(void);

	if (supported < 0)
	{
		typedef void (GLAPIENTRY *compiler_threads_function)(GLuint);
		compiler_threads_function compiler_threads;

		supported = SDL_GL_ExtensionSupported("GL_KHR_parallel_shader_compile") ||
			SDL_GL_ExtensionSupported("GL_ARB_parallel_shader_compile");
		compiler_threads = (compiler_threads_function)SDL_GL_GetProcAddress("glMaxShaderCompilerThreadsKHR");
		if (!compiler_threads)
			compiler_threads = (compiler_threads_function)SDL_GL_GetProcAddress("glMaxShaderCompilerThreadsARB");
		if (supported && compiler_threads)
			compiler_threads(~0u);
		platform_log("gl: shader compilation completion polling %s", supported ? "available" : "unavailable; driver calls may block presentation");
	}
	if (!supported)
		return;
	gl_load_trace("gl.completion.begin", "object=%u program=%d", object, program);
	while (!completed)
	{
		started = trace ? SDL_GetTicksNS() : 0;
		/* COMPLETION_STATUS_KHR and COMPLETION_STATUS_ARB have this value. */
		if (program)
			glGetProgramiv(object, 0x91B1, &completed);
		else
			glGetShaderiv(object, 0x91B1, &completed);
		if (trace)
		{
			Uint64 elapsed = SDL_GetTicksNS() - started;

			if (elapsed > maximum)
				maximum = elapsed;
			polls++;
		}
		if (!completed)
		{
			host_render_stream_loading_service();
			SDL_Delay(1);
		}
	}
	gl_load_trace("gl.completion.end", "object=%u program=%d polls=%u maximum_ns=%llu",
		object, program, polls, (unsigned long long)maximum);
#else
	(void)object;
	(void)program;
#endif
}

GLuint xgpu_compile_shader(GLenum type, const char *source, const char *what)
{
	GLuint shader = glCreateShader(type);
	GLint status = 0;

	glShaderSource(shader, 1, &source, NULL);
	gl_load_trace("gl.compile.begin", "shader=%u type=%u name=%s", shader, type, what);
	glCompileShader(shader);
	gl_compilation_wait(shader, FALSE);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	gl_load_trace("gl.compile.end", "shader=%u status=%d", shader, status);
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
	gl_load_trace("gl.link.begin", "program=%u", program);
	glLinkProgram(program);
	gl_compilation_wait(program, TRUE);
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	gl_load_trace("gl.link.end", "program=%u status=%d", program, status);
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

void xgpu_backend_target_create(xgpu_target number, const struct xgpu_target_description *description)
{
	struct gl_target *target = gl_target(number);

	memset(target, 0, sizeof(*target));
	target->made = TRUE;
	target->gl_width = description->width;
	target->gl_height = description->height;
	target->depth = description->depth;
	target->layers = description->layers;
	glGenTextures(1, &target->texture);
#ifdef HALO_VR
	if (target->layers > 1)
	{
		glBindTexture(GL_TEXTURE_2D_ARRAY, target->texture);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
		if (target->depth)
			glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH24_STENCIL8, (GLsizei)target->gl_width,
				(GLsizei)target->gl_height, target->layers, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
		else
			glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, (GLsizei)target->gl_width,
				(GLsizei)target->gl_height, target->layers, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	}
	else
#endif
	{
	glBindTexture(GL_TEXTURE_2D, target->texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	if (target->depth)
		glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, (GLsizei)target->gl_width,
			(GLsizei)target->gl_height, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
	else
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)target->gl_width, (GLsizei)target->gl_height,
			0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	}
	xgpu_gl_state_invalidate();
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

/* the multisampled pixels of a target drawn into since into its texture */
static void render_target_resolve(struct gl_target *target)
{
	GLuint read, draw;

	if (!target->unresolved)
		return;
#ifdef HALO_VR
	vr_gpu_time_begin();
#endif
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
static void render_target_multisample(struct gl_target *target, int samples)
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

#ifdef HALO_VR
/* the bound targets' rows: the eye pass draws upside down (vr_flip_rows) */
static unsigned long target_height;

/* The eye pass draws upside down (its vertex shaders flip y, xgpu_glsl.c),
as OpenXR's GL images have their first row at the bottom where the game's
targets have it at the top: the rows of its rectangles count from the
other end, and its layered targets are sampled upside down (xgpu_glsl.c). */
static void vr_flip_rows(GLint *y, GLint height)
{
	if (vr_gl.flipped)
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

void xgpu_backend_target_multisample(xgpu_target target, int samples)
{
	render_target_multisample(gl_target(target), samples);
}

void xgpu_backend_target_resolve(xgpu_target target)
{
	render_target_resolve(gl_target(target));
}

#ifdef HALO_VR
static GLuint vr_group_target(const struct gl_target *hud, int group);
#endif

void xgpu_backend_set_targets(xgpu_target color_number, int hud_group, xgpu_target depth_number, int views, int base,
	int samples)
{
	struct gl_target *color = color_number ? gl_target(color_number) : NULL;
	struct gl_target *depth = depth_number ? gl_target(depth_number) : NULL;
	GLuint color_texture = color ? color->texture : 0;

#ifdef HALO_VR
	/* a group of the HUD's into its own target, for its panel */
	if (color && hud_group >= 0)
		color_texture = vr_group_target(color, hud_group);
	/* (the eyes' layers, all or one: drawn upside down) */
	vr_gl.flipped = views != 1;
	target_height = color ? color->gl_height : depth->gl_height;
	framebuffer_views = views;
	framebuffer_base = base;
#else
	(void)hud_group;
	(void)views;
	(void)base;
#endif
	if (samples)
	{
		state_framebuffer(framebuffer_find(color ? color->multisample : 0, depth ? depth->multisample : 0, TRUE));
		if (color)
			color->unresolved = TRUE;
		if (depth)
			depth->unresolved = TRUE;
	}
	else
	{
		state_framebuffer(framebuffer_get(color_texture, depth ? depth->texture : 0));
	}
#ifdef HALO_VR
	framebuffer_views = 1;
	framebuffer_base = 0;
#endif
}

void xgpu_backend_clear_target(xgpu_target number, const float color[4])
{
	struct gl_target *target = gl_target(number);

#ifdef HALO_VR
	vr_gpu_time_begin();
#endif
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_get(target->texture, 0));
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClearColor(color[0], color[1], color[2], color[3]);
	glClear(GL_COLOR_BUFFER_BIT);
}

/* ---------- the backend made ready */

#ifdef XGPU_SAMPLE_COUNTERS
/* the visibility tests' counters and their snapshots (with
SAMPLE_COUNTING) */
static void sample_counters_create(void)
{
	static const GLuint zeros[VISIBILITY_TEST_SLOTS + 1];
	int ring;

	if (!SAMPLE_COUNTING || device.visibility_counters)
		return;
	glGenBuffers(1, &device.visibility_counters);
	glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, device.visibility_counters);
	glBufferData(GL_ATOMIC_COUNTER_BUFFER, sizeof(zeros), zeros, GL_DYNAMIC_COPY);
	glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, 0);
	glGenBuffers(1, &device.counter_zeros);
	glBindBuffer(GL_COPY_READ_BUFFER, device.counter_zeros);
	glBufferData(GL_COPY_READ_BUFFER, VISIBILITY_TEST_SLOTS * sizeof(GLuint), zeros, GL_STATIC_COPY);
	glBindBuffer(GL_COPY_READ_BUFFER, 0);
	glGenBuffers(STREAM_BUFFER_RING, device.counter_snapshots);
	for (ring = 0; ring < STREAM_BUFFER_RING; ring++)
	{
		glBindBuffer(GL_COPY_WRITE_BUFFER, device.counter_snapshots[ring]);
		glBufferData(GL_COPY_WRITE_BUFFER, VISIBILITY_TEST_SLOTS * sizeof(GLuint), zeros, GL_STREAM_READ);
	}
	glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
	device.counter_active = VISIBILITY_SPARE_COUNTER;
	xgpu_gl_state_invalidate();
}
#endif

void xgpu_backend_initialize(void)
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
	device.visibility_results = glMapBufferRange(GL_QUERY_BUFFER, 0, VISIBILITY_TEST_SLOTS * sizeof(GLuint),
		GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
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
		/* (the input registers' w is 1 until the game sets them) */
		static const float initial[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

		glVertexAttrib4fv(index, initial);
	}
	debug_settings.dump_shaders = *config_string("debug.gpu_dump_shaders") ?
		config_string("debug.gpu_dump_shaders") : NULL;
	{
		GLint samples = 0, target_size = 0, renderbuffer_size = 0, anisotropy = 1;

		glGetIntegerv(GL_MAX_SAMPLES, &samples);
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &target_size);
		glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &renderbuffer_size);
		if (renderbuffer_size < target_size)
			target_size = renderbuffer_size;
#ifdef HALO_GLES
		if (xgpu_capabilities.anisotropy)
			glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &anisotropy);
#else
		glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &anisotropy);
#endif
		xgpu_backend_capabilities.largest_target = target_size;
		xgpu_backend_capabilities.most_samples = samples;
		xgpu_backend_capabilities.most_anisotropy = anisotropy;
	}
#ifdef HALO_GLES
	xgpu_backend_capabilities.sample_counting = SAMPLE_COUNTING;
	xgpu_backend_capabilities.bgra_vertex_colors = FALSE;
	xgpu_backend_capabilities.base_vertex = xgpu_capabilities.base_vertex;
	xgpu_backend_capabilities.sample_mask = FALSE;
	xgpu_backend_capabilities.dynamic_lights = FALSE;
#else
	xgpu_backend_capabilities.sample_counting = FALSE;
	xgpu_backend_capabilities.bgra_vertex_colors = TRUE;
	xgpu_backend_capabilities.base_vertex = TRUE;
	xgpu_backend_capabilities.sample_mask = TRUE;
	xgpu_backend_capabilities.dynamic_lights = TRUE;
#endif
#ifdef HALO_WEB
	xgpu_backend_capabilities.mixed_constant_blend = FALSE;
	xgpu_backend_capabilities.sample_drawn_target = FALSE;
#else
	xgpu_backend_capabilities.mixed_constant_blend = TRUE;
	xgpu_backend_capabilities.sample_drawn_target = TRUE;
#endif
	xgpu_gl_state_invalidate();
	device.gl_ready = TRUE;
}

void xgpu_backend_invalidate(void)
{
	xgpu_gl_state_invalidate();
}

void xgpu_backend_flush(void)
{
	glFlush();
}

void xgpu_backend_consume(void)
{
	/* Texture uploads have already copied the application's bytes. */
}

void xgpu_backend_anti_aliasing_prepare(BOOL smaa)
{
	if (!xgpu_post_prepare(smaa))
		platform_log("anti-aliasing: its programs do not build, so the 3D view is not antialiased");
}

void xgpu_backend_anti_alias(xgpu_target number, BOOL smaa, const long corners[4])
{
	struct gl_target *target = gl_target(number);
	GLint gl_corners[4];
	int index;

	for (index = 0; index < 4; index++)
		gl_corners[index] = (GLint)corners[index];
	xgpu_post_anti_alias(smaa, framebuffer_get(target->texture, 0), target->gl_width, target->gl_height, gl_corners);
	glBindVertexArray(device.vertex_array);
	xgpu_gl_state_invalidate();
}

/* ---------- visibility (occlusion) tests */

void xgpu_backend_visibility_begin(void)
{
	/* the query object is chosen when the test ends; use a scratch one */
#ifdef XGPU_SAMPLE_COUNTERS
	if (SAMPLE_COUNTING)
	{
		/* the frame's next counter, which is 0 (frame_end_buffers) */
		unsigned long count = device.ring_test_count[device.buffer_ring];

		device.counter_active = count < VISIBILITY_TEST_SLOTS ? count : VISIBILITY_SPARE_COUNTER;
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

/* area: the target's pixels for each of the game's; views: the views drawn
into at once; samples: a pixel's */
void xgpu_backend_visibility_end(unsigned long index, float area, int views, int samples)
{
	GLuint scratch;

#ifdef XGPU_SAMPLE_COUNTERS
	if (SAMPLE_COUNTING)
	{
		unsigned long *count = &device.ring_test_count[device.buffer_ring];

		/* (a slot tested twice in a frame is listed twice: the later
		counter, resolved after, wins; a test begun in an earlier frame,
		whose counter is now another's, is not listed) */
		if (device.counter_active == *count && *count < VISIBILITY_TEST_SLOTS)
		{
			device.ring_tests[device.buffer_ring][*count] = (unsigned short)index;
			(*count)++;
		}
		device.counter_active = VISIBILITY_SPARE_COUNTER;
		device.query_pending[index] = TRUE;
#ifdef HALO_VR
		/* (a count of the game's pixels, as below: drawn into both eyes,
		the test counts each pixel twice; the counters count pixels, not
		samples) */
		device.query_area[index] = area * (float)views;
#else
		(void)area;
		(void)views;
#endif
		(void)samples;
		return;
	}
#endif
	(void)views;
	glEndQuery(VISIBILITY_QUERY);
	/* the target's samples to a game pixel (its pixels, by its samples a
	pixel with multisampling): the result is a count of the game's pixels
	(visibility_unscaled), which the game divides by its own test's area
	(lens flares, rasterizer_lights.c), a split-screen window's or the
	screen's alike */
	device.query_area[index] = area * (float)samples;
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
		return;
	}
#endif
	/* swap the scratch query into the requested slot */
	scratch = device.scratch_query;
	device.scratch_query = device.queries[index];
	device.queries[index] = scratch;
	device.query_pending[index] = TRUE;
	device.visibility_unread[index] = TRUE;
}

#ifndef HALO_GLES
/* a count of pixels in the game's pixels */
static GLuint visibility_unscaled(GLuint samples, DWORD index)
{
	float area = device.query_area[index];

	return area > 1.0f ? (GLuint)(samples / area + 0.5f) : samples;
}

#endif
static GLuint visibility_count(unsigned long index)
{
	GLuint available = 0, samples = 0;

	if (!device.query_pending[index])
		return 0;
#ifdef XGPU_SAMPLE_COUNTERS
	if (SAMPLE_COUNTING)
	{
		/* the latest count the GPU has finished (counter_snapshots) */
#ifdef HALO_VR
		return visibility_unscaled(device.visibility_latest[index], index);
#else
		return device.visibility_latest[index];
#endif
	}
#endif
#ifndef HALO_GLES
	if (device.visibility_results)
	{
		/* the latest count the GPU has written: from this test, or while
		the GPU is still behind, from the slot's earlier ones */
		return visibility_unscaled(device.visibility_results[index], index);
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
	return device.visibility_known[index];
}

UINT xgpu_backend_visibility_result(unsigned long index)
{
	GLuint count = visibility_count(index);

	return count < XGPU_VISIBILITY_COUNT_MOST ? count : XGPU_VISIBILITY_COUNT_MOST;
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

/* what the GLSL is written for (xgpu_glsl.c): this context's language, and
the VR mode's foveation once it is chosen (vr_gl_initialize) */
static const struct xgpu_glsl_options *glsl_options(void)
{
	static struct xgpu_glsl_options options;

#ifdef HALO_GLES
	options.version = xgpu_capabilities.shading_language;
	options.es = TRUE;
#else
	options.version = "450 core";
	options.es = FALSE;
#endif
	options.foveated = xgpu_foveated != 0;
	options.debug_expression = config_string("debug.gpu_debug_expression");
	options.debug_texture0 = config_boolean("debug.gpu_debug_texture0") != 0;
	options.debug_flat = config_boolean("debug.gpu_debug_flat") != 0;
	return &options;
}

/* ---------- vertex programs (xgpu_backend_vertex_program) and their shaders

The programs the game makes, by their hash; each one's GL vertex shaders by
what changes the GLSL: the declaration's packed attributes, immediate mode,
the views drawn, the lighting handed on. */

#define VERTEX_PROGRAM_BUCKETS 256
#define VERTEX_SHADER_BUCKETS 1024

struct vertex_program_entry
{
	struct vertex_program_entry *next;
	unsigned long id;
	struct xgpu_vertex_program program;
	/* a model lighting program's (xgpu_backend_vertex_lighting) */
	struct nv2a_vertex_lighting lighting;
};

struct vertex_shader_entry
{
	struct vertex_shader_entry *next;
	unsigned long long program;
	unsigned long packed;
	unsigned char immediate, views, lighting;
	/* 0 if it does not compile (not compiled again at each draw) */
	GLuint shader;
};

static struct vertex_program_entry *vertex_program_buckets[VERTEX_PROGRAM_BUCKETS];
static struct vertex_shader_entry *vertex_shader_buckets[VERTEX_SHADER_BUCKETS];

static struct vertex_program_entry *vertex_program_find(unsigned long long hash)
{
	struct vertex_program_entry *entry;

	for (entry = vertex_program_buckets[hash % VERTEX_PROGRAM_BUCKETS]; entry; entry = entry->next)
	{
		if (entry->program.hash == hash)
			return entry;
	}
	return NULL;
}

void xgpu_backend_vertex_program(unsigned long id, const struct xgpu_vertex_program *program)
{
	struct vertex_program_entry *entry;

	if (vertex_program_find(program->hash))
		return;
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return;
	entry->id = id;
	entry->program = *program;
	entry->next = vertex_program_buckets[program->hash % VERTEX_PROGRAM_BUCKETS];
	vertex_program_buckets[program->hash % VERTEX_PROGRAM_BUCKETS] = entry;
}

void xgpu_backend_vertex_lighting(unsigned long long program, const struct nv2a_vertex_lighting *lighting)
{
	struct vertex_program_entry *entry = vertex_program_find(program);

	if (entry)
		entry->lighting = *lighting;
}

/* the vertex shader of a pipeline key's program and variant, compiled the
first time (0 if it does not compile): lighting hands on the normal and
position (a model lighting program's own, or a structure's lightmap pass's,
XGPU_ENVIRONMENT_LIGHTING); views: XGPU_MULTIVIEW_*, the VR mode's eyes */
static GLuint vertex_shader_get(const struct xgpu_pipeline_key *key)
{
	static const struct nv2a_vertex_lighting environment = { XGPU_ENVIRONMENT_LIGHTING };
	struct vertex_program_entry *program = vertex_program_find(key->vertex_program);
	unsigned long packed = key->immediate ? 0 : key->packed_attributes;
	unsigned long bucket = (unsigned long)((key->vertex_program ^ packed * 2654435761UL ^
		(key->immediate + key->views * 2 + key->lighting * 16)) % VERTEX_SHADER_BUCKETS);
	const struct nv2a_vertex_lighting *lighting;
	struct vertex_shader_entry *entry;
	char *source;

	if (!program)
		return 0;
	for (entry = vertex_shader_buckets[bucket]; entry; entry = entry->next)
	{
		if (entry->program == key->vertex_program && entry->packed == packed && entry->immediate == key->immediate &&
			entry->views == key->views && entry->lighting == key->lighting)
		{
			return entry->shader;
		}
	}
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return 0;
	entry->program = key->vertex_program;
	entry->packed = packed;
	entry->immediate = key->immediate;
	entry->views = key->views;
	entry->lighting = key->lighting;
	entry->next = vertex_shader_buckets[bucket];
	vertex_shader_buckets[bucket] = entry;
	lighting = key->lighting == XGPU_LIGHTING_NONE ? NULL : key->lighting == XGPU_LIGHTING_ENVIRONMENT ? &environment :
		&program->lighting;
	source = xgpu_glsl_vertex_shader(&program->program, packed, lighting, key->views, glsl_options());
	entry->shader = xgpu_compile_shader(GL_VERTEX_SHADER, source, "vertex");
	if (debug_settings.dump_shaders)
	{
		char path[512];
		FILE *file;

		snprintf(path, sizeof(path), "%s/vs%03lu_%d%s.glsl", debug_settings.dump_shaders, program->id,
			key->immediate + key->views * 2, lighting ? "_lit" : "");
		if ((file = fopen(path, "w")) != NULL)
		{
			fputs(source, file);
			fclose(file);
		}
	}
	free(source);
	return entry->shader;
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
	struct xgpu_pixel_program program;
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
#ifdef XGPU_GL_HOST
	xgpu_host_pixel_program(key, &program);
#else
	nv2a_pixel_program_decode(key, &program);
#endif
	source = xgpu_glsl_pixel_shader(&program, glsl_options());
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

/* ---------- textures */

static GLenum address_mode(unsigned char mode)
{
	switch (mode)
	{
	case XGPU_ADDRESS_MIRROR: return GL_MIRRORED_REPEAT;
	case XGPU_ADDRESS_CLAMP: return GL_CLAMP_TO_EDGE;
#ifdef HALO_GLES
	case XGPU_ADDRESS_BORDER: return xgpu_capabilities.border_clamp ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE;
#else
	case XGPU_ADDRESS_BORDER: return GL_CLAMP_TO_BORDER;
#endif
	default: return GL_REPEAT;
	}
}

/* ---------- sampler objects

A sampler object for each sampler the game uses, made once: a draw binds
the one its state needs, rather than changing a sampler's parameters, which
costs a GL call each, and on Zink a new Vulkan sampler. The game uses a few
dozen. */

#define SAMPLER_CACHE_SIZE 512

static struct
{
	struct xgpu_sampler state;
	GLuint sampler;
} sampler_cache[SAMPLER_CACHE_SIZE];
static unsigned long sampler_cache_count;

/* the VR mode samples sRGB images without decoding them (vr_gl_initialize) */
static BOOL samplers_skip_srgb_decode;

typedef char sampler_size_assert[sizeof(struct xgpu_sampler) % 4 == 0 ? 1 : -1];

/* a sampler object's parameters */
static void sampler_parameters(GLuint sampler, const struct xgpu_sampler *state)
{
	GLenum minification;
	float border[4];

	if (state->minify == XGPU_FILTER_POINT)
		minification = state->mip == XGPU_FILTER_NONE ? GL_NEAREST :
			state->mip == XGPU_FILTER_POINT ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_LINEAR;
	else
		minification = state->mip == XGPU_FILTER_NONE ? GL_LINEAR :
			state->mip == XGPU_FILTER_POINT ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
	glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, (GLint)minification);
	glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, state->magnify == XGPU_FILTER_POINT ? GL_NEAREST : GL_LINEAR);
	glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, (GLint)address_mode(state->address[0]));
	glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, (GLint)address_mode(state->address[1]));
	glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, (GLint)address_mode(state->address[2]));
#ifdef HALO_GLES
	/* ES has no sampler LOD bias; the pixel shader applies it
	(texture_lod_bias) */
	glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, (float)state->first_level);
	if (xgpu_capabilities.anisotropy)
		glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT,
			(state->minify == XGPU_FILTER_ANISOTROPIC && state->anisotropy > 1) ? (float)state->anisotropy : 1.0f);
	if (xgpu_capabilities.border_clamp)
	{
		color_to_vec4(state->border_color, border);
		glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, border);
	}
#else
	glSamplerParameterf(sampler, GL_TEXTURE_LOD_BIAS, state->lod_bias);
	glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, (float)state->first_level);
	glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY,
		(state->minify == XGPU_FILTER_ANISOTROPIC && state->anisotropy > 1) ? (float)state->anisotropy : 1.0f);
	color_to_vec4(state->border_color, border);
	glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, border);
#endif
	if (samplers_skip_srgb_decode)
		glSamplerParameteri(sampler, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
}

/* the sampler object of a sampler, made the first time; with the cache full
(never seen), the stage's own sampler set to it */
static GLuint sampler_get(int stage, const struct xgpu_sampler *state)
{
	const DWORD *words = (const DWORD *)state;
	unsigned long hash = 2166136261UL, index, probe;
	GLuint sampler;

	for (index = 0; index < sizeof(*state) / 4; index++)
		hash = (hash ^ words[index]) * 16777619UL;
	for (probe = 0; probe < SAMPLER_CACHE_SIZE; probe++)
	{
		index = (hash + probe) % SAMPLER_CACHE_SIZE;
		if (!sampler_cache[index].sampler)
			break;
		if (!memcmp(&sampler_cache[index].state, state, sizeof(*state)))
			return sampler_cache[index].sampler;
	}
	if (probe == SAMPLER_CACHE_SIZE || sampler_cache_count >= SAMPLER_CACHE_SIZE * 3 / 4)
	{
		sampler_parameters(device.samplers[stage], state);
		return device.samplers[stage];
	}
	glGenSamplers(1, &sampler);
	sampler_parameters(sampler, state);
	sampler_cache[index].state = *state;
	sampler_cache[index].sampler = sampler;
	sampler_cache_count++;
	return sampler;
}

/* a stage's sampler bound */
static void configure_sampler(int stage, const struct xgpu_sampler *state)
{
	/* the sampler each stage's was last chosen for */
	static struct xgpu_sampler configured[D3DTSS_MAXSTAGES];
	static GLuint configured_sampler[D3DTSS_MAXSTAGES];

	if (!configured_sampler[stage] || memcmp(&configured[stage], state, sizeof(*state)))
	{
		configured[stage] = *state;
		configured_sampler[stage] = sampler_get(stage, state);
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

#define MIP_COMPOSITE_LEVELS XGPU_MIP_LEVELS

struct mip_composite
{
	struct mip_composite *next;
	unsigned long data, width, height, levels;
	GLuint texture;
	/* the levels last copied, and each one's target and written serial
	then */
	unsigned long rendered_levels;
	xgpu_target level_sources[MIP_COMPOSITE_LEVELS];
	unsigned long level_written[MIP_COMPOSITE_LEVELS];
};

static struct mip_composite *mip_composites;

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

void xgpu_backend_target_delete(xgpu_target number)
{
	struct gl_target *target = gl_target(number);
	struct mip_composite *composite;

	framebuffers_forget(target->texture, FALSE);
	if (target->multisample)
	{
		framebuffers_forget(target->multisample, TRUE);
		glDeleteRenderbuffers(1, &target->multisample);
	}
	/* (a composite that copied from the target copies again) */
	for (composite = mip_composites; composite; composite = composite->next)
	{
		unsigned long level;

		for (level = 0; level < composite->rendered_levels; level++)
		{
			if (composite->level_sources[level] == number)
			{
				composite->rendered_levels = 0;
				break;
			}
		}
	}
	xgpu_gl_texture_deleted(target->texture);
	glDeleteTextures(1, &target->texture);
	memset(target, 0, sizeof(*target));
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

static GLuint mip_composite_get(const struct xgpu_texture_source *source)
{
	struct mip_composite *composite;
	unsigned long level, rendered_levels = source->rendered_levels;
	BOOL changed;

	for (composite = mip_composites; composite; composite = composite->next)
	{
		if (composite->data == source->data && composite->width == source->width &&
			composite->height == source->height && composite->levels == source->levels)
		{
			break;
		}
	}
	if (!composite)
	{
		composite = calloc(1, sizeof(*composite));
		composite->data = source->data;
		composite->width = source->width;
		composite->height = source->height;
		composite->levels = source->levels;
		glGenTextures(1, &composite->texture);
		glBindTexture(GL_TEXTURE_2D, composite->texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)source->levels - 1);
		for (level = 0; level < source->levels; level++)
		{
			GLsizei width = (GLsizei)(source->width >> level ? source->width >> level : 1);
			GLsizei height = (GLsizei)(source->height >> level ? source->height >> level : 1);

			glTexImage2D(GL_TEXTURE_2D, (GLint)level, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
		}
		composite->rendered_levels = ~0UL;
		composite->next = mip_composites;
		mip_composites = composite;
	}
	changed = rendered_levels != composite->rendered_levels;
	for (level = 0; level < rendered_levels && !changed; level++)
	{
		changed = source->level_targets[level] != composite->level_sources[level] ||
			source->level_written[level] != composite->level_written[level];
	}
	if (!changed)
		return composite->texture;
#ifdef HALO_VR
	vr_gpu_time_begin();
#endif
	composite->rendered_levels = rendered_levels;
	for (level = 0; level < rendered_levels; level++)
	{
		struct gl_target *target = gl_target(source->level_targets[level]);
		GLsizei width = (GLsizei)target->gl_width, height = (GLsizei)target->gl_height;

		composite->level_sources[level] = source->level_targets[level];
		composite->level_written[level] = source->level_written[level];
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
	if (rendered_levels < source->levels)
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
static GLuint feedback_copy(struct gl_target *target, unsigned long written)
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
	if (copied_texture != target->texture || copied_written != written)
	{
		copy_level_by_blit(target->texture, copy, 0, copy_width, copy_height);
		copied_texture = target->texture;
		copied_written = written;
	}
	return copy;
}
#endif

#ifndef XGPU_GL_HOST
/* the textures the draw's descriptions found (xgpu_backend_texture_describe,
for its stages, before xgpu_backend_set_textures binds them): bound as
found, not looked up again */
static struct
{
	DWORD resource, palette;
	GLuint texture;
	GLenum target;
} described[D3DTSS_MAXSTAGES];
static int described_count;

void xgpu_backend_texture_describe(DWORD resource, DWORD palette, struct xgpu_texture_description *description, int *kind)
{
	GLenum target;
	GLuint texture = xgpu_texture_get((const DWORD *)resource, (const D3DCOLOR *)palette, &target, description);

	if (described_count < D3DTSS_MAXSTAGES)
	{
		described[described_count].resource = resource;
		described[described_count].palette = palette;
		described[described_count].texture = texture;
		described[described_count].target = target;
		described_count++;
	}
	*kind = target == GL_TEXTURE_CUBE_MAP ? XGPU_TEXTURE_KIND_CUBE : target == GL_TEXTURE_3D ? XGPU_TEXTURE_KIND_3D :
		XGPU_TEXTURE_KIND_2D;
}
#endif

void xgpu_backend_set_textures(const struct xgpu_texture_source stages[4])
{
	/* Bind only after resolving every stage, since texture uploads can
	overwrite the active unit's binding. */
	GLenum gl_targets[D3DTSS_MAXSTAGES];
	GLuint gl_textures[D3DTSS_MAXSTAGES];
	int stage;

	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		const struct xgpu_texture_source *source = &stages[stage];

		gl_targets[stage] = GL_TEXTURE_2D;
		gl_textures[stage] = 0;
		if (source->source == XGPU_SOURCE_TARGET)
		{
			struct gl_target *target = gl_target(source->target);

			/* (multisampled: its pixels drawn since, resolved) */
			render_target_resolve(target);
			gl_textures[stage] = source->levels ? mip_composite_get(source) : target->texture;
#ifdef HALO_VR
			if (source->sampler_type == _xgpu_sampler_2d_layered)
				gl_targets[stage] = GL_TEXTURE_2D_ARRAY;
#endif
#ifdef HALO_WEB
			if (source->drawn && gl_textures[stage] == gl_target(source->drawn)->texture)
				gl_textures[stage] = feedback_copy(target, source->written);
#endif
		}
		else if (source->source == XGPU_SOURCE_TEXTURE)
		{
#ifdef XGPU_GL_HOST
			/* (the host's cache, by the stage's record: the guest answered the
			descriptions) */
			gl_textures[stage] = xgpu_host_texture_get(stage, &gl_targets[stage]);
#else
			int found;

			for (found = 0; found < described_count; found++)
			{
				if (described[found].resource == source->resource && described[found].palette == source->palette)
					break;
			}
			if (found < described_count)
			{
				gl_textures[stage] = described[found].texture;
				gl_targets[stage] = described[found].target;
			}
			else
			{
				struct xgpu_texture_description description;

				gl_textures[stage] = xgpu_texture_get((const DWORD *)source->resource,
					(const D3DCOLOR *)source->palette, &gl_targets[stage], &description);
			}
#endif
		}
		if (source->source != XGPU_SOURCE_NONE)
			configure_sampler(stage, &source->sampler);
	}
#ifndef XGPU_GL_HOST
	described_count = 0;
#endif
#ifdef HALO_GLES
	/* (a stage the pixel shader does not sample keeps what it had: unbinding
	it, to bind it again at the next draw that has it, is two calls) */
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		if (gl_textures[stage] || stages[stage].sampled)
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

/* ---------- the GL pipeline cache

A pipeline for each pipeline key, made the first time: the program its
shaders link to (shared by the keys that differ in fixed-function state
alone: program_get) and the GL values of its fixed-function state, applied
through the state cache at each draw. */

struct gl_pipeline
{
	struct gl_pipeline *next;
	unsigned long hash;
	struct xgpu_pipeline_key key;
	/* NULL: its program does not compile or link */
	struct program_entry *program;
	GLenum depth_function, stencil_function, stencil_operations[3];
	GLenum blend_source, blend_destination, blend_equation;
	GLenum front_face, cull_face, polygon_mode;
};

#define PIPELINE_BUCKETS 1024

static struct gl_pipeline *pipeline_buckets[PIPELINE_BUCKETS];

typedef char pipeline_key_size_assert[sizeof(struct xgpu_pipeline_key) % 4 == 0 ? 1 : -1];

static GLenum gl_compare_function(unsigned char function)
{
	static const GLenum functions[] =
	{
		GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS,
	};

	return functions[function & 7];
}

static GLenum gl_blend_factor(unsigned char factor)
{
	static const GLenum factors[] =
	{
		GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA,
		GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE, GL_CONSTANT_COLOR,
		GL_ONE_MINUS_CONSTANT_COLOR, GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA,
	};

	return factor < sizeof(factors) / sizeof(factors[0]) ? factors[factor] : GL_ONE;
}

static GLenum gl_blend_equation(unsigned char operation)
{
	switch (operation)
	{
	case XGPU_BLEND_SUBTRACT: return GL_FUNC_SUBTRACT;
	case XGPU_BLEND_REVERSE_SUBTRACT: return GL_FUNC_REVERSE_SUBTRACT;
	case XGPU_BLEND_MIN: return GL_MIN;
	case XGPU_BLEND_MAX: return GL_MAX;
	default: return GL_FUNC_ADD;
	}
}

static GLenum gl_stencil_operation(unsigned char operation)
{
	static const GLenum operations[] =
	{
		GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP,
	};

	return operations[operation & 7];
}

/* the pipeline of a key, made the first time */
static struct gl_pipeline *gl_pipeline_get(const struct xgpu_pipeline_key *key)
{
	static struct gl_pipeline *last;
	struct gl_pipeline **bucket, *pipeline;
	unsigned long hash;

	/* (consecutive draws mostly share their pipeline) */
	if (last && words_equal(&last->key, key, sizeof(*key)))
		return last;
	hash = hash_words(key, sizeof(*key));
	bucket = &pipeline_buckets[hash % PIPELINE_BUCKETS];
	for (pipeline = *bucket; pipeline; pipeline = pipeline->next)
	{
		if (pipeline->hash == hash && words_equal(&pipeline->key, key, sizeof(*key)))
		{
			last = pipeline;
			return pipeline;
		}
	}
	pipeline = calloc(1, sizeof(*pipeline));
	pipeline->hash = hash;
	pipeline->key = *key;
	pipeline->program = program_get(vertex_shader_get(key), fragment_shader_get(&key->pixel));
	pipeline->depth_function = gl_compare_function(key->depth_function);
	pipeline->stencil_function = gl_compare_function(key->stencil_function);
	pipeline->stencil_operations[0] = gl_stencil_operation(key->stencil_fail);
	pipeline->stencil_operations[1] = gl_stencil_operation(key->stencil_depth_fail);
	pipeline->stencil_operations[2] = gl_stencil_operation(key->stencil_pass);
	pipeline->blend_source = gl_blend_factor(key->blend_source);
	pipeline->blend_destination = gl_blend_factor(key->blend_destination);
	pipeline->blend_equation = gl_blend_equation(key->blend_operation);
	pipeline->front_face = key->front_counter_clockwise ? GL_CCW : GL_CW;
#ifdef HALO_GLES
	/* the vertex shader flips y in clip space, which (unlike desktop
	GL's upper-left clip origin) also flips the winding */
	pipeline->front_face = pipeline->front_face == GL_CW ? GL_CCW : GL_CW;
#endif
	/* (as does the VR mode's eye pass: drawn upside down, vr_flip_rows) */
	if (key->views != XGPU_MULTIVIEW_NONE)
		pipeline->front_face = pipeline->front_face == GL_CW ? GL_CCW : GL_CW;
	pipeline->cull_face = key->cull_front ? GL_FRONT : GL_BACK;
#ifndef HALO_GLES
	pipeline->polygon_mode = key->fill == XGPU_FILL_LINES ? GL_LINE : key->fill == XGPU_FILL_POINTS ? GL_POINT : GL_FILL;
#endif
	pipeline->next = *bucket;
	*bucket = pipeline;
	last = pipeline;
	return pipeline;
}

/* a pipeline's fixed-function state and the draw's dynamic state, set
through the state cache */
static void gl_pipeline_apply(const struct gl_pipeline *pipeline, const struct xgpu_dynamic_state *dynamic)
{
	const struct xgpu_pipeline_key *key = &pipeline->key;
	GLint viewport[4];
	GLint scissor[4];
	float depth_range[2];
	int index;

	for (index = 0; index < 4; index++)
	{
		viewport[index] = (GLint)dynamic->viewport[index];
		scissor[index] = (GLint)dynamic->scissor[index];
	}
#ifdef HALO_VR
	vr_flip_rows(&viewport[1], viewport[3]);
	vr_flip_rows(&scissor[1], scissor[3]);
#endif
	if (memcmp(gl_state.viewport, viewport, sizeof(viewport)))
	{
		memcpy(gl_state.viewport, viewport, sizeof(viewport));
		glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	}
	/* (glScissor takes the corner and the size, as glViewport does) */
	if (memcmp(gl_state.scissor, scissor, sizeof(scissor)))
	{
		memcpy(gl_state.scissor, scissor, sizeof(scissor));
		glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
	}
	state_enable(&gl_state.scissor_test, GL_SCISSOR_TEST, scissor[2] > 0 && scissor[3] > 0);
	depth_range[0] = dynamic->depth_range[0];
	depth_range[1] = dynamic->depth_range[1];
	if (memcmp(gl_state.depth_range, depth_range, sizeof(depth_range)))
	{
		memcpy(gl_state.depth_range, depth_range, sizeof(depth_range));
		glDepthRange(depth_range[0], depth_range[1]);
	}

	state_enable(&gl_state.depth_test, GL_DEPTH_TEST, key->depth_test);
	if (key->depth_test && gl_state.depth_function != pipeline->depth_function)
	{
		gl_state.depth_function = pipeline->depth_function;
		glDepthFunc(pipeline->depth_function);
	}
	if (gl_state.depth_mask != key->depth_write)
	{
		gl_state.depth_mask = key->depth_write;
		glDepthMask(key->depth_write ? GL_TRUE : GL_FALSE);
	}

	state_enable(&gl_state.stencil_test, GL_STENCIL_TEST, key->stencil_test);
	if (key->stencil_test)
	{
		if (gl_state.stencil_function != pipeline->stencil_function ||
			gl_state.stencil_reference != (GLint)dynamic->stencil_reference ||
			gl_state.stencil_value_mask != (GLuint)key->stencil_read_mask)
		{
			gl_state.stencil_function = pipeline->stencil_function;
			gl_state.stencil_reference = (GLint)dynamic->stencil_reference;
			gl_state.stencil_value_mask = (GLuint)key->stencil_read_mask;
			glStencilFunc(pipeline->stencil_function, (GLint)dynamic->stencil_reference, (GLuint)key->stencil_read_mask);
		}
		if (memcmp(gl_state.stencil_operations, pipeline->stencil_operations, sizeof(pipeline->stencil_operations)))
		{
			memcpy(gl_state.stencil_operations, pipeline->stencil_operations, sizeof(pipeline->stencil_operations));
			glStencilOp(pipeline->stencil_operations[0], pipeline->stencil_operations[1],
				pipeline->stencil_operations[2]);
		}
		if (gl_state.stencil_write_mask != (GLuint)key->stencil_write_mask)
		{
			gl_state.stencil_write_mask = (GLuint)key->stencil_write_mask;
			glStencilMask((GLuint)key->stencil_write_mask);
		}
	}

	state_enable(&gl_state.blend, GL_BLEND, key->blend);
	if (key->blend)
	{
		if (gl_state.blend_source != pipeline->blend_source || gl_state.blend_destination != pipeline->blend_destination)
		{
			gl_state.blend_source = pipeline->blend_source;
			gl_state.blend_destination = pipeline->blend_destination;
			glBlendFunc(gl_state.blend_source, gl_state.blend_destination);
		}
		if (gl_state.blend_equation != pipeline->blend_equation)
		{
			gl_state.blend_equation = pipeline->blend_equation;
			glBlendEquation(pipeline->blend_equation);
		}
		if (memcmp(gl_state.blend_color, dynamic->blend_color, sizeof(dynamic->blend_color)))
		{
			memcpy(gl_state.blend_color, dynamic->blend_color, sizeof(dynamic->blend_color));
			glBlendColor(dynamic->blend_color[0], dynamic->blend_color[1], dynamic->blend_color[2],
				dynamic->blend_color[3]);
		}
	}
	if (gl_state.color_mask != key->color_mask)
	{
		gl_state.color_mask = key->color_mask;
		glColorMask((key->color_mask & XGPU_WRITE_RED) != 0, (key->color_mask & XGPU_WRITE_GREEN) != 0,
			(key->color_mask & XGPU_WRITE_BLUE) != 0, (key->color_mask & XGPU_WRITE_ALPHA) != 0);
	}

	state_enable(&gl_state.cull_face, GL_CULL_FACE, key->cull);
	if (key->cull)
	{
		if (gl_state.front_face != pipeline->front_face)
		{
			gl_state.front_face = pipeline->front_face;
			glFrontFace(pipeline->front_face);
		}
		if (gl_state.cull_mode != pipeline->cull_face)
		{
			gl_state.cull_mode = pipeline->cull_face;
			glCullFace(pipeline->cull_face);
		}
	}
#ifndef HALO_GLES
	/* ES draws filled polygons only (wireframe is a debug mode) */
	if (gl_state.polygon_mode != pipeline->polygon_mode)
	{
		gl_state.polygon_mode = pipeline->polygon_mode;
		glPolygonMode(GL_FRONT_AND_BACK, pipeline->polygon_mode);
	}
#endif

	state_enable(&gl_state.offset_fill, GL_POLYGON_OFFSET_FILL, key->depth_bias);
#ifndef HALO_GLES
	state_enable(&gl_state.offset_line, GL_POLYGON_OFFSET_LINE, key->depth_bias);
#endif
	if (key->depth_bias && memcmp(gl_state.polygon_offset, dynamic->depth_bias, sizeof(dynamic->depth_bias)))
	{
		memcpy(gl_state.polygon_offset, dynamic->depth_bias, sizeof(dynamic->depth_bias));
		glPolygonOffset(dynamic->depth_bias[0], dynamic->depth_bias[1]);
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

/* ---------- enhanced dynamic lights (display.dynamic_lights; xgpu.h)

The front end works out the frame's lights and the grid's cells; here
they are a uniform block at XGPU_DYNAMIC_LIGHTS_BINDING and an integer 3D
texture at XGPU_DYNAMIC_LIGHTS_UNIT, and the glows are drawn. Desktop
OpenGL only (layout bindings: GLSL 4.20). */
#ifndef HALO_GLES
static struct
{
	BOOL failed;
	GLuint block, cells;
	/* the glows' programs: drawn into 1, 2 and 4 layers */
	GLuint halo_programs[3];
	BOOL halo_failed[3];
	GLuint depth_sampler;
} dynamic_lights;

BOOL xgpu_backend_dynamic_lights_prepare(void)
{
	if (dynamic_lights.failed)
		return FALSE;
	if (dynamic_lights.block)
		return TRUE;
	glGenBuffers(1, &dynamic_lights.block);
	glBindBuffer(GL_UNIFORM_BUFFER, dynamic_lights.block);
	glBufferData(GL_UNIFORM_BUFFER, sizeof(struct xgpu_dynamic_lights_block), NULL, GL_DYNAMIC_DRAW);
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
		dynamic_lights.failed = TRUE;
		return FALSE;
	}
	return TRUE;
}

void xgpu_backend_dynamic_lights_upload(const struct xgpu_dynamic_lights_block *block, const void *cells)
{
	glBindBuffer(GL_UNIFORM_BUFFER, dynamic_lights.block);
	glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(*block), block);
	glBindBufferBase(GL_UNIFORM_BUFFER, XGPU_DYNAMIC_LIGHTS_BINDING, dynamic_lights.block);
	glActiveTexture(GL_TEXTURE0 + XGPU_DYNAMIC_LIGHTS_UNIT);
	glBindTexture(GL_TEXTURE_3D, dynamic_lights.cells);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, XGPU_DYNAMIC_LIGHTS_GRID_X, XGPU_DYNAMIC_LIGHTS_GRID_Y,
		XGPU_DYNAMIC_LIGHTS_GRID_Z, GL_RGBA_INTEGER, GL_UNSIGNED_INT, cells);
	glActiveTexture(GL_TEXTURE0);
	xgpu_gl_state_invalidate();
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
transparent geometry: added to what is there */
void xgpu_backend_dynamic_lights_halos(xgpu_target color_number, xgpu_target depth_number, int layers,
	const struct xgpu_dynamic_lights_halos *halos)
{
	struct gl_target *color = gl_target(color_number), *depth = gl_target(depth_number);
	GLuint program, framebuffer;
	float c[6][4], depth_parameters[4];

	program = dynamic_lights_halo_program(layers);
	if (!program)
		return;
	/* the depth the scene was drawn with, in its texture */
	if (depth->samples)
		render_target_resolve(depth);
#ifdef HALO_VR
	if (layers > 1)
		vr_fovea_views();
	framebuffer_views = layers;
	framebuffer_base = 0;
#endif
	if (color->samples)
	{
		framebuffer = framebuffer_find(color->multisample, 0, TRUE);
		color->unresolved = TRUE;
	}
	else
	{
		framebuffer = framebuffer_get(color->texture, 0);
	}
#ifdef HALO_VR
	framebuffer_views = 1;
#endif
	/* the world's projection and the screen conversion, as the lights were
	set; the depth for a distance, from two points ahead */
	memcpy(c, halos->projection, sizeof(c));
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
				p[axis] = halos->camera[axis] + forward[axis] / (length > 0.0f ? length : 1.0f) * t;
			z[point] = c[2][0] * p[0] + c[2][1] * p[1] + c[2][2] * p[2] + c[2][3];
			w[point] = c[3][0] * p[0] + c[3][1] * p[1] + c[3][2] * p[2] + c[3][3];
		}
		b = (z[0] / w[0] - z[1] / w[1]) / (1.0f / w[0] - 1.0f / w[1]);
		a = z[0] / w[0] - b / w[0];
		scale_z = halos->viewport_scale[2] != 0.0f ? halos->viewport_scale[2] : 1.0f;
		near_z = gl_state.depth_range[0] >= 0.0f && gl_state.depth_range[0] <= 1.0f ? gl_state.depth_range[0] : 0.0f;
		far_z = gl_state.depth_range[1] >= 0.0f && gl_state.depth_range[1] <= 1.0f ? gl_state.depth_range[1] : 1.0f;
		depth_parameters[0] = near_z + (far_z - near_z) * (a * c[4][2] + c[5][2] - halos->viewport_offset[2]) /
			scale_z;
		depth_parameters[1] = (far_z - near_z) * b * c[4][2] / scale_z;
		depth_parameters[2] = halos->softness;
		depth_parameters[3] = 0.0f;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glUseProgram(program);
	glUniform4fv(glGetUniformLocation(program, "c"), 6, c[0]);
	glUniform4fv(glGetUniformLocation(program, "viewport_scale"), 1, halos->viewport_scale);
	glUniform4fv(glGetUniformLocation(program, "viewport_offset"), 1, halos->viewport_offset);
	glUniform1f(glGetUniformLocation(program, "screen_offset"), halos->screen_offset);
	glUniform4fv(glGetUniformLocation(program, "halos"), halos->count * 2, halos->halos[0]);
	{
		float axes[2][4];

		memcpy(axes[0], halos->camera_right, sizeof(halos->camera_right));
		memcpy(axes[1], halos->camera_up, sizeof(halos->camera_up));
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
	glBindTexture(layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D, depth->texture);
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
	glDrawArrays(GL_TRIANGLES, 0, halos->count * 6);
	xgpu_gl_state_invalidate();
}
#else
BOOL xgpu_backend_dynamic_lights_prepare(void)
{
	return FALSE;
}

void xgpu_backend_dynamic_lights_upload(const struct xgpu_dynamic_lights_block *block, const void *cells)
{
	(void)block;
	(void)cells;
}

void xgpu_backend_dynamic_lights_halos(xgpu_target color, xgpu_target depth, int layers,
	const struct xgpu_dynamic_lights_halos *halos)
{
	(void)color;
	(void)depth;
	(void)layers;
	(void)halos;
}
#endif

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

/* the vertex constants as the game has set them (xgpu_backend_set_constants) */
static float gl_constants[XGPU_VERTEX_CONSTANT_COUNT][4];

void xgpu_backend_set_constants(unsigned long first, const float (*values)[4], unsigned long count)
{
	unsigned long index;

	for (index = 0; index < count; index++)
	{
		if (memcmp(gl_constants[first + index], values[index], sizeof(gl_constants[0])))
		{
			memcpy(gl_constants[first + index], values[index], sizeof(gl_constants[0]));
			constant_serials[first + index] = ++constants_serial;
			constant_log[constants_serial % CONSTANT_LOG_SIZE] = (unsigned char)(first + index);
			if (constants_checkpoint_first > first + index)
				constants_checkpoint_first = first + index;
			if (constants_checkpoint_last < first + index)
				constants_checkpoint_last = first + index;
		}
	}
}

/* ---------- a draw's state

The current pipeline's fixed-function state through the state cache, then
its program and the uniforms each program keeps (each one's own copy:
glUniform only for what changed since it last had them). */

static struct gl_pipeline *current_pipeline;

BOOL xgpu_backend_pipeline(const struct xgpu_pipeline_key *key)
{
	current_pipeline = gl_pipeline_get(key);
	if (!current_pipeline->program)
	{
		gl_check_errors("program");
		return FALSE;
	}
	return TRUE;
}

void xgpu_backend_draw_state(const struct xgpu_dynamic_state *dynamic, const struct xgpu_draw_uniforms *uniforms,
	unsigned long uniforms_serial)
{
	struct gl_pipeline *pipeline = current_pipeline;
	struct program_entry *entry = pipeline->program;

	gl_pipeline_apply(pipeline, dynamic);
	gl_check_errors("state");
#ifdef HALO_WEB
	device.draw_serial++;
#endif
	draw_flush();
	state_program(entry->program);
	if (pipeline->key.pixel.source_blend && entry->blend_constant >= 0)
	{
		float blend_constant[4];

		memcpy(blend_constant, dynamic->blend_color, sizeof(blend_constant));
		if (memcmp(entry->blend_constant_value, blend_constant, sizeof(blend_constant)))
		{
			memcpy(entry->blend_constant_value, blend_constant, sizeof(blend_constant));
			glUniform4fv(entry->blend_constant, 1, blend_constant);
		}
	}
#ifdef XGPU_SAMPLE_COUNTERS
	if (pipeline->key.pixel.count_samples)
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
				glUniform4fv(entry->constants + (GLint)first, (GLsizei)(last - first + 1), gl_constants[first]);
			else
				glUniform4fv(entry->constants, XGPU_VERTEX_CONSTANT_COUNT, &gl_constants[0][0]);
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
			if (constant_serials[XGPU_MODEL_LIGHT_REGISTER(light)] > entry->model_lights_serial)
				break;
		}
		if (light < XGPU_MODEL_LIGHT_COUNT)
		{
			float lights[XGPU_MODEL_LIGHT_COUNT][4];

			for (light = 0; light < XGPU_MODEL_LIGHT_COUNT; light++)
				memcpy(lights[light], gl_constants[XGPU_MODEL_LIGHT_REGISTER(light)], sizeof(lights[0]));
			glUniform4fv(entry->model_lights, XGPU_MODEL_LIGHT_COUNT, lights[0]);
		}
		entry->model_lights_serial = constants_serial;
	}

	/* and a program that has had them since needs none of them */
	if (entry->uniforms_serial == uniforms_serial)
		return;
	entry->uniforms_serial = uniforms_serial;
	uniform_vec4(entry->viewport_scale, entry->uniforms.viewport_scale, uniforms->viewport_scale, 1);
	uniform_vec4(entry->viewport_offset, entry->uniforms.viewport_offset, uniforms->viewport_offset, 1);
	uniform_float(entry->point_size, &entry->uniforms.point_size, uniforms->point_size);
	uniform_vec4(entry->ps_c0, entry->uniforms.ps_c0[0], uniforms->ps_c0[0], 8);
	uniform_vec4(entry->ps_c1, entry->uniforms.ps_c1[0], uniforms->ps_c1[0], 8);
	uniform_vec4(entry->ps_final_c0, entry->uniforms.ps_final_c0, uniforms->ps_final_c0, 1);
	uniform_vec4(entry->ps_final_c1, entry->uniforms.ps_final_c1, uniforms->ps_final_c1, 1);
	uniform_vec4(entry->fog_color, entry->uniforms.fog_color, uniforms->fog_color, 1);
	uniform_vec4(entry->fog_parameters, entry->uniforms.fog_parameters, uniforms->fog_parameters, 1);
	uniform_float(entry->alpha_reference, &entry->uniforms.alpha_reference, uniforms->alpha_reference);
	uniform_vec4(entry->bump_matrix, entry->uniforms.bump_matrix[0], uniforms->bump_matrix[0], 4);
	uniform_vec4(entry->bump_luminance, entry->uniforms.bump_luminance[0], uniforms->bump_luminance[0], 4);
	uniform_vec4(entry->texture_scale, entry->uniforms.texture_scale[0], uniforms->texture_scale[0], 4);
	uniform_float(entry->screen_offset, &entry->uniforms.screen_offset, uniforms->screen_offset);
	uniform_vec4(entry->texture_lod_bias, entry->uniforms.texture_lod_bias, uniforms->texture_lod_bias, 1);
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

/* ---------- the mirror's segments (xgpu_backend_mirror_write)

The front end keeps which pages of the contiguous window the mirror holds
and when they change; their copies are GL buffers here, one per segment,
made when first written. */

#define MIRROR_SEGMENT_COUNT (PLATFORM_CONTIGUOUS_SIZE / XGPU_MIRROR_SEGMENT_SIZE)

static struct
{
	GLuint buffers[MIRROR_SEGMENT_COUNT];
#ifdef XGPU_STREAM_RING
	/* the frame of the segment's latest glBufferSubData */
	unsigned long subdata_frame[MIRROR_SEGMENT_COUNT];
#endif
} mirror;

void xgpu_backend_mirror_write(unsigned long segment, unsigned long offset, unsigned long size, const void *data,
	BOOL unused)
{
	if (!mirror.buffers[segment])
	{
		glGenBuffers(1, &mirror.buffers[segment]);
		glBindBuffer(GL_COPY_WRITE_BUFFER, mirror.buffers[segment]);
		glBufferData(GL_COPY_WRITE_BUFFER, XGPU_MIRROR_SEGMENT_SIZE, NULL, GL_DYNAMIC_DRAW);
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
		host_gl_buffer_write(GL_COPY_WRITE_BUFFER, (unsigned int)offset, (unsigned int)size, data);
		return;
	}
	mirror.subdata_frame[segment] = device.frame;
#else
	(void)unused;
#endif
	buffer_upload(GL_COPY_WRITE_BUFFER, offset, size, data);
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
static unsigned long stream_upload_swizzled(const struct xgpu_vertex_input *declaration, unsigned long stream,
	const unsigned char *data, unsigned long size, unsigned long stride)
{
	static unsigned char *scratch;
	static unsigned long scratch_size;
	unsigned long offsets[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned long count = 0, index, vertex;

	for (index = 0; index < declaration->element_count; index++)
	{
		const struct xgpu_vertex_element *element = &declaration->elements[index];

		if (element->stream == stream && element->format == XGPU_VERTEX_COLOR)
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

/* an element's GL format */
static void attribute_format(const struct xgpu_vertex_element *element, GLint *size, GLenum *type, GLboolean *normalized)
{
	*normalized = GL_FALSE;
	switch (element->format)
	{
	case XGPU_VERTEX_FLOAT1: *size = 1; *type = GL_FLOAT; break;
	case XGPU_VERTEX_FLOAT2: *size = 2; *type = GL_FLOAT; break;
	case XGPU_VERTEX_FLOAT3: case XGPU_VERTEX_FLOAT2H: *size = 3; *type = GL_FLOAT; break;
	case XGPU_VERTEX_FLOAT4: *size = 4; *type = GL_FLOAT; break;
#ifdef HALO_GLES
	/* ES has no BGRA attributes: stream_upload_swizzled swaps the bytes */
	case XGPU_VERTEX_COLOR: *size = 4; *type = GL_UNSIGNED_BYTE; *normalized = GL_TRUE; break;
#else
	case XGPU_VERTEX_COLOR: *size = GL_BGRA; *type = GL_UNSIGNED_BYTE; *normalized = GL_TRUE; break;
#endif
	case XGPU_VERTEX_SHORT1: *size = 1; *type = GL_SHORT; break;
	case XGPU_VERTEX_SHORT2: *size = 2; *type = GL_SHORT; break;
	case XGPU_VERTEX_SHORT3: *size = 3; *type = GL_SHORT; break;
	case XGPU_VERTEX_SHORT4: *size = 4; *type = GL_SHORT; break;
	case XGPU_VERTEX_NORMSHORT1: *size = 1; *type = GL_SHORT; *normalized = GL_TRUE; break;
	case XGPU_VERTEX_NORMSHORT2: *size = 2; *type = GL_SHORT; *normalized = GL_TRUE; break;
	case XGPU_VERTEX_NORMSHORT3: *size = 3; *type = GL_SHORT; *normalized = GL_TRUE; break;
	case XGPU_VERTEX_NORMSHORT4: *size = 4; *type = GL_SHORT; *normalized = GL_TRUE; break;
	case XGPU_VERTEX_BYTE1: *size = 1; *type = GL_UNSIGNED_BYTE; *normalized = GL_TRUE; break;
	case XGPU_VERTEX_BYTE2: *size = 2; *type = GL_UNSIGNED_BYTE; *normalized = GL_TRUE; break;
	case XGPU_VERTEX_BYTE3: *size = 3; *type = GL_UNSIGNED_BYTE; *normalized = GL_TRUE; break;
	case XGPU_VERTEX_BYTE4: *size = 4; *type = GL_UNSIGNED_BYTE; *normalized = GL_TRUE; break;
	default: *size = 4; *type = GL_FLOAT; break;
	}
}

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
	unsigned long declaration;
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
static BOOL vertex_array_cache_use(const struct xgpu_vertex_input *declaration, unsigned long streams,
	const GLuint *buffers, const unsigned long *offsets)
{
	unsigned long hash = 2166136261UL ^ declaration->declaration, stream, slot, probe, index;
	struct vertex_array_cache_entry *entry = NULL;

	for (stream = 0; stream < 16; stream++)
	{
		if (streams & (1UL << stream))
		{
			hash = (hash ^ buffers[stream]) * 16777619UL;
			hash = (hash ^ offsets[stream]) * 16777619UL;
			hash = (hash ^ declaration->streams[stream].stride) * 16777619UL;
		}
	}
	hash = (hash ^ streams) * 16777619UL;
	slot = hash % VERTEX_ARRAY_CACHE_SLOTS;
	for (probe = 0; probe < VERTEX_ARRAY_CACHE_SLOTS; probe++, slot = (slot + 1) % VERTEX_ARRAY_CACHE_SLOTS)
	{
		struct vertex_array_cache_entry *candidate = vertex_array_cache.slots[slot];

		if (!candidate)
			break;
		if (candidate->declaration != declaration->declaration || candidate->streams != streams)
			continue;
		for (stream = 0; stream < 16; stream++)
		{
			if ((streams & (1UL << stream)) && (candidate->buffers[stream] != buffers[stream] ||
				candidate->offsets[stream] != offsets[stream] || candidate->strides[stream] != declaration->streams[stream].stride))
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
		entry->declaration = declaration->declaration;
		entry->streams = streams;
		for (stream = 0; stream < 16; stream++)
		{
			if (streams & (1UL << stream))
			{
				entry->buffers[stream] = buffers[stream];
				entry->offsets[stream] = offsets[stream];
				entry->strides[stream] = declaration->streams[stream].stride;
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
			const struct xgpu_vertex_element *element = &declaration->elements[index];
			GLint size;
			GLenum type;
			GLboolean normalized;

			stream = element->stream;
			if (!(streams & (1UL << stream)) || element->format == XGPU_VERTEX_NONE_FORMAT)
				continue;
			state_array_buffer(buffers[stream]);
			glEnableVertexAttribArray(element->reg);
			if (element->format == XGPU_VERTEX_PACKED3)
			{
				glVertexAttribIPointer(element->reg, 1, GL_UNSIGNED_INT, (GLsizei)declaration->streams[stream].stride,
					(const void *)(offsets[stream] + element->offset));
			}
			else
			{
				attribute_format(element, &size, &type, &normalized);
				glVertexAttribPointer(element->reg, size, type, normalized, (GLsizei)declaration->streams[stream].stride,
					(const void *)(offsets[stream] + element->offset));
			}
		}
		return TRUE;
	}
	vertex_array_cache_bind(entry);
	return TRUE;
}
#endif

#ifndef HALO_GLES
/* the vertex array each declaration's draws last used, and the streams they
had (xgpu_backend_set_vertices: the layout follows from the two), by its
number */
static struct
{
	struct vertex_array_entry *vertex_array;
	unsigned long streams;
} *declaration_arrays;
static unsigned long declaration_array_count;
#endif

void xgpu_backend_set_vertices(const struct xgpu_vertex_input *declaration)
{
	GLuint stream_buffers[16];
	unsigned long stream_offsets[16];
	BOOL enabled[XGPU_VERTEX_ATTRIBUTE_COUNT] = { FALSE };
	unsigned long index, total = 0;
#ifndef HALO_GLES
	struct vertex_layout layout;
	unsigned long streams_used = 0;

	/* (zeroed: layouts are compared and hashed whole) */
	memset(&layout, 0, sizeof(layout));
#endif

	/* the mirror's; then one reservation for everything streamed */
	for (index = 0; index < 16; index++)
	{
		if (!declaration->streams[index].present)
			continue;
		if (declaration->streams[index].segment >= 0)
		{
			stream_buffers[index] = mirror.buffers[declaration->streams[index].segment];
			stream_offsets[index] = declaration->streams[index].offset;
			continue;
		}
		stream_buffers[index] = 0;
		total += (declaration->streams[index].bytes + 15) & ~15UL;
	}
#ifdef HALO_WEB
	{
		/* every stream from the mirror: a vertex array of their own
		(vertex_array_cache) */
		unsigned long streams = 0;

		for (index = 0; index < 16; index++)
		{
			if (declaration->streams[index].present)
				streams |= 1UL << index;
		}
		if (!total && streams && vertex_array_cache_use(declaration, streams, stream_buffers, stream_offsets))
		{
			for (index = 0; index < declaration->element_count; index++)
			{
				const struct xgpu_vertex_element *element = &declaration->elements[index];

				if (declaration->streams[element->stream].present && element->format != XGPU_VERTEX_NONE_FORMAT)
					enabled[element->reg] = TRUE;
			}
			for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
			{
				if (!enabled[index])
					state_attribute_constant(index, declaration->packed_mask & (1UL << index) ? NULL :
						declaration->attributes[index]);
			}
			return;
		}
		vertex_array_cache_bind(NULL);
	}
#endif
	stream_reserve(total);
	for (index = 0; index < declaration->element_count; index++)
	{
		const struct xgpu_vertex_element *element = &declaration->elements[index];
		unsigned long stream = element->stream;
		unsigned long stride = declaration->streams[stream].stride;
		GLint size;
		GLenum type;
		GLboolean normalized;

		if (!declaration->streams[stream].present || element->format == XGPU_VERTEX_NONE_FORMAT)
			continue;
		if (!stream_buffers[stream])
		{
			const unsigned char *base = (const unsigned char *)declaration->streams[stream].data;
			unsigned long bytes = declaration->streams[stream].bytes;

#ifdef HALO_GLES
			stream_offsets[stream] = stream_upload_swizzled(declaration, stream, base, bytes, stride);
#else
			stream_offsets[stream] = stream_upload(base, bytes);
#endif
			stream_buffers[stream] = device.stream_buffer;
		}
#ifdef HALO_GLES
		if (element->format == XGPU_VERTEX_PACKED3)
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
		(void)stride;
		if (element->format == XGPU_VERTEX_PACKED3)
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
	if (declaration->declaration >= declaration_array_count)
	{
		unsigned long count = declaration_array_count ? declaration_array_count : 256;
		void *grown;

		while (count <= declaration->declaration)
			count *= 2;
		grown = realloc(declaration_arrays, count * sizeof(*declaration_arrays));
		if (!grown)
		{
			platform_log("GPU: out of memory for vertex arrays");
			abort();
		}
		declaration_arrays = grown;
		memset(declaration_arrays + declaration_array_count, 0,
			(count - declaration_array_count) * sizeof(*declaration_arrays));
		declaration_array_count = count;
	}
	if (!declaration_arrays[declaration->declaration].vertex_array ||
		declaration_arrays[declaration->declaration].streams != streams_used)
	{
		declaration_arrays[declaration->declaration].vertex_array = vertex_array_get(&layout);
		declaration_arrays[declaration->declaration].streams = streams_used;
	}
	state_vertex_array(declaration_arrays[declaration->declaration].vertex_array);
	for (index = 0; index < 16; index++)
	{
		if (streams_used & (1UL << index))
			state_vertex_buffer((GLuint)index, stream_buffers[index], stream_offsets[index],
				(GLsizei)declaration->streams[index].stride);
	}
#endif
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		if (!enabled[index])
			state_attribute_value(index, declaration->packed_mask & (1UL << index) ? NULL : declaration->attributes[index]);
	}
}

static GLenum primitive_mode(int primitive)
{
	switch (primitive)
	{
	case XGPU_POINTS: return GL_POINTS;
	case XGPU_LINES: return GL_LINES;
	case XGPU_LINE_LOOP: return GL_LINE_LOOP;
	case XGPU_LINE_STRIP: return GL_LINE_STRIP;
	case XGPU_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
	case XGPU_TRIANGLE_FAN: return GL_TRIANGLE_FAN;
	default: return GL_TRIANGLES;
	}
}

void xgpu_backend_draw(int primitive, unsigned long count)
{
#ifdef HALO_VR
	vr_gpu_time_begin();
#endif
	glDrawArrays(primitive_mode(primitive), 0, (GLsizei)count);
	gl_check_errors("draw");
}

void xgpu_backend_draw_indices(int primitive, const WORD *indices, unsigned long count)
{
#ifdef HALO_VR
	vr_gpu_time_begin();
#endif
	glDrawElements(primitive_mode(primitive), (GLsizei)count, GL_UNSIGNED_SHORT,
		(const void *)index_upload(indices, count * sizeof(WORD)));
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
	{
		unsigned long low = 0xffff, high = 0;

		for (index = 0; index < count; index++)
		{
			if (indices[index] < low)
				low = indices[index];
			if (indices[index] > high)
				high = indices[index];
		}
		*minimum = low;
		*maximum = high;
	}
	entry->minimum = (WORD)*minimum;
	entry->maximum = (WORD)*maximum;
	return FALSE;
}
#endif

void xgpu_backend_draw_indexed(int primitive, const WORD *indices, unsigned long count, unsigned long minimum,
	long segment, unsigned long offset, BOOL temporary)
{
	unsigned long index;

#ifdef HALO_VR
	vr_gpu_time_begin();
#endif
	if (segment >= 0)
	{
		/* the attributes start at vertex minimum */
		state_element_array_buffer(mirror.buffers[segment]);
		glDrawElementsBaseVertex(primitive_mode(primitive), (GLsizei)count, GL_UNSIGNED_SHORT, (const void *)offset,
			-(GLint)minimum);
		return;
	}
#ifdef HALO_WEB
	{
		unsigned long kept_minimum, kept_maximum, kept_offset;

		if (!temporary && index_cache_get(indices, count, &kept_minimum, &kept_maximum, &kept_offset))
		{
			state_element_array_buffer(index_cache.buffer);
			glDrawElements(primitive_mode(primitive), (GLsizei)count, GL_UNSIGNED_SHORT, (const void *)kept_offset);
			return;
		}
	}
#else
	(void)temporary;
#endif
#ifdef HALO_GLES
	if (!xgpu_capabilities.base_vertex)
	{
		/* the indices are copied anyway: rebase them */
		WORD *rebased = malloc(count * sizeof(WORD) + 2);

		for (index = 0; index < count; index++)
			rebased[index] = (WORD)(indices[index] - minimum);
		glDrawElements(primitive_mode(primitive), (GLsizei)count, GL_UNSIGNED_SHORT,
			(const void *)index_upload(rebased, count * sizeof(WORD)));
		free(rebased);
		return;
	}
#endif
	(void)index;
	glDrawElementsBaseVertex(primitive_mode(primitive), (GLsizei)count, GL_UNSIGNED_SHORT,
		(const void *)index_upload(indices, count * sizeof(WORD)), -(GLint)minimum);
}

void xgpu_backend_draw_immediate(int primitive, const float *vertices, unsigned long count, unsigned long written,
	const float (*attributes)[4], const WORD *indices, unsigned long index_count)
{
	unsigned long stride = XGPU_VERTEX_ATTRIBUTE_COUNT * 4 * sizeof(float);
	unsigned long offset, index;
	/* (the vertices' place in the streaming buffer, where the attributes'
	pointers start at its beginning) */
	unsigned long first_vertex = 0;

#ifdef HALO_VR
	vr_gpu_time_begin();
#endif
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
			if (written & (1UL << index))
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
						vertices + (vertex * XGPU_VERTEX_ATTRIBUTE_COUNT + registers[slot]) * 4,
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
					if (arrays[slot].entry && arrays[slot].written == written)
						entry = arrays[slot].entry;
				}
				if (!entry)
				{
					struct vertex_layout layout;

					memset(&layout, 0, sizeof(layout));
					for (slot = 0; slot < register_count; slot++)
						layout_attribute(&layout, registers[slot], 0, 4, GL_FLOAT, GL_FALSE, FALSE, slot * attribute_size);
					entry = vertex_array_get(&layout);
					arrays[next_array % 8].written = written;
					arrays[next_array % 8].entry = entry;
					next_array++;
				}
				state_vertex_array(entry);
				state_vertex_buffer(0, device.stream_buffer, offset, (GLsizei)vertex_size);
			}
#endif
			for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
			{
				if (!(written & (1UL << index)))
					state_attribute_value(index, attributes[index]);
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
						vertices + (vertex * XGPU_VERTEX_ATTRIBUTE_COUNT + index) * 4, attribute_size);
				}
			}
			offset = stream_upload(packed, count * stride);
			for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
			{
				state_attribute_stream(index, 0, device.stream_buffer, 4, GL_FLOAT, GL_FALSE, FALSE,
					(GLsizei)attribute_size, offset, index * count * attribute_size);
			}
#elif defined(HALO_GLES)
			offset = stream_upload(vertices, count * stride);
			for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
			{
				state_attribute_stream(index, 0, device.stream_buffer, 4, GL_FLOAT, GL_FALSE, FALSE, (GLsizei)stride,
					offset, index * 4 * sizeof(float));
			}
#else
			offset = stream_upload(vertices, count * stride);
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
	if (indices)
	{
		if (first_vertex)
		{
			/* (the indices from the first vertex on, past what 16 bits hold) */
			GLuint *placed = malloc(index_count * sizeof(GLuint));

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
	}
	else
	{
		glDrawArrays(primitive_mode(primitive), (GLint)first_vertex, (GLsizei)count);
	}
	gl_check_errors("immediate draw");
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

void xgpu_backend_clear(unsigned long flags, const float color[4], float depth, unsigned long stencil,
	const long (*rectangles)[4], unsigned long count)
{
	GLbitfield mask = 0;
	unsigned long index;

#ifdef HALO_VR
	vr_gpu_time_begin();
#endif
	if (flags & XGPU_CLEAR_COLOR)
	{
		glColorMask((flags & XGPU_CLEAR_RED) != 0, (flags & XGPU_CLEAR_GREEN) != 0, (flags & XGPU_CLEAR_BLUE) != 0,
			(flags & XGPU_CLEAR_ALPHA) != 0);
		glClearColor(color[0], color[1], color[2], color[3]);
		mask |= GL_COLOR_BUFFER_BIT;
	}
	if (flags & XGPU_CLEAR_DEPTH)
	{
		glDepthMask(GL_TRUE);
		glClearDepth(depth);
		mask |= GL_DEPTH_BUFFER_BIT;
	}
	if (flags & XGPU_CLEAR_STENCIL)
	{
		glStencilMask(0xff);
		glClearStencil((GLint)stencil);
		mask |= GL_STENCIL_BUFFER_BIT;
	}
	glEnable(GL_SCISSOR_TEST);
	for (index = 0; index < count; index++)
	{
		GLint y0 = (GLint)rectangles[index][1];
		GLint height = (GLint)rectangles[index][3];

#ifdef HALO_VR
		vr_flip_rows(&y0, height);
#endif
		glScissor((GLint)rectangles[index][0], y0, (GLint)rectangles[index][2], height);
		glClear(mask);
	}
	glDisable(GL_SCISSOR_TEST);
	clear_state_forget();
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

/* a target's pixels as the key's screenshot reads them: BGRA (on OpenGL ES,
GL_RGBA: what it reads), the caller's readback state kept */
static unsigned char *read_target_raw(struct gl_target *target)
{
	unsigned long width = target->gl_width, height = target->gl_height;
	unsigned char *pixels = malloc(width * height * 4);
	GLint framebuffer, draw_framebuffer, pack_buffer, alignment, row_length, skip_rows, skip_pixels;
	GLenum error;

	if (!pixels)
		return NULL;
	/* Read the render target before the display blit's vertical flip. Its
	row zero is the top of the image. Preserve the caller's readback state. */
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &framebuffer);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer);
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
	glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
	glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
	glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
	glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer_get(target->texture, 0));
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)width);
	glPixelStorei(GL_PACK_SKIP_ROWS, 0);
	glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
	glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
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
		platform_log("GPU: a screenshot's readback failed (%04x)", (unsigned)error);
		free(pixels);
		return NULL;
	}
	return pixels;
}

unsigned char *xgpu_backend_read_target(xgpu_target number, int layer, BOOL raw)
{
	struct gl_target *target = gl_target(number);

	if (raw)
		return read_target_raw(target);
	return read_target(target->texture, layer, target->gl_width, target->gl_height);
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

void xgpu_backend_visibility_frame(void)
{
#ifndef HALO_GLES
	visibility_copy_batch();
#endif
}

#ifndef XGPU_GL_HOST
void xgpu_backend_loading_art(const unsigned char *rgba)
{
	platform_loading_art(rgba);
}

int xgpu_backend_loading_owned(void)
{
#ifdef HALO_WEB
	/* The browser overlay has no ownership acknowledgement yet. */
	return 0;
#else
	return platform_loading_active() == 1;
#endif
}

void xgpu_backend_loading(int phase)
{
	/* The loading context never reads the game's textures. Complete old
	window work before giving it the default framebuffer. */
	if (phase == 1 && !platform_loading_active())
		glFinish();
	platform_loading_set(phase);
}
#endif

void xgpu_backend_present(xgpu_target number)
{
	struct gl_target *back_buffer = gl_target(number);
	int window_width, window_height, width, height, x, y;

#ifndef XGPU_GL_HOST
	if (platform_loading_active() == 1)
		return;
	if (platform_loading_active() == 2)
		platform_loading_set(0);
#endif
	platform_video_drawable_size(&window_width, &window_height);
	/* letterbox to the back buffer's aspect ratio */
	width = window_width;
	height = (int)((long)window_width * back_buffer->gl_height / back_buffer->gl_width);
	if (height > window_height)
	{
		height = window_height;
		width = (int)((long)window_height * back_buffer->gl_width / back_buffer->gl_height);
	}
	x = (window_width - width) / 2;
	y = (window_height - height) / 2;
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer_get(back_buffer->texture, 0));
	/* row 0 of the render target is the top of the picture */
	glBlitFramebuffer(0, 0, (GLint)back_buffer->gl_width, (GLint)back_buffer->gl_height,
		x, y + height, x + width, y, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	platform_video_swap();
	xgpu_gl_state_invalidate();
}

/* the end of a frame's GL work: the streamed data and visibility results of
the frames the GPU may still be drawing stay as they are */
static void frame_end_buffers(void)
{
#ifdef XGPU_SAMPLE_COUNTERS
	if (SAMPLE_COUNTING)
	{
		unsigned long count = device.ring_test_count[device.buffer_ring];
		/* (with a test still going on, its counter too: the rest of its
		draws count into the spare one, and it is not listed) */
		unsigned long used = device.counter_active < VISIBILITY_TEST_SLOTS ? device.counter_active + 1 : count;

		device.counter_active = VISIBILITY_SPARE_COUNTER;
		/* this frame's counts, for when the GPU is done with it (the
		barrier makes the shaders' counter writes visible to the copy,
		which ES 3.1 does not promise without one), then its counters
		set to 0 for the next frame's tests, after the copy */
		if (used)
			glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
		if (count)
		{
			glBindBuffer(GL_COPY_READ_BUFFER, device.visibility_counters);
			glBindBuffer(GL_COPY_WRITE_BUFFER, device.counter_snapshots[device.buffer_ring]);
			glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0,
				(GLsizeiptr)(count * sizeof(GLuint)));
		}
		if (used)
		{
			glBindBuffer(GL_COPY_READ_BUFFER, device.counter_zeros);
			glBindBuffer(GL_COPY_WRITE_BUFFER, device.visibility_counters);
			glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0,
				(GLsizeiptr)(used * sizeof(GLuint)));
			glBindBuffer(GL_COPY_READ_BUFFER, 0);
			glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
		}
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
		host_gl_read_buffer(device.counter_snapshots[ring], 0,
			(unsigned int)(device.ring_test_count[ring] * sizeof(GLuint)), device.counter_values);
		for (test = 0; test < device.ring_test_count[ring]; test++)
			device.visibility_latest[device.ring_tests[ring][test]] = device.counter_values[test];
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
}

void xgpu_backend_frame_end(void)
{
	xgpu_texture_cache_begin_frame();
	frame_end_buffers();
	device.frame++;
}

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
	struct gl_target *target = &vr_gl.eye_target.target;

	target->made = TRUE;
	target->texture = foveated ? vr_gl.fovea_texture : vr_gl.eye_image;
	target->gl_width = (unsigned long)(foveated ? vr_gl.fovea_width : vr_gl.eye_width);
	target->gl_height = (unsigned long)(foveated ? vr_gl.fovea_height : vr_gl.eye_height);
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
static void vr_fovea_composite(BOOL prepare_only)
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
	if (failed || (!prepare_only && (!vr_gl.eyes_drawn || !vr_gl.eye_image)))
	{
		if (!prepare_only)
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
		gl_load_trace("gl.link.begin", "program=%u", program);
		glLinkProgram(program);
		gl_compilation_wait(program, TRUE);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		gl_load_trace("gl.link.end", "program=%u status=%d", program, status);
		if (!vertex || !fragment || !status)
		{
			platform_log("vr: cannot make the foveation's program; the eyes show nothing");
			failed = TRUE;
			if (!prepare_only)
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
	if (prepare_only)
		return;
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

BOOL xgpu_backend_vr_supported(void)
{
	if (!host_gl_has_extension("GL_OVR_multiview2") || !glFramebufferTextureMultiviewOVR)
	{
		platform_log("vr: the driver has no GL_OVR_multiview2; playing flat");
		return FALSE;
	}
	return TRUE;
}

void xgpu_backend_vr_initialize(struct xgpu_vr_info *info)
{
	int stage;

	vr_gl.eye_width = info->eye_width;
	vr_gl.eye_height = info->eye_height;
	/* Preparation has the same two-layer attachment without borrowing an
	OpenXR image while the loading frame loop owns the runtime. */
	glGenTextures(1, &vr_gl.preparation_image);
	glBindTexture(GL_TEXTURE_2D_ARRAY, vr_gl.preparation_image);
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_SRGB8_ALPHA8, info->eye_width, info->eye_height,
		2, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
	vr_gl.hud_width = info->hud_width;
	vr_gl.hud_height = info->hud_height;
	vr_gl.scope_size = info->scope_size;
	vr_gl.statistics = info->statistics;
	vr_gl.gpu_time = vr_gl.statistics && config_boolean("debug.vr_gpu_time");
	if (vr_gl.gpu_time)
	{
#ifndef HALO_GLES
		vr_query_counter = (void (GLAPIENTRY *)(GLuint, GLenum))SDL_GL_GetProcAddress("glQueryCounter");
		vr_query_result = (void (GLAPIENTRY *)(GLuint, GLenum, GLuint64 *))SDL_GL_GetProcAddress("glGetQueryObjectui64v");
		vr_query_bits = (void (GLAPIENTRY *)(GLenum, GLenum, GLint *))SDL_GL_GetProcAddress("glGetQueryiv");
		if (vr_query_counter && vr_query_result && vr_query_bits)
			vr_query_bits(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &vr_gl.gpu_query_bits);
#endif
		if (vr_gl.gpu_query_bits > 0)
			glGenQueries(VR_GPU_TIME_FRAMES * 2, &vr_gl.gpu_queries[0][0]);
		else
		{
			platform_log("vr: this GL context has no GPU timestamps; GPU duration unavailable");
			vr_gl.gpu_time = FALSE;
		}
	}
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
	memset(vr_gl.eye_transforms, 0, sizeof(vr_gl.eye_transforms));
	for (stage = 0; stage < 4; stage++)
	{
		vr_gl.eye_transforms[0][stage * 5] = 1.0f;
		vr_gl.eye_transforms[1][stage * 5] = 1.0f;
	}
	vr_gl.screen_corrections[0][0] = vr_gl.screen_corrections[0][1] = 1.0f;
	vr_gl.screen_corrections[1][0] = vr_gl.screen_corrections[1][1] = 1.0f;
	{
		GLint counters = 0;

		/* the eye pass's visibility tests are counted by the pixel shaders
		(XGPU_SAMPLE_COUNTERS) */
		glGetIntegerv(GL_MAX_FRAGMENT_ATOMIC_COUNTERS, &counters);
		SAMPLE_COUNTING = counters > 0;
		sample_counters_create();
		if (!SAMPLE_COUNTING)
			platform_log("vr: no atomic counters: the visibility tests are drawn in the left eye alone");
		xgpu_backend_capabilities.sample_counting = SAMPLE_COUNTING;
	}
	vr_fovea_initialize();
	vr_gl.ready = TRUE;
	info->foveated = vr_gl.foveated;
	info->fovea_width = vr_gl.fovea_width;
	info->fovea_height = vr_gl.fovea_height;
}

void xgpu_backend_vr_eyes(xgpu_target number, unsigned int image)
{
	if (!image)
		image = vr_gl.preparation_image;
	if (vr_gl.eye_image != image)
	{
		glBindTexture(GL_TEXTURE_2D_ARRAY, image);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
	}
	vr_gl.eye_image = image;
	vr_gl.eye_target.number = number;
	/* (foveated, its four layers, made into the image at the present) */
	vr_eye_target(vr_gl.foveated);
	if (vr_gl.foveated)
		vr_fovea_place();
}

void xgpu_backend_vr_pass(int pass)
{
	vr_gl.pass = pass;
	vr_gl.flipped = FALSE;
	/* (depth clamping: the sky's alone, halo_vr_sky) */
	glDisable(GL_DEPTH_CLAMP);
}

void xgpu_backend_vr_groups_reset(void)
{
	vr_gl.groups_drawn = 0;
}

void xgpu_backend_vr_timing(int mark)
{
	vr_timing(mark);
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
#ifndef XGPU_GL_HOST
	halo_gpu_preparation_sky(sky);
#endif
	if (!vr_gl.ready)
		return;
	if (sky && vr_gl.pass == HALO_VR_PASS_EYES)
		glEnable(GL_DEPTH_CLAMP);
	else
		glDisable(GL_DEPTH_CLAMP);
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

/* A submitted layer, premultiplied over grey, for the same screenshot
checks as the eyes. The separate HUD panels need their own pictures. */
static void vr_write_layer_screenshot(const char *name, GLuint image, int width, int height)
{
	unsigned char *pixels;
	int index, channel;

	if (!image || width <= 0 || height <= 0 || !(pixels = read_target(image, -1, width, height)))
		return;
	for (index = 0; index < width * height; index++)
	{
		unsigned char *pixel = pixels + index * 4;

		for (channel = 0; channel < 3; channel++)
		{
			int value = pixel[channel] + (255 - pixel[3]) * 96 / 255;

			pixel[channel] = (unsigned char)(value > 255 ? 255 : value);
		}
		pixel[3] = 0xff;
	}
	xgpu_write_bmp(name, pixels, width, height, TRUE);
	free(pixels);
}

/* debug.screenshot_every in VR: both eyes side by side (eyes<frame>.bmp),
the HUD over grey (hud<frame>.bmp), and both eyes as the headset shows them,
the layers over them as the frame submitted them (view<frame>.bmp: the
compositor's work, done here, roughly, for a picture) */
static void vr_write_screenshots(struct gl_target *hud, GLuint hud_image)
{
	unsigned long width = (unsigned long)vr_gl.eye_width, height = (unsigned long)vr_gl.eye_height;
	unsigned long row, index;
	struct vr_host_layers layers;
	struct vr_host_pose head;

	if (!*config_string("debug.screenshot_directory"))
		return;
	vr_write_layer_screenshot("hud-layer", hud_image, vr_gl.hud_width, vr_gl.hud_height);
	{
		int panel;

		for (panel = 0; panel < VR_PANEL_COUNT; panel++)
		{
			int panel_width, panel_height;
			char name[32];

			halo_vr_panel_size(panel, &panel_width, &panel_height);
			snprintf(name, sizeof(name), "panel%d", panel);
			vr_write_layer_screenshot(name, vr_gl.panel_images[panel], panel_width, panel_height);
		}
	}
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
		xgpu_write_bmp("eyes", both, width * 2, height, TRUE);
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
#ifdef XGPU_GL_HOST
			/* (the view's picture is composed by vr_visor.c, which is the
			guest's: in the host, the images alone) */
			(void)eyes;
#else
			vr_visor_composite(eyes, (int)width, (int)height, &layers, &head, hud_pixels, vr_gl.hud_width,
				vr_gl.hud_height, panel_pixels, panel_width,
				panel_height, scope_pixels, vr_gl.scope_size);
#endif
			for (panel = 0; panel < VR_PANEL_COUNT; panel++)
				free(panel_pixels[panel]);
#ifndef XGPU_GL_HOST
			for (row = 0; row < height; row++)
			{
				memcpy(both + row * width * 8, left + row * width * 4, width * 4);
				memcpy(both + row * width * 8 + width * 4, right + row * width * 4, width * 4);
			}
			for (index = 0; index < width * 2 * height; index++)
				both[index * 4 + 3] = 0xff;
			xgpu_write_bmp("view", both, width * 2, height, TRUE);
#endif
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
				xgpu_write_bmp("scope", scope_pixels, (unsigned long)vr_gl.scope_size, (unsigned long)vr_gl.scope_size, TRUE);
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
		unsigned char *pixels = read_target(hud->texture, -1, hud->gl_width, hud->gl_height);

		for (index = 0; index < hud->gl_width * hud->gl_height; index++)
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
		xgpu_write_bmp("hud", pixels, hud->gl_width, hud->gl_height, FALSE);
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


/* a group's target, as large as the HUD's, cleared as it is first drawn
into in the frame */
static GLuint vr_group_target(const struct gl_target *hud, int group)
{
	GLuint *texture = &vr_gl.group_textures[group];

	if (vr_gl.group_width != hud->gl_width || vr_gl.group_height != hud->gl_height)
	{
		int index;

		for (index = 0; index < HALO_VR_HUD_GROUPS; index++)
		{
			if (vr_gl.group_textures[index])
				glDeleteTextures(1, &vr_gl.group_textures[index]);
			vr_gl.group_textures[index] = 0;
		}
		vr_gl.group_width = hud->gl_width;
		vr_gl.group_height = hud->gl_height;
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
static GLuint vr_light_map(const struct vr_visor_hud_image *look, BOOL prepare_only)
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

	if (failed || (!prepare_only && (look->react <= 0.0f || !look->curve)))
		return 0;
	if (!prepare_only && (!vr_gl.eyes_drawn || !vr_gl.eye_target.target.texture))
		return filled ? texture : 0;
	if (!program)
	{
		GLuint vertex = xgpu_compile_shader(GL_VERTEX_SHADER, vertex_source, "VR light vertex");
		GLuint fragment = xgpu_compile_shader(GL_FRAGMENT_SHADER, fragment_source, "VR light pixel");
		GLint status = 0;

		program = glCreateProgram();
		glAttachShader(program, vertex);
		glAttachShader(program, fragment);
		gl_load_trace("gl.link.begin", "program=%u", program);
		glLinkProgram(program);
		gl_compilation_wait(program, TRUE);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		gl_load_trace("gl.link.end", "program=%u status=%d", program, status);
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
	if (prepare_only)
		return 0;
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
		gl_load_trace("gl.link.begin", "program=%u", program);
		glLinkProgram(program);
		gl_compilation_wait(program, TRUE);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		gl_load_trace("gl.link.end", "program=%u status=%d", program, status);
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

static BOOL vr_copy_hud(struct gl_target *hud, GLuint image)
{
	struct vr_visor_hud_image look;
	struct vr_visor_panel panels[VR_PANEL_COUNT];
	int panel;

	GLuint light;

	vr_visor_hud_image(&look);
	light = vr_light_map(&look, FALSE);
	if (!vr_copy_hud_image(hud->texture, image, vr_gl.hud_width, vr_gl.hud_height, &look, NULL, light))
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
static BOOL vr_copy_scope(GLuint source, GLuint image, int size, BOOL prepare_only)
{
	static GLuint program, sampler;
	static BOOL failed;
	static GLint size_location, zoom_location, zoom_cell_location, smartlink_location, window_location,
		height_location, text_location, text_cell_location, animation_location;
	float zoom_height, reticle[2], range, animation[4] = { 0.0f };
	int zoom, smartlink;
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
		gl_load_trace("gl.link.begin", "program=%u", program);
		glLinkProgram(program);
		gl_compilation_wait(program, TRUE);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		gl_load_trace("gl.link.end", "program=%u status=%d", program, status);
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
	if (prepare_only)
		return TRUE;
	zoom = (int)(halo_vr_scope_zoom(&zoom_height) + 0.5f);
	smartlink = halo_vr_scope_smartlink(reticle) && halo_vr_scope_window(&window);
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
		gl_load_trace("gl.link.begin", "program=%u", program);
		glLinkProgram(program);
		gl_compilation_wait(program, TRUE);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		gl_load_trace("gl.link.end", "program=%u status=%d", program, status);
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
			GLenum target = 0;
#ifdef XGPU_GL_HOST
			/* (the texture's header's guest address: the host's cache) */
			GLuint texture = xgpu_host_texture_header((unsigned int)(uintptr_t)look.noise_texture[map], &target);
#else
			struct xgpu_texture_description description;
			GLuint texture = xgpu_texture_get((const DWORD *)look.noise_texture[map], NULL, &target, &description);
#endif

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
	struct gl_target *depth = NULL;
	int eye, row;
	float rows[6][4];

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
		gl_load_trace("gl.link.begin", "program=%u", program);
		glLinkProgram(program);
		gl_compilation_wait(program, TRUE);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		gl_load_trace("gl.link.end", "program=%u status=%d", program, status);
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
	depth = vr_gl.foveated ? NULL : vr_gl.eye_depth;
	if (depth && depth->layers != 2)
		depth = NULL;
	for (eye = 0; eye < 2; eye++)
	{
		for (row = 0; row < 3; row++)
			memcpy(rows[eye * 3 + row], rim.eye[eye][row], sizeof(rows[0]));
	}
	framebuffer_views = 2;
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_get(vr_gl.eye_target.target.texture, depth ? depth->texture : 0));
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
		gl_load_trace("gl.link.begin", "program=%u", program);
		glLinkProgram(program);
		gl_compilation_wait(program, TRUE);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		gl_load_trace("gl.link.end", "program=%u status=%d", program, status);
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
the world through, where each eye's ray meets the curved wall it is in (a
cylinder about the eye: the opening all the way round it, its half angle
180 degrees, or a part of one), given the feed's look as the smart-link's
window has it: a faint blue tint, scan lines, its edges darkened, a thin
line of the HUD's blue about it with heavier corners. All the way round, it
is panels between the ribs every 45 degrees (none ahead nor behind): the
front one's from 45
degrees left to 45 right, with the readout along its bottom (the zoom, the
range, the rounds of the vehicle's weapon and its second's), and about it
the hull cameras', a little dimmer, labelled L HULL, R HULL and REAR. The
feed washes over the opening from its top down as the grate closes (wash 0
to 1: a bright line sweeping down, interference about it, the world above
it and the feed below; the readout typed last); dark (off), it collapses as
a screen switched off. What is nearer than the opening (the hands, the
ribs) is left as it is. The images are copied first (the pass reads them),
and written as they are encoded. */
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
		"const int glyphs[25] = int[25](31599, 11415, 29671, 29647, 23497, 31183, 31215, 29257, 31727,\n"
		"	31695, 2728, 27565, 11245, 27501, 14699, 31143, 24557, 11114, 4772, 2, 448, 0, 18727, 23533, 23407);\n"
		/* (the hull cameras' labels: L HULL, R HULL, REAR; L, H and U after
		the space) */
		"const int labels[18] = int[18](22, 21, 23, 24, 22, 22, 11, 21, 23, 24, 22, 22, 11, 15, 12, 11, -1, -1);\n"
		"float label_lit(vec2 q, int which, float typed)\n"
		"{\n"
		"	if (q.x < 0.0 || q.y < 0.0 || q.y >= 5.0)\n"
		"		return 0.0;\n"
		"	ivec2 cell = ivec2(floor(q));\n"
		"	int glyph = cell.x / 4, column = cell.x - glyph * 4, count = which == 2 ? 4 : 6;\n"
		"	if (glyph >= count || float(glyph) >= typed || column > 2)\n"
		"		return 0.0;\n"
		"	return float((glyphs[labels[which * 6 + glyph]] >> (14 - cell.y * 3 - column)) & 1);\n"
		"}\n"
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
		"	float angle = atan(dot(rel, right[eye].xyz), dot(rel, forward[eye].xyz));\n"
		"	float v = dot(rel, u) / shape.z;\n"
		"	float av = abs(v);\n"
		"	float px = 1.5 * length(vec2(dFdx(v), dFdy(v)));\n"
		"	if (abs(angle) > shape.y + 6.0 * px * shape.z / shape.x || av > 1.0 + 6.0 * px)\n"
		"		discard;\n"
		"	if (depth_map.x > 0.0)\n"
		"	{\n"
		"		float z = texture(depth_texture, vec3(f, float(layers == 4 ? 2 * eye : eye))).r;\n"
		"		if (z < depth_map.x * (1.0 - depth_map.y / (0.96 * s)))\n"
		"			discard;\n"
		"	}\n"
		"	vec3 hud = vec3(0.45, 0.72, 1.0), white = mix(hud, vec3(1.0), 0.6);\n"
		"	float pi = 3.14159265, ad = abs(angle);\n"
		"	float centre = ad < 0.25 * pi ? 0.0 : ad < 0.5 * pi ? 0.375 * pi : ad < 0.75 * pi ? 0.625 * pi : pi;\n"
		"	float halfw = ad < 0.25 * pi || ad >= 0.75 * pi ? 0.25 * pi : 0.125 * pi;\n"
		"	float lx = (ad - centre) / halfw;\n"
		"	if (shape.y < pi - 0.01)\n"
		"	{\n"
		"		centre = 0.0;\n"
		"		halfw = shape.y;\n"
		"		lx = angle / shape.y;\n"
		"	}\n"
		"	float alx = abs(lx);\n"
		"	bool front = ad < 0.25 * pi || shape.y < pi - 0.01;\n"
		"	float line_y = 1.0 - 2.0 * wash.x;\n"
		"	float washed = wash.x >= 1.0 ? 1.0 : smoothstep(line_y + 0.02, line_y - 0.02, v);\n"
		"	float near_line = wash.x < 1.0 ? exp(-abs(v - line_y) / 0.08) : 0.0;\n"
		"	float band = hash(vec2(floor((v + 1.0) * 24.0), wash.y));\n"
		"	float offset = near_line * step(0.6, band) * (band - 0.8) * 0.04 * image.x;\n"
		"	vec3 col = texture(eyes, vec3((gl_FragCoord.xy + vec2(offset, 0.0)) / image, float(eye))).rgb;\n"
		"	vec3 world = texture(eyes, vec3(f, float(eye))).rgb;\n"
		"	col += near_line * 0.15 * (hash(gl_FragCoord.xy + vec2(wash.y)) - 0.5);\n"
		"	float edge = min((1.0 - alx) * halfw * shape.x / shape.z, 1.0 - av);\n"
		"	vec3 feed = col * vec3(0.92, 1.0, 1.08) * (front ? 1.0 : 0.78);\n"
		"	feed *= 1.0 - 0.07 * (0.5 + 0.5 * sin(gl_FragCoord.y * 3.14159 / 3.0));\n"
		"	feed *= 0.6 + 0.4 * smoothstep(0.0, 0.12, edge);\n"
		"	vec3 result = mix(world, feed, washed);\n"
		"	float sweep = wash.x > 0.0 && wash.x < 1.0 ? exp(-abs(v - line_y) / 0.006) : 0.0;\n"
		"	result = mix(result, white, sweep * 0.9);\n"
		"	float outside = max((alx - 1.0) * halfw * shape.x / shape.z, av - 1.0);\n"
		"	float frame = (1.0 - smoothstep(0.0, 2.0 * px, abs(outside - 2.0 * px))) * 0.7;\n"
		"	float corner = step(1.0 - 0.07 * shape.z / (halfw * shape.x), alx) * step(0.85, av);\n"
		"	frame = max(frame, corner * (1.0 - smoothstep(0.0, 3.0 * px, abs(outside - 3.0 * px))));\n"
		"	result = mix(result, hud, frame * clamp(wash.x * 3.0, 0.0, 1.0) * (front ? 1.0 : 0.7));\n"
		"	float cell = 0.0038 / shape.z;\n"
		"	float across = 0.0038 / shape.x;\n"
		"	float typed_part = clamp((wash.x - 0.7) / 0.3, 0.0, 1.0);\n"
		"	if (front)\n"
		"	{\n"
		"		int count = 0;\n"
		"		for (int i = 0; i < 44; i++)\n"
		"			count += text[i] >= 0 ? 1 : 0;\n"
		"		vec2 g = vec2(angle / across + float(count * 4 - 1) * 0.5, (-1.0 + 0.045 / shape.z + 5.0 * cell - v) / cell);\n"
		"		float typed = floor(typed_part * float(count) + 0.5);\n"
		"		result = mix(result, vec3(0.0), line_lit(g - vec2(0.5, 0.5), typed) * 0.7 * washed);\n"
		"		result = mix(result, hud, line_lit(g, typed) * 0.95 * washed);\n"
		"	}\n"
		"	else\n"
		"	{\n"
		"		int which = ad >= 0.75 * pi ? 2 : angle < 0.0 ? 0 : 1;\n"
		"		int count = which == 2 ? 4 : 6;\n"
		"		vec2 g = vec2((ad - centre) * sign(angle) / across + float(count * 4 - 1) * 0.5, (-1.0 + 0.045 / shape.z + 5.0 * cell - v) / cell);\n"
		"		if (which == 2)\n"
		"			g.x = (angle >= 0.0 ? angle - pi : angle + pi) / across + float(count * 4 - 1) * 0.5;\n"
		"		float lit = label_lit(g, which, floor(typed_part * float(count) + 0.5));\n"
		"		float shadow = label_lit(g - vec2(0.5, 0.5), which, floor(typed_part * float(count) + 0.5));\n"
		"		result = mix(result, vec3(0.0), shadow * 0.6 * washed);\n"
		"		result = mix(result, hud * 0.85, lit * 0.9 * washed);\n"
		"	}\n"
		"	float off = shape.w;\n"
		"	if (off > 0.0)\n"
		"	{\n"
		"		float open_half = max(1.0 - smoothstep(0.0, 0.7, off), 0.004);\n"
		"		float shown = step(av, open_half) * step(alx, 1.0);\n"
		"		float line = (1.0 - smoothstep(0.0, 0.02, abs(av - open_half))) * step(alx, 1.0 - smoothstep(0.7, 1.0, off));\n"
		"		result = mix(vec3(0.01), result, shown * (1.0 - smoothstep(0.85, 1.0, off)));\n"
		"		result = mix(result, white, line * (1.0 - smoothstep(0.9, 1.0, off)) * step(0.05, off));\n"
		"		if (alx > 1.0 || av > 1.0)\n"
		"			result = mix(result, vec3(0.01), off);\n"
		"	}\n"
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

	if (failed || !vr_gl.eyes_drawn || !eyes || !halo_vr_cockpit_window(&window, &display))
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
		gl_load_trace("gl.link.begin", "program=%u", program);
		glLinkProgram(program);
		gl_compilation_wait(program, TRUE);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		gl_load_trace("gl.link.end", "program=%u status=%d", program, status);
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
		float shape[4] = { display.radius, display.half_angle, display.half_height, display.off };

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
		gl_load_trace("gl.link.begin", "program=%u", program);
		glLinkProgram(program);
		gl_compilation_wait(program, TRUE);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		gl_load_trace("gl.link.end", "program=%u status=%d", program, status);
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
		gl_load_trace("gl.link.begin", "program=%u", program);
		glLinkProgram(program);
		gl_compilation_wait(program, TRUE);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		gl_load_trace("gl.link.end", "program=%u status=%d", program, status);
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
	struct gl_target *depth;
	GLuint image;

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
	depth = vr_gl.eye_depth;
	if (!depth || depth->layers != 2 || !(image = halo_vr_image(VR_SWAPCHAIN_DEPTH)))
		return;
	if (!program)
	{
		GLuint vertex = xgpu_compile_shader(GL_VERTEX_SHADER, vertex_source, "VR depth vertex");
		GLuint fragment = xgpu_compile_shader(GL_FRAGMENT_SHADER, fragment_source, "VR depth pixel");
		GLint status = 0;

		program = glCreateProgram();
		glAttachShader(program, vertex);
		glAttachShader(program, fragment);
		gl_load_trace("gl.link.begin", "program=%u", program);
		glLinkProgram(program);
		gl_compilation_wait(program, TRUE);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		gl_load_trace("gl.link.end", "program=%u status=%d", program, status);
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
	glBindTexture(GL_TEXTURE_2D_ARRAY, depth->texture);
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
		gl_load_trace("gl.link.begin", "program=%u", program);
		glLinkProgram(program);
		gl_compilation_wait(program, TRUE);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		gl_load_trace("gl.link.end", "program=%u status=%d", program, status);
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

void xgpu_backend_vr_compose(const struct xgpu_vr_frame *frame)
{
	struct gl_target *eye_depth = frame->eye_depth ? gl_target(frame->eye_depth) : NULL;
	struct gl_target *hud = frame->hud ? gl_target(frame->hud) : NULL;

	vr_gl.eyes_drawn = frame->eyes_drawn;
	vr_gl.hud_drawn = frame->hud_drawn;
	vr_gl.scope_drawn = frame->scope_drawn;
	vr_gl.hud_panels = frame->hud_panels;
	vr_gl.eye_depth = eye_depth;
	vr_gl.hud_image = 0;
	/* foveated, the eyes' images made from the inset and the periphery,
	which the rest draws into */
	vr_fovea_composite(FALSE);
	/* a feed cockpit's display, under the smart-link's window */
	vr_draw_cockpit_display(eye_depth ? eye_depth->texture : 0, eye_depth ? eye_depth->layers : 0);
	/* the scope's view: the smart-link's window drawn into the eyes, or
	the lens's image for its layer */
	vr_gl.scope_image = 0;
	if (vr_gl.scope_drawn && vr_gl.eyes_drawn)
	{
		struct gl_target *scope = frame->scope ? gl_target(frame->scope) : NULL;
		struct halo_vr_scope_window window;
		GLuint image;

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
			if (vr_copy_scope(scope->texture, vr_gl.scope_window_texture, vr_gl.scope_size, FALSE))
				vr_draw_scope_window(eye_depth ? eye_depth->texture : 0, eye_depth ? eye_depth->layers : 0);
		}
		else if (scope && (image = halo_vr_image(VR_SWAPCHAIN_SCOPE)) != 0 &&
			vr_copy_scope(scope->texture, image, vr_gl.scope_size, FALSE))
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
	if (hud)
	{
		vr_gl.hud_image = halo_vr_image(VR_SWAPCHAIN_HUD);
		if (!vr_gl.hud_image || !vr_copy_hud(hud, vr_gl.hud_image))
			vr_gl.hud_image = 0;
	}
	vr_gl.pass = HALO_VR_PASS_NONE;
	vr_gl.flipped = FALSE;
}

void xgpu_backend_vr_submit(const struct xgpu_vr_frame *frame, BOOL screenshot)
{
	xgpu_gl_state_invalidate();
	/* Arm after the last present, start at the first clear or draw after
	the runtime's frame wait, and include this frame's composition. */
	vr_gpu_time_end();
	/* (the GPU starts on the frame while the game updates: halo_vr_present) */
	glFlush();
	if (vr_gl.gpu_time)
		vr_gpu_time_read();
	halo_vr_present(vr_gl.eyes_drawn, vr_gl.hud_image != 0, vr_gl.scope_image != 0);
	if (screenshot)
		vr_write_screenshots(frame->hud ? gl_target(frame->hud) : NULL, vr_gl.hud_image);
	vr_gl.eyes_drawn = FALSE;
	vr_gl.hud_drawn = FALSE;
	vr_gl.scope_drawn = FALSE;
	vr_gl.eye_depth = NULL;
}
#else
BOOL xgpu_backend_vr_supported(void)
{
	return FALSE;
}

void xgpu_backend_vr_initialize(struct xgpu_vr_info *info)
{
	(void)info;
}

void xgpu_backend_vr_eyes(xgpu_target number, unsigned int image)
{
	(void)number;
	(void)image;
}

void xgpu_backend_vr_pass(int pass)
{
	(void)pass;
}

void xgpu_backend_vr_groups_reset(void)
{
}

void xgpu_backend_vr_timing(int mark)
{
	(void)mark;
}

void xgpu_backend_vr_compose(const struct xgpu_vr_frame *frame)
{
	(void)frame;
}

void xgpu_backend_vr_submit(const struct xgpu_vr_frame *frame, BOOL screenshot)
{
	(void)frame;
	(void)screenshot;
}
#endif

/* Map draws prepare their own programs. Composition has fixed programs
too: make these before handing presentation back to the first real frame. */
double xgpu_backend_prepared(void)
{
	Uint64 started = SDL_GetTicksNS();

#ifdef HALO_VR
	if (vr_gl.ready)
	{
		vr_fovea_composite(TRUE);
		if (vr_gl.scope_size > 0)
			vr_copy_scope(0, 0, 0, TRUE);
		vr_light_map(NULL, TRUE);
		xgpu_gl_state_invalidate();
	}
#endif
	return (double)(SDL_GetTicksNS() - started) * 1e-9;
}

void xgpu_backend_prepare(void)
{
}
