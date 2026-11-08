/*
HOST_RENDER_REPLAY.C

The replays' shared part (host_render_replay.h). The guest's memory is a
reservation of 4 GB, at a base as the game's host has it with the base
register (host_memory_base.c), else at the low 4 GB of the process as the
x86-64 Linux host has it; the tool is the guest, writing the recorded
records into the ring where they were and submitting them as the encoder
did (a frame's end, a synchronous record, the ring full), and the same
render thread, decoder and backend play them. Before each record is played,
the memory it reads (the recording's chunks) is put back where it was, and
the pages that changed count as written for the textures' cache (the write
tracking's generations, here).

The backend the stream plays into is the platform's, behind a table of the
replay's own that hands each record on and watches the draws, the targets
and the present: the frame asked for is read there, and the draws are
numbered for its ID buffer.

The recording is played again from its start for a frame before the one
last played: the positions in the ring go on (the records' own, moved by
whole rings), so that the render thread sees one stream.
*/

#include "host.h"
#include "host_render_record.h"
#include "host_render_replay.h"
#include "host_render_stream.h"

#ifndef HALO_REPLAY_NO_SDL
#include <SDL3/SDL.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stddef.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef HALO_REPLAY_BUILD
#define HALO_REPLAY_BUILD "unknown"
#endif

/* ---------- what the host gives the backend and the decoder, here */

#ifdef HALO_GUEST_BASE
uintptr_t host_guest_base;
#endif
void (*host_memory_remapped)(uint32_t address, uint32_t size);
/* (debug.pass_times: off) */
int host_pass_times;
int host_render_replay_verbose;

void host_logf(int priority, const char *format, ...)
{
	va_list arguments;

	if (priority < HOST_LOG_WARN && !host_render_replay_verbose)
		return;
	fprintf(stderr, "host: ");
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
}

void host_fatal(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
	exit(2);
}

/* the guest's memory: pages put back (readable) and their generations */
static size_t page_size;
static uint8_t *pages_restored;
static uint32_t *page_generations;
static _Atomic uint32_t watch_serial = 1;

size_t host_page_size(void)
{
	return page_size;
}

static int pages_readable(uint32_t address, uint32_t size)
{
	uint64_t page, last = ((uint64_t)address + size + page_size - 1) / page_size;

	if (!address)
		return 0;
	for (page = address / page_size; page < last; page++)
	{
		if (page >= 0x100000000ull / page_size || !pages_restored[page])
			return 0;
	}
	return 1;
}

/* readable if every page has been put back; the extent is none, so that
the backend copies what it would read in place (the tool rewrites the
memory while frames are in flight, which the game does not) */
int host_memory_readable_extent(uint32_t address, uint32_t size, uint32_t limit_start, uint32_t limit_end,
	uint32_t *start, uint32_t *end)
{
	(void)limit_start;
	(void)limit_end;
	if (!pages_readable(address, size))
		return 0;
	*start = *end = 0;
	return 1;
}

/* (the low 4 GB's: Vulkan's textures ask it where the guest has no base) */
int host_low_owns(uintptr_t address, size_t size)
{
	return address < 0x100000000ull && size <= 0x100000000ull - address &&
		pages_readable((uint32_t)address, (uint32_t)size);
}

void host_memory_watch_protect(uint32_t address, uint32_t size)
{
	(void)address;
	(void)size;
}

uint32_t host_memory_watch_generation(uint32_t address, uint32_t size)
{
	uint64_t page, last = ((uint64_t)address + size + page_size - 1) / page_size;
	uint32_t newest = 0;

	for (page = address / page_size; page < last && page < 0x100000000ull / page_size; page++)
	{
		if (page_generations[page] > newest)
			newest = page_generations[page];
	}
	return newest;
}

uint32_t host_memory_watch_serial(void)
{
	return atomic_load(&watch_serial);
}

/* (no write tracking here: no faults) */
uint64_t host_memory_watch_faults(void)
{
	return 0;
}

static void pages_mark(uint32_t address, uint32_t size, int written)
{
	uint64_t page, last = ((uint64_t)address + size + page_size - 1) / page_size;
	uint32_t serial = written ? atomic_fetch_add(&watch_serial, 1) + 1 : 0;

	for (page = address / page_size; page < last && page < 0x100000000ull / page_size; page++)
	{
		pages_restored[page] = 1;
		if (written)
			page_generations[page] = serial;
	}
}

/* ---------- LZ4 (Apple's framing of it) */

static uint32_t load32(const unsigned char *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

/* a block of LZ4's format into to + at (its matches may reach back into
what was decoded before it, from to); the bytes decoded, or SIZE_MAX */
static size_t lz4_block(unsigned char *to, size_t at, size_t to_size, const unsigned char *from, size_t from_size)
{
	size_t in = 0, out = at;

	while (in < from_size)
	{
		unsigned token = from[in++];
		size_t literals = token >> 4, match, offset;
		unsigned char more;

		if (literals == 15)
		{
			do
			{
				if (in >= from_size)
					return SIZE_MAX;
				more = from[in++];
				literals += more;
			} while (more == 255);
		}
		if (literals > from_size - in || literals > to_size - out)
			return SIZE_MAX;
		memcpy(to + out, from + in, literals);
		in += literals;
		out += literals;
		/* (the last sequence has literals alone) */
		if (in == from_size)
			break;
		if (from_size - in < 2)
			return SIZE_MAX;
		offset = (size_t)from[in] | (size_t)from[in + 1] << 8;
		in += 2;
		if (!offset || offset > out)
			return SIZE_MAX;
		match = token & 15;
		if (match == 15)
		{
			do
			{
				if (in >= from_size)
					return SIZE_MAX;
				more = from[in++];
				match += more;
			} while (more == 255);
		}
		match += 4;
		if (match > to_size - out)
			return SIZE_MAX;
		if (offset >= match)
			memcpy(to + out, to + out - offset, match);
		else
		{
			size_t index;

			for (index = 0; index < match; index++)
				to[out + index] = to[out - offset + index];
		}
		out += match;
	}
	return out - at;
}

size_t host_render_replay_lz4_decode(unsigned char *to, size_t to_size, const unsigned char *from, size_t from_size)
{
	size_t in = 0, out = 0;

	while (from_size - in >= 4)
	{
		const unsigned char *header = from + in;

		if (!memcmp(header, "bv4$", 4))
			return out;
		if (!memcmp(header, "bv4-", 4))
		{
			uint32_t size;

			if (from_size - in < 8)
				return 0;
			size = load32(header + 4);
			in += 8;
			if (size > from_size - in || size > to_size - out)
				return 0;
			memcpy(to + out, from + in, size);
			in += size;
			out += size;
		}
		else if (!memcmp(header, "bv41", 4))
		{
			uint32_t decoded, encoded;

			if (from_size - in < 12)
				return 0;
			decoded = load32(header + 4);
			encoded = load32(header + 8);
			in += 12;
			if (encoded > from_size - in || decoded > to_size - out ||
				lz4_block(to, out, out + decoded, from + in, encoded) != decoded)
				return 0;
			in += encoded;
			out += decoded;
		}
		else
		{
			return 0;
		}
	}
	return out;
}

char *host_render_replay_base64(const unsigned char *data, size_t size)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char *text = malloc((size + 2) / 3 * 4 + 1), *to = text;
	size_t index;

	if (!text)
		return NULL;
	for (index = 0; index + 2 < size; index += 3)
	{
		uint32_t triple = (uint32_t)data[index] << 16 | (uint32_t)data[index + 1] << 8 | data[index + 2];

		*to++ = alphabet[triple >> 18];
		*to++ = alphabet[(triple >> 12) & 63];
		*to++ = alphabet[(triple >> 6) & 63];
		*to++ = alphabet[triple & 63];
	}
	if (index < size)
	{
		uint32_t triple = (uint32_t)data[index] << 16 | (index + 1 < size ? (uint32_t)data[index + 1] << 8 : 0);

		*to++ = alphabet[triple >> 18];
		*to++ = alphabet[(triple >> 12) & 63];
		*to++ = index + 1 < size ? alphabet[(triple >> 6) & 63] : '=';
		*to++ = '=';
	}
	*to = 0;
	return text;
}

/* ---------- the recording */

struct chunk
{
	uint32_t address, size;
	const unsigned char *data;
};

struct record
{
	uint32_t position, type, size, bytes;
	const unsigned char *data;
	uint32_t chunk_first, chunk_count;
};

/* a frame: its records (first, to the one after its FRAME_END), the
target it presents (0 none) and its size, its draws */
struct frame
{
	uint32_t first, end;
	uint32_t present, width, height;
	uint32_t draws;
	/* its first draw's number in the recording's (marks.draw_owners'), and
	its views (marks.views') */
	uint32_t draw_first, view_first, view_count;
};

static struct
{
	const unsigned char *map;
	size_t map_size;
	/* the blocks' bytes, together: the events */
	unsigned char *body;
	size_t body_size;
	struct host_render_record_file header;
	struct record *records;
	uint32_t record_count;
	struct chunk *chunks;
	uint32_t chunk_count;
	/* the contents by hash (open table) */
	uint64_t *hashes;
	const unsigned char **contents;
	uint32_t content_mask;
	struct frame *frames;
	uint32_t frame_count;
	uint64_t content_bytes;
} recording;

static const unsigned char *content_find(uint64_t hash)
{
	uint32_t index;

	for (index = (uint32_t)hash & recording.content_mask; recording.hashes[index];
		index = (index + 1) & recording.content_mask)
	{
		if (recording.hashes[index] == hash)
			return recording.contents[index];
	}
	return NULL;
}

static void content_put(uint64_t hash, const unsigned char *data)
{
	uint32_t index;

	for (index = (uint32_t)hash & recording.content_mask; recording.hashes[index];
		index = (index + 1) & recording.content_mask)
	{
		if (recording.hashes[index] == hash)
			return;
	}
	recording.hashes[index] = hash;
	recording.contents[index] = data;
}

static int is_draw(uint32_t type)
{
	return type == XGPU_STREAM_DRAW || type == XGPU_STREAM_DRAW_INDICES || type == XGPU_STREAM_DRAW_INDEXED ||
		type == XGPU_STREAM_DRAW_IMMEDIATE;
}

/* ---------- debug.render_marks: what the recording's MARK records say of
its draws (xgpu_stream.h): the strings by their numbers, the owners in
their order, each draw's owner and the game renderer's category it was
drawn in (the CATEGORY records, which every recording has), the views */

static const char *const mark_kind_names[XGPU_STREAM_MARK_KINDS] =
{
	"other", "world_object", "first_person", "bsp", "sky", "decal", "effect", "particle", "contrail", "lightning",
	"weather", "water", "hud", "text", "menu_widget", "cinematic_letterbox", "debug", "detail_object", "lens_flare",
	"fog", "screen_effect", "light_volume", "shadow", "clear",
};

/* rasterizer.h's _rasterizer_profile_* */
static const char *const category_names[] =
{
	"clear", "model_sky", "models", "environment_lightmaps", "environment_shadows", "environment_diffuse_lights",
	"environment_decals_light", "environment_decals_alpha_tested", "environment_textures",
	"environment_decals_primary", "environment_decals_secondary", "environment_specular_lights",
	"environment_specular_lightmaps", "environment_reflection_lightmap_masks", "environment_reflection_mirrors",
	"environment_reflections", "environment_transparents", "environment_fog", "environment_fog_screen", "water",
	"environment_decals_water", "detail_objects", "queued_transparents", "lens_flare_occlusion_submit",
	"lens_flare_occlusion_query", "lens_flares", "screen_effect", "hud", "screen_flash",
};

#define CATEGORY_NAMES (sizeof(category_names) / sizeof(*category_names))

