/*
XGPU_VULKAN_TEXTURES.C

The Vulkan backend's cache of the Xbox's textures and the high-res
replacements' images (xgpu_vulkan_textures.h), as the OpenGL backend's
texture cache (port/linux/src/xbox_textures.c) and the Metal backend's
(port/macos/xgpu_metal_textures.c) have them, with the same decoding
(xbox_texture_decode.c, built into the host beside a stand-in for the
platform header: xgpu_vulkan_platform.h).

An Xbox texture is found by its Data, Format and Size words (and a
palettized one's palette, and the channels a Custom Edition map's texels are
sampled from); its texels are read from the guest's memory where they are.
DXT textures are uploaded as they are (BC1 to BC3), as OpenGL's are, where
the device has them, a volume's decoded; every other format is decoded to
BGRA. The pages it was read from are write-protected (the write tracking,
host_memory_watch.c), and the texture is read again once any of them has
been written: the check is a comparison of the tracking's serial while no
page has been, and a look at the texture's pages' generations otherwise. The
host reads a frame after the game drew it: a texture whose memory the game
has let go of since (a map's, as the next loads) is not read (its pages are
no longer the guest's).

An image (a high-res replacement: port/linux/src/xgpu_images.h) is found by
its number and made again when its serial changes; its levels are made from
its first.
*/

#include "xgpu.h"
#include "xgpu_backend.h"
#include "xgpu_stream.h"
#include "host.h"
#include "xgpu_vulkan.h"
#include "xgpu_vulkan_textures.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define BUCKET_COUNT 4096
/* frames a texture is kept without being drawn, and how often they are
looked for */
#define IDLE_FRAMES 1800
#define EVICT_EVERY 600
/* the copies of one texture for its palettes (the game cycles some through
a few) */
#define PALETTE_VARIANTS 8
/* D3DFMT_P8 */
#define FORMAT_PALETTIZED 0x0b

/* the Xbox's contiguous memory, where the textures are */
#define CONTIGUOUS_BASE HALO_GUEST_WINDOW_BASE
#define CONTIGUOUS_SIZE HALO_GUEST_WINDOW_SIZE

enum
{
	_entry_xbox,
	_entry_image,
};

struct entry
{
	struct entry *next;
	uint8_t kind;
	/* an Xbox texture's words, its palette's hash and its channels; an
	image's number (data) and serial (format_word) */
	uint32_t data, format_word, size_word, palette_hash, channels;
	struct xgpu_vulkan_image *image;
	struct xgpu_texture_description description;
	/* (an Xbox texture's) the range read, and its pages' newest generation
	as of the tracking's serial watched_serial */
	uint32_t address, size;
	uint32_t generation, watched_serial;
	uint64_t last_used;
	/* its DXT texels decoded (the device has no BC formats) */
	int decoded;
};

static struct
{
	struct entry *buckets[BUCKET_COUNT];
	uint64_t frame;
	/* the decoded texels of the largest level so far */
	DWORD *converted;
	size_t converted_size;
	struct xgpu_vulkan_texture_statistics statistics;
} cache;

static uint32_t bucket_index(uint32_t data, uint32_t format_word, uint32_t size_word)
{
	uint32_t hash = data * 2654435761u ^ format_word * 2246822519u ^ size_word * 3266489917u;

	return (hash >> 20) % BUCKET_COUNT;
}

static uint32_t palette_hash(const uint32_t *palette)
{
	uint32_t hash = 2166136261u;
	int index;

	if (!palette)
		return 0;
	for (index = 0; index < 256; index++)
		hash = (hash ^ palette[index]) * 16777619u;
	return hash ? hash : 1;
}

static DWORD *converted_get(size_t texels)
{
	if (texels > cache.converted_size)
	{
		DWORD *grown = realloc(cache.converted, texels * sizeof(DWORD));

		if (!grown)
			return NULL;
		cache.converted = grown;
		cache.converted_size = texels;
	}
	return cache.converted;
}

static uint32_t levels_most(unsigned long width, unsigned long height, unsigned long depth)
{
	unsigned long largest = width > height ? width : height;
	uint32_t levels = 1;

	if (depth > largest)
		largest = depth;
	while (largest > 1)
	{
		largest >>= 1;
		levels++;
	}
	return levels;
}

/* ---------- Xbox textures */

