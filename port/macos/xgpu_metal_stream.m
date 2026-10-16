/*
XGPU_METAL_STREAM.M

The Metal backend as the command stream's backend table (struct
host_render_backend, port/android/host/host_render_stream.h): each record's
entry, in terms of xgpu_metal.h's functions. The render thread calls them,
in the records' order.

What the records name, this file keeps: the game's render targets (by its
numbers) as the backend's textures; the mirror's segments as buffers; the
pipelines' definitions, the vertex programs, the shaders made for each
pipeline; the vertex layout of the last set_vertices. A draw's pipeline is
the pipeline's fixed-function part with that layout's attributes.

A pipeline's shaders come from a provider (xgpu_metal_stream_set_shaders:
the MSL emitter's, xgpu_metal_shaders.c; tests give their own), made for
the pipeline's key and the draw's variant (the inputs read as integers, the
border colours the shader draws), as are the uniforms both stages read. The
stages' textures are the cache's (xgpu_metal_textures.c), looked at again
at a draw once a stage has changed or a watched page has been written; a
stage that samples the target the draw draws into samples a copy of it.

Not here: the enhanced dynamic lights, anti-aliasing's passes and the VR
mode (no entries: the stream skips them), and a render target's mip levels
drawn one by one (its first level is sampled).

The interface's values (xgpu_backend.h: the primitives, the clear's parts,
the samplers' filters and addresses, the vertex elements' formats) are read
by the functions under "the interface's values".
*/

#if __has_include("host_render_stream.h")

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

/* (host.h's host_page_size is Mach's too: not used here) */
#define host_page_size host_page_size_of_guest_memory
#include "host.h"
#undef host_page_size
#include "host_render_stream.h"
#include "xgpu_metal_internal.h"
#include "xgpu_metal_pipeline.h"
#include "xgpu_metal_stream.h"
#include "xgpu_metal_textures.h"

#include <string.h>

/* the mirror's segments: 4 MB of the contiguous window each (d3d8_gl.c's
MIRROR_SEGMENT_SIZE) */
#define MIRROR_SEGMENT_SIZE 0x400000u
#define MIRROR_SEGMENT_MAXIMUM 64

_Static_assert(sizeof(struct xgpu_stream_dynamic_state) == sizeof(struct xgpu_metal_dynamic_state),
	"the dynamic state's layouts");

struct stream_target
{
	uint32_t texture;
	struct xgpu_metal_texture_description description;
	/* counts the draws and clears into it */
	uint32_t written;
};

struct stream_pipeline
{
	/* XGPU_STREAM_PIPELINE_* */
	uint8_t state;
	/* the shaders, made for this variant of the draw's */
	uint32_t vertex_shader, pixel_shader;
	struct xgpu_metal_stream_variant variant;
};

static struct
{
	unsigned char *ring;
	uint32_t ring_size;
	NSMutableDictionary<NSNumber *, NSValue *> *targets;
	uint32_t segments[MIRROR_SEGMENT_MAXIMUM];
	NSMutableDictionary<NSNumber *, NSData *> *vertex_programs, *vertex_lightings;
	struct stream_pipeline pipelines[XGPU_STREAM_PIPELINES];
	struct xgpu_metal_stream_shaders shaders;
	/* the draws' pipeline (without its attributes and shaders), its number
	and definition, and its layout; and the layout of immediate mode's
	draws */
	struct xgpu_metal_pipeline pipeline;
	uint32_t pipeline_id;
	const struct xgpu_stream_pipeline_define *definition;
	/* the inputs the layout feeds as integers; the stages' axes whose
	border colour the shader draws (xgpu_metal_stream_variant) */
	uint16_t integer_attributes;
	uint8_t border[4];
	/* what the uniforms are made of (the provider's uniforms), and whether
	it changed since they were made */
	struct xgpu_stream_draw_uniforms uniforms;
	float constants[XGPU_METAL_VERTEX_CONSTANT_COUNT][4];
	float blend_color[4];
	struct xgpu_stream_sampler samplers[4];
	BOOL uniforms_dirty;
	/* the stages as set; set since they were bound, and the write
	tracking's serial then; a target's copy bound (checked at each draw) */
	struct xgpu_stream_texture_stage stages[4];
	BOOL textures_dirty, feedback_bound;
	uint32_t textures_serial;
	/* the copy of a target drawn into that a draw samples: of which
	texture, as of which of its draws */
	uint32_t feedback, feedback_target, feedback_written;
	struct xgpu_metal_texture_description feedback_description;
	struct xgpu_metal_vertex_attribute attributes[16];
	uint16_t strides[XGPU_METAL_STREAM_COUNT];
	struct xgpu_metal_source streams[XGPU_METAL_STREAM_COUNT];
	float constant_attributes[16][4];
	uint32_t color, depth, samples;
	uint32_t back_buffer;
	uint32_t uniforms_serial;
	BOOL uniforms_set;
} stream;

