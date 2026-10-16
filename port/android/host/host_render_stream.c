/*
HOST_RENDER_STREAM.C

The renderer's command stream, the host's half (host_render_stream.h,
port/linux/src/xgpu_stream.h): the decoder that plays the guest's records
into a backend, the render thread it runs on, and the guest's imports that
hand it the frames.

The guest submits records up to its ring header's committed; the render
thread plays them in order. After each frame's last record, and whenever
the guest waits for room or for a synchronous record, it asks the backend
for a fence; with nothing left to play it waits for the oldest fence and
releases the ring's bytes up to it, retiring the frames it ends. The game
so waits only when the ring is full, more than frames_in_flight frames are
unretired, or a record is synchronous: the GPU draws frame N while this
thread plays N + 1 and the game writes N + 2.
*/

#include "host.h"
#include "host_render_record.h"
#include "host_render_stream.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- the decoder */

struct host_render_decoder
{
	const struct host_render_backend *backend;
	void *context;
	struct xgpu_stream_ring *ring;
	unsigned char *buffer;
	uint32_t size, mask;
	volatile uint8_t *pipeline_states;

	struct host_render_texture_stage stages[4];
	struct host_render_vertex_input input;
	float immediate_attributes[16][4];
	struct xgpu_stream_dynamic_state dynamic;
	struct xgpu_stream_draw_uniforms uniforms;
	uint32_t uniforms_serial;
	struct xgpu_stream_pipeline_define *pipelines[XGPU_STREAM_PIPELINES];
	struct xgpu_stream_layout_define *layouts[XGPU_STREAM_LAYOUTS];

	/* the records played reached a frame's end */
	int frame_ended;
};

/* the least size of each type's record */
static const uint32_t record_minimum[XGPU_STREAM_RECORD_TYPES] =
{
	[XGPU_STREAM_SKIP] = sizeof(struct xgpu_stream_record),
	[XGPU_STREAM_INITIALIZE] = sizeof(struct xgpu_stream_reply_record),
	[XGPU_STREAM_INVALIDATE] = sizeof(struct xgpu_stream_record),
	[XGPU_STREAM_FLUSH] = sizeof(struct xgpu_stream_record),
	[XGPU_STREAM_ANTI_ALIASING_PREPARE] = sizeof(struct xgpu_stream_value),
	[XGPU_STREAM_ANTI_ALIAS] = sizeof(struct xgpu_stream_anti_alias),
	[XGPU_STREAM_TARGET_CREATE] = sizeof(struct xgpu_stream_target_create),
	[XGPU_STREAM_TARGET_DELETE] = sizeof(struct xgpu_stream_value),
	[XGPU_STREAM_TARGET_MULTISAMPLE] = sizeof(struct xgpu_stream_target_multisample),
	[XGPU_STREAM_TARGET_RESOLVE] = sizeof(struct xgpu_stream_value),
	[XGPU_STREAM_SET_TARGETS] = sizeof(struct xgpu_stream_set_targets),
	[XGPU_STREAM_CLEAR] = sizeof(struct xgpu_stream_clear),
	[XGPU_STREAM_CLEAR_TARGET] = sizeof(struct xgpu_stream_clear_target),
	[XGPU_STREAM_READ_TARGET] = sizeof(struct xgpu_stream_read_target),
	[XGPU_STREAM_SET_TEXTURES] = sizeof(struct xgpu_stream_set_textures),
	[XGPU_STREAM_CONSTANTS] = sizeof(struct xgpu_stream_constants),
	[XGPU_STREAM_VERTEX_PROGRAM] = sizeof(struct xgpu_stream_vertex_program),
	[XGPU_STREAM_VERTEX_LIGHTING] = sizeof(struct xgpu_stream_vertex_lighting),
	[XGPU_STREAM_PIPELINE_DEFINE] = sizeof(struct xgpu_stream_pipeline_define),
	[XGPU_STREAM_PIPELINE] = sizeof(struct xgpu_stream_value),
	[XGPU_STREAM_DRAW_STATE] = sizeof(struct xgpu_stream_draw_state),
	[XGPU_STREAM_MIRROR_WRITE] = sizeof(struct xgpu_stream_mirror_write),
	[XGPU_STREAM_LAYOUT_DEFINE] = sizeof(struct xgpu_stream_layout_define),
	[XGPU_STREAM_VERTICES] = sizeof(struct xgpu_stream_vertices),
	[XGPU_STREAM_DRAW] = sizeof(struct xgpu_stream_draw),
	[XGPU_STREAM_DRAW_INDICES] = sizeof(struct xgpu_stream_draw_indices),
	[XGPU_STREAM_DRAW_INDEXED] = sizeof(struct xgpu_stream_draw_indexed),
	[XGPU_STREAM_DRAW_IMMEDIATE] = sizeof(struct xgpu_stream_draw_immediate),
	[XGPU_STREAM_VISIBILITY_BEGIN] = sizeof(struct xgpu_stream_record),
	[XGPU_STREAM_VISIBILITY_END] = sizeof(struct xgpu_stream_visibility_end),
	[XGPU_STREAM_VISIBILITY_FRAME] = sizeof(struct xgpu_stream_record),
	[XGPU_STREAM_PRESENT] = sizeof(struct xgpu_stream_value),
	[XGPU_STREAM_FRAME_END] = sizeof(struct xgpu_stream_record),
	[XGPU_STREAM_DYNAMIC_LIGHTS_PREPARE] = sizeof(struct xgpu_stream_record),
	[XGPU_STREAM_DYNAMIC_LIGHTS_UPLOAD] = sizeof(struct xgpu_stream_dynamic_lights_upload),
	[XGPU_STREAM_DYNAMIC_LIGHTS_HALOS] = sizeof(struct xgpu_stream_dynamic_lights_halos),
	[XGPU_STREAM_VR_SUPPORTED] = sizeof(struct xgpu_stream_reply_record),
	[XGPU_STREAM_VR_INITIALIZE] = sizeof(struct xgpu_stream_reply_record),
	[XGPU_STREAM_VR_EYES] = sizeof(struct xgpu_stream_vr_eyes),
	[XGPU_STREAM_VR_PASS] = sizeof(struct xgpu_stream_value),
	[XGPU_STREAM_VR_GROUPS_RESET] = sizeof(struct xgpu_stream_record),
	[XGPU_STREAM_VR_TIMING] = sizeof(struct xgpu_stream_value),
	[XGPU_STREAM_VR_COMPOSE] = sizeof(struct xgpu_stream_vr_frame_record),
	[XGPU_STREAM_VR_SUBMIT] = sizeof(struct xgpu_stream_vr_frame_record),
};