/* the texture's image, made for its description */
static struct xgpu_vulkan_image *xbox_texture_make(struct entry *entry)
{
	const struct xgpu_texture_description *description = &entry->description;
	struct xgpu_vulkan_image_description made;
	struct xgpu_vulkan_capabilities capabilities;
	uint32_t most = levels_most(description->width, description->height, description->depth);

	xgpu_vulkan_capabilities(&capabilities);
	memset(&made, 0, sizeof(made));
	made.type = description->cube_map ? XGPU_VULKAN_IMAGE_CUBE : description->depth > 1 ? XGPU_VULKAN_IMAGE_3D :
		XGPU_VULKAN_IMAGE_2D;
	/* (DXT as it is, but for a volume's, which is decoded, and on a device
	without BC formats) */
	entry->decoded = description->compressed && (description->depth > 1 || !capabilities.block_compression);
	if (description->compressed && !entry->decoded)
		made.format = description->format == 0x0c ? XGPU_VULKAN_FORMAT_DXT1 : description->format == 0x0e ?
			XGPU_VULKAN_FORMAT_DXT3 : XGPU_VULKAN_FORMAT_DXT5;
	else
		made.format = XGPU_VULKAN_FORMAT_BGRA8;
	made.usage = XGPU_VULKAN_USAGE_SAMPLED;
	made.width = (uint32_t)description->width;
	made.height = (uint32_t)description->height;
	made.depth_or_layers = (uint32_t)(description->depth > 1 ? description->depth : 1);
	made.levels = (uint32_t)(description->levels < most ? description->levels : most);
	if (!made.levels)
		made.levels = 1;
	made.channels = entry->channels;
	return xgpu_vulkan_image_create(&made);
}

/* reads the texture's texels into it */
static void xbox_texture_upload(struct entry *entry, const D3DCOLOR *palette)
{
	const struct xgpu_texture_description *description = &entry->description;
	const unsigned char *base = host_pointer(entry->address);
	unsigned long faces = description->cube_map ? 6 : 1, face_size = xgpu_texture_face_size(description);
	unsigned long levels = xgpu_vulkan_image_description(entry->image)->levels, face, level;
	int blocks = description->compressed && !entry->decoded;

	for (face = 0; face < faces; face++)
	{
		for (level = 0; level < levels; level++)
		{
			const unsigned char *source = base + face * face_size + xgpu_texture_level_offset(description, level);
			uint32_t width = (uint32_t)xgpu_texture_level_dimension(description->width, level);
			uint32_t height = (uint32_t)xgpu_texture_level_dimension(description->height, level);
			uint32_t depth = (uint32_t)xgpu_texture_level_dimension(description->depth, level);
			DWORD *converted;

			if (blocks)
			{
				uint32_t row = (uint32_t)xgpu_texture_level_pitch(description, level);

				xgpu_vulkan_image_upload(entry->image, (uint32_t)level, (uint32_t)face, 0, 0, 0, width, height, 1, source, row,
					0);
				continue;
			}
			converted = converted_get((size_t)width * height * depth);
			if (!converted)
				return;
			if (description->compressed)
				xgpu_texture_decode_dxt(description, level, source, converted);
			else if (!xgpu_texture_decode_level(description, level, source, palette, converted))
				return;
			xgpu_vulkan_image_upload(entry->image, (uint32_t)level, (uint32_t)face, 0, 0, 0, width, height, depth, converted,
				width * 4, width * height * 4);
		}
	}
	cache.statistics.bytes += entry->size;
}

static struct entry *xbox_entry_find(const struct xgpu_stream_texture_stage *stage, const D3DCOLOR *palette)
{
	const uint32_t *header = host_pointer(stage->resource);
	uint32_t data, format_word = stage->resource_format, size_word = stage->resource_size;
	BOOL palettized = ((format_word & D3DFORMAT_FORMAT_MASK) >> D3DFORMAT_FORMAT_SHIFT) == FORMAT_PALETTIZED;
	uint32_t hash = palettized ? palette_hash(palette) : 0;
	struct entry **bucket, *entry, *oldest = NULL;
	unsigned long variants = 0;