void xgpu_metal_stream_set_shaders(const struct xgpu_metal_stream_shaders *shaders)
{
	uint32_t index;

	stream.shaders = *shaders;
	/* (the pipelines are made again by these) */
	for (index = 0; index < XGPU_STREAM_PIPELINES; index++)
		stream.pipelines[index].state = XGPU_STREAM_PIPELINE_UNKNOWN;
	stream.uniforms_dirty = YES;
}

/* ---------- the interface's values */

/* XGPU_POINTS ... XGPU_TRIANGLE_FAN */
enum
{
	_primitive_points, _primitive_lines, _primitive_line_loop, _primitive_line_strip, _primitive_triangles,
	_primitive_triangle_strip, _primitive_triangle_fan,
};

/* XGPU_CLEAR_*: XGPU_METAL_CLEAR_* are the same */
static uint32_t clear_parts(uint32_t flags)
{
	return flags & (XGPU_METAL_CLEAR_CHANNELS | XGPU_METAL_CLEAR_COLOR | XGPU_METAL_CLEAR_DEPTH | XGPU_METAL_CLEAR_STENCIL);
}

/* XGPU_FILTER_NONE, _POINT, _LINEAR, _ANISOTROPIC; XGPU_ADDRESS_WRAP,
_MIRROR, _CLAMP, _BORDER (XGPU_METAL_ADDRESS_*'s first four); the border
colour as ARGB */
enum
{
	_filter_none, _filter_point, _filter_linear, _filter_anisotropic,
};

/* the sampler; axes: those whose border the shader draws, clamped to the
edge here */
static void sampler_convert(const struct xgpu_stream_sampler *from, uint8_t axes, struct xgpu_metal_sampler *to)
{
	memset(to, 0, sizeof(*to));
	to->min_filter = from->minify >= _filter_linear ? XGPU_METAL_FILTER_LINEAR : XGPU_METAL_FILTER_POINT;
	to->mag_filter = from->magnify >= _filter_linear ? XGPU_METAL_FILTER_LINEAR : XGPU_METAL_FILTER_POINT;
	to->mip_filter = from->mip == _filter_none ? XGPU_METAL_MIP_NONE :
		from->mip == _filter_point ? XGPU_METAL_MIP_POINT : XGPU_METAL_MIP_LINEAR;
	to->address_u = (axes & 1) ? XGPU_METAL_ADDRESS_CLAMP : from->address[0];
	to->address_v = (axes & 2) ? XGPU_METAL_ADDRESS_CLAMP : from->address[1];
	to->address_w = (axes & 4) ? XGPU_METAL_ADDRESS_CLAMP : from->address[2];
	/* (the others are the shader's to blend in, where it can: a cube's or a
	layered target's are transparent black) */
	to->border = from->border_color == 0xff000000u ? XGPU_METAL_BORDER_OPAQUE_BLACK :
		from->border_color == 0xffffffffu ? XGPU_METAL_BORDER_OPAQUE_WHITE : XGPU_METAL_BORDER_TRANSPARENT_BLACK;
	to->max_anisotropy = (uint8_t)(from->minify == _filter_anisotropic && from->anisotropy > 1 ?
		(from->anisotropy > 16 ? 16 : from->anisotropy) : 1);
	to->min_lod = (float)from->first_level;
}

/* ---------- targets */

static struct stream_target *target_get(uint32_t target)
{
	NSValue *value = stream.targets[@(target)];

	return value ? (struct stream_target *)value.pointerValue : NULL;
}

static uint32_t target_texture(uint32_t target)
{
	struct stream_target *entry = target ? target_get(target) : NULL;

	return entry ? entry->texture : 0;
}

static void target_forget(uint32_t target)
{
	struct stream_target *entry = target_get(target);

	if (!entry)
		return;
	xgpu_metal_texture_destroy(entry->texture);
	free(entry);
	[stream.targets removeObjectForKey:@(target)];
	stream.textures_dirty = YES;
}

static void target_make(uint32_t target, const struct xgpu_metal_texture_description *description)
{
	struct stream_target *entry = calloc(1, sizeof(*entry));

	target_forget(target);
	if (!entry)
		return;
	entry->description = *description;
	entry->texture = xgpu_metal_texture_create(description);
	stream.targets[@(target)] = [NSValue valueWithPointer:entry];
	stream.textures_dirty = YES;
}

static void stream_target_create(void *context, const struct xgpu_stream_target_create *record)
{
	struct xgpu_metal_texture_description description;

	(void)context;
	memset(&description, 0, sizeof(description));
	description.type = record->layers > 1 ? XGPU_METAL_TEXTURE_2D_ARRAY : XGPU_METAL_TEXTURE_2D;
	description.format = record->depth ? XGPU_METAL_FORMAT_D24S8 : XGPU_METAL_FORMAT_BGRA8;
	description.usage = XGPU_METAL_USAGE_SAMPLED | XGPU_METAL_USAGE_TARGET;
	description.width = (uint16_t)record->width;
	description.height = (uint16_t)record->height;
	description.depth_or_layers = record->layers > 1 ? record->layers : 1;
	description.levels = 1;
	target_make(record->target, &description);
}