struct host_render_decoder *host_render_decoder_new(const struct host_render_backend *backend,
	struct xgpu_stream_ring *ring)
{
	struct host_render_decoder *decoder = calloc(1, sizeof(*decoder));

	if (!decoder)
		return NULL;
	decoder->backend = backend;
	decoder->context = backend->context;
	decoder->ring = ring;
	decoder->buffer = host_pointer(ring->buffer);
	decoder->size = ring->size;
	decoder->mask = ring->size - 1;
	decoder->pipeline_states = host_pointer(ring->pipelines);
	return decoder;
}

void host_render_decoder_free(struct host_render_decoder *decoder)
{
	uint32_t index;

	if (!decoder)
		return;
	for (index = 0; index < XGPU_STREAM_PIPELINES; index++)
		free(decoder->pipelines[index]);
	for (index = 0; index < XGPU_STREAM_LAYOUTS; index++)
		free(decoder->layouts[index]);
	free(decoder);
}

/* data a record names, bytes of it at address: in place anywhere in the
guest's memory, or if in the ring, inside the record */
static int data_inside(const struct host_render_decoder *decoder, const void *record, uint32_t record_size,
	uint32_t address, uint32_t bytes)
{
	uint32_t buffer = decoder->ring->buffer;
	uint32_t start = guest_address(record);

	if (!bytes)
		return 1;
	if (!address)
		return 0;
	if (address - buffer >= decoder->size)
		return address + bytes >= address;
	return address >= start && address - start <= record_size && bytes <= record_size - (address - start);
}

static void answer(uint32_t reply, const void *value, size_t size)
{
	if (reply)
		memcpy(host_pointer(reply), value, size);
}

#define CALL(name, ...) do { if (backend->name) backend->name(context, __VA_ARGS__); } while (0)
#define CALL0(name) do { if (backend->name) backend->name(context); } while (0)

