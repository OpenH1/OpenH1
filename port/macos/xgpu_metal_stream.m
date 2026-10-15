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

Not here yet, waiting on other parts:
- shaders: the MSL emitter (xgpu_msl.c) makes them. Until it is wired, a
  provider (xgpu_metal_stream_set_shaders) gives each pipeline its two
  functions; without one, every pipeline fails, as the stream reports to the
  front end.
- textures read from the Xbox's memory (a stage's resource): they need the
  Xbox's texture decoding (swizzle, palettes) on the host (the split's
  deferred step). Stages that sample a render target work.
- the enhanced dynamic lights, anti-aliasing's passes and the VR mode: no
  entries (the stream skips them).

The interface's values (xgpu_backend.h: the primitives, the clear's parts,
the samplers' filters and addresses, the vertex elements' formats) are read
by the functions under "the interface's values".
*/

#if __has_include("host_render_stream.h")

#import <Foundation/Foundation.h>

/* (host.h's host_page_size is Mach's too: not used here) */
#define host_page_size host_page_size_of_guest_memory
#include "host.h"
#undef host_page_size
#include "host_render_stream.h"
#include "xgpu_metal_internal.h"
#include "xgpu_metal_stream.h"

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
};

struct stream_pipeline
{
	/* XGPU_STREAM_PIPELINE_* */
	uint8_t state;
	uint32_t vertex_shader, pixel_shader;
};