static void stream_target_delete(void *context, uint32_t target)
{
	(void)context;
	target_forget(target);
}

static void stream_target_multisample(void *context, const struct xgpu_stream_target_multisample *record)
{
	struct stream_target *entry = target_get(record->target);
	struct xgpu_metal_texture_description description;

	(void)context;
	if (!entry || entry->description.samples == (record->samples > 1 ? record->samples : 0))
		return;
	description = entry->description;
	description.samples = (uint8_t)(record->samples > 1 ? record->samples : 0);
	/* (its pixels go: the front end makes a target multisampled before it
	draws into it) */
	target_make(record->target, &description);
}

static void stream_set_targets(void *context, const struct xgpu_stream_set_targets *record)
{
	(void)context;
	stream.color = record->color;
	stream.depth = record->depth;
	/* (whether a stage samples the target drawn into) */
	stream.textures_dirty = YES;
	stream.samples = record->samples;
	/* (the macOS app draws no VR views: views and hud_group are the VR
	mode's) */
	xgpu_metal_set_targets(target_texture(record->color), 0, record->views < 0 ? (uint32_t)(-record->views - 1) +
		record->base : record->base, target_texture(record->depth), record->samples != 0);
}

static void stream_clear(void *context, const struct xgpu_stream_clear *record)
{
	(void)context;
	if (target_get(stream.color))
		target_get(stream.color)->written++;
	xgpu_metal_clear((const int32_t (*)[4])record->rectangles, record->count, clear_parts(record->flags), record->color,
		record->depth, record->stencil);
}

static void stream_clear_target(void *context, const struct xgpu_stream_clear_target *record)
{
	(void)context;
	if (target_get(record->target))
		target_get(record->target)->written++;
	xgpu_metal_set_targets(target_texture(record->target), 0, 0, 0, 0);
	xgpu_metal_clear(NULL, 0, XGPU_METAL_CLEAR_COLOR | XGPU_METAL_CLEAR_CHANNELS, record->color, 1.0f, 0);
	xgpu_metal_set_targets(target_texture(stream.color), 0, 0, target_texture(stream.depth), stream.samples != 0);
}

static int stream_read_target(void *context, const struct xgpu_stream_read_target *record)
{
	struct stream_target *entry = target_get(record->target);
	uint32_t width, height;

	(void)context;
	if (!entry || !record->pixels)
		return 0;
	width = entry->description.width;
	height = entry->description.height;
	if ((uint64_t)width * height * 4 > record->size)
		return 0;
	return xgpu_metal_read_pixels(entry->texture, 0, record->layer > 0 ? (uint32_t)record->layer : 0, 0, 0, width, height,
		host_pointer(record->pixels), width * 4);
}

/* a target's picture as a PNG (host_render_record.h's shots: a frame's back
buffer as it is presented); its pixels' alpha is not written */
int xgpu_metal_stream_shot(uint32_t target, const char *path)
{
	struct stream_target *entry = target_get(target);
	uint32_t width, height;
	unsigned char *pixels;
	int written = 0;

	if (!entry || entry->description.format != XGPU_METAL_FORMAT_BGRA8)
		return 0;
	width = entry->description.width;
	height = entry->description.height;
	pixels = malloc((size_t)width * height * 4);
	if (!pixels)
		return 0;
	if (xgpu_metal_read_pixels(entry->texture, 0, 0, 0, 0, width, height, pixels, width * 4))
	{
		CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
		CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels, (size_t)width * height * 4, NULL);
		CGImageRef image = CGImageCreate(width, height, 8, 32, width * 4, space,
			kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaNoneSkipFirst, provider, NULL, false,
			kCGRenderingIntentDefault);
		NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
		CGImageDestinationRef destination = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1,
			NULL);

		if (destination && image)
		{
			CGImageDestinationAddImage(destination, image, NULL);
			written = CGImageDestinationFinalize(destination) ? 1 : 0;
		}
		if (destination)
			CFRelease(destination);
		CGImageRelease(image);
		CGDataProviderRelease(provider);
		CGColorSpaceRelease(space);
	}
	free(pixels);
	return written;
}

/* ---------- textures */

/* XGPU_SOURCE_* */
enum
{
	_source_none, _source_texture, _source_target,
};

/* xgpu.h's _xgpu_sampler_* */
enum
{
	_xgpu_sampler_none, _xgpu_sampler_2d, _xgpu_sampler_3d, _xgpu_sampler_cube, _xgpu_sampler_2d_layered,
};