/* plays one record; 0, or an XGPU_STREAM_ERROR_* */
static int decode_record(struct host_render_decoder *decoder, const unsigned char *start, uint32_t type, uint32_t size)
{
	const struct host_render_backend *backend = decoder->backend;
	void *context = decoder->context;
	const struct xgpu_stream_value *value = (const void *)start;

	switch (type)
	{
	case XGPU_STREAM_SKIP:
		break;
	case XGPU_STREAM_INITIALIZE:
	{
		struct xgpu_stream_capabilities capabilities;

		memset(&capabilities, 0, sizeof(capabilities));
		CALL(initialize, &capabilities);
		answer(((const struct xgpu_stream_reply_record *)start)->reply, &capabilities, sizeof(capabilities));
		break;
	}
	case XGPU_STREAM_INVALIDATE:
		CALL0(invalidate);
		break;
	case XGPU_STREAM_FLUSH:
		CALL0(flush);
		break;
	case XGPU_STREAM_ANTI_ALIASING_PREPARE:
		CALL(anti_aliasing_prepare, value->value);
		break;
	case XGPU_STREAM_ANTI_ALIAS:
		CALL(anti_alias, (const void *)start);
		break;
	case XGPU_STREAM_TARGET_CREATE:
		CALL(target_create, (const void *)start);
		break;
	case XGPU_STREAM_TARGET_DELETE:
		CALL(target_delete, value->value);
		break;
	case XGPU_STREAM_TARGET_MULTISAMPLE:
		CALL(target_multisample, (const void *)start);
		break;
	case XGPU_STREAM_TARGET_RESOLVE:
		CALL(target_resolve, value->value);
		break;
	case XGPU_STREAM_SET_TARGETS:
		CALL(set_targets, (const void *)start);
		break;
	case XGPU_STREAM_CLEAR:
	{
		const struct xgpu_stream_clear *clear = (const void *)start;

		if (clear->count > (size - sizeof(*clear)) / sizeof(clear->rectangles[0]))
			return XGPU_STREAM_ERROR_RECORD;
		CALL(clear, clear);
		break;
	}
	case XGPU_STREAM_CLEAR_TARGET:
		CALL(clear_target, (const void *)start);
		break;
	case XGPU_STREAM_READ_TARGET:
	{
		const struct xgpu_stream_read_target *read = (const void *)start;
		uint32_t written = 0;

		if (backend->read_target && read->pixels && read->size)
			written = backend->read_target(context, read) ? 1 : 0;
		answer(read->reply, &written, sizeof(written));
		break;
	}
	case XGPU_STREAM_SET_TEXTURES:
	{
		const struct xgpu_stream_set_textures *textures = (const void *)start;
		const unsigned char *place = (const unsigned char *)(textures + 1);
		const unsigned char *end = start + size;
		int stage;

		for (stage = 0; stage < 4; stage++)
		{
			struct host_render_texture_stage *to = &decoder->stages[stage];
			uint32_t level_bytes;

			if (!(textures->changed & (1u << stage)))
				continue;
			if ((size_t)(end - place) < sizeof(to->stage))
				return XGPU_STREAM_ERROR_RECORD;
			memcpy(&to->stage, place, sizeof(to->stage));
			place += sizeof(to->stage);
			if (to->stage.level_count > XGPU_STREAM_MIP_LEVELS)
				return XGPU_STREAM_ERROR_RECORD;
			level_bytes = to->stage.level_count * sizeof(to->levels[0]);
			if ((size_t)(end - place) < level_bytes)
				return XGPU_STREAM_ERROR_RECORD;
			memset(to->levels, 0, sizeof(to->levels));
			memcpy(to->levels, place, level_bytes);
			place += level_bytes;
		}
		CALL(set_textures, decoder->stages);
		break;
	}
	case XGPU_STREAM_CONSTANTS:
	{
		const struct xgpu_stream_constants *constants = (const void *)start;

		if (constants->count > (size - sizeof(*constants)) / sizeof(constants->values[0]) ||
			constants->first > 192 || constants->count > 192 - constants->first)
			return XGPU_STREAM_ERROR_RECORD;
		CALL(set_constants, constants);
		break;
	}
	case XGPU_STREAM_VERTEX_PROGRAM:
	{
		const struct xgpu_stream_vertex_program *program = (const void *)start;

		if (program->instruction_count > XGPU_STREAM_VERTEX_PROGRAM_MAXIMUM ||
			program->instruction_count > (size - sizeof(*program)) / XGPU_STREAM_VERTEX_INSTRUCTION_SIZE)
			return XGPU_STREAM_ERROR_RECORD;
		CALL(vertex_program, program);
		break;
	}
	case XGPU_STREAM_VERTEX_LIGHTING:
		CALL(vertex_lighting, (const void *)start);
		break;
	case XGPU_STREAM_PIPELINE_DEFINE:
	{
		const struct xgpu_stream_pipeline_define *define = (const void *)start;
		struct xgpu_stream_pipeline_define **slot;

		if (!define->id || define->id >= XGPU_STREAM_PIPELINES)
			return XGPU_STREAM_ERROR_ID;
		slot = &decoder->pipelines[define->id];
		if (!*slot && !(*slot = malloc(sizeof(**slot))))
			return XGPU_STREAM_ERROR_ID;
		memcpy(*slot, define, sizeof(**slot));
		__atomic_store_n(&decoder->pipeline_states[define->id], XGPU_STREAM_PIPELINE_UNKNOWN, __ATOMIC_RELAXED);
		CALL(pipeline_define, *slot);
		break;
	}
	case XGPU_STREAM_PIPELINE:
	{
		uint32_t id = value->value;
		int made = 1;

		if (!id || id >= XGPU_STREAM_PIPELINES || !decoder->pipelines[id])
			return XGPU_STREAM_ERROR_ID;
		if (backend->pipeline)
			made = backend->pipeline(context, id, decoder->pipelines[id]);
		if (__atomic_load_n(&decoder->pipeline_states[id], __ATOMIC_RELAXED) == XGPU_STREAM_PIPELINE_UNKNOWN)
			__atomic_store_n(&decoder->pipeline_states[id], made ? XGPU_STREAM_PIPELINE_MADE : XGPU_STREAM_PIPELINE_FAILED,
				__ATOMIC_RELAXED);
		break;
	}
	case XGPU_STREAM_DRAW_STATE:
	{
		const struct xgpu_stream_draw_state *state = (const void *)start;
		const unsigned char *place = (const unsigned char *)(state + 1);
		unsigned char *uniforms = (unsigned char *)&decoder->uniforms;
		uint32_t pieces = (uint32_t)__builtin_popcount(state->pieces[0]) + (uint32_t)__builtin_popcount(state->pieces[1]);
		uint32_t needed = sizeof(*state) + (state->parts & XGPU_STREAM_DRAW_DYNAMIC ? sizeof(decoder->dynamic) : 0) +
			pieces * 16;
		uint32_t piece;

		if (needed > size || (XGPU_STREAM_UNIFORM_PIECES < 64 && state->pieces[1] >> (XGPU_STREAM_UNIFORM_PIECES - 32)))
			return XGPU_STREAM_ERROR_RECORD;
		if (state->parts & XGPU_STREAM_DRAW_DYNAMIC)
		{
			memcpy(&decoder->dynamic, place, sizeof(decoder->dynamic));
			place += sizeof(decoder->dynamic);
		}
		for (piece = 0; piece < XGPU_STREAM_UNIFORM_PIECES; piece++)
		{
			uint32_t offset = piece * 16;

			if (!(state->pieces[piece / 32] & (1u << (piece % 32))))
				continue;
			memcpy(uniforms + offset, place, sizeof(decoder->uniforms) - offset < 16 ? sizeof(decoder->uniforms) - offset : 16);
			place += 16;
		}
		if (state->parts & XGPU_STREAM_DRAW_UNIFORMS)
			decoder->uniforms_serial = state->uniforms_serial;
		CALL(draw_state, &decoder->dynamic, &decoder->uniforms, decoder->uniforms_serial);
		break;
	}
	case XGPU_STREAM_MIRROR_WRITE:
	{
		const struct xgpu_stream_mirror_write *write = (const void *)start;

		if (!data_inside(decoder, start, size, write->data, write->size))
			return XGPU_STREAM_ERROR_BOUNDS;
		CALL(mirror_write, write);
		break;
	}
	case XGPU_STREAM_LAYOUT_DEFINE:
	{
		const struct xgpu_stream_layout_define *define = (const void *)start;
		struct xgpu_stream_layout_define **slot;

		if (!define->id || define->id >= XGPU_STREAM_LAYOUTS)
			return XGPU_STREAM_ERROR_ID;
		if (define->element_count > 16 ||
			define->element_count > (size - sizeof(*define)) / sizeof(define->elements[0]))
			return XGPU_STREAM_ERROR_RECORD;
		slot = &decoder->layouts[define->id];
		if (!*slot && !(*slot = malloc(sizeof(**slot) + 16 * sizeof(define->elements[0]))))
			return XGPU_STREAM_ERROR_ID;
		memcpy(*slot, define, sizeof(*define) + define->element_count * sizeof(define->elements[0]));
		break;
	}
	case XGPU_STREAM_VERTICES:
	{
		const struct xgpu_stream_vertices *vertices = (const void *)start;
		const struct xgpu_stream_vertex_stream *streams = (const void *)(vertices + 1);
		struct host_render_vertex_input *input = &decoder->input;
		uint32_t stream_count = (uint32_t)__builtin_popcount(vertices->streams);
		uint32_t attribute_count = (uint32_t)__builtin_popcount(vertices->attributes);
		const float (*values)[4];
		int s, reg;

		if (!vertices->layout || vertices->layout >= XGPU_STREAM_LAYOUTS || !decoder->layouts[vertices->layout])
			return XGPU_STREAM_ERROR_ID;
		if (sizeof(*vertices) + stream_count * sizeof(*streams) + attribute_count * 16 > size)
			return XGPU_STREAM_ERROR_RECORD;
		input->layout = decoder->layouts[vertices->layout];
		input->present = vertices->streams;
		memset(input->streams, 0, sizeof(input->streams));
		for (s = 0; s < 16; s++)
		{
			if (!(vertices->streams & (1u << s)))
				continue;
			if (streams->segment < 0 && !data_inside(decoder, start, size, streams->data, streams->bytes))
				return XGPU_STREAM_ERROR_BOUNDS;
			input->streams[s] = *streams++;
		}
		values = (const void *)streams;
		for (reg = 0; reg < 16; reg++)
		{
			if (vertices->attributes & (1u << reg))
				memcpy(input->attributes[reg], *values++, 16);
		}
		CALL(set_vertices, input);
		break;
	}
	case XGPU_STREAM_DRAW:
		CALL(draw, (const void *)start);
		break;
	case XGPU_STREAM_DRAW_INDICES:
	{
		const struct xgpu_stream_draw_indices *draw = (const void *)start;

		if (draw->count > 0x7fffffffu || !data_inside(decoder, start, size, draw->indices, draw->count * 2))
			return XGPU_STREAM_ERROR_BOUNDS;
		CALL(draw_indices, draw);
		break;
	}
	case XGPU_STREAM_DRAW_INDEXED:
	{
		const struct xgpu_stream_draw_indexed *draw = (const void *)start;

		if (draw->segment < 0 &&
			(draw->count > 0x7fffffffu || !data_inside(decoder, start, size, draw->indices, draw->count * 2)))
			return XGPU_STREAM_ERROR_BOUNDS;
		CALL(draw_indexed, draw);
		break;
	}
	case XGPU_STREAM_DRAW_IMMEDIATE:
	{
		const struct xgpu_stream_draw_immediate *draw = (const void *)start;
		const float (*values)[4] = (const void *)(draw + 1);
		uint64_t vertex_bytes = (uint64_t)draw->count * (uint32_t)__builtin_popcount(draw->written) * 16;
		int reg;

		if (sizeof(*draw) + (uint32_t)__builtin_popcount(draw->attributes) * 16 > size)
			return XGPU_STREAM_ERROR_RECORD;
		if (vertex_bytes > 0xffffffffu || !data_inside(decoder, start, size, draw->vertices, (uint32_t)vertex_bytes) ||
			draw->index_count > 0x7fffffffu || !data_inside(decoder, start, size, draw->indices, draw->index_count * 2))
			return XGPU_STREAM_ERROR_BOUNDS;
		for (reg = 0; reg < 16; reg++)
		{
			if (draw->attributes & (1u << reg))
				memcpy(decoder->immediate_attributes[reg], *values++, 16);
		}
		CALL(draw_immediate, draw, (const float (*)[4])decoder->immediate_attributes);
		break;
	}
	case XGPU_STREAM_VISIBILITY_BEGIN:
		CALL0(visibility_begin);
		break;
	case XGPU_STREAM_VISIBILITY_END:
		if (((const struct xgpu_stream_visibility_end *)start)->slot >= XGPU_STREAM_VISIBILITY_SLOTS)
			return XGPU_STREAM_ERROR_ID;
		CALL(visibility_end, (const void *)start);
		break;
	case XGPU_STREAM_VISIBILITY_FRAME:
		CALL0(visibility_frame);
		break;
	case XGPU_STREAM_PRESENT:
		CALL(present, value->value);
		break;
	case XGPU_STREAM_FRAME_END:
		CALL0(frame_end);
		decoder->frame_ended = 1;
		break;
	case XGPU_STREAM_DYNAMIC_LIGHTS_PREPARE:
		if (backend->dynamic_lights_prepare)
			backend->dynamic_lights_prepare(context);
		break;
	case XGPU_STREAM_DYNAMIC_LIGHTS_UPLOAD:
	{
		const struct xgpu_stream_dynamic_lights_upload *upload = (const void *)start;

		if (upload->count > XGPU_STREAM_DYNAMIC_LIGHTS_MAXIMUM ||
			!data_inside(decoder, start, size, upload->cells, upload->size))
			return XGPU_STREAM_ERROR_BOUNDS;
		CALL(dynamic_lights_upload, upload);
		break;
	}
	case XGPU_STREAM_DYNAMIC_LIGHTS_HALOS:
	{
		const struct xgpu_stream_dynamic_lights_halos *halos = (const void *)start;

		if (halos->count < 0 || halos->count > XGPU_STREAM_DYNAMIC_LIGHTS_MAXIMUM ||
			(uint32_t)halos->count * 32 > size - sizeof(*halos))
			return XGPU_STREAM_ERROR_RECORD;
		CALL(dynamic_lights_halos, halos);
		break;
	}
	case XGPU_STREAM_VR_SUPPORTED:
	{
		uint32_t supported = backend->vr_supported ? (backend->vr_supported(context) ? 1 : 0) : 0;

		answer(((const struct xgpu_stream_reply_record *)start)->reply, &supported, sizeof(supported));
		break;
	}
	case XGPU_STREAM_VR_INITIALIZE:
	{
		uint32_t reply = ((const struct xgpu_stream_reply_record *)start)->reply;
		struct xgpu_stream_vr_info info;

		if (!reply)
			break;
		memcpy(&info, host_pointer(reply), sizeof(info));
		CALL(vr_initialize, &info);
		answer(reply, &info, sizeof(info));
		break;
	}
	case XGPU_STREAM_VR_EYES:
		CALL(vr_eyes, (const void *)start);
		break;
	case XGPU_STREAM_VR_PASS:
		CALL(vr_pass, (int32_t)value->value);
		break;
	case XGPU_STREAM_VR_GROUPS_RESET:
		CALL0(vr_groups_reset);
		break;
	case XGPU_STREAM_VR_TIMING:
		CALL(vr_timing, (int32_t)value->value);
		break;
	case XGPU_STREAM_VR_COMPOSE:
		CALL(vr_compose, &((const struct xgpu_stream_vr_frame_record *)start)->frame);
		break;
	case XGPU_STREAM_VR_SUBMIT:
	{
		const struct xgpu_stream_vr_frame_record *submit = (const void *)start;

		CALL(vr_submit, &submit->frame, submit->screenshot);
		break;
	}
	default:
		return XGPU_STREAM_ERROR_RECORD;
	}
	return XGPU_STREAM_ERROR_NONE;
}