static struct
{
	unsigned char *ring;
	uint32_t ring_size;
	NSMutableDictionary<NSNumber *, NSValue *> *targets;
	uint32_t segments[MIRROR_SEGMENT_MAXIMUM];
	NSMutableDictionary<NSNumber *, NSData *> *vertex_programs;
	struct stream_pipeline pipelines[XGPU_STREAM_PIPELINES];
	struct xgpu_metal_stream_shaders shaders;
	/* the draws' pipeline (without its attributes) and layout; and the
	layout of immediate mode's draws */
	struct xgpu_metal_pipeline pipeline;
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
	stream.shaders = *shaders;
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

/* XGPU_VERTEX_*: XGPU_METAL_ATTRIBUTE_* are the same */
static uint8_t attribute_format(uint8_t format)
{
	return format < XGPU_METAL_ATTRIBUTE_COUNT ? format : XGPU_METAL_ATTRIBUTE_NONE;
}

/* XGPU_FILTER_NONE, _POINT, _LINEAR, _ANISOTROPIC; XGPU_ADDRESS_WRAP,
_MIRROR, _CLAMP, _BORDER (XGPU_METAL_ADDRESS_*'s first four); the border
colour as ARGB */
enum
{
	_filter_none, _filter_point, _filter_linear, _filter_anisotropic,
};

static void sampler_convert(const struct xgpu_stream_sampler *from, struct xgpu_metal_sampler *to)
{
	memset(to, 0, sizeof(*to));
	to->min_filter = from->minify >= _filter_linear ? XGPU_METAL_FILTER_LINEAR : XGPU_METAL_FILTER_POINT;
	to->mag_filter = from->magnify >= _filter_linear ? XGPU_METAL_FILTER_LINEAR : XGPU_METAL_FILTER_POINT;
	to->mip_filter = from->mip == _filter_none ? XGPU_METAL_MIP_NONE :
		from->mip == _filter_point ? XGPU_METAL_MIP_POINT : XGPU_METAL_MIP_LINEAR;
	to->address_u = from->address[0];
	to->address_v = from->address[1];
	to->address_w = from->address[2];
	/* (the others are the emitter's to blend in) */
	to->border = from->border_color == 0xff000000u ? XGPU_METAL_BORDER_OPAQUE_BLACK :
		from->border_color == 0xffffffffu ? XGPU_METAL_BORDER_OPAQUE_WHITE : XGPU_METAL_BORDER_TRANSPARENT_BLACK;
	to->max_anisotropy = (uint8_t)(from->minify == _filter_anisotropic && from->anisotropy > 1 ?
		(from->anisotropy > 16 ? 16 : from->anisotropy) : 1);
	to->min_lod = (float)from->first_level;
}

/* the targets' formats: XGPU_FORMAT_NONE, _BGRA8, _D24S8 */
static uint8_t target_format(uint8_t format)
{
	return format == 1 ? XGPU_METAL_FORMAT_BGRA8 : format == 2 ? XGPU_METAL_FORMAT_D24S8 : XGPU_METAL_FORMAT_NONE;
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
	stream.samples = record->samples;
	/* (the macOS app draws no VR views: views and hud_group are the VR
	mode's) */
	xgpu_metal_set_targets(target_texture(record->color), 0, record->views < 0 ? (uint32_t)(-record->views - 1) +
		record->base : record->base, target_texture(record->depth), record->samples != 0);
}

static void stream_clear(void *context, const struct xgpu_stream_clear *record)
{
	(void)context;
	xgpu_metal_clear((const int32_t (*)[4])record->rectangles, record->count, clear_parts(record->flags), record->color,
		record->depth, record->stencil);
}

static void stream_clear_target(void *context, const struct xgpu_stream_clear_target *record)
{
	(void)context;
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

/* ---------- textures */

/* XGPU_SOURCE_* */
enum
{
	_source_none, _source_texture, _source_target,
};

static void stream_set_textures(void *context, const struct host_render_texture_stage stages[4])
{
	static BOOL told;
	int index;

	(void)context;
	for (index = 0; index < 4; index++)
	{
		const struct xgpu_stream_texture_stage *stage = &stages[index].stage;
		struct xgpu_metal_sampler sampler;
		uint32_t texture = 0;

		sampler_convert(&stage->sampler, &sampler);
		if (stage->source == _source_target)
			texture = target_texture(stage->target);
		else if (stage->source == _source_texture && !told)
		{
			host_logf(HOST_LOG_WARN, "metal: textures in the Xbox's memory are not drawn yet (their decoding is the "
				"front end's next step)");
			told = YES;
		}
		xgpu_metal_set_texture((uint32_t)index, texture, &sampler);
	}
}

/* ---------- pipelines */

static void stream_set_constants(void *context, const struct xgpu_stream_constants *record)
{
	(void)context;
	xgpu_metal_set_vertex_constants(record->first, record->count, &record->values[0][0]);
}

static void stream_vertex_program(void *context, const struct xgpu_stream_vertex_program *record)
{
	uint64_t hash = record->hash[0] | ((uint64_t)record->hash[1] << 32);

	(void)context;
	stream.vertex_programs[@(hash)] = [NSData dataWithBytes:record length:XGPU_STREAM_HEADER_SIZE(record->header)];
}

static int stream_pipeline(void *context, uint32_t id, const struct xgpu_stream_pipeline_define *definition)
{
	struct stream_pipeline *entry;
	const struct xgpu_stream_pipeline_key *key = &definition->key;
	struct xgpu_metal_pipeline *pipeline = &stream.pipeline;

	(void)context;
	if (!id || id >= XGPU_STREAM_PIPELINES)
		return 0;
	entry = &stream.pipelines[id];
	if (entry->state == XGPU_STREAM_PIPELINE_UNKNOWN)
	{
		uint64_t hash = key->vertex_program[0] | ((uint64_t)key->vertex_program[1] << 32);
		NSData *program = stream.vertex_programs[@(hash)];

		entry->state = XGPU_STREAM_PIPELINE_FAILED;
		if (stream.shaders.make && stream.shaders.make(stream.shaders.context, definition,
			program ? program.bytes : NULL, &entry->vertex_shader, &entry->pixel_shader) && entry->vertex_shader)
			entry->state = XGPU_STREAM_PIPELINE_MADE;
	}
	if (entry->state != XGPU_STREAM_PIPELINE_MADE)
		return 0;
	memset(pipeline, 0, sizeof(*pipeline));
	pipeline->vertex_shader = entry->vertex_shader;
	pipeline->pixel_shader = entry->pixel_shader;
	pipeline->blend = key->blend;
	pipeline->blend_source = key->blend_source;
	pipeline->blend_destination = key->blend_destination;
	pipeline->blend_operation = key->blend_operation;
	pipeline->color_mask = key->color_mask;
	pipeline->depth_test = key->depth_test;
	pipeline->depth_function = key->depth_function;
	pipeline->depth_write = key->depth_write;
	pipeline->stencil_test = key->stencil_test;
	pipeline->stencil_function = key->stencil_function;
	pipeline->stencil_fail = key->stencil_fail;
	pipeline->stencil_depth_fail = key->stencil_depth_fail;
	pipeline->stencil_pass = key->stencil_pass;
	pipeline->stencil_read_mask = (uint8_t)key->stencil_read_mask;
	pipeline->stencil_write_mask = (uint8_t)key->stencil_write_mask;
	pipeline->cull = key->cull;
	pipeline->front_counter_clockwise = key->front_counter_clockwise;
	pipeline->cull_front = key->cull_front;
	pipeline->fill = key->fill;
	pipeline->depth_bias = key->depth_bias;
	pipeline->color_format = target_format(key->color_format);
	pipeline->depth_format = target_format(key->depth_format);
	pipeline->samples = key->samples > 1 ? key->samples : 1;
	return 1;
}

static void stream_draw_state(void *context, const struct xgpu_stream_dynamic_state *dynamic,
	const struct xgpu_stream_draw_uniforms *uniforms, uint32_t uniforms_serial)
{
	(void)context;
	if (dynamic)
		xgpu_metal_set_dynamic_state((const struct xgpu_metal_dynamic_state *)dynamic);
	if (uniforms && (!stream.uniforms_set || uniforms_serial != stream.uniforms_serial))
	{
		xgpu_metal_set_draw_uniforms(uniforms, sizeof(*uniforms));
		stream.uniforms_serial = uniforms_serial;
		stream.uniforms_set = YES;
	}
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
	uint32_t fed = 0, index;

	(void)context;
	memset(stream.attributes, 0, sizeof(stream.attributes));
	memset(stream.strides, 0, sizeof(stream.strides));
	if (input->layout)
	{
		for (index = 0; index < input->layout->element_count; index++)
		{
			const struct xgpu_stream_vertex_element *element = &input->layout->elements[index];

			if (element->reg >= 16 || element->stream >= XGPU_METAL_STREAM_COUNT)
				continue;
			stream.attributes[element->reg].format = attribute_format(element->format);
			stream.attributes[element->reg].stream = element->stream;
			stream.attributes[element->reg].offset = element->offset;
			if (stream.attributes[element->reg].format != XGPU_METAL_ATTRIBUTE_NONE)
				fed |= 1u << element->reg;
		}
	}
	/* (every other input, from its value) */
	for (index = 0; index < 16; index++)
	{
		if (!(fed & (1u << index)))
		{
			stream.attributes[index].format = XGPU_METAL_ATTRIBUTE_FLOAT4;
			stream.attributes[index].stream = XGPU_METAL_STREAM_CONSTANT;
		}
	}
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

/* the pipeline and layout for a draw: set_vertices's, or immediate mode's */
static void draw_prepare(const struct xgpu_metal_vertex_attribute *attributes, const uint16_t *strides,
	const struct xgpu_metal_source *streams, const float (*constants)[4])
{
	struct xgpu_metal_pipeline pipeline = stream.pipeline;
	uint32_t index;

	memcpy(pipeline.attributes, attributes, sizeof(pipeline.attributes));
	memcpy(pipeline.strides, strides, sizeof(pipeline.strides));
	xgpu_metal_set_pipeline(&pipeline);
	for (index = 0; index < XGPU_METAL_STREAM_COUNT; index++)
		xgpu_metal_set_stream(index, streams[index]);
	xgpu_metal_set_constant_attributes(constants);
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
	draw_prepare(stream.attributes, stream.strides, stream.streams, (const float (*)[4])stream.constant_attributes);
	draw_converted(record->primitive, record->count, NULL, (struct xgpu_metal_source){ XGPU_METAL_SOURCE_NONE, 0, 0, 0 }, 0);
}

static void stream_draw_indices(void *context, const struct xgpu_stream_draw_indices *record)
{
	struct xgpu_metal_source source = ring_source(record->indices, record->count * 2);

	(void)context;
	if (source.kind == XGPU_METAL_SOURCE_NONE)
		return;
	draw_prepare(stream.attributes, stream.strides, stream.streams, (const float (*)[4])stream.constant_attributes);
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
	draw_prepare(stream.attributes, stream.strides, stream.streams, (const float (*)[4])stream.constant_attributes);
	/* (the streams start at vertex minimum) */
	draw_converted(record->primitive, record->count, indices, source, -(int32_t)record->minimum);
}

static void stream_draw_immediate(void *context, const struct xgpu_stream_draw_immediate *record,
	const float (*attributes)[4])
{
	struct xgpu_metal_vertex_attribute layout[16];
	uint16_t strides[XGPU_METAL_STREAM_COUNT];
	struct xgpu_metal_source streams[XGPU_METAL_STREAM_COUNT];
	uint32_t index, written = 0, size;

	(void)context;
	memset(layout, 0, sizeof(layout));
	memset(strides, 0, sizeof(strides));
	memset(streams, 0, sizeof(streams));
	/* the registers written, a vec4 each, in order; the rest from their
	values */
	for (index = 0; index < 16; index++)
	{
		layout[index].format = XGPU_METAL_ATTRIBUTE_FLOAT4;
		if (record->written & (1u << index))
		{
			layout[index].stream = 0;
			layout[index].offset = (uint16_t)(written++ * 16);
		}
		else
		{
			layout[index].stream = XGPU_METAL_STREAM_CONSTANT;
		}
	}
	if (!written || !record->count)
		return;
	strides[0] = (uint16_t)(written * 16);
	size = record->count * written * 16;
	streams[0] = ring_source(record->vertices, size);
	if (streams[0].kind == XGPU_METAL_SOURCE_NONE)
		return;
	draw_prepare(layout, strides, streams, attributes);
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

static void stream_frame_end(void *context)
{
	(void)context;
	xgpu_metal_frame_end(target_texture(stream.back_buffer));
	stream.back_buffer = 0;
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