static void stream_set_textures(void *context, const struct host_render_texture_stage stages[4])
{
	int index;

	(void)context;
	for (index = 0; index < 4; index++)
	{
		const struct xgpu_stream_texture_stage *stage = &stages[index].stage;

		stream.stages[index] = *stage;
		stream.border[index] = xgpu_metal_border_axes(stage->source, stage->sampler_type, &stage->sampler);
		if (memcmp(&stream.samplers[index], &stage->sampler, sizeof(stage->sampler)))
		{
			stream.samplers[index] = stage->sampler;
			stream.uniforms_dirty = YES;
		}
	}
	stream.textures_dirty = YES;
}

/* a copy of a target the draw draws into, which it samples (Metal cannot
sample an attachment of the pass): copied again once the target has been
drawn into since (as the web build's OpenGL does: xgpu_gl.c feedback_copy) */
static uint32_t feedback_copy(struct stream_target *target)
{
	if (target->description.format != XGPU_METAL_FORMAT_BGRA8)
		return target->texture;
	if (!stream.feedback || stream.feedback_description.width != target->description.width ||
		stream.feedback_description.height != target->description.height)
	{
		struct xgpu_metal_texture_description description = target->description;

		if (stream.feedback)
			xgpu_metal_texture_destroy(stream.feedback);
		description.type = XGPU_METAL_TEXTURE_2D;
		description.usage = XGPU_METAL_USAGE_SAMPLED | XGPU_METAL_USAGE_TARGET;
		description.samples = 0;
		description.depth_or_layers = 1;
		description.levels = 1;
		stream.feedback = xgpu_metal_texture_create(&description);
		stream.feedback_description = description;
		stream.feedback_target = 0;
	}
	if (stream.feedback && (stream.feedback_target != target->texture || stream.feedback_written != target->written))
	{
		xgpu_metal_texture_copy(target->texture, stream.feedback);
		stream.feedback_target = target->texture;
		stream.feedback_written = target->written;
	}
	return stream.feedback ? stream.feedback : target->texture;
}

/* the stages' textures and samplers for a draw: the textures are looked up
again where a stage changed or a watched page has been written since (the
textures' cache makes them again from pages written since), and a target the
draw draws into is copied */
static void textures_bind(void)
{
	uint32_t serial = host_memory_watch_serial(), index;

	if (!stream.textures_dirty && serial == stream.textures_serial && !stream.feedback_bound)
		return;
	stream.textures_dirty = NO;
	stream.textures_serial = serial;
	stream.feedback_bound = NO;
	for (index = 0; index < 4; index++)
	{
		const struct xgpu_stream_texture_stage *stage = &stream.stages[index];
		struct xgpu_metal_sampler sampler;
		uint32_t texture = 0;

		sampler_convert(&stage->sampler, stream.border[index], &sampler);
		if (stage->source == _source_target)
		{
			struct stream_target *target = stage->target ? target_get(stage->target) : NULL;

			if (target)
			{
				texture = target->texture;
				/* (drawn into by this draw: its copy, checked at each draw) */
				if (stage->drawn == stage->target && stage->target == stream.color)
				{
					texture = feedback_copy(target);
					stream.feedback_bound = YES;
				}
			}
		}
		else if (stage->source == _source_texture || stage->source == XGPU_STREAM_SOURCE_IMAGE)
		{
			texture = xgpu_metal_textures_get(stage);
		}
		xgpu_metal_set_texture(index, texture, &sampler);
	}
}

/* ---------- pipelines */

/* the per-pixel lighting's lights' registers (xgpu_backend.h's
XGPU_MODEL_LIGHT_REGISTER: c[-82], c[-79] to c[-69]) */
#define MODEL_LIGHT_FIRST (96 - 82)
#define MODEL_LIGHT_LAST (96 - 69)

static void stream_set_constants(void *context, const struct xgpu_stream_constants *record)
{
	uint32_t count = record->count;

	(void)context;
	if (record->first >= XGPU_METAL_VERTEX_CONSTANT_COUNT)
		return;
	if (count > XGPU_METAL_VERTEX_CONSTANT_COUNT - record->first)
		count = XGPU_METAL_VERTEX_CONSTANT_COUNT - record->first;
	memcpy(stream.constants[record->first], record->values, (size_t)count * sizeof(stream.constants[0]));
	if (record->first <= MODEL_LIGHT_LAST && record->first + count > MODEL_LIGHT_FIRST)
		stream.uniforms_dirty = YES;
	xgpu_metal_set_vertex_constants(record->first, count, &record->values[0][0]);
}

static void stream_vertex_program(void *context, const struct xgpu_stream_vertex_program *record)
{
	uint64_t hash = record->hash[0] | ((uint64_t)record->hash[1] << 32);

	(void)context;
	stream.vertex_programs[@(hash)] = [NSData dataWithBytes:record length:XGPU_STREAM_HEADER_SIZE(record->header)];
}

