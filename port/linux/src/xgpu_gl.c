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

#define gl_load_trace(...) ((void)0)

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

#ifdef HALO_GLES
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
by queries: on ES */
#ifdef HALO_GLES
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
#endif

/* (xgpu.h; the front end sets it from xgpu_backend_vr_initialize's answer:
never here, as this backend has no VR mode, the Vulkan one's alone) */
extern int xgpu_foveated;

/* a target's storage by its number (grown as the front end numbers more) */
static struct gl_target *gl_target(xgpu_target number)
{
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
	(void)object;
	(void)program;
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


void xgpu_backend_target_multisample(xgpu_target target, int samples)
{
	render_target_multisample(gl_target(target), samples);
}

void xgpu_backend_target_resolve(xgpu_target target)
{
	render_target_resolve(gl_target(target));
}


void xgpu_backend_set_targets(xgpu_target color_number, int hud_group, xgpu_target depth_number, int views, int base,
	int samples)
{
	struct gl_target *color = color_number ? gl_target(color_number) : NULL;
	struct gl_target *depth = depth_number ? gl_target(depth_number) : NULL;
	GLuint color_texture = color ? color->texture : 0;

	(void)hud_group;
	(void)views;
	(void)base;
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
}

void xgpu_backend_clear_target(xgpu_target number, const float color[4])
{
	struct gl_target *target = gl_target(number);

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
	/* (debug.gl_debug: gpu_validation's old name) */
	if (config_boolean("debug.gpu_validation") || config_boolean("debug.gl_debug"))
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

/* display.hdr: none here (xgpu_backend_capabilities.hdr is FALSE, so the
front end never asks for it) */
void xgpu_backend_hdr(BOOL active, int mode, int paper_white)
{
	(void)active;
	(void)mode;
	(void)paper_white;
}

void xgpu_backend_tone_map(xgpu_target target, const long corners[4])
{
	(void)target;
	(void)corners;
}

BOOL xgpu_backend_display_hdr(float *headroom)
{
	*headroom = 1.0f;
	return FALSE;
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
		(void)area;
		(void)views;
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
		return device.visibility_latest[index];
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
	nv2a_pixel_program_decode(key, &program);
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
#ifdef HALO_WEB
			if (source->drawn && gl_textures[stage] == gl_target(source->drawn)->texture)
				gl_textures[stage] = feedback_copy(target, source->written);
#endif
		}
		else if (source->source == XGPU_SOURCE_TEXTURE)
		{
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
		}
		if (source->source != XGPU_SOURCE_NONE)
			configure_sampler(stage, &source->sampler);
	}
	described_count = 0;
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
/* ES has no debug callback in 3.0; debug.gpu_validation (or its old name,
debug.gl_debug) polls glGetError around each draw instead, reporting each
distinct error a few times */
static void gl_check_errors(const char *where)
{
	static int enabled = -1;
	static unsigned long reports;
	GLenum error;

	if (enabled < 0)
		enabled = config_boolean("debug.gpu_validation") || config_boolean("debug.gl_debug");
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
	if (color->samples)
	{
		framebuffer = framebuffer_find(color->multisample, 0, TRUE);
		color->unresolved = TRUE;
	}
	else
	{
		framebuffer = framebuffer_get(color->texture, 0);
	}
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
	glDrawArrays(primitive_mode(primitive), 0, (GLsizei)count);
	gl_check_errors("draw");
}

void xgpu_backend_draw_indices(int primitive, const WORD *indices, unsigned long count)
{
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

void xgpu_backend_loading_art(const unsigned char *rgba)
{
	platform_loading_art(rgba);
}

void xgpu_backend_loading_scene(const void *data, unsigned long bytes)
{
	platform_loading_scene(data, bytes);
}

void xgpu_backend_loading_progress(float progress)
{
	platform_loading_progress(progress);
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

void xgpu_backend_present(xgpu_target number)
{
	struct gl_target *back_buffer = gl_target(number);
	int window_width, window_height, width, height, x, y;

	if (platform_loading_active() == 1)
		return;
	if (platform_loading_active() == 2)
		platform_loading_set(0);
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

/* Map draws prepare their own programs. Composition has fixed programs
too: make these before handing presentation back to the first real frame. */
double xgpu_backend_prepared(void)
{
	Uint64 started = SDL_GetTicksNS();

	return (double)(SDL_GetTicksNS() - started) * 1e-9;
}

void xgpu_backend_prepare(void)
{
}