static struct
{
	/* the recording has MARK records */
	int present;
	char **strings;
	uint32_t string_capacity;
	struct xgpu_stream_mark_owner *owners;
	uint32_t owner_count, owner_capacity;
	struct xgpu_stream_mark_view *views;
	uint32_t view_count, view_capacity;
	/* each draw's owner (its index plus one, 0 none) and category (plus
	one, 0 none) */
	uint32_t *draw_owners;
	uint8_t *draw_categories;
} marks;

static int grow(void **array, uint32_t *capacity, uint32_t wanted, size_t size)
{
	uint32_t larger = *capacity ? *capacity : 64;
	void *grown;

	if (wanted < *capacity)
		return 1;
	while (larger <= wanted)
		larger *= 2;
	grown = realloc(*array, (size_t)larger * size);
	if (!grown)
		return 0;
	memset((unsigned char *)grown + (size_t)*capacity * size, 0, (size_t)(larger - *capacity) * size);
	*array = grown;
	*capacity = larger;
	return 1;
}

/* a string of the stream's, kept: bytes that are not UTF-8 as '?' (a
label cut in a character), control characters as spaces */
static char *mark_text(const unsigned char *from, uint32_t length)
{
	char *text = malloc((size_t)length + 1);
	uint32_t at = 0, to = 0;

	if (!text)
		return NULL;
	while (at < length)
	{
		unsigned char byte = from[at];
		uint32_t extra = byte < 0x80 ? 0 : (byte & 0xe0) == 0xc0 ? 1 : (byte & 0xf0) == 0xe0 ? 2 :
			(byte & 0xf8) == 0xf0 ? 3 : 4, index;
		int whole = extra < 4 && at + extra < length;

		for (index = 1; whole && index <= extra; index++)
			whole = (from[at + index] & 0xc0) == 0x80;
		if (!whole || (extra && byte < 0xc2))
		{
			text[to++] = '?';
			at++;
			continue;
		}
		if (!extra)
			text[to++] = byte < 0x20 ? ' ' : (char)byte;
		else
		{
			memcpy(text + to, from + at, extra + 1);
			to += extra + 1;
		}
		at += extra + 1;
	}
	text[to] = 0;
	return text;
}

/* (the frames' index) a MARK record */
static void mark_index(const struct record *record, struct frame *frame, uint32_t *owner)
{
	uint32_t mask = recording.header.ring.size - 1;

	marks.present = 1;
	if (record->type == XGPU_STREAM_MARK_STRING && record->bytes >= sizeof(struct xgpu_stream_mark_string))
	{
		struct xgpu_stream_mark_string string;
		uint32_t offset;

		memcpy(&string, record->data, sizeof(string));
		offset = string.data - (recording.header.ring.buffer + (record->position & mask));
		if (string.number && string.number < 0x1000000u && offset >= sizeof(string) && offset <= record->bytes &&
			string.length < record->bytes - offset &&
			grow((void **)&marks.strings, &marks.string_capacity, string.number, sizeof(*marks.strings)))
		{
			free(marks.strings[string.number]);
			marks.strings[string.number] = mark_text(record->data + offset, string.length);
		}
	}
	else if (record->type == XGPU_STREAM_MARK_VIEW && record->bytes >= sizeof(struct xgpu_stream_mark_view) &&
		grow((void **)&marks.views, &marks.view_capacity, marks.view_count, sizeof(*marks.views)))
	{
		memcpy(&marks.views[marks.view_count++], record->data, sizeof(*marks.views));
		frame->view_count++;
	}
	else if (record->type == XGPU_STREAM_MARK_OWNER && record->bytes >= sizeof(struct xgpu_stream_mark_owner) &&
		grow((void **)&marks.owners, &marks.owner_capacity, marks.owner_count, sizeof(*marks.owners)))
	{
		memcpy(&marks.owners[marks.owner_count++], record->data, sizeof(*marks.owners));
		*owner = marks.owner_count;
	}
}

static const char *mark_string(uint32_t number)
{
	return number && number < marks.string_capacity ? marks.strings[number] : NULL;
}

/* the frames: their records, what they present and its size (the
targets' sizes as they were made before it), their draws */
#define TARGETS_MOST 4096

static int frames_index(void)
{
	static uint32_t widths[TARGETS_MOST], heights[TARGETS_MOST];
	uint32_t index, frame = 0, first = 0, draws = 0, present = 0, all_draws = 0, drawn = 0, owner = 0, category = 0;
	struct frame next = { 0 };

	recording.frames = calloc(recording.frame_count ? recording.frame_count : 1, sizeof(*recording.frames));
	for (index = 0; index < recording.record_count; index++)
		all_draws += is_draw(recording.records[index].type);
	marks.draw_owners = calloc(all_draws ? all_draws : 1, sizeof(*marks.draw_owners));
	marks.draw_categories = calloc(all_draws ? all_draws : 1, 1);
	if (!recording.frames || !marks.draw_owners || !marks.draw_categories)
		return 0;
	for (index = 0; index < recording.record_count; index++)
	{
		const struct record *record = &recording.records[index];

		if (record->type == XGPU_STREAM_MARK_STRING || record->type == XGPU_STREAM_MARK_VIEW ||
			record->type == XGPU_STREAM_MARK_OWNER)
			mark_index(record, &next, &owner);
		else if (record->type == XGPU_STREAM_CATEGORY && record->bytes >= sizeof(struct xgpu_stream_value))
		{
			struct xgpu_stream_value value;

			memcpy(&value, record->data, sizeof(value));
			category = value.value < CATEGORY_NAMES ? value.value + 1 : 0;
		}
		else if (is_draw(record->type) && drawn < all_draws)
		{
			marks.draw_owners[drawn] = owner;
			marks.draw_categories[drawn++] = (uint8_t)category;
		}

		if (record->type == XGPU_STREAM_TARGET_CREATE && record->bytes >= sizeof(struct xgpu_stream_target_create))
		{
			struct xgpu_stream_target_create create;

			memcpy(&create, record->data, sizeof(create));
			if (create.target < TARGETS_MOST)
			{
				widths[create.target] = create.width;
				heights[create.target] = create.height;
			}
		}
		else if (record->type == XGPU_STREAM_PRESENT && record->bytes >= sizeof(struct xgpu_stream_value))
		{
			struct xgpu_stream_value value;

			memcpy(&value, record->data, sizeof(value));
			present = value.value;
		}
		draws += is_draw(record->type);
		if (record->type == XGPU_STREAM_FRAME_END)
		{
			struct frame *to = &recording.frames[frame++];

			*to = next;
			to->draw_first = drawn - draws;
			to->view_first = marks.view_count - next.view_count;
			next.view_count = 0;
			to->first = first;
			to->end = index + 1;
			to->present = present;
			to->width = present < TARGETS_MOST ? widths[present] : 0;
			to->height = present < TARGETS_MOST ? heights[present] : 0;
			to->draws = draws;
			first = index + 1;
			draws = 0;
			present = 0;
		}
	}
	return 1;
}

/* the blocks decompressed into the body, on threads of their own (each
block alone: its place in the body is the sizes before it) */
#define DECODE_THREADS 8

struct block_job
{
	const unsigned char *from;
	uint32_t stored, size;
	size_t at;
};

static struct
{
	struct block_job *jobs;
	uint32_t count;
	_Atomic uint32_t next;
	_Atomic int failed;
} decoding;

static void *blocks_decode_thread(void *unused)
{
	uint32_t index;

	(void)unused;
	while ((index = atomic_fetch_add(&decoding.next, 1)) < decoding.count)
	{
		const struct block_job *job = &decoding.jobs[index];

		if (job->stored == job->size)
			memcpy(recording.body + job->at, job->from, job->size);
		else if (host_render_replay_lz4_decode(recording.body + job->at, job->size, job->from, job->stored) != job->size)
			atomic_store(&decoding.failed, 1);
	}
	return NULL;
}

static int blocks_decode(const unsigned char *place, const unsigned char *map_end)
{
	pthread_t threads[DECODE_THREADS];
	uint32_t capacity = 0, index, started = 0;

	decoding.jobs = NULL;
	decoding.count = 0;
	while (place + sizeof(struct host_render_record_block) <= map_end)
	{
		struct host_render_record_block block;

		memcpy(&block, place, sizeof(block));
		if ((size_t)(map_end - place) - sizeof(block) < block.stored)
		{
			fprintf(stderr, "halo_replay: the recording is cut short (its last %zu bytes)\n", (size_t)(map_end - place));
			break;
		}
		if (!grow((void **)&decoding.jobs, &capacity, decoding.count, sizeof(*decoding.jobs)))
			return 0;
		decoding.jobs[decoding.count].from = place + sizeof(block);
		decoding.jobs[decoding.count].stored = block.stored;
		decoding.jobs[decoding.count].size = block.size;
		decoding.jobs[decoding.count].at = recording.body_size;
		decoding.count++;
		recording.body_size += block.size;
		place += sizeof(block) + block.stored;
	}
	atomic_store(&decoding.next, 0);
	atomic_store(&decoding.failed, 0);
	for (index = 0; index < DECODE_THREADS && index < decoding.count; index++)
		started += !pthread_create(&threads[started], NULL, blocks_decode_thread, NULL);
	/* (and this thread, which finishes them if none started) */
	blocks_decode_thread(NULL);
	for (index = 0; index < started; index++)
		pthread_join(threads[index], NULL);
	free(decoding.jobs);
	if (atomic_load(&decoding.failed))
	{
		fprintf(stderr, "halo_replay: a block does not decompress\n");
		return 0;
	}
	return 1;
}