static void stream_vertex_lighting(void *context, const struct xgpu_stream_vertex_lighting *record)
{
	uint64_t hash = record->hash[0] | ((uint64_t)record->hash[1] << 32);

	(void)context;
	stream.vertex_lightings[@(hash)] = [NSData dataWithBytes:record length:sizeof(*record)];
}

static void stream_pipeline_define(void *context, const struct xgpu_stream_pipeline_define *record)
{
	(void)context;
	/* (a number defined again is another pipeline) */
	if (record->id && record->id < XGPU_STREAM_PIPELINES)
		stream.pipelines[record->id].state = XGPU_STREAM_PIPELINE_UNKNOWN;
	if (record->id == stream.pipeline_id)
		stream.pipeline_id = 0;
}

/* a pipeline's shaders for a variant of the draw's: made by the provider
if the pipeline has none for it; 0 if they cannot be */
static int pipeline_shaders(struct stream_pipeline *entry, const struct xgpu_stream_pipeline_define *definition,
	const struct xgpu_metal_stream_variant *variant)
{
	const struct xgpu_stream_pipeline_key *key = &definition->key;
	uint64_t hash = key->vertex_program[0] | ((uint64_t)key->vertex_program[1] << 32);
	NSData *program, *lighting;

	if (entry->state == XGPU_STREAM_PIPELINE_MADE && !memcmp(&entry->variant, variant, sizeof(*variant)))
		return 1;
	if (entry->state == XGPU_STREAM_PIPELINE_FAILED || !stream.shaders.make)
		return 0;
	program = stream.vertex_programs[@(hash)];
	lighting = stream.vertex_lightings[@(hash)];
	entry->variant = *variant;
	if (stream.shaders.make(stream.shaders.context, definition, program ? program.bytes : NULL,
		lighting ? lighting.bytes : NULL, variant, &entry->vertex_shader, &entry->pixel_shader) && entry->vertex_shader)
	{
		entry->state = XGPU_STREAM_PIPELINE_MADE;
		return 1;
	}
	/* (one that fails is not tried again for every draw) */
	entry->state = XGPU_STREAM_PIPELINE_FAILED;
	return 0;
}

static int stream_pipeline(void *context, uint32_t id, const struct xgpu_stream_pipeline_define *definition)
{
	struct stream_pipeline *entry;
	const struct xgpu_stream_pipeline_key *key = &definition->key;
	struct xgpu_metal_pipeline *pipeline = &stream.pipeline;

	(void)context;
	stream.pipeline_id = 0;
	if (!id || id >= XGPU_STREAM_PIPELINES)
		return 0;
	entry = &stream.pipelines[id];
	/* (made at first in the variant the draws have now, to tell the guest
	whether it can be) */
	if (entry->state == XGPU_STREAM_PIPELINE_UNKNOWN)
	{
		struct xgpu_metal_stream_variant variant;

		memset(&variant, 0, sizeof(variant));
		variant.integer_attributes = key->immediate ? 0 : stream.integer_attributes;
		memcpy(variant.border, stream.border, sizeof(variant.border));
		pipeline_shaders(entry, definition, &variant);
	}
	if (entry->state != XGPU_STREAM_PIPELINE_MADE)
		return 0;
	stream.pipeline_id = id;
	stream.definition = definition;
	xgpu_metal_pipeline_from_key(key, pipeline);
	return 1;
}

static void stream_draw_state(void *context, const struct xgpu_stream_dynamic_state *dynamic,
	const struct xgpu_stream_draw_uniforms *uniforms, uint32_t uniforms_serial)
{
	(void)context;
	if (dynamic)
	{
		xgpu_metal_set_dynamic_state((const struct xgpu_metal_dynamic_state *)dynamic);
		if (memcmp(stream.blend_color, dynamic->blend_color, sizeof(stream.blend_color)))
		{
			memcpy(stream.blend_color, dynamic->blend_color, sizeof(stream.blend_color));
			stream.uniforms_dirty = YES;
		}
	}
	if (uniforms && (!stream.uniforms_set || uniforms_serial != stream.uniforms_serial))
	{
		stream.uniforms = *uniforms;
		stream.uniforms_serial = uniforms_serial;
		stream.uniforms_set = YES;
		stream.uniforms_dirty = YES;
	}
}

/* the uniforms both stages read, made again where what they are made of
changed */
static void uniforms_update(void)
{
	struct xgpu_metal_stream_uniform_inputs inputs;
	unsigned char bytes[4096];
	uint32_t size;

	if (!stream.uniforms_dirty || !stream.uniforms_set)
		return;
	stream.uniforms_dirty = NO;
	if (!stream.shaders.uniforms)
	{
		xgpu_metal_set_draw_uniforms(&stream.uniforms, sizeof(stream.uniforms));
		return;
	}
	inputs.uniforms = &stream.uniforms;
	inputs.blend_color = stream.blend_color;
	inputs.constants = (const float (*)[4])stream.constants;
	inputs.samplers = stream.samplers;
	size = stream.shaders.uniforms(stream.shaders.context, &inputs, bytes, sizeof(bytes));
	if (size)
		xgpu_metal_set_draw_uniforms(bytes, size);
}