uint32_t host_render_decode(struct host_render_decoder *decoder, uint32_t from, uint32_t to)
{
	struct xgpu_stream_ring *ring = decoder->ring;
	uint32_t position = from;

	decoder->frame_ended = 0;
	while (position != to && !decoder->frame_ended)
	{
		uint32_t offset = position & decoder->mask;
		uint32_t available = to - position;
		const unsigned char *start = decoder->buffer + offset;
		uint32_t header, type, size;
		int error;

		header = *(const uint32_t *)start;
		type = XGPU_STREAM_HEADER_TYPE(header);
		size = XGPU_STREAM_HEADER_SIZE(header);
		if (type == XGPU_STREAM_SKIP && !size)
			size = decoder->size - offset;
		if (size > available || size > decoder->size - offset)
		{
			error = XGPU_STREAM_ERROR_BOUNDS;
		}
		else if (type >= XGPU_STREAM_RECORD_TYPES || size < record_minimum[type] || size % XGPU_STREAM_RECORD_ALIGNMENT)
		{
			error = XGPU_STREAM_ERROR_RECORD;
		}
		else if (host_render_record_active)
		{
			/* (recorded, or the replay's memory restored: host_render_record.h) */
			host_render_record_before(ring, position, start, type, size);
			error = decode_record(decoder, start, type, size);
			host_render_record_after();
		}
		else
		{
			error = decode_record(decoder, start, type, size);
		}
		if (error)
		{
			ring->error = (uint32_t)error;
			ring->error_position = position;
			host_logf(HOST_LOG_ERROR, "render stream: record %u of %u bytes at %u: error %d", type, size, position,
				error);
			return position;
		}
		position += size;
		__atomic_store_n(&ring->decoded, position, __ATOMIC_RELEASE);
	}
	return position;
}