static int recording_load(const char *path)
{
	int descriptor = open(path, O_RDONLY);
	struct stat status;
	const unsigned char *place, *end;
	uint32_t records = 0, chunks = 0, contents = 0, record_capacity, chunk_capacity, size;
	uint32_t pending_first = 0;

	if (descriptor < 0 || fstat(descriptor, &status) || (size_t)status.st_size < sizeof(recording.header))
	{
		fprintf(stderr, "halo_replay: cannot read %s\n", path);
		if (descriptor >= 0)
			close(descriptor);
		return 0;
	}
	recording.map_size = (size_t)status.st_size;
	recording.map = mmap(NULL, recording.map_size, PROT_READ, MAP_PRIVATE, descriptor, 0);
	close(descriptor);
	if (recording.map == MAP_FAILED)
		return 0;
	memcpy(&recording.header, recording.map, sizeof(recording.header));
	/* (a recording of 1: the stream's 21, the same records but the marks) */
	if (recording.header.magic != HOST_RENDER_RECORD_MAGIC || recording.header.version < 1 ||
		recording.header.version > HOST_RENDER_RECORD_VERSION || recording.header.header_size != sizeof(recording.header) ||
		recording.header.ring.version != XGPU_STREAM_VERSION - (HOST_RENDER_RECORD_VERSION - recording.header.version))
	{
		fprintf(stderr, "halo_replay: %s is not a recording of this version (%u, the stream's %u)\n", path,
			recording.header.version, recording.header.ring.version);
		return 0;
	}
	/* the blocks, decompressed */
	{
		const unsigned char *block_place = recording.map + sizeof(recording.header), *map_end = recording.map +
			recording.map_size;
		size_t total = 0;

		for (place = block_place; place + sizeof(struct host_render_record_block) <= map_end;)
		{
			struct host_render_record_block block;

			memcpy(&block, place, sizeof(block));
			if ((size_t)(map_end - place) - sizeof(block) < block.stored)
				break;
			total += block.size;
			place += sizeof(block) + block.stored;
		}
		recording.body = malloc(total ? total : 1);
		if (!recording.body || !blocks_decode(block_place, map_end))
			return 0;
	}
	/* (counted first, then indexed) */
	end = recording.body + recording.body_size;
	for (place = recording.body; place + sizeof(struct host_render_record_event) <= end;)
	{
		struct host_render_record_event event;

		memcpy(&event, place, sizeof(event));
		if ((size_t)(end - place) - sizeof(event) < event.size)
			break;
		records += event.type == HOST_RENDER_EVENT_RECORD;
		chunks += event.type == HOST_RENDER_EVENT_CHUNK;
		contents += event.type == HOST_RENDER_EVENT_CONTENT;
		place += sizeof(event) + event.size;
	}
	record_capacity = records ? records : 1;
	chunk_capacity = chunks ? chunks : 1;
	recording.records = calloc(record_capacity, sizeof(*recording.records));
	recording.chunks = calloc(chunk_capacity, sizeof(*recording.chunks));
	for (size = 16; size < contents * 2 + 2; size *= 2)
		;
	recording.content_mask = size - 1;
	recording.hashes = calloc(size, sizeof(*recording.hashes));
	recording.contents = calloc(size, sizeof(*recording.contents));
	if (!recording.records || !recording.chunks || !recording.hashes || !recording.contents)
		return 0;
	for (place = recording.body; place + sizeof(struct host_render_record_event) <= end;)
	{
		struct host_render_record_event event;
		const unsigned char *body = place + sizeof(event);

		memcpy(&event, place, sizeof(event));
		if ((size_t)(end - place) - sizeof(event) < event.size)
		{
			fprintf(stderr, "halo_replay: the recording is cut short (its last %zu bytes)\n", (size_t)(end - place));
			break;
		}
		place = body + event.size;
		if (event.type == HOST_RENDER_EVENT_CONTENT && event.size >= 8)
		{
			uint64_t hash;

			memcpy(&hash, body, 8);
			content_put(hash, body + 8);
			recording.content_bytes += event.size - 8;
		}
		else if (event.type == HOST_RENDER_EVENT_CHUNK && event.size >= sizeof(struct host_render_record_chunk))
		{
			struct host_render_record_chunk chunk;
			struct chunk *to = &recording.chunks[recording.chunk_count];

			memcpy(&chunk, body, sizeof(chunk));
			to->address = chunk.address;
			to->size = chunk.size;
			to->data = content_find((uint64_t)chunk.hash[0] | (uint64_t)chunk.hash[1] << 32);
			if (!to->data)
			{
				fprintf(stderr, "halo_replay: a chunk of %u bytes at %08x has no content\n", chunk.size, chunk.address);
				continue;
			}
			recording.chunk_count++;
		}
		else if (event.type == HOST_RENDER_EVENT_RECORD && event.size >= 8)
		{
			struct record *to = &recording.records[recording.record_count++];
			uint32_t header;

			memcpy(&to->position, body, 4);
			memcpy(&header, body + 4, 4);
			to->data = body + 4;
			to->bytes = event.size - 4;
			to->type = XGPU_STREAM_HEADER_TYPE(header);
			to->size = XGPU_STREAM_HEADER_SIZE(header);
			if (to->type == XGPU_STREAM_SKIP && !to->size)
				to->size = recording.header.ring.size - (to->position & (recording.header.ring.size - 1));
			to->chunk_first = pending_first;
			to->chunk_count = recording.chunk_count - pending_first;
			pending_first = recording.chunk_count;
			recording.frame_count += to->type == XGPU_STREAM_FRAME_END;
		}
	}
	return recording.record_count > 0 && frames_index();
}

/* ---------- the guest's memory */

static int memory_make(void)
{
	const struct xgpu_stream_ring *ring = &recording.header.ring;
	uint64_t pages;

	page_size = (size_t)sysconf(_SC_PAGESIZE);
	if (recording.header.page_size && recording.header.page_size != page_size)
		fprintf(stderr, "halo_replay: recorded with %u-byte pages, replayed with %zu\n", recording.header.page_size,
			page_size);
	pages = 0x100000000ull / page_size;
	pages_restored = calloc(pages, 1);
	page_generations = calloc(pages, sizeof(*page_generations));
	if (!pages_restored || !page_generations)
		return 0;
#ifdef HALO_GUEST_BASE
	{
		/* (4 GB-aligned, as host_memory_base.c places it) */
		uintptr_t reservation = (uintptr_t)mmap(NULL, 0x200000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE,
			-1, 0);

		if ((void *)reservation == MAP_FAILED)
			return 0;
		host_guest_base = (reservation + 0xffffffffull) & ~(uintptr_t)0xffffffffull;
		if (mmap((void *)host_guest_base, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED |
			MAP_NORESERVE, -1, 0) == MAP_FAILED)
			return 0;
	}
#else
	{
		/* the low 4 GB, as the host without a base has the guest's (from
		64 KB: the kernel keeps the lowest pages; nothing of the process may
		be there yet) */
		uintptr_t first = 0x10000;

		if (mmap((void *)first, 0x100000000ull - first, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0) != (void *)first)
		{
			fprintf(stderr, "halo_replay: the low 4 GB are not free (%s)\n", strerror(errno));
			return 0;
		}
	}
#endif
	/* the ring's header, its bytes and the host's answers, where they were */
	pages_mark(ring->buffer, ring->size, 0);
	pages_mark(ring->visibility, XGPU_STREAM_VISIBILITY_SLOTS * 2 * 4, 0);
	pages_mark(ring->pipelines, XGPU_STREAM_PIPELINES, 0);
	return 1;
}

/* ---------- the frame inspected: its picture, ID buffer and draws */

struct draw_seen
{
	uint32_t type, primitive, count;
	uint32_t pipeline, color, depth;
	struct
	{
		uint8_t source;
		uint32_t resource, target, format, width, height;
	} textures[4];
};

static struct
{
	/* the frame wanted (UINT32_MAX none), and what of it */
	uint32_t frame;
	int pixels_wanted, ids_wanted;
	/* (the render thread) what it gave */
	unsigned char *pixels;
	uint32_t width, height;
	uint32_t *ids;
	uint32_t ids_width, ids_height;
	struct draw_seen *draws;
	uint32_t draw_count, draw_capacity;
	int ids_active;
} inspect = { .frame = UINT32_MAX };

/* ---------- playing: the render thread's side */

struct frame_measure
{
	double interval_ms, cpu_ms, gpu_ms, compile_ms, shader_ms;
	uint32_t draws, compiles, shaders;
};

static const struct host_render_replay_platform *platform;

static struct
{
	/* the records played: the next expected (its index), and the offset of
	the play's positions from the recording's (a multiple of the ring) */
	uint32_t index, offset;
	/* the frame being played (its number in the recording) and its draws
	so far; the draw being handed to the backend (its number in the frame) */
	uint32_t frame, frame_draws, draw;
	uint32_t loop, loops;
	uint32_t first_frame, last_frame;
	double last_end;
	struct frame_measure *frames;
	const char *shots, *shot_frames;
	int window, unpaced, quiet, measuring;
	/* a hitch: an interval over this (and twice the median) */
	double hitch_ms;
	uint32_t mismatches;
	uint64_t restored_bytes;
	/* the backend's state the draws are handed with */
	uint32_t pipeline, color, depth;
	struct host_render_texture_stage stages[4];
	/* the backend's own table, and the replay's in front of it */
	const struct host_render_backend *inner;
	struct host_render_backend outer;
} play;

static double seconds_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec * 1.0e-9;
}

/* (the render thread) a frame's end has been played */
static void frame_closed(void)
{
	struct host_render_replay_statistics statistics;
	double now = seconds_now();
	uint32_t frame = play.frame;

	memset(&statistics, 0, sizeof(statistics));
	if (platform->frame_statistics)
		platform->frame_statistics(&statistics);
	if (play.measuring && frame >= play.first_frame && frame <= play.last_frame && play.loop < play.loops)
	{
		struct frame_measure *measure = &play.frames[(size_t)play.loop * (play.last_frame - play.first_frame + 1) +
			(frame - play.first_frame)];

		measure->interval_ms = (now - play.last_end) * 1000.0;
		measure->cpu_ms = statistics.cpu_ms;
		measure->gpu_ms = statistics.gpu_ms;
		measure->compile_ms = statistics.compile_ms;
		measure->draws = statistics.draws;
		measure->compiles = statistics.compiles;
		measure->shader_ms = statistics.shader_ms;
		measure->shaders = statistics.shaders;
	}
	play.last_end = now;
	play.frame++;
	play.frame_draws = 0;
	if (play.window && play.frame == play.first_frame && platform->set_unpaced)
		platform->set_unpaced(play.unpaced);
}

/* (the render thread) the recording begins again */
static void loop_begin(void)
{
	char directory[1100];

	play.frame = 0;
	play.frame_draws = 0;
	host_render_record_set_frame(0);
	if (play.window && platform->set_unpaced)
		platform->set_unpaced(play.first_frame > 0 ? 1 : play.unpaced);
	if (play.shots)
	{
		if (play.loops > 1)
		{
			mkdir(play.shots, 0755);
			snprintf(directory, sizeof(directory), "%s/loop%u", play.shots, play.loop);
		}
		else
		{
			snprintf(directory, sizeof(directory), "%s", play.shots);
		}
		host_render_record_shots(directory, play.shot_frames);
	}
	play.last_end = seconds_now();
}

/* (the render thread) the memory a record reads put back */
static void chunks_restore(const struct record *record)
{
	uint32_t c;

	for (c = 0; c < record->chunk_count; c++)
	{
		const struct chunk *chunk = &recording.chunks[record->chunk_first + c];
		void *to = host_pointer(chunk->address);

		if (memcmp(to, chunk->data, chunk->size))
		{
			memcpy(to, chunk->data, chunk->size);
			pages_mark(chunk->address, chunk->size, 1);
			play.restored_bytes += chunk->size;
		}
		else
		{
			pages_mark(chunk->address, chunk->size, 0);
		}
	}
}

/* (the render thread) before each record is played: the memory it reads
put back */
static void restore(uint32_t position, uint32_t type, const unsigned char *bytes)
{
	const struct record *record;

	(void)bytes;
	if (play.index >= recording.record_count ||
		(uint32_t)(recording.records[play.index].position + play.offset) != position)
	{
		/* (the tool's own skips, to a play's start) */
		if (type != XGPU_STREAM_SKIP && play.mismatches++ < 8)
			fprintf(stderr, "halo_replay: a record played at %u is not the recording's %u\n", position,
				play.index < recording.record_count ? recording.records[play.index].position + play.offset : 0);
		return;
	}
	if (play.index == 0)
		loop_begin();
	record = &recording.records[play.index++];
	/* (a record the play left out, on its way to a frame: a SKIP where it
	was, play_to) */
	if (type == XGPU_STREAM_SKIP && record->type != XGPU_STREAM_SKIP)
	{
		chunks_restore(record);
		if (is_draw(record->type))
			play.frame_draws++;
		return;
	}
	chunks_restore(record);
	/* (the frame inspected begins) */
	if (play.frame == inspect.frame && play.frame < recording.frame_count &&
		play.index - 1 == recording.frames[play.frame].first)
	{
		inspect.draw_count = 0;
		inspect.ids_active = inspect.ids_wanted && platform->ids_begin &&
			platform->ids_begin(recording.frames[play.frame].present, HOST_RENDER_REPLAY_ID_DEPTH);
	}
	if (is_draw(type))
		play.draw = play.frame_draws++;
}

/* ---------- the replay's table, in front of the backend's */

static void outer_set_targets(void *context, const struct xgpu_stream_set_targets *record)
{
	play.color = record->color;
	play.depth = record->depth;
	play.inner->set_targets(context, record);
}

static void outer_set_textures(void *context, const struct host_render_texture_stage stages[4])
{
	memcpy(play.stages, stages, sizeof(play.stages));
	play.inner->set_textures(context, stages);
}