/* ---------- vertices */

static uint32_t segment_buffer(int32_t segment)
{
	if (segment < 0 || segment >= MIRROR_SEGMENT_MAXIMUM)
		return 0;
	if (!stream.segments[segment])
		stream.segments[segment] = xgpu_metal_buffer_create(MIRROR_SEGMENT_SIZE);
	return stream.segments[segment];
}

static void stream_mirror_write(void *context, const struct xgpu_stream_mirror_write *record)
{
	(void)context;
	if ((uint64_t)record->offset + record->size > MIRROR_SEGMENT_SIZE)
		return;
	xgpu_metal_buffer_write(segment_buffer((int32_t)record->segment), record->offset, host_pointer(record->data),
		record->size, record->unused != 0);
}

/* inline data at a guest address: in the ring, or (too large for it) in a
buffer of its own in guest memory, read in place as well */
static struct xgpu_metal_source ring_source(uint32_t address, uint32_t size)
{
	struct xgpu_metal_source source = { XGPU_METAL_SOURCE_STREAM_RING, 0, size, 0 };
	unsigned char *data = host_pointer(address);

	if (!data)
		return (struct xgpu_metal_source){ XGPU_METAL_SOURCE_NONE, 0, 0, 0 };
	if (data < stream.ring || data + size > stream.ring + stream.ring_size)
		return xgpu_metal_guest(address, size);
	source.offset = (uint32_t)(data - stream.ring);
	return source;
}

static void stream_set_vertices(void *context, const struct host_render_vertex_input *input)
{
	uint32_t index;

	(void)context;
	memset(stream.strides, 0, sizeof(stream.strides));
	stream.integer_attributes = xgpu_metal_layout_attributes(input->layout ? input->layout->elements : NULL,
		input->layout ? input->layout->element_count : 0, stream.attributes);
	for (index = 0; index < XGPU_METAL_STREAM_COUNT; index++)
	{
		const struct xgpu_stream_vertex_stream *source = &input->streams[index];

		if (!(input->present & (1u << index)))
		{
			stream.streams[index] = (struct xgpu_metal_source){ XGPU_METAL_SOURCE_NONE, 0, 0, 0 };
			continue;
		}
		stream.strides[index] = (uint16_t)source->stride;
		if (source->segment >= 0)
			stream.streams[index] = (struct xgpu_metal_source){ XGPU_METAL_SOURCE_BUFFER, source->offset,
				MIRROR_SEGMENT_SIZE - source->offset, segment_buffer(source->segment) };
		else
			stream.streams[index] = ring_source(source->data, source->bytes);
	}
	memcpy(stream.constant_attributes, input->attributes, sizeof(stream.constant_attributes));
}

/* the pipeline and layout for a draw: set_vertices's, or immediate mode's
(integers: the inputs read as integers); NO if it cannot be drawn */
static BOOL draw_prepare(const struct xgpu_metal_vertex_attribute *attributes, const uint16_t *strides,
	const struct xgpu_metal_source *streams, const float (*constants)[4], uint16_t integers)
{
	struct xgpu_metal_pipeline pipeline = stream.pipeline;
	struct xgpu_metal_stream_variant variant;
	struct stream_pipeline *entry;
	uint32_t index;

	if (!stream.pipeline_id || !stream.definition)
		return NO;
	entry = &stream.pipelines[stream.pipeline_id];
	memset(&variant, 0, sizeof(variant));
	variant.integer_attributes = stream.definition->key.immediate ? 0 : integers;
	memcpy(variant.border, stream.border, sizeof(variant.border));
	if (!pipeline_shaders(entry, stream.definition, &variant))
		return NO;
	pipeline.vertex_shader = entry->vertex_shader;
	pipeline.pixel_shader = entry->pixel_shader;
	uniforms_update();
	textures_bind();
	if (target_get(stream.color))
		target_get(stream.color)->written++;
	memcpy(pipeline.attributes, attributes, sizeof(pipeline.attributes));
	memcpy(pipeline.strides, strides, sizeof(pipeline.strides));
	xgpu_metal_set_pipeline(&pipeline);
	for (index = 0; index < XGPU_METAL_STREAM_COUNT; index++)
		xgpu_metal_set_stream(index, streams[index]);
	xgpu_metal_set_constant_attributes(constants);
	return YES;
}