/* ---------- the render thread */

#define FENCE_QUEUE 64

struct fence_entry
{
	/* the ring's bytes it covers, to there; the frames it ends */
	uint32_t position;
	uint32_t frames;
	uint64_t fence;
};

static const struct host_render_backend *render_backend;

static struct
{
	pthread_mutex_t lock;
	pthread_cond_t work;    /* for the render thread: records, a waiting guest, the end */
	pthread_cond_t done;    /* for the guest: bytes released, frames retired */
	pthread_t thread;
	int running, stopping;
	struct xgpu_stream_ring *ring;
	struct host_render_decoder *decoder;
	uint32_t committed, decoded;
	/* positions the guest waits to see released (room, or a synchronous
	record), while it does */
	int guest_waiting;
	struct fence_entry fences[FENCE_QUEUE];
	uint32_t fence_first, fence_count;
	/* the position the last fence covers, the frames played and not yet
	in a fence */
	uint32_t fenced;
	uint32_t frames_unfenced;
	uint32_t retired_frames;
	uint64_t frame_started;
} render =
{
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.work = PTHREAD_COND_INITIALIZER,
	.done = PTHREAD_COND_INITIALIZER,
};

static uint64_t monotonic_nanoseconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

void host_render_stream_set_backend(const struct host_render_backend *backend)
{
	render_backend = backend;
}