static int outer_pipeline(void *context, uint32_t id, const struct xgpu_stream_pipeline_define *definition)
{
	play.pipeline = id;
	return play.inner->pipeline(context, id, definition);
}

/* (a draw of the frame inspected) what it was handed with */
static void draw_seen(uint32_t type, uint32_t primitive, uint32_t count)
{
	struct draw_seen *seen;
	int stage;

	if (play.frame != inspect.frame)
		return;
	if (inspect.ids_active)
		platform->ids_draw(play.draw);
	/* (numbered as the records are: a draw the decoder did not hand on
	leaves its number empty) */
	if (play.draw >= inspect.draw_capacity)
	{
		uint32_t capacity = inspect.draw_capacity ? inspect.draw_capacity * 2 : 1024;
		struct draw_seen *grown;

		while (capacity <= play.draw)
			capacity *= 2;
		grown = realloc(inspect.draws, capacity * sizeof(*grown));
		if (!grown)
			return;
		memset(grown + inspect.draw_capacity, 0, (capacity - inspect.draw_capacity) * sizeof(*grown));
		inspect.draws = grown;
		inspect.draw_capacity = capacity;
	}
	if (play.draw + 1 > inspect.draw_count)
		inspect.draw_count = play.draw + 1;
	seen = &inspect.draws[play.draw];
	seen->type = type;
	seen->primitive = primitive;
	seen->count = count;
	seen->pipeline = play.pipeline;
	seen->color = play.color;
	seen->depth = play.depth;
	for (stage = 0; stage < 4; stage++)
	{
		const struct xgpu_stream_texture_stage *from = &play.stages[stage].stage;

		seen->textures[stage].source = from->source;
		seen->textures[stage].resource = from->resource;
		seen->textures[stage].target = from->target;
		seen->textures[stage].format = from->resource_format;
		seen->textures[stage].width = from->width;
		seen->textures[stage].height = from->height;
	}
}

/* (the draw handed on) */
static void draw_done(void)
{
	if (inspect.ids_active && play.frame == inspect.frame)
		platform->ids_draw(UINT32_MAX);
}

static void outer_draw(void *context, const struct xgpu_stream_draw *record)
{
	draw_seen(XGPU_STREAM_DRAW, record->primitive, record->count);
	play.inner->draw(context, record);
	draw_done();
}

static void outer_draw_indices(void *context, const struct xgpu_stream_draw_indices *record)
{
	draw_seen(XGPU_STREAM_DRAW_INDICES, record->primitive, record->count);
	play.inner->draw_indices(context, record);
	draw_done();
}

static void outer_draw_indexed(void *context, const struct xgpu_stream_draw_indexed *record)
{
	draw_seen(XGPU_STREAM_DRAW_INDEXED, record->primitive, record->count);
	play.inner->draw_indexed(context, record);
	draw_done();
}

static void outer_draw_immediate(void *context, const struct xgpu_stream_draw_immediate *record,
	const float (*attributes)[4])
{
	draw_seen(XGPU_STREAM_DRAW_IMMEDIATE, record->primitive, record->index_count ? record->index_count : record->count);
	play.inner->draw_immediate(context, record, attributes);
	draw_done();
}

/* the pictures of the frames played on the way to one asked for (the
RECENT before it), kept so that a step back is not the recording played
again from its start; recent_from: the first of them wanted */
#define RECENT 16

static struct
{
	uint32_t frame;
	unsigned char *pixels;
	uint32_t width, height;
} recent[RECENT];
static uint32_t recent_from = UINT32_MAX;
/* (not while the timeline's thumbnails are taken) */
static int recent_keep = 1;

static void outer_present(void *context, uint32_t back_buffer)
{
	if (play.frame >= recent_from && play.frame < inspect.frame && inspect.frame != UINT32_MAX && platform->read_target)
	{
		uint32_t slot = play.frame % RECENT;

		free(recent[slot].pixels);
		recent[slot].frame = play.frame;
		recent[slot].pixels = platform->read_target(back_buffer, &recent[slot].width, &recent[slot].height);
	}
	if (play.frame == inspect.frame)
	{
		if (inspect.pixels_wanted && platform->read_target && !inspect.pixels)
			inspect.pixels = platform->read_target(back_buffer, &inspect.width, &inspect.height);
		if (inspect.ids_active)
		{
			inspect.ids = platform->ids_read(back_buffer, &inspect.ids_width, &inspect.ids_height);
			platform->ids_end();
			inspect.ids_active = 0;
		}
	}
	play.inner->present(context, back_buffer);
}

static void outer_frame_end(void *context)
{
	play.inner->frame_end(context);
	/* (a frame inspected that presented nothing) */
	if (inspect.ids_active && play.frame == inspect.frame)
	{
		platform->ids_end();
		inspect.ids_active = 0;
	}
	frame_closed();
}

static void outer_install(void)
{
	play.inner = host_render_stream_backend();
	play.outer = *play.inner;
	if (play.inner->set_targets)
		play.outer.set_targets = outer_set_targets;
	if (play.inner->set_textures)
		play.outer.set_textures = outer_set_textures;
	if (play.inner->pipeline)
		play.outer.pipeline = outer_pipeline;
	if (play.inner->draw)
		play.outer.draw = outer_draw;
	if (play.inner->draw_indices)
		play.outer.draw_indices = outer_draw_indices;
	if (play.inner->draw_indexed)
		play.outer.draw_indexed = outer_draw_indexed;
	if (play.inner->draw_immediate)
		play.outer.draw_immediate = outer_draw_immediate;
	if (play.inner->present)
		play.outer.present = outer_present;
	if (play.inner->frame_end)
		play.outer.frame_end = outer_frame_end;
	host_render_stream_set_backend(&play.outer);
}

/* ---------- playing: the guest's side */

static struct xgpu_stream_ring *ring;
/* the end of the records written so far (all of them may be played), and
the next record to write */
static uint32_t written;
static uint32_t next_record;

static void submit(uint32_t position, uint32_t flags)
{
	__atomic_store_n(&ring->committed, position, __ATOMIC_RELEASE);
	host_render_stream_submit(flags);
}

/* waits until the ring's bytes [position, position + size) are free (what
is written so far submitted: a wait for room that only records written and
not yet submitted would free never ends) */
static void room(uint32_t position, uint32_t size)
{
	while (position + size - __atomic_load_n(&ring->released, __ATOMIC_ACQUIRE) > ring->size)
	{
		submit(written, 0);
		host_render_stream_wait(position + size - ring->size);
	}
}

static void skip_write(uint32_t position, uint32_t size)
{
	uint32_t header = XGPU_STREAM_HEADER(XGPU_STREAM_SKIP, size);

	room(position, size ? size : ring->size - (position & (ring->size - 1)));
	memcpy((unsigned char *)host_pointer(ring->buffer) + (position & (ring->size - 1)), &header, 4);
	written = position + (size ? size : ring->size - (position & (ring->size - 1)));
}

static int synchronous(uint32_t type)
{
	return type == XGPU_STREAM_INITIALIZE || type == XGPU_STREAM_READ_TARGET || type == XGPU_STREAM_VR_SUPPORTED ||
		type == XGPU_STREAM_VR_INITIALIZE;
}

/* everything written played (the render thread is waiting for more) */
static void sync_all(void)
{
	submit(written, XGPU_STREAM_SUBMIT_SYNC);
}

/* what a play on its way to a frame leaves out of the frames well before
it (play_to's skip): the GPU's work of their draws, clears, anti-aliasing,
tone map, Panini pass and present, whose pictures nothing reads (the frames played whole before the one asked
for, SEEK_WHOLE_FRAMES, draw what it shows, the way the game had); the
state and the memory every record gives are kept (an immediate draw's
attributes: its vertices left out) */
#define SEEK_WHOLE_FRAMES 8

static int left_out(uint32_t type)
{
	return type == XGPU_STREAM_DRAW || type == XGPU_STREAM_DRAW_INDICES || type == XGPU_STREAM_DRAW_INDEXED ||
		type == XGPU_STREAM_CLEAR || type == XGPU_STREAM_CLEAR_TARGET || type == XGPU_STREAM_ANTI_ALIAS ||
		type == XGPU_STREAM_TONE_MAP || type == XGPU_STREAM_PANINI || type == XGPU_STREAM_PRESENT;
}

/* the records written to (not including) last; those before skip that
draw or clear left out (a SKIP in their place, which restore knows) */
static void play_to_skipping(uint32_t last, uint32_t skip)
{
	uint32_t mask = ring->size - 1;

	for (; next_record < last; next_record++)
	{
		const struct record *record = &recording.records[next_record];
		uint32_t at = record->position + play.offset;

		if (next_record < skip && left_out(record->type))
		{
			skip_write(at, record->size);
			continue;
		}
		room(at, record->size);
		memcpy((unsigned char *)host_pointer(ring->buffer) + (at & mask), record->data, record->bytes);
		if (next_record < skip && record->type == XGPU_STREAM_DRAW_IMMEDIATE &&
			record->bytes >= sizeof(struct xgpu_stream_draw_immediate))
		{
			struct xgpu_stream_draw_immediate *draw = (void *)((unsigned char *)host_pointer(ring->buffer) + (at & mask));

			draw->count = 0;
			draw->index_count = 0;
		}
		written = at + record->size;
		if (record->type == XGPU_STREAM_FRAME_END)
		{
			ring->frames_submitted++;
			submit(written, XGPU_STREAM_SUBMIT_FRAME);
		}
		else if (synchronous(record->type))
		{
			submit(written, XGPU_STREAM_SUBMIT_SYNC);
		}
	}
}

static void play_to(uint32_t last)
{
	play_to_skipping(last, 0);
}

/* the recording played again from its start: what was written played,
then skips to a position of the ring's where its first record was */
static void play_again(void)
{
	uint32_t mask = ring->size - 1, start;

	sync_all();
	/* (the next start: the first record's offset in the ring, past what
	was written) */
	start = recording.records[0].position + play.offset;
	start += ((written - start) + mask) & ~mask;
	while (written != start)
	{
		uint32_t offset = written & mask, gap = start - written;

		if (offset + gap > ring->size || gap > XGPU_STREAM_RECORD_MAXIMUM)
			skip_write(written, 0);
		else
			skip_write(written, gap);
	}
	sync_all();
	play.offset = start - recording.records[0].position;
	play.index = 0;
	next_record = 0;
}

/* the records of the frames measured, played play.loops times (done once
they have been) */
static _Atomic int played_all;

static void *play_loops(void *unused)
{
	uint32_t last = recording.frames[play.last_frame].end, loop;

	(void)unused;
	for (loop = 0; loop < play.loops; loop++)
	{
		if (loop)
			play_again();
		play.loop = loop;
		play_to(last);
		sync_all();
	}
	atomic_store(&played_all, 1);
	return NULL;
}

/* ---------- the report */