/* draws count vertices (indices: 16-bit at data, or NULL for 0 to count),
making fans and loops into lists, which Metal has */
static void draw_converted(uint32_t primitive, uint32_t count, const uint16_t *indices, struct xgpu_metal_source source,
	int32_t base_vertex)
{
	static const uint32_t metal_primitives[] = {
		[_primitive_points] = XGPU_METAL_PRIMITIVE_POINTS,
		[_primitive_lines] = XGPU_METAL_PRIMITIVE_LINES,
		[_primitive_line_strip] = XGPU_METAL_PRIMITIVE_LINE_STRIP,
		[_primitive_triangles] = XGPU_METAL_PRIMITIVE_TRIANGLES,
		[_primitive_triangle_strip] = XGPU_METAL_PRIMITIVE_TRIANGLE_STRIP,
	};
	uint16_t *list;
	uint32_t index, written = 0;

	if (primitive != _primitive_triangle_fan && primitive != _primitive_line_loop)
	{
		if (primitive < sizeof(metal_primitives) / sizeof(metal_primitives[0]))
			xgpu_metal_draw(metal_primitives[primitive], count, 0, base_vertex, source);
		return;
	}
	if (count < 2 || count > 65536)
		return;
	list = malloc(sizeof(*list) * (size_t)count * 3);
	if (!list)
		return;
	if (primitive == _primitive_triangle_fan)
	{
		for (index = 1; index + 1 < count; index++)
		{
			list[written++] = indices ? indices[0] : 0;
			list[written++] = indices ? indices[index] : (uint16_t)index;
			list[written++] = indices ? indices[index + 1] : (uint16_t)(index + 1);
		}
	}
	else
	{
		for (index = 0; index < count; index++)
		{
			list[written++] = indices ? indices[index] : (uint16_t)index;
			list[written++] = indices ? indices[(index + 1) % count] : (uint16_t)((index + 1) % count);
		}
	}
	if (written)
		xgpu_metal_draw(primitive == _primitive_triangle_fan ? XGPU_METAL_PRIMITIVE_TRIANGLES : XGPU_METAL_PRIMITIVE_LINES,
			written, 0, base_vertex, xgpu_metal_stream(list, written * (uint32_t)sizeof(*list)));
	free(list);
}

static void stream_draw(void *context, const struct xgpu_stream_draw *record)
{
	(void)context;
	if (!draw_prepare(stream.attributes, stream.strides, stream.streams, (const float (*)[4])stream.constant_attributes,
		stream.integer_attributes))
		return;
	draw_converted(record->primitive, record->count, NULL, (struct xgpu_metal_source){ XGPU_METAL_SOURCE_NONE, 0, 0, 0 }, 0);
}

static void stream_draw_indices(void *context, const struct xgpu_stream_draw_indices *record)
{
	struct xgpu_metal_source source = ring_source(record->indices, record->count * 2);

	(void)context;
	if (source.kind == XGPU_METAL_SOURCE_NONE)
		return;
	if (!draw_prepare(stream.attributes, stream.strides, stream.streams, (const float (*)[4])stream.constant_attributes,
		stream.integer_attributes))
		return;
	draw_converted(record->primitive, record->count, host_pointer(record->indices), source, 0);
}

static void stream_draw_indexed(void *context, const struct xgpu_stream_draw_indexed *record)
{
	struct xgpu_metal_source source;
	const uint16_t *indices = NULL;

	(void)context;
	if (record->segment >= 0)
	{
		uint32_t buffer = segment_buffer(record->segment);

		if ((uint64_t)record->offset + record->count * 2 > MIRROR_SEGMENT_SIZE || !buffer)
			return;
		source = (struct xgpu_metal_source){ XGPU_METAL_SOURCE_BUFFER, record->offset, record->count * 2, buffer };
		/* (a fan's or a loop's indices, made into a list, are read from the
		mirror's copy) */
		indices = (const uint16_t *)((const unsigned char *)xgpu_metal_buffer_contents(buffer) + record->offset);
	}
	else
	{
		source = ring_source(record->indices, record->count * 2);
		indices = host_pointer(record->indices);
	}
	if (source.kind == XGPU_METAL_SOURCE_NONE)
		return;
	if (!draw_prepare(stream.attributes, stream.strides, stream.streams, (const float (*)[4])stream.constant_attributes,
		stream.integer_attributes))
		return;
	/* (the streams start at vertex minimum) */
	draw_converted(record->primitive, record->count, indices, source, -(int32_t)record->minimum);
}

static void stream_draw_immediate(void *context, const struct xgpu_stream_draw_immediate *record,
	const float (*attributes)[4])
{
	struct xgpu_metal_vertex_attribute layout[16];
	uint16_t strides[XGPU_METAL_STREAM_COUNT];
	struct xgpu_metal_source streams[XGPU_METAL_STREAM_COUNT];
	uint32_t written, size;

	(void)context;
	memset(streams, 0, sizeof(streams));
	/* the registers written, a vec4 each, in order; the rest from their
	values */
	written = xgpu_metal_layout_immediate(record->written, layout, strides);
	if (!written || !record->count)
		return;
	size = record->count * written * 16;
	streams[0] = ring_source(record->vertices, size);
	if (streams[0].kind == XGPU_METAL_SOURCE_NONE)
		return;
	if (!draw_prepare(layout, strides, streams, attributes, 0))
		return;
	if (record->indices && record->index_count)
		draw_converted(record->primitive, record->index_count, host_pointer(record->indices),
			ring_source(record->indices, record->index_count * 2), 0);
	else
		draw_converted(record->primitive, record->count, NULL, (struct xgpu_metal_source){ XGPU_METAL_SOURCE_NONE, 0, 0, 0 }, 0);
}