/* (render.lock held) a fence over what has been played, unless there is
nothing new */
static void fence_push(void)
{
	const struct host_render_backend *backend = render_backend;
	struct fence_entry *entry;
	uint64_t fence;

	if (render.fenced == render.decoded && !render.frames_unfenced)
		return;
	if (render.fence_count == FENCE_QUEUE)
		return;
	pthread_mutex_unlock(&render.lock);
	fence = backend->fence ? backend->fence(backend->context) : 0;
	pthread_mutex_lock(&render.lock);
	entry = &render.fences[(render.fence_first + render.fence_count) % FENCE_QUEUE];
	entry->position = render.decoded;
	entry->frames = render.frames_unfenced;
	entry->fence = fence;
	render.fence_count++;
	render.fenced = render.decoded;
	render.frames_unfenced = 0;
}

/* (render.lock held) waits for the oldest fence; its bytes released, its
frames retired */
static void fence_retire(void)
{
	const struct host_render_backend *backend = render_backend;
	struct fence_entry entry = render.fences[render.fence_first];

	pthread_mutex_unlock(&render.lock);
	if (backend->wait)
		backend->wait(backend->context, entry.fence);
	pthread_mutex_lock(&render.lock);
	render.fence_first = (render.fence_first + 1) % FENCE_QUEUE;
	render.fence_count--;
	render.retired_frames += entry.frames;
	__atomic_store_n(&render.ring->frames_retired, render.retired_frames, __ATOMIC_RELEASE);
	__atomic_store_n(&render.ring->released, entry.position, __ATOMIC_RELEASE);
	pthread_cond_broadcast(&render.done);
}