static int double_compare(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

static void report(uint32_t loop, double seconds)
{
	uint32_t count = play.last_frame - play.first_frame + 1, index, hitches = 0, compiles = 0, shaders = 0;
	double shader_ms = 0;
	struct frame_measure *frames = &play.frames[(size_t)loop * count];
	double *sorted = malloc(sizeof(double) * count), median, threshold, cpu = 0, gpu = 0, compile_ms = 0, sum = 0;
	char hitch_list[512] = "";
	size_t used = 0;

	if (!sorted)
		return;
	for (index = 0; index < count; index++)
		sorted[index] = frames[index].interval_ms;
	qsort(sorted, count, sizeof(double), double_compare);
	median = sorted[count / 2];
	threshold = median * 2.0 > play.hitch_ms ? median * 2.0 : play.hitch_ms;
	for (index = 0; index < count; index++)
	{
		const struct frame_measure *frame = &frames[index];
		/* (a shot's frame waits for its picture's PNG: not counted) */
		int shot = play.shots && host_render_record_shot_wanted(play.first_frame + index);
		int hitch = !shot && frame->interval_ms > threshold;

		if (!play.quiet)
			printf("frame %6u  %7.2f ms  cpu %6.2f  gpu %6.2f  draws %5u  pipelines %3u (%6.1f ms)  shaders %3u (%6.1f ms)%s\n",
				play.first_frame + index, frame->interval_ms, frame->cpu_ms, frame->gpu_ms, frame->draws, frame->compiles,
				frame->compile_ms, frame->shaders, frame->shader_ms, hitch ? "  HITCH" : shot ? "  (shot)" : "");
		if (hitch)
		{
			hitches++;
			if (used < sizeof(hitch_list) - 48)
				used += (size_t)snprintf(hitch_list + used, sizeof(hitch_list) - used, frame->shaders || frame->compiles ?
					" %u (%.1f ms; compiling %.1f)" : " %u (%.1f ms)", play.first_frame + index, frame->interval_ms,
					frame->shader_ms + frame->compile_ms);
		}
		cpu += frame->cpu_ms;
		gpu += frame->gpu_ms;
		compiles += frame->compiles;
		compile_ms += frame->compile_ms;
		shaders += frame->shaders;
		shader_ms += frame->shader_ms;
		sum += frame->interval_ms;
	}
	printf("loop %u: frames %u-%u (%u) in %.2f s (%.1f fps; the whole replay %.2f s): interval mean %.2f ms, median %.2f, "
		"p95 %.2f, p99 %.2f, max %.2f; render CPU %.2f ms a frame, GPU %.2f; pipelines made %u (%.1f ms), shaders "
		"compiled %u (%.1f ms); hitches %u (over %.2f ms)%s\n",
		loop, play.first_frame, play.last_frame, count, sum / 1000.0, count * 1000.0 / sum, seconds, sum / count, median,
		sorted[(size_t)(count * 0.95)], sorted[(size_t)(count * 0.99)], sorted[count - 1], cpu / count, gpu / count,
		compiles, compile_ms, shaders, shader_ms, hitches, threshold, hitches ? ":" : "");
	if (hitches)
		printf("  hitches:%s%s\n", hitch_list, used >= sizeof(hitch_list) - 48 ? " ..." : "");
	if (platform->report)
		platform->report();
	free(sorted);
}

/* ---------- pictures */

/* ---------- PNG files written

The service's pictures are written as they are, uncompressed (deflate's
stored blocks): a frame's PNG in a few milliseconds, where SDL's took 130
ms of a 1280x720 frame for a file two thirds as large, and the Inspector
waits for each. */

static uint32_t crc_table[8][256];

static uint32_t crc_update(uint32_t crc, const unsigned char *data, size_t size)
{
	size_t index = 0;

	if (!crc_table[0][1])
	{
		uint32_t value, bit, slice;

		for (value = 0; value < 256; value++)
		{
			uint32_t entry = value;

			for (bit = 0; bit < 8; bit++)
				entry = entry & 1 ? 0xedb88320u ^ (entry >> 1) : entry >> 1;
			crc_table[0][value] = entry;
		}
		for (value = 0; value < 256; value++)
			for (slice = 1; slice < 8; slice++)
				crc_table[slice][value] = (crc_table[slice - 1][value] >> 8) ^
					crc_table[0][crc_table[slice - 1][value] & 0xff];
	}
	/* (eight bytes at a time: the tables of a byte's CRC shifted on by
	one to seven more) */
	for (; index + 8 <= size; index += 8)
	{
		uint32_t low = crc ^ ((uint32_t)data[index] | (uint32_t)data[index + 1] << 8 | (uint32_t)data[index + 2] << 16 |
			(uint32_t)data[index + 3] << 24);

		crc = crc_table[7][low & 0xff] ^ crc_table[6][(low >> 8) & 0xff] ^ crc_table[5][(low >> 16) & 0xff] ^
			crc_table[4][low >> 24] ^ crc_table[3][data[index + 4]] ^ crc_table[2][data[index + 5]] ^
			crc_table[1][data[index + 6]] ^ crc_table[0][data[index + 7]];
	}
	for (; index < size; index++)
		crc = crc_table[0][(crc ^ data[index]) & 0xff] ^ (crc >> 8);
	return crc;
}

static void store32(unsigned char *to, uint32_t value)
{
	to[0] = (unsigned char)(value >> 24);
	to[1] = (unsigned char)(value >> 16);
	to[2] = (unsigned char)(value >> 8);
	to[3] = (unsigned char)value;
}

/* a chunk's length, type and data at to (its CRC after them); past it */
static unsigned char *chunk_close(unsigned char *start, unsigned char *end)
{
	uint32_t length = (uint32_t)(end - start - 8);

	store32(start, length);
	store32(end, crc_update(0xffffffffu, start + 4, length + 4) ^ 0xffffffffu);
	return end + 4;
}

/* BGRA pixels (rows from the top) as an RGB PNG's bytes, malloc'd */
static unsigned char *png_encode(const unsigned char *bgra, uint32_t width, uint32_t height, size_t *size)
{
	size_t row_bytes = (size_t)width * 3 + 1, raw = row_bytes * height, blocks = (raw + 65534) / 65535;
	size_t capacity = 8 + 25 + 12 + 2 + raw + blocks * 5 + 4 + 12;
	unsigned char *png = malloc(capacity), *place, *chunk;
	uint32_t adler_a = 1, adler_b = 0, row, column;
	size_t left = raw, in_block = 0;

	if (!png || !width || !height)
	{
		free(png);
		return NULL;
	}
	memcpy(png, "\x89PNG\r\n\x1a\n", 8);
	chunk = png + 8;
	memcpy(chunk + 4, "IHDR", 4);
	store32(chunk + 8, width);
	store32(chunk + 12, height);
	/* (8 bits a channel, RGB, deflate, no filter method's choice, no interlace) */
	memcpy(chunk + 16, "\x08\x02\x00\x00\x00", 5);
	place = chunk_close(chunk, chunk + 21);
	chunk = place;
	memcpy(chunk + 4, "IDAT", 4);
	place = chunk + 8;
	*place++ = 0x78;
	*place++ = 0x01;
	for (row = 0; row < height; row++)
	{
		const unsigned char *from = bgra + (size_t)row * width * 4;
		unsigned char line[3 * 16384 + 1];
		size_t at, length = row_bytes;

		/* (filter none, then the pixels as RGB) */
		line[0] = 0;
		for (column = 0; column < width && column < 16384; column++)
		{
			line[1 + column * 3] = from[column * 4 + 2];
			line[2 + column * 3] = from[column * 4 + 1];
			line[3 + column * 3] = from[column * 4];
		}
		/* (Adler-32 reduced once each 5552 bytes, zlib's NMAX: no sum
		overflows before) */
		for (at = 0; at < length;)
		{
			size_t run = length - at < 5552 ? length - at : 5552, end = at + run;

			for (; at < end; at++)
			{
				adler_a += line[at];
				adler_b += adler_a;
			}
			adler_a %= 65521;
			adler_b %= 65521;
		}
		for (at = 0; at < length;)
		{
			size_t part;

			if (!in_block)
			{
				uint32_t block = left > 65535 ? 65535 : (uint32_t)left;

				*place++ = left <= 65535;
				*place++ = (unsigned char)block;
				*place++ = (unsigned char)(block >> 8);
				*place++ = (unsigned char)~block;
				*place++ = (unsigned char)(~block >> 8);
				in_block = block;
			}
			part = length - at < in_block ? length - at : in_block;
			memcpy(place, line + at, part);
			place += part;
			at += part;
			in_block -= part;
			left -= part;
		}
	}
	store32(place, adler_b << 16 | adler_a);
	place = chunk_close(chunk, place + 4);
	memcpy(place + 4, "IEND", 4);
	place = chunk_close(place, place + 8);
	*size = (size_t)(place - png);
	return png;
}

/* BGRA pixels as a PNG file */
static int png_write(const unsigned char *bgra, uint32_t width, uint32_t height, const char *path)
{
	size_t size;
	unsigned char *bytes = png_encode(bgra, width, height, &size);
	FILE *file;
	int written = 0;

	if (!bytes)
		return 0;
	file = fopen(path, "wb");
	if (file)
	{
		written = fwrite(bytes, 1, size, file) == size;
		written = fclose(file) == 0 && written;
	}
	free(bytes);
	return written;
}

/* BGRA pixels made width wide (each of its pixels the mean of the
pixels it covers), malloc'd; *height made the picture's at that width */
static unsigned char *picture_scale(const unsigned char *bgra, uint32_t from_width, uint32_t from_height,
	uint32_t width, uint32_t *height)
{
	unsigned char *scaled;
	uint32_t x, y;

	*height = (uint32_t)((double)from_height * width / (from_width ? from_width : 1) + 0.5);
	if (!*height)
		*height = 1;
	scaled = malloc((size_t)width * *height * 4);
	if (!scaled)
		return NULL;
	for (y = 0; y < *height; y++)
	{
		uint32_t top = (uint32_t)((uint64_t)y * from_height / *height);
		uint32_t bottom = (uint32_t)((uint64_t)(y + 1) * from_height / *height);

		if (bottom <= top)
			bottom = top + 1;
		for (x = 0; x < width; x++)
		{
			uint32_t left = (uint32_t)((uint64_t)x * from_width / width);
			uint32_t right = (uint32_t)((uint64_t)(x + 1) * from_width / width), row, column, channel;
			uint32_t sums[4] = { 0, 0, 0, 0 }, count;

			if (right <= left)
				right = left + 1;
			count = (bottom - top) * (right - left);
			for (row = top; row < bottom && row < from_height; row++)
			{
				for (column = left; column < right && column < from_width; column++)
				{
					for (channel = 0; channel < 4; channel++)
						sums[channel] += bgra[((size_t)row * from_width + column) * 4 + channel];
				}
			}
			for (channel = 0; channel < 4; channel++)
				scaled[((size_t)y * width + x) * 4 + channel] = (unsigned char)((sums[channel] + count / 2) / count);
		}
	}
	return scaled;
}

#ifndef HALO_REPLAY_NO_SDL
/* a PNG's pixels, BGRA */
static unsigned char *png_read(const char *path, size_t *width, size_t *height)
{
	SDL_Surface *loaded = SDL_LoadPNG(path), *converted;
	unsigned char *pixels = NULL;

	if (!loaded)
		return NULL;
	converted = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_BGRA32);
	SDL_DestroySurface(loaded);
	if (!converted)
		return NULL;
	*width = (size_t)converted->w;
	*height = (size_t)converted->h;
	pixels = malloc(*width * *height * 4);
	if (pixels)
	{
		size_t row;

		for (row = 0; row < *height; row++)
			memcpy(pixels + row * *width * 4, (const unsigned char *)converted->pixels + row * (size_t)converted->pitch,
				*width * 4);
	}
	SDL_DestroySurface(converted);
	return pixels;
}