	if (!header || !header[1])
		return NULL;
	data = header[1];
	bucket = &cache.buckets[bucket_index(data, format_word, size_word)];
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->kind != _entry_xbox || entry->data != data || entry->format_word != format_word ||
			entry->size_word != size_word || entry->channels != stage->target)
			continue;
		if (entry->palette_hash == hash)
			return entry;
		variants++;
		if (!oldest || entry->last_used < oldest->last_used)
			oldest = entry;
	}
	if (variants >= PALETTE_VARIANTS)
	{
		/* (a palette that keeps changing: the stalest copy is read again) */
		oldest->palette_hash = hash;
		oldest->generation = 0;
		return oldest;
	}
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return NULL;
	entry->kind = _entry_xbox;
	entry->data = data;
	entry->format_word = format_word;
	entry->size_word = size_word;
	entry->palette_hash = hash;
	entry->channels = stage->target;
	xgpu_texture_describe(format_word, size_word, &entry->description);
	entry->address = CONTIGUOUS_BASE | data;
	/* (one larger than the device takes is never drawn, nor one outside the
	contiguous memory: as the OpenGL cache) */
	if (xgpu_texture_size_supported(&entry->description))
	{
		uint64_t size = (uint64_t)xgpu_texture_face_size(&entry->description) * (entry->description.cube_map ? 6 : 1);

		if (size && entry->address >= CONTIGUOUS_BASE && (uint64_t)entry->address + size <=
			(uint64_t)CONTIGUOUS_BASE + CONTIGUOUS_SIZE)
			entry->size = (uint32_t)size;
	}
	if (entry->size)
		entry->image = xbox_texture_make(entry);
	if (!entry->size || !entry->image)
		host_logf(HOST_LOG_WARN, "vulkan: a %lux%lux%lu texture of format %02x at %08x is not drawn",
			entry->description.width, entry->description.height, entry->description.depth,
			(unsigned)entry->description.format, data);
	entry->next = *bucket;
	*bucket = entry;
	cache.statistics.made++;
	return entry;
}

/* the guest's pages of a range are mapped and readable (looked at on whole
host pages, the extent's limits: no further). Without the base register
(x86-64's guest, the low 4 GB) the memory layer keeps no such account: the
Xbox's contiguous window is mapped whole, and other ranges are mapped if
mincore finds their pages. */
static BOOL readable(uint32_t address, uint32_t size)
{
	uint64_t page = host_page_size();
	uint64_t first = address & ~(page - 1), last = ((uint64_t)address + size + page - 1) & ~(page - 1);

	if (!address || last > 0xffffffffu)
		return FALSE;
#ifdef HALO_GUEST_BASE
	{
		uint32_t start, end;

		return host_memory_readable_extent(address, size, (uint32_t)first, (uint32_t)last, &start, &end) != 0;
	}
#else
	if ((uint64_t)address >= CONTIGUOUS_BASE && (uint64_t)address + size <= (uint64_t)CONTIGUOUS_BASE + CONTIGUOUS_SIZE)
		return TRUE;
	{
		unsigned char residency[64];
		uint64_t at;

		/* (ENOMEM for a page that is not mapped) */
		for (at = first; at < last; at += page * sizeof(residency))
		{
			uint64_t length = last - at < page * sizeof(residency) ? last - at : page * sizeof(residency);

			if (mincore(host_pointer((uint32_t)at), (size_t)length, residency) != 0)
				return FALSE;
		}
		return TRUE;
	}
#endif
}

static struct xgpu_vulkan_image *xbox_texture_get(const struct xgpu_stream_texture_stage *stage)
{
	const D3DCOLOR *palette = readable(stage->palette, 256 * sizeof(D3DCOLOR)) ? host_pointer(stage->palette) : NULL;
	struct entry *entry;
	uint32_t serial, generation;

	if (!readable(stage->resource, 5 * sizeof(uint32_t)))
		return NULL;
	entry = xbox_entry_find(stage, palette);
	if (!entry || !entry->image)
		return NULL;
	entry->last_used = cache.frame;
	serial = host_memory_watch_serial();
	if (entry->generation && entry->watched_serial == serial)
		return entry->image;
	generation = host_memory_watch_generation(entry->address, entry->size);
	if ((!entry->generation || generation > entry->generation) && readable(entry->address, entry->size))
	{
		if (entry->generation)
			cache.statistics.remade++;
		/* protect first, so a write racing with the reading is noticed */
		host_memory_watch_protect(entry->address, entry->size);
		entry->generation = host_memory_watch_generation(entry->address, entry->size);
		if (!entry->generation)
			entry->generation = 1;
		xbox_texture_upload(entry, palette);
	}
	entry->watched_serial = serial;
	return entry->image;
}

/* ---------- images */

/* XGPU_IMAGE_* (xgpu_images.h) */
enum
{
	_image_rgba,
	_image_coverage,
};