/* after a malformed record: nothing more is played, and nothing waits */
static void stream_failed(void)
{
	render.decoded = render.fenced = render.committed;
	render.fence_count = 0;
	render.retired_frames = render.ring->frames_submitted;
	__atomic_store_n(&render.ring->frames_retired, render.retired_frames, __ATOMIC_RELEASE);
	__atomic_store_n(&render.ring->released, render.committed, __ATOMIC_RELEASE);
	pthread_cond_broadcast(&render.done);
}

static _Atomic int render_playing;

int host_render_stream_playing(void)
{
	return atomic_load(&render_playing);
}

static void *render_thread(void *argument)
{
	(void)argument;
	pthread_mutex_lock(&render.lock);
	for (;;)
	{
		if (render.ring->error)
		{
			stream_failed();
			if (render.stopping)
				break;
			pthread_cond_wait(&render.work, &render.lock);
			continue;
		}
		if (render.decoded != render.committed)
		{
			uint32_t from = render.decoded, to = render.committed, reached;
			int frame_ended;

			pthread_mutex_unlock(&render.lock);
			if (!render.frame_started)
				render.frame_started = monotonic_nanoseconds();
			atomic_store(&render_playing, 1);
			reached = host_render_decode(render.decoder, from, to);
			atomic_store(&render_playing, 0);
			frame_ended = render.decoder->frame_ended;
			if (frame_ended)
			{
				render.ring->decode_nanoseconds = (uint32_t)(monotonic_nanoseconds() - render.frame_started);
				render.frame_started = 0;
			}
			pthread_mutex_lock(&render.lock);
			if (render.ring->error)
				continue;
			render.decoded = reached;
			if (frame_ended)
			{
				render.frames_unfenced++;
				fence_push();
			}
			continue;
		}
		else if (render.guest_waiting && render.fenced != render.decoded && render.fence_count < FENCE_QUEUE)
		{
			/* the guest waits on what is played and not yet fenced */
			fence_push();
			continue;
		}
		else if (render.fence_count)
		{
			/* nothing to play: the GPU's oldest work waited for */
			fence_retire();
			continue;
		}
		if (render.stopping)
			break;
		pthread_cond_wait(&render.work, &render.lock);
	}
	pthread_mutex_unlock(&render.lock);
	return NULL;
}