/* (for --compare's listing) */
static int name_compare(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static SDL_EnumerationResult SDLCALL name_add(void *userdata, const char *directory, const char *name)
{
	struct
	{
		char **names;
		int count, capacity;
	} *list = userdata;

	(void)directory;
	if (strlen(name) < 4 || strcmp(name + strlen(name) - 4, ".png"))
		return SDL_ENUM_CONTINUE;
	if (list->count == list->capacity)
	{
		int capacity = list->capacity ? list->capacity * 2 : 64;
		char **grown = realloc(list->names, (size_t)capacity * sizeof(*grown));

		if (!grown)
			return SDL_ENUM_FAILURE;
		list->names = grown;
		list->capacity = capacity;
	}
	list->names[list->count++] = strdup(name);
	return SDL_ENUM_CONTINUE;
}

static int compare(const char *first, const char *second)
{
	struct
	{
		char **names;
		int count, capacity;
	} list = { NULL, 0, 0 };
	int compared = 0, differ = 0, index;

	SDL_EnumerateDirectory(first, name_add, &list);
	if (list.count)
		qsort(list.names, (size_t)list.count, sizeof(*list.names), name_compare);
	for (index = 0; index < list.count; index++)
	{
		const char *name = list.names[index];
		char path_a[4200], path_b[4200];
		size_t width_a = 0, height_a = 0, width_b = 0, height_b = 0, pixel, different = 0;
		unsigned char *a, *b;
		int most = 0;

		snprintf(path_a, sizeof(path_a), "%s/%s", first, name);
		snprintf(path_b, sizeof(path_b), "%s/%s", second, name);
		a = png_read(path_a, &width_a, &height_a);
		b = png_read(path_b, &width_b, &height_b);
		compared++;
		if (!a || !b || width_a != width_b || height_a != height_b)
		{
			printf("%s: %s\n", name, !b ? "missing in the second" : !a ? "unreadable" : "of another size");
			differ++;
		}
		else
		{
			for (pixel = 0; pixel < width_a * height_a; pixel++)
			{
				int channel, pixel_differs = 0;

				for (channel = 0; channel < 3; channel++)
				{
					int delta = abs((int)a[pixel * 4 + channel] - (int)b[pixel * 4 + channel]);

					if (delta)
						pixel_differs = 1;
					if (delta > most)
						most = delta;
				}
				different += pixel_differs;
			}
			printf("%s: %s (%zu of %zu pixels differ, by at most %d)\n", name, different ? "DIFFERENT" : "identical",
				different, width_a * height_a, most);
			differ += different != 0;
		}
		free(a);
		free(b);
		free(list.names[index]);
	}
	free(list.names);
	printf("%d compared, %d differ\n", compared, differ);
	return compared && !differ ? 0 : 1;
}
#else
/* (the tests' build, without SDL: no PNG read) */
static int compare(const char *first, const char *second)
{
	(void)first;
	(void)second;
	fprintf(stderr, "halo_replay: --compare is not built in\n");
	return 2;
}
#endif

/* ---------- the service: JSON */

struct text
{
	char *data;
	size_t used, capacity;
};

static void text_add(struct text *text, const char *data, size_t size)
{
	if (text->used + size + 1 > text->capacity)
	{
		size_t capacity = text->capacity ? text->capacity : 4096;
		char *grown;

		while (capacity < text->used + size + 1)
			capacity *= 2;
		grown = realloc(text->data, capacity);
		if (!grown)
			return;
		text->data = grown;
		text->capacity = capacity;
	}
	memcpy(text->data + text->used, data, size);
	text->used += size;
	text->data[text->used] = 0;
}

/* (a string literal's bytes) */
#define TEXT(text, literal) text_add(text, literal, sizeof(literal) - 1)

static void text_printf(struct text *text, const char *format, ...) __attribute__((format(printf, 2, 3)));

static void text_printf(struct text *text, const char *format, ...)
{
	char small[512];
	va_list arguments;
	int length;

	va_start(arguments, format);
	length = vsnprintf(small, sizeof(small), format, arguments);
	va_end(arguments);
	if (length < 0)
		return;
	if ((size_t)length < sizeof(small))
	{
		text_add(text, small, (size_t)length);
		return;
	}
	{
		char *large = malloc((size_t)length + 1);

		if (!large)
			return;
		va_start(arguments, format);
		vsnprintf(large, (size_t)length + 1, format, arguments);
		va_end(arguments);
		text_add(text, large, (size_t)length);
		free(large);
	}
}

static void text_string(struct text *text, const char *string)
{
	const unsigned char *place;

	TEXT(text, "\"");
	for (place = (const unsigned char *)string; *place; place++)
	{
		if (*place == '"' || *place == '\\')
		{
			char escaped[2] = { '\\', (char)*place };

			text_add(text, escaped, 2);
		}
		else if (*place < 0x20)
			text_printf(text, "\\u%04x", *place);
		else
			text_add(text, (const char *)place, 1);
	}
	TEXT(text, "\"");
}

/* a request's values: its keys at the top level (numbers, strings,
booleans; an array of numbers) */
#define REQUEST_VALUES 16
#define REQUEST_NUMBERS 4096

struct request
{
	int count;
	struct
	{
		char key[32];
		char string[4096];
		double number;
		int kind; /* 's' string, 'n' number, 'b' boolean, 'a' numbers */
	} values[REQUEST_VALUES];
	double numbers[REQUEST_NUMBERS];
	int number_count;
};

static const char *skip_space(const char *place)
{
	while (*place == ' ' || *place == '\t' || *place == '\r' || *place == '\n')
		place++;
	return place;
}

/* a JSON string at place (after its quote) into to; past it, or NULL */
static const char *string_parse(const char *place, char *to, size_t size)
{
	size_t used = 0;

	while (*place && *place != '"')
	{
		char character = *place++;

		if (character == '\\')
		{
			character = *place++;
			if (character == 'n')
				character = '\n';
			else if (character == 't')
				character = '\t';
			else if (character == 'u')
			{
				/* (\u of ASCII alone; others as '?') */
				unsigned value = 0;
				int digit;

				for (digit = 0; digit < 4 && *place; digit++, place++)
					value = value * 16 + (unsigned)(*place <= '9' ? *place - '0' : (*place | 32) - 'a' + 10);
				character = value < 0x80 ? (char)value : '?';
			}
			else if (!character)
				return NULL;
		}
		if (used + 1 < size)
			to[used++] = character;
	}
	to[used] = 0;
	return *place == '"' ? place + 1 : NULL;
}

static int request_parse(const char *line, struct request *request)
{
	const char *place = skip_space(line);

	memset(request, 0, offsetof(struct request, numbers));
	request->number_count = 0;
	if (*place++ != '{')
		return 0;
	for (;;)
	{
		char key[32];
		int slot;

		place = skip_space(place);
		if (*place == '}')
			return 1;
		if (*place++ != '"' || !(place = string_parse(place, key, sizeof(key))))
			return 0;
		place = skip_space(place);
		if (*place++ != ':')
			return 0;
		place = skip_space(place);
		slot = request->count < REQUEST_VALUES ? request->count++ : REQUEST_VALUES - 1;
		snprintf(request->values[slot].key, sizeof(request->values[slot].key), "%s", key);
		if (*place == '"')
		{
			request->values[slot].kind = 's';
			if (!(place = string_parse(place + 1, request->values[slot].string, sizeof(request->values[slot].string))))
				return 0;
		}
		else if (!strncmp(place, "true", 4) || !strncmp(place, "false", 5))
		{
			request->values[slot].kind = 'b';
			request->values[slot].number = *place == 't';
			place += *place == 't' ? 4 : 5;
		}
		else if (!strncmp(place, "null", 4))
		{
			request->values[slot].kind = 0;
			place += 4;
		}
		else if (*place == '[')
		{
			request->values[slot].kind = 'a';
			place = skip_space(place + 1);
			while (*place && *place != ']')
			{
				char *end;
				double value = strtod(place, &end);

				if (end == place)
					return 0;
				if (request->number_count < REQUEST_NUMBERS)
					request->numbers[request->number_count++] = value;
				place = skip_space(end);
				if (*place == ',')
					place = skip_space(place + 1);
			}
			if (*place++ != ']')
				return 0;
		}
		else
		{
			char *end;

			request->values[slot].kind = 'n';
			request->values[slot].number = strtod(place, &end);
			if (end == place)
				return 0;
			place = end;
		}
		place = skip_space(place);
		if (*place == ',')
			place++;
		else if (*place != '}')
			return 0;
	}
}

static int request_find(const struct request *request, const char *key)
{
	int index;

	for (index = 0; index < request->count; index++)
	{
		if (!strcmp(request->values[index].key, key) && request->values[index].kind)
			return index;
	}
	return -1;
}

static const char *request_string(const struct request *request, const char *key)
{
	int index = request_find(request, key);

	return index >= 0 && request->values[index].kind == 's' ? request->values[index].string : NULL;
}

static int request_number(const struct request *request, const char *key, double *value)
{
	int index = request_find(request, key);

	if (index < 0 || (request->values[index].kind != 'n' && request->values[index].kind != 'b'))
		return 0;
	*value = request->values[index].number;
	return 1;
}

static int request_true(const struct request *request, const char *key)
{
	double value;

	return request_number(request, key, &value) && value != 0.0;
}

/* a PNG's bytes inline in a reply: ,"png_base64":"..." */
static void png_inline(struct text *text, const unsigned char *bgra, uint32_t width, uint32_t height)
{
	size_t size;
	unsigned char *bytes = png_encode(bgra, width, height, &size);
	char *encoded = bytes ? host_render_replay_base64(bytes, size) : NULL;

	if (encoded)
	{
		TEXT(text, ",\"png_base64\":\"");
		text_add(text, encoded, strlen(encoded));
		TEXT(text, "\"");
	}
	free(encoded);
	free(bytes);
}

/* ---------- the service: frames */

/* the frame last inspected, kept: its picture, ID buffer and draws */
static struct
{
	uint32_t frame;
	unsigned char *pixels;
	uint32_t width, height;
	uint32_t *ids;
	uint32_t ids_width, ids_height;
	struct draw_seen *draws;
	uint32_t draw_count;
	int draws_seen;
	double ms;
} kept = { .frame = UINT32_MAX };

static void kept_forget(void)
{
	free(kept.pixels);
	free(kept.ids);
	free(kept.draws);
	memset(&kept, 0, sizeof(kept));
	kept.frame = UINT32_MAX;
}

/* (HALO_REPLAY_SEEK_SKIPS=0: every frame on the way played whole) */
static int seek_skips = 1;

/* frame n played exactly, its picture read (and its ID buffer with ids):
into kept, with its draws unless it is a step back to a picture kept
(draws: they are wanted too); NULL, or the reason it was not */
static const char *frame_play(uint32_t n, int ids, int draws)
{
	double started = seconds_now();
	uint32_t slot = n % RECENT;

	if (n >= recording.frame_count)
		return "no such frame";
	if (kept.frame == n && kept.pixels && (!ids || kept.ids) && (!draws || kept.draws_seen))
		return NULL;
	if (ids && !platform->ids_begin)
		return "this backend has no ID buffer";
	kept_forget();
	if (!ids && !draws && recent[slot].pixels && recent[slot].frame == n)
	{
		kept.frame = n;
		kept.pixels = malloc((size_t)recent[slot].width * recent[slot].height * 4);
		if (!kept.pixels)
			return "out of memory";
		memcpy(kept.pixels, recent[slot].pixels, (size_t)recent[slot].width * recent[slot].height * 4);
		kept.width = recent[slot].width;
		kept.height = recent[slot].height;
		kept.ms = (seconds_now() - started) * 1000.0;
		return NULL;
	}
	/* (the frame is played again: from the start for one played already) */
	if (play.frame > n || (play.frame == n && next_record > recording.frames[n].first))
	{
		uint32_t index;

		play_again();
		for (index = 0; index < RECENT; index++)
			recent[index].frame = UINT32_MAX;
	}
	recent_from = !recent_keep ? UINT32_MAX : n >= RECENT ? n - (RECENT - 1) : 0;
	inspect.frame = n;
	{
		uint32_t whole = n > SEEK_WHOLE_FRAMES ? n - SEEK_WHOLE_FRAMES : 0;

		/* (the pictures kept only of frames drawn whole) */
		if (recent_from != UINT32_MAX && recent_from < whole && next_record < recording.frames[whole].first)
			recent_from = whole;
		play_to_skipping(recording.frames[n].first, seek_skips ? recording.frames[whole].first : 0);
	}
	inspect.pixels_wanted = 1;
	inspect.ids_wanted = ids;
	play_to(recording.frames[n].end);
	sync_all();
	inspect.frame = UINT32_MAX;
	recent_from = UINT32_MAX;
	kept.frame = n;
	kept.pixels = inspect.pixels;
	kept.width = inspect.width;
	kept.height = inspect.height;
	kept.ids = inspect.ids;
	kept.ids_width = inspect.ids_width;
	kept.ids_height = inspect.ids_height;
	kept.draws = inspect.draws;
	kept.draw_count = inspect.draw_count;
	kept.draws_seen = 1;
	inspect.pixels = NULL;
	inspect.ids = NULL;
	inspect.draws = NULL;
	inspect.draw_count = inspect.draw_capacity = 0;
	kept.ms = (seconds_now() - started) * 1000.0;
	if (!kept.pixels)
		return "the frame's picture was not read";
	if (ids && !kept.ids)
		return "the frame's ID buffer was not read";
	return NULL;
}

static int file_write(const char *path, const void *data, size_t size)
{
	FILE *file = fopen(path, "wb");
	int done;

	if (!file)
		return 0;
	done = fwrite(data, 1, size, file) == size;
	return fclose(file) == 0 && done;
}

static const char *primitive_name(uint32_t primitive)
{
	static const char *names[] = { "points", "lines", "line_loop", "line_strip", "triangles", "triangle_strip",
		"triangle_fan" };

	return primitive < sizeof(names) / sizeof(*names) ? names[primitive] : "unknown";
}

static uint32_t primitives_of(uint32_t primitive, uint32_t count)
{
	switch (primitive)
	{
	case 0: return count;
	case 1: return count / 2;
	case 2: return count;
	case 3: return count > 1 ? count - 1 : 0;
	case 4: return count / 3;
	default: return count > 2 ? count - 2 : 0;
	}
}

static const char *draw_type_name(uint32_t type)
{
	return type == XGPU_STREAM_DRAW ? "draw" : type == XGPU_STREAM_DRAW_INDICES ? "draw_indices" :
		type == XGPU_STREAM_DRAW_INDEXED ? "draw_indexed" : type == XGPU_STREAM_DRAW_IMMEDIATE ? "draw_immediate" :
		"none";
}

/* what the marks say of a draw of the frame kept (its kind, label, id,
tags, a menu widget's tag and generator source, the category): without
them, the record's kind and primitive as its label */
static void draw_marks_json(struct text *text, uint32_t index)
{
	const struct frame *frame = &recording.frames[kept.frame];
	const struct draw_seen *draw = index < kept.draw_count ? &kept.draws[index] : NULL;
	uint32_t global = frame->draw_first + index, owner_number = 0, category = 0;
	const struct xgpu_stream_mark_owner *owner = NULL;
	const char *label = NULL, *source;

	if (index < frame->draws)
	{
		owner_number = marks.draw_owners[global];
		category = marks.draw_categories[global];
	}
	if (owner_number)
		owner = &marks.owners[owner_number - 1];
	TEXT(text, ",\"kind\":");
	text_string(text, owner && owner->kind < XGPU_STREAM_MARK_KINDS ? mark_kind_names[owner->kind] : "other");
	TEXT(text, ",\"label\":");
	if (owner && (label = mark_string(owner->label)))
		text_string(text, label);
	else if (draw && draw->type)
		text_printf(text, "\"%s %s\"", draw_type_name(draw->type), primitive_name(draw->primitive));
	else
		TEXT(text, "\"(not drawn)\"");
	if (owner && owner->id != XGPU_STREAM_MARK_NONE)
		text_printf(text, ",\"id\":%u", owner->id);
	else
		TEXT(text, ",\"id\":null");
	TEXT(text, ",\"tags\":{");
	if (owner)
	{
		static const char *const names[4] = { "object", "model", "shader", "bitmap" };
		const uint32_t numbers[4] = { owner->object, owner->model, owner->shader, owner->bitmap };
		int tag, first = 1;

		for (tag = 0; tag < 4; tag++)
		{
			const char *path = mark_string(numbers[tag]);

			if (!path)
				continue;
			text_printf(text, "%s\"%s\":", first ? "" : ",", names[tag]);
			text_string(text, path);
			first = 0;
		}
	}
	TEXT(text, "}");
	if (owner && mark_string(owner->widget))
	{
		TEXT(text, ",\"widget\":");
		text_string(text, mark_string(owner->widget));
	}
	/* (tools/menu_sources.py's source attribute: its file, and its text
	whole, which menu_sources.locate reads) */
	if (owner && (source = mark_string(owner->source)))
	{
		const char *space = strchr(source, ' ');
		char file[512];

		snprintf(file, sizeof(file), "%.*s", space ? (int)(space - source) : (int)strlen(source), source);
		TEXT(text, ",\"source\":{\"file\":");
		text_string(text, file);
		TEXT(text, ",\"text\":");
		text_string(text, source);
		TEXT(text, "}");
	}
	else
		TEXT(text, ",\"source\":null");
	TEXT(text, ",\"category\":");
	if (category)
		text_string(text, category_names[category - 1]);
	else
		TEXT(text, "null");
}

/* a view mark's fields */
static void view_json(struct text *text, const struct xgpu_stream_mark_view *view)
{
	const char *map = mark_string(view->map);

	TEXT(text, "\"map\":");
	if (map)
		text_string(text, map);
	else
		TEXT(text, "null");
	text_printf(text, ",\"tick\":%u,\"window\":", view->tick);
	if (view->window == XGPU_STREAM_MARK_NONE)
		TEXT(text, "null");
	else
		text_printf(text, "%u", view->window);
	text_printf(text, ",\"camera\":{\"position\":[%.9g,%.9g,%.9g],\"forward\":[%.9g,%.9g,%.9g],"
		"\"up\":[%.9g,%.9g,%.9g],\"fov\":%.9g},\"viewport\":[%d,%d,%d,%d]", view->position[0], view->position[1],
		view->position[2], view->forward[0], view->forward[1], view->forward[2], view->up[0], view->up[1], view->up[2],
		view->field_of_view, view->viewport[0], view->viewport[1], view->viewport[2], view->viewport[3]);
}

/* the frame kept's marks: its first window's view (or the frame's start's,
for the map and the tick), and every view */
static void frame_marks_json(struct text *text)
{
	const struct frame *frame = &recording.frames[kept.frame];
	const struct xgpu_stream_mark_view *main_view = NULL;
	uint32_t index;

	for (index = 0; index < frame->view_count; index++)
	{
		const struct xgpu_stream_mark_view *view = &marks.views[frame->view_first + index];

		if (!main_view || (main_view->window == XGPU_STREAM_MARK_NONE && view->window != XGPU_STREAM_MARK_NONE))
			main_view = view;
	}
	if (!main_view)
		return;
	TEXT(text, ",");
	view_json(text, main_view);
	TEXT(text, ",\"views\":[");
	for (index = 0; index < frame->view_count; index++)
	{
		if (index)
			TEXT(text, ",");
		TEXT(text, "{");
		view_json(text, &marks.views[frame->view_first + index]);
		TEXT(text, "}");
	}
	TEXT(text, "]");
}

/* a draw of the frame kept, as the service gives it */
static void draw_json(struct text *text, uint32_t index)
{
	const struct draw_seen *draw = index < kept.draw_count ? &kept.draws[index] : NULL;
	int stage, first = 1;

	text_printf(text, "{\"i\":%u", index);
	if (!draw || !draw->type)
	{
		draw_marks_json(text, index);
		TEXT(text, ",\"textures\":[]}");
		return;
	}
	draw_marks_json(text, index);
	text_printf(text, ",\"record\":\"%s\",\"primitive\":\"%s\",\"count\":%u,\"prims\":%u,\"pipeline\":\"%u\","
		"\"target\":%u,\"depth_target\":%u", draw_type_name(draw->type), primitive_name(draw->primitive), draw->count,
		primitives_of(draw->primitive, draw->count), draw->pipeline, draw->color, draw->depth);
	TEXT(text, ",\"textures\":[");
	for (stage = 0; stage < 4; stage++)
	{
		const char *source;

		if (!draw->textures[stage].source)
			continue;
		source = draw->textures[stage].source == XGPU_STREAM_SOURCE_IMAGE ? "image" :
			draw->textures[stage].target ? "target" : "texture";
		text_printf(text, "%s{\"stage\":%d,\"source\":\"%s\",\"resource\":\"%08x\",\"target\":%u,\"format\":\"%08x\"}",
			first ? "" : ",", stage, source, draw->textures[stage].resource, draw->textures[stage].target,
			draw->textures[stage].format);
		first = 0;
	}
	TEXT(text, "]}");
}

static void reply_error(struct text *text, const char *error)
{
	text->used = 0;
	TEXT(text, "{\"ok\":false,\"error\":");
	text_string(text, error);
	TEXT(text, "}");
}

static void serve_info(struct text *text)
{
	const struct frame *last = recording.frame_count ? &recording.frames[recording.frame_count - 1] : NULL;

	text_printf(text, "{\"ok\":true,\"frames\":%u,\"width\":%u,\"height\":%u,\"backend\":\"%s\",\"build\":\"%s\","
		"\"marks\":%s,\"records\":%u}", recording.frame_count, last ? last->width : 0, last ? last->height : 0,
		platform->backend, HALO_REPLAY_BUILD, marks.present ? "true" : "false", recording.record_count);
}

static void serve_frame(struct text *text, const struct request *request)
{
	double number;
	const char *png = request_string(request, "png"), *ids_path = request_string(request, "ids"), *error;
	int inline_reply = request_true(request, "inline"), ids = ids_path != NULL || request_true(request, "ids");
	uint32_t n;

	if (!request_number(request, "n", &number) || number < 0)
	{
		reply_error(text, "no frame (n)");
		return;
	}
	n = (uint32_t)number;
	if ((error = frame_play(n, ids, 0)))
	{
		reply_error(text, error);
		return;
	}
	text_printf(text, "{\"ok\":true,\"n\":%u,\"ms\":%.1f,\"width\":%u,\"height\":%u", n, kept.ms, kept.width,
		kept.height);
	if (png && !inline_reply)
	{
		if (!png_write(kept.pixels, kept.width, kept.height, png))
		{
			reply_error(text, "the PNG cannot be written");
			return;
		}
	}
	else if (inline_reply)
	{
		png_inline(text, kept.pixels, kept.width, kept.height);
	}
	if (ids)
	{
		/* (the topmost of each pixel's) */
		size_t pixels = (size_t)kept.ids_width * kept.ids_height, pixel;
		uint32_t *top = malloc(pixels * 4 + 1);

		if (!top)
		{
			reply_error(text, "out of memory");
			return;
		}
		for (pixel = 0; pixel < pixels; pixel++)
			top[pixel] = kept.ids[pixel * HOST_RENDER_REPLAY_ID_DEPTH];
		if (kept.ids_width != kept.width || kept.ids_height != kept.height)
			fprintf(stderr, "halo_replay: frame %u's ID buffer is %ux%u, its picture %ux%u\n", n, kept.ids_width,
				kept.ids_height, kept.width, kept.height);
		if (ids_path && !inline_reply)
		{
			if (!file_write(ids_path, top, pixels * 4))
			{
				free(top);
				reply_error(text, "the ID buffer cannot be written");
				return;
			}
		}
		else
		{
			char *encoded = host_render_replay_base64((const unsigned char *)top, pixels * 4);

			if (encoded)
			{
				TEXT(text, ",\"ids_base64\":\"");
				text_add(text, encoded, strlen(encoded));
				TEXT(text, "\"");
			}
			free(encoded);
		}
		free(top);
	}
	TEXT(text, "}");
}

static void serve_draws(struct text *text, const struct request *request)
{
	double number;
	const char *error;
	uint32_t index;

	if (!request_number(request, "n", &number) || number < 0)
	{
		reply_error(text, "no frame (n)");
		return;
	}
	if ((error = frame_play((uint32_t)number, 0, 1)))
	{
		reply_error(text, error);
		return;
	}
	text_printf(text, "{\"ok\":true,\"n\":%u,\"frame\":{\"n\":%u,\"present\":%u", kept.frame, kept.frame,
		recording.frames[kept.frame].present);
	frame_marks_json(text);
	TEXT(text, "},\"draws\":[");
	for (index = 0; index < kept.draw_count; index++)
	{
		if (index)
			TEXT(text, ",");
		draw_json(text, index);
	}
	TEXT(text, "]}");
}

static void serve_pick(struct text *text, const struct request *request)
{
	double number, x, y;
	const char *error;
	const uint32_t *stack;
	int depth;

	if (!request_number(request, "n", &number) || number < 0 || !request_number(request, "x", &x) ||
		!request_number(request, "y", &y))
	{
		reply_error(text, "no frame and pixel (n, x, y)");
		return;
	}
	if ((error = frame_play((uint32_t)number, 1, 1)))
	{
		reply_error(text, error);
		return;
	}
	if (x < 0 || y < 0 || x >= kept.ids_width || y >= kept.ids_height)
	{
		reply_error(text, "the pixel is outside the frame");
		return;
	}
	stack = &kept.ids[((size_t)y * kept.ids_width + (size_t)x) * HOST_RENDER_REPLAY_ID_DEPTH];
	text_printf(text, "{\"ok\":true,\"n\":%u,\"x\":%d,\"y\":%d,\"stack\":[", kept.frame, (int)x, (int)y);
	for (depth = 0; depth < HOST_RENDER_REPLAY_ID_DEPTH && stack[depth]; depth++)
	{
		if (depth)
			TEXT(text, ",");
		draw_json(text, stack[depth] - 1);
	}
	TEXT(text, "]}");
}

static void serve_thumbs(struct text *text, const struct request *request)
{
	const char *directory = request_string(request, "dir");
	int inline_reply = request_true(request, "inline") || !directory, index, count, first = 1;
	double every = 30, width = 160;
	uint32_t *wanted;

	request_number(request, "every", &every);
	request_number(request, "width", &width);
	if (every < 1)
		every = 1;
	if (width < 8)
		width = 8;
	if (request_find(request, "frames") >= 0)
		count = request->number_count;
	else
		count = (int)((recording.frame_count + (uint32_t)every - 1) / (uint32_t)every);
	wanted = malloc(sizeof(*wanted) * (size_t)(count ? count : 1));
	if (!wanted)
	{
		reply_error(text, "out of memory");
		return;
	}
	for (index = 0; index < count; index++)
		wanted[index] = request_find(request, "frames") >= 0 ? (uint32_t)request->numbers[index] :
			(uint32_t)index * (uint32_t)every;
	if (directory)
		mkdir(directory, 0755);
	TEXT(text, "{\"ok\":true,\"thumbs\":[");
	recent_keep = 0;
	for (index = 0; index < count; index++)
	{
		uint32_t n = wanted[index], height;
		unsigned char *scaled;

		if (n >= recording.frame_count || frame_play(n, 0, 0) ||
			!(scaled = picture_scale(kept.pixels, kept.width, kept.height, (uint32_t)width, &height)))
			continue;
		text_printf(text, "%s{\"n\":%u", first ? "" : ",", n);
		first = 0;
		if (!inline_reply)
		{
			char path[4200];

			snprintf(path, sizeof(path), "%s/thumb_%u.png", directory, n);
			if (png_write(scaled, (uint32_t)width, height, path))
			{
				TEXT(text, ",\"png\":");
				text_string(text, path);
			}
		}
		else
		{
			png_inline(text, scaled, (uint32_t)width, height);
		}
		TEXT(text, "}");
		free(scaled);
	}
	recent_keep = 1;
	TEXT(text, "]}");
	free(wanted);
}

static int serve(void)
{
	char *line = NULL;
	size_t capacity = 0;
	struct text reply = { NULL, 0, 0 };
	struct request *request = malloc(sizeof(*request));

	if (!request)
		return 2;
	if (getenv("HALO_REPLAY_SEEK_SKIPS") && *getenv("HALO_REPLAY_SEEK_SKIPS"))
		seek_skips = atoi(getenv("HALO_REPLAY_SEEK_SKIPS")) != 0;
	fprintf(stderr, "halo_replay: serving (%u frames)\n", recording.frame_count);
	while (getline(&line, &capacity, stdin) > 0)
	{
		const char *command;

		reply.used = 0;
		if (*skip_space(line) == 0)
			continue;
		if (!request_parse(line, request) || !(command = request_string(request, "cmd")))
			reply_error(&reply, "not a request");
		else if (!strcmp(command, "info"))
			serve_info(&reply);
		else if (!strcmp(command, "frame"))
			serve_frame(&reply, request);
		else if (!strcmp(command, "draws"))
			serve_draws(&reply, request);
		else if (!strcmp(command, "pick"))
			serve_pick(&reply, request);
		else if (!strcmp(command, "thumbs"))
			serve_thumbs(&reply, request);
		else if (!strcmp(command, "quit"))
			break;
		else
			reply_error(&reply, "unknown command");
		if (ring->error)
		{
			fprintf(stderr, "halo_replay: the stream stopped (error %u)\n", ring->error);
			reply_error(&reply, "the stream stopped");
		}
		text_add(&reply, "\n", 1);
		fwrite(reply.data, 1, reply.used, stdout);
		fflush(stdout);
		if (ring->error)
			break;
	}
	free(line);
	free(reply.data);
	free(request);
	return 0;
}

/* ---------- main */

static void usage(void)
{
	fprintf(stderr, "usage: halo_replay <file> [--frames a-b] [--window] [--unpaced] [--shots dir] [--shot-frames list]\n"
		"\t[--loop N] [--hitch ms] [--quiet] [--verbose]\n"
		"       halo_replay <file> --serve\n"
		"       halo_replay --compare <shots> <shots>\n");
}

int host_render_replay_main(int argc, char *argv[], const struct host_render_replay_platform *given)
{
	const char *path = NULL, *frames = NULL;
	char shot_default[64];
	int index, serving = 0, status = 0;
	double started;

	platform = given;
	if (argc == 4 && !strcmp(argv[1], "--compare"))
		return compare(argv[2], argv[3]);
	play.loops = 1;
	/* (1.5 refreshes at 120 Hz: a frame that misses one) */
	play.hitch_ms = 12.5;
	for (index = 1; index < argc; index++)
	{
		if (!strcmp(argv[index], "--frames") && index + 1 < argc)
			frames = argv[++index];
		else if (!strcmp(argv[index], "--window") && platform->window_supported)
			play.window = 1;
		else if (!strcmp(argv[index], "--unpaced"))
			play.unpaced = 1;
		else if (!strcmp(argv[index], "--shots") && index + 1 < argc)
			play.shots = argv[++index];
		else if (!strcmp(argv[index], "--shot-frames") && index + 1 < argc)
			play.shot_frames = argv[++index];
		else if (!strcmp(argv[index], "--loop") && index + 1 < argc)
			play.loops = (uint32_t)atoi(argv[++index]);
		else if (!strcmp(argv[index], "--hitch") && index + 1 < argc)
			play.hitch_ms = atof(argv[++index]);
		else if (!strcmp(argv[index], "--quiet"))
			play.quiet = 1;
		else if (!strcmp(argv[index], "--verbose"))
			host_render_replay_verbose = 1;
		else if (!strcmp(argv[index], "--serve"))
			serving = 1;
		else if (argv[index][0] != '-' && !path)
			path = argv[index];
		else
		{
			usage();
			return 2;
		}
	}
	if (!path || play.loops < 1 || (serving && (play.window || play.shots || frames || play.loops > 1)))
	{
		usage();
		return 2;
	}
	started = seconds_now();
	if (!recording_load(path) || !recording.frame_count)
	{
		fprintf(stderr, "halo_replay: no frames in %s\n", path);
		return 2;
	}
	fprintf(serving ? stderr : stdout, "%s: %u records, %u frames, %u chunks, %.1f MB of contents (read in %.2f s)\n",
		path, recording.record_count, recording.frame_count, recording.chunk_count, recording.content_bytes / 1048576.0,
		seconds_now() - started);
	play.first_frame = 0;
	play.last_frame = recording.frame_count - 1;
	if (frames)
	{
		char *end;

		play.first_frame = (uint32_t)strtoul(frames, &end, 10);
		play.last_frame = *end == '-' ? (uint32_t)strtoul(end + 1, NULL, 10) : play.first_frame;
	}
	if (play.last_frame >= recording.frame_count)
		play.last_frame = recording.frame_count - 1;
	if (play.first_frame > play.last_frame)
	{
		fprintf(stderr, "halo_replay: no frames %s (the recording has %u)\n", frames, recording.frame_count);
		return 2;
	}
	if (play.shots && !play.shot_frames)
	{
		snprintf(shot_default, sizeof(shot_default), "%u-%u", play.first_frame, play.last_frame);
		play.shot_frames = shot_default;
	}
	play.measuring = !serving;
	play.frames = calloc((size_t)play.loops * (play.last_frame - play.first_frame + 1), sizeof(*play.frames));
	if (!play.frames || !memory_make())
	{
		fprintf(stderr, "halo_replay: cannot reserve the guest's memory\n");
		return 2;
	}
	{
		/* (the size of the first target presented: the back buffer's) */
		uint32_t frame, width = 1280, height = 720;

		for (frame = 0; frame < recording.frame_count; frame++)
		{
			if (recording.frames[frame].width >= 320)
			{
				width = recording.frames[frame].width;
				height = recording.frames[frame].height;
				break;
			}
		}
		if (!platform->initialize(play.window, (int)width, (int)height))
		{
			fprintf(stderr, "halo_replay: no %s device\n", platform->backend);
			return 2;
		}
	}
	outer_install();
	host_render_record_set_restorer(restore);
	/* (the Screenshot key's pictures are the game's, not the replay's) */
	host_render_stream_set_screenshot_writer(NULL);

	/* the ring's header, as it was when the recording began */
	ring = host_pointer(recording.header.ring_address);
	pages_mark(recording.header.ring_address, sizeof(*ring), 0);
	*ring = recording.header.ring;
	/* (a recording of the stream's 21 is a stream of 22 without marks) */
	ring->version = XGPU_STREAM_VERSION;
	ring->committed = ring->released = written = recording.records[0].position;
	ring->error = 0;
	if (host_render_stream_open(recording.header.ring_address))
	{
		fprintf(stderr, "halo_replay: the stream did not open\n");
		return 2;
	}
	started = seconds_now();
	if (serving)
	{
		status = serve();
	}
	else
	{
		/* (the guest's side on a thread of its own: the main thread keeps
		the window's events going) */
		pthread_t thread;

		if (pthread_create(&thread, NULL, play_loops, NULL))
			return 2;
		if (play.window && platform->pump)
		{
			struct timespec pause = { 0, 2000000 };

			while (!atomic_load(&played_all))
			{
				platform->pump();
				nanosleep(&pause, NULL);
			}
		}
		pthread_join(thread, NULL);
	}
	host_render_stream_close();
	if (!serving)
	{
		double seconds = seconds_now() - started;
		uint32_t loop;

		for (loop = 0; loop < play.loops; loop++)
			report(loop, seconds);
		printf("replayed %u records x %u in %.2f s; %.1f MB of memory put back; %u records out of place\n",
			recording.frames[play.last_frame].end, play.loops, seconds, play.restored_bytes / 1048576.0, play.mismatches);
	}
	if (platform->shutdown)
		platform->shutdown();
	return status;
}