/* ---------- visibility tests, frames */

static void stream_visibility_begin(void *context)
{
	(void)context;
	xgpu_metal_visibility_begin();
}

static void stream_visibility_end(void *context, const struct xgpu_stream_visibility_end *record)
{
	(void)context;
	/* (the count written for the game is of its pixels: divided by the
	samples one of them is, as the GL backend's) */
	xgpu_metal_visibility_end(record->slot, record->area * (float)(record->samples > 1 ? record->samples : 1));
}

static void stream_present(void *context, uint32_t back_buffer)
{
	(void)context;
	stream.back_buffer = back_buffer;
}

static void stream_prepare(void *context)
{
	(void)context;
	xgpu_metal_prepare_begin();
}

static void stream_exiting(void *context)
{
	(void)context;
	xgpu_metal_exiting();
}

static uint32_t stream_prepared(void *context)
{
	(void)context;
	return (uint32_t)(xgpu_metal_prepared() * 1000.0 + 0.5);
}

static void stream_frame_end(void *context)
{
	(void)context;
	xgpu_metal_frame_end(target_texture(stream.back_buffer));
	stream.back_buffer = 0;
	xgpu_metal_textures_frame_end();
}

static void stream_open(void *context, void *ring, uint32_t ring_size, volatile uint32_t *visibility)
{
	(void)context;
	stream.ring = ring;
	stream.ring_size = ring_size;
	xgpu_metal_set_stream_ring(ring, ring_size);
	xgpu_metal_set_visibility_output(visibility);
}

static uint64_t stream_fence(void *context)
{
	(void)context;
	return xgpu_metal_fence();
}

static void stream_wait(void *context, uint64_t fence)
{
	(void)context;
	xgpu_metal_frame_wait(fence);
}

static void stream_initialize(void *context, struct xgpu_stream_capabilities *capabilities)
{
	struct xgpu_metal_capabilities metal;

	(void)context;
	xgpu_metal_capabilities(&metal);
	memset(capabilities, 0, sizeof(*capabilities));
	capabilities->largest_target = (int32_t)metal.largest_target;
	capabilities->most_samples = (int32_t)metal.samples;
	capabilities->most_anisotropy = (int32_t)metal.anisotropy;
	/* (visibility counts from the visibility result buffer, not the pixel
	shaders) */
	capabilities->sample_counting = 0;
	capabilities->bgra_vertex_colors = 1;
	capabilities->base_vertex = 1;
	capabilities->sample_mask = 1;
	capabilities->mixed_constant_blend = 1;
}

const struct host_render_backend *xgpu_metal_stream_backend(void)
{
	static struct host_render_backend backend;

	if (!backend.open)
	{
		stream.targets = [NSMutableDictionary new];
		stream.vertex_programs = [NSMutableDictionary new];
		stream.vertex_lightings = [NSMutableDictionary new];
		backend.open = stream_open;
		backend.fence = stream_fence;
		backend.wait = stream_wait;
		backend.initialize = stream_initialize;
		backend.target_create = stream_target_create;
		backend.target_delete = stream_target_delete;
		backend.target_multisample = stream_target_multisample;
		backend.set_targets = stream_set_targets;
		backend.clear = stream_clear;
		backend.clear_target = stream_clear_target;
		backend.read_target = stream_read_target;
		backend.set_textures = stream_set_textures;
		backend.set_constants = stream_set_constants;
		backend.vertex_program = stream_vertex_program;
		backend.vertex_lighting = stream_vertex_lighting;
		backend.pipeline_define = stream_pipeline_define;
		backend.prepared = stream_prepared;
		backend.exiting = stream_exiting;
		backend.prepare = stream_prepare;
		backend.pipeline = stream_pipeline;
		backend.draw_state = stream_draw_state;
		backend.mirror_write = stream_mirror_write;
		backend.set_vertices = stream_set_vertices;
		backend.draw = stream_draw;
		backend.draw_indices = stream_draw_indices;
		backend.draw_indexed = stream_draw_indexed;
		backend.draw_immediate = stream_draw_immediate;
		backend.visibility_begin = stream_visibility_begin;
		backend.visibility_end = stream_visibility_end;
		backend.present = stream_present;
		backend.frame_end = stream_frame_end;
	}
	return &backend;
}

#endif