/* the render thread; on macOS, at the QoS of an app's user interface: each
frame is the display's (pthread_attr_set_qos_class_np) */
static int render_thread_start(void)
{
	pthread_attr_t attributes;
	int result;

	pthread_attr_init(&attributes);
#ifdef __APPLE__
	pthread_attr_set_qos_class_np(&attributes, QOS_CLASS_USER_INTERACTIVE, 0);
#endif
	result = pthread_create(&render.thread, &attributes, render_thread, NULL);
	pthread_attr_destroy(&attributes);
	return result;
}

int host_render_stream_open(uint32_t ring_address)
{
	const struct host_render_backend *backend = render_backend;
	struct xgpu_stream_ring *ring = host_pointer(ring_address);

	if (!backend || !ring || render.running)
		return -1;
	if (ring->magic != XGPU_STREAM_MAGIC || ring->version != XGPU_STREAM_VERSION ||
		ring->header_size != sizeof(*ring))
	{
		host_logf(HOST_LOG_ERROR, "render stream: the guest's ring is of another version (%u)", ring->version);
		return -1;
	}
	if (ring->size < 0x10000 || (ring->size & (ring->size - 1)) || ring->buffer % XGPU_STREAM_RING_ALIGNMENT ||
		ring->buffer + ring->size < ring->buffer || !ring->frames_in_flight || ring->frames_in_flight > 8 ||
		!ring->visibility || !ring->pipelines)
	{
		host_logf(HOST_LOG_ERROR, "render stream: the guest's ring is out of place");
		return -1;
	}
	render.ring = ring;
	render.decoder = host_render_decoder_new(backend, ring);
	if (!render.decoder)
		return -1;
	render.committed = render.decoded = render.fenced = ring->committed;
	render.fence_first = render.fence_count = 0;
	render.frames_unfenced = 0;
	render.retired_frames = ring->frames_retired = ring->frames_submitted;
	ring->released = ring->committed;
	ring->error = 0;
	render.stopping = 0;
	if (backend->open)
		backend->open(backend->context, host_pointer(ring->buffer), ring->size, host_pointer(ring->visibility));
	host_render_record_start(ring);
	if (render_thread_start())
	{
		host_render_decoder_free(render.decoder);
		render.decoder = NULL;
		return -1;
	}
	render.running = 1;
	return 0;
}

/* (render.lock held) the guest waits for done, counted in its statistics */
static void guest_wait(uint64_t *start)
{
	if (!*start)
	{
		*start = monotonic_nanoseconds();
		render.ring->waits++;
	}
	render.guest_waiting++;
	pthread_cond_signal(&render.work);
	pthread_cond_wait(&render.done, &render.lock);
	render.guest_waiting--;
}

static void guest_waited(uint64_t start)
{
	if (start)
		render.ring->wait_nanoseconds += (uint32_t)(monotonic_nanoseconds() - start);
}

void host_render_stream_submit(uint32_t flags)
{
	struct xgpu_stream_ring *ring = render.ring;
	uint64_t start = 0;

	if (!render.running)
		return;
	pthread_mutex_lock(&render.lock);
	render.committed = __atomic_load_n(&ring->committed, __ATOMIC_ACQUIRE);
	pthread_cond_signal(&render.work);
	while (!ring->error)
	{
		if ((flags & XGPU_STREAM_SUBMIT_FRAME) && ring->frames_submitted - render.retired_frames > ring->frames_in_flight)
			guest_wait(&start);
		else if ((flags & XGPU_STREAM_SUBMIT_SYNC) && (int32_t)(ring->released - render.committed) < 0)
			guest_wait(&start);
		else
			break;
	}
	guest_waited(start);
	pthread_mutex_unlock(&render.lock);
}

void host_render_stream_wait(uint32_t position)
{
	struct xgpu_stream_ring *ring = render.ring;
	uint64_t start = 0;

	if (!render.running)
		return;
	pthread_mutex_lock(&render.lock);
	while (!ring->error && (int32_t)(ring->released - position) < 0)
		guest_wait(&start);
	guest_waited(start);
	pthread_mutex_unlock(&render.lock);
}

const struct xgpu_stream_ring *host_render_stream_ring(void)
{
	return render.running ? render.ring : NULL;
}

void host_render_stream_close(void)
{
	if (!render.running)
		return;
	pthread_mutex_lock(&render.lock);
	render.stopping = 1;
	pthread_cond_signal(&render.work);
	pthread_mutex_unlock(&render.lock);
	pthread_join(render.thread, NULL);
	host_render_record_stop();
	host_render_decoder_free(render.decoder);
	render.decoder = NULL;
	render.running = 0;
}