static struct xgpu_vulkan_image *image_get(const struct xgpu_stream_texture_stage *stage)
{
	struct entry **bucket = &cache.buckets[bucket_index(stage->resource, 0, 1)];
	struct entry *entry;
	const void *pixels = host_pointer(stage->data);
	uint32_t width = stage->width, height = stage->height;
	uint32_t texel = stage->resource_format == _image_coverage ? 1 : 4;

	if (!pixels || !width || !height || width > 16384 || height > 16384 || !readable(stage->data, width * height * texel))
		return NULL;
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->kind == _entry_image && entry->data == stage->resource)
			break;
	}
	if (!entry)
	{
		entry = calloc(1, sizeof(*entry));
		if (!entry)
			return NULL;
		entry->kind = _entry_image;
		entry->data = stage->resource;
		entry->next = *bucket;
		*bucket = entry;
	}
	entry->last_used = cache.frame;
	if (entry->image && entry->format_word == stage->resource_size)
		return entry->image;
	/* (its pixels changed, or it is another image of the number's) */
	if (!entry->image || entry->description.width != width || entry->description.height != height ||
		entry->description.levels != stage->levels || entry->size_word != stage->resource_format)
	{
		struct xgpu_vulkan_image_description made;
		uint32_t most = levels_most(width, height, 1);

		if (entry->image)
			xgpu_vulkan_image_destroy(entry->image);
		memset(&made, 0, sizeof(made));
		made.type = XGPU_VULKAN_IMAGE_2D;
		made.format = stage->resource_format == _image_coverage ? XGPU_VULKAN_FORMAT_COVERAGE : XGPU_VULKAN_FORMAT_RGBA8;
		made.usage = XGPU_VULKAN_USAGE_SAMPLED;
		made.width = width;
		made.height = height;
		made.depth_or_layers = 1;
		made.levels = stage->levels > 1 ? (stage->levels < most ? stage->levels : most) : 1;
		entry->image = xgpu_vulkan_image_create(&made);
		entry->description.width = width;
		entry->description.height = height;
		entry->description.levels = stage->levels;
		entry->size_word = stage->resource_format;
		if (!entry->image)
			return NULL;
	}
	entry->format_word = stage->resource_size;
	xgpu_vulkan_image_upload(entry->image, 0, 0, 0, 0, 0, width, height, 1, pixels, width * texel, 0);
	if (stage->levels > 1)
		xgpu_vulkan_image_make_levels(entry->image, 1);
	cache.statistics.images++;
	cache.statistics.bytes += (uint64_t)width * height * texel;
	return entry->image;
}

/* ---------- the cache */

struct xgpu_vulkan_image *xgpu_vulkan_textures_get(const struct xgpu_stream_texture_stage *stage)
{
	if (stage->source == XGPU_STREAM_SOURCE_IMAGE)
		return image_get(stage);
	return stage->resource ? xbox_texture_get(stage) : NULL;
}

struct xgpu_vulkan_image *xgpu_vulkan_textures_header(uint32_t header)
{
	struct xgpu_stream_texture_stage stage;
	const uint32_t *words;

	if (!readable(header, 5 * sizeof(uint32_t)))
		return NULL;
	words = host_pointer(header);
	memset(&stage, 0, sizeof(stage));
	stage.source = XGPU_SOURCE_TEXTURE;
	stage.resource = header;
	stage.resource_format = words[3];
	stage.resource_size = words[4];
	return xbox_texture_get(&stage);
}

void xgpu_vulkan_textures_frame_end(void)
{
	unsigned long index;

	cache.frame++;
	if (cache.frame % EVICT_EVERY)
		return;
	for (index = 0; index < BUCKET_COUNT; index++)
	{
		struct entry **link = &cache.buckets[index];

		while (*link)
		{
			struct entry *entry = *link;

			if (cache.frame - entry->last_used > IDLE_FRAMES)
			{
				*link = entry->next;
				if (entry->image)
					xgpu_vulkan_image_destroy(entry->image);
				free(entry);
			}
			else
			{
				link = &entry->next;
			}
		}
	}
}

void xgpu_vulkan_textures_statistics_peek(struct xgpu_vulkan_texture_statistics *statistics)
{
	*statistics = cache.statistics;
}

void xgpu_vulkan_textures_statistics(struct xgpu_vulkan_texture_statistics *statistics)
{
	*statistics = cache.statistics;
	memset(&cache.statistics, 0, sizeof(cache.statistics));
}
