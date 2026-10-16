/*
HOST_GL_TEXTURES.C

The host's OpenGL backend's cache of the Xbox's textures and the high-res
replacements' images (host_gl_renderer.h), as the guest's OpenGL texture
cache (port/linux/src/xbox_textures.c) has them, the texels uploaded as it
uploads them (DXT as it is where the context takes it; every other format
decoded to BGRA, xbox_texture_decode.c), and found as the Metal backend's
cache finds them (port/macos/xgpu_metal_textures.c), on the render thread a
frame after the game drew.

An Xbox texture is found by its Data, Format and Size words (and a
palettized one's palette, and the channels a Custom Edition map's texels
are sampled from, which the stream's stage carries); its texels are read
from the guest's memory where they are. The pages it was read from are
write-protected (the write tracking, host_memory_watch.c), and the texture
is read again once any of them has been written.

An image (a high-res replacement: port/linux/src/xgpu_images.h) is found by
its number and made again when its serial changes, its levels made from the
first, as the guest's replacements made theirs (hud_hires.c, text_hires.c).
*/

#include "host.h"
#include "host_gl_renderer.h"
#include "xgpu_gl.h"
#include "xgpu_stream.h"

#include <stdlib.h>
#include <string.h>

#define BUCKET_COUNT 4096
/* frames a texture is kept without being drawn, and how often they are
looked for (xbox_textures.c's) */
#define IDLE_FRAMES 1800
#define EVICT_EVERY 600
/* the copies of one texture for its palettes (the game cycles some
through a few) */
#define PALETTE_VARIANTS 8
/* D3DFMT_P8 */
#define FORMAT_PALETTIZED 0x0b

#ifdef HALO_GLES
#define GL_BGRA GL_RGBA
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83f1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83f2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83f3
#endif

/* the Xbox's contiguous memory, where the textures are: physical address
P is guest address HALO_GUEST_WINDOW_BASE + P */
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
	image's number (data), serial (format_word) and pixels' kind
	(size_word) */
	uint32_t data, format_word, size_word, palette_hash, channels;
	GLuint texture;
	GLenum target;
	struct xgpu_texture_description description;
	/* (an Xbox texture's) the range read, and its pages' newest generation
	as of the tracking's serial watched_serial */
	uint32_t address, size;
	uint32_t generation, watched_serial;
	uint64_t last_used;
};

static struct
{
	struct entry *buckets[BUCKET_COUNT];
	uint64_t frame;
	/* the decoded texels of the largest level so far */
	DWORD *converted;
	size_t converted_size;
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

/* the guest's pages of a range are mapped and readable: with the base
register, as the host's memory has them; in the low 4 GB, the Xbox's
memory always is (host_memory.c), and the rest is taken to be */
static BOOL readable(uint32_t address, uint32_t size)
{
#ifdef HALO_GUEST_BASE
	uint64_t page = host_page_size();
	uint64_t first = address & ~(page - 1), last = ((uint64_t)address + size + page - 1) & ~(page - 1);
	uint32_t start, end;

	if (!address || last > 0xffffffffu)
		return FALSE;
	return host_memory_readable_extent(address, size, (uint32_t)first, (uint32_t)last, &start, &end) != 0;
#else
	return address && (uint64_t)address + size <= 0x100000000ull;
#endif
}

/* ---------- Xbox textures */

/* (D3DFMT_DXT1, D3DFMT_DXT2 and 3, D3DFMT_DXT4 and 5) */
static GLenum compressed_format(DWORD format)
{
	switch (format)
	{
	case 0x0c: return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
	case 0x0e: return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
	default: return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
	}
}

/* reads the texture's texels into it, as xbox_textures.c's upload */
static void xbox_texture_upload(struct entry *entry, const D3DCOLOR *palette)
{
	const struct xgpu_texture_description *description = &entry->description;
	const unsigned char *base = host_pointer(entry->address);
	GLenum target = entry->target;
	unsigned long face_count = description->cube_map ? 6 : 1;
	unsigned long face_size = xgpu_texture_face_size(description);
	unsigned long largest = description->width * description->height * description->depth;
	BOOL decode_compressed = FALSE;
	DWORD *converted = NULL;
	unsigned long face, level;

#ifdef HALO_GLES
	decode_compressed = description->compressed && !xgpu_capabilities.s3tc;
#endif
	if (!(description->compressed && !decode_compressed))
	{
		converted = converted_get(largest);
		if (!converted)
		{
			host_logf(HOST_LOG_WARN, "textures: no memory to convert a %lux%lux%lu texture; it is not drawn",
				description->width, description->height, description->depth);
			return;
		}
	}
	glBindTexture(target, entry->texture);
	xgpu_gl_state_invalidate();
	/* the channel of the texels each channel is sampled from, set on every
	upload (the stage's channels: 0 as they are, else 0x100 and two bits for
	each of red, green, blue and alpha, the channel taken from) */
	{
		GLint channels[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };

#ifdef HALO_GLES
		/* converted texels are BGRA in memory (32-bit ARGB words); ES takes
		RGBA */
		if (converted)
		{
			channels[0] = GL_BLUE;
			channels[2] = GL_RED;
		}
#endif
		if (entry->channels & 0x100)
		{
			GLint stored[4] = { channels[0], channels[1], channels[2], channels[3] };
			unsigned long channel;

			for (channel = 0; channel < 4; channel++)
				channels[channel] = stored[(entry->channels >> (channel * 2)) & 3];
		}
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, channels[0]);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, channels[1]);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, channels[2]);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, channels[3]);
	}
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, (GLint)description->levels - 1);
	for (face = 0; face < face_count; face++)
	{
		GLenum image_target = description->cube_map ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : target;

		for (level = 0; level < description->levels; level++)
		{
			const unsigned char *source = base + face * face_size + xgpu_texture_level_offset(description, level);
			GLsizei width = (GLsizei)xgpu_texture_level_dimension(description->width, level);
			GLsizei height = (GLsizei)xgpu_texture_level_dimension(description->height, level);
			GLsizei depth = (GLsizei)xgpu_texture_level_dimension(description->depth, level);

			if (description->compressed && !decode_compressed)
			{
				if (target == GL_TEXTURE_3D)
					glCompressedTexImage3D(image_target, (GLint)level, compressed_format(description->format), width, height,
						depth, 0, (GLsizei)xgpu_texture_level_size(description, level), source);
				else
					glCompressedTexImage2D(image_target, (GLint)level, compressed_format(description->format), width, height,
						0, (GLsizei)xgpu_texture_level_size(description, level), source);
				continue;
			}
			if (decode_compressed)
				xgpu_texture_decode_dxt(description, level, source, converted);
			else if (!xgpu_texture_decode_level(description, level, source, palette, converted))
			{
				host_logf(HOST_LOG_WARN, "textures: no memory to convert a %lux%lux%lu texture; it is not drawn",
					description->width, description->height, description->depth);
				return;
			}
			if (target == GL_TEXTURE_3D)
				glTexImage3D(image_target, (GLint)level, GL_RGBA8, width, height, depth, 0, GL_BGRA, GL_UNSIGNED_BYTE,
					converted);
			else
				glTexImage2D(image_target, (GLint)level, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, converted);
		}
	}
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
	entry->target = entry->description.cube_map ? GL_TEXTURE_CUBE_MAP : entry->description.depth > 1 ? GL_TEXTURE_3D :
		GL_TEXTURE_2D;
	entry->address = CONTIGUOUS_BASE | data;
	/* (one larger than the device takes is never drawn, nor one outside the
	contiguous memory: as the guest's cache) */
	if (xgpu_texture_size_supported(&entry->description))
	{
		uint64_t size = (uint64_t)xgpu_texture_face_size(&entry->description) * (entry->description.cube_map ? 6 : 1);

		if (size && entry->address >= CONTIGUOUS_BASE && (uint64_t)entry->address + size <=
			(uint64_t)CONTIGUOUS_BASE + CONTIGUOUS_SIZE)
			entry->size = (uint32_t)size;
	}
	if (entry->size)
		glGenTextures(1, &entry->texture);
	else
		host_logf(HOST_LOG_WARN, "textures: a %lux%lux%lu texture of format %02x at %08x is not drawn",
			entry->description.width, entry->description.height, entry->description.depth,
			(unsigned)entry->description.format, data);
	entry->next = *bucket;
	*bucket = entry;
	return entry;
}

static GLuint xbox_texture_get(const struct xgpu_stream_texture_stage *stage, GLenum *target)
{
	const D3DCOLOR *palette = readable(stage->palette, 256 * sizeof(D3DCOLOR)) ? host_pointer(stage->palette) : NULL;
	struct entry *entry;
	uint32_t serial, generation;

	if (!readable(stage->resource, 5 * sizeof(uint32_t)))
		return 0;
	entry = xbox_entry_find(stage, palette);
	if (!entry || !entry->texture)
		return 0;
	*target = entry->target;
	entry->last_used = cache.frame;
	serial = host_memory_watch_serial();
	if (entry->generation && entry->watched_serial == serial)
		return entry->texture;
	generation = host_memory_watch_generation(entry->address, entry->size);
	if ((!entry->generation || generation > entry->generation) && readable(entry->address, entry->size))
	{
		/* protect first, so a write racing with the reading is noticed */
		host_memory_watch_protect(entry->address, entry->size);
		entry->generation = host_memory_watch_generation(entry->address, entry->size);
		if (!entry->generation)
			entry->generation = 1;
		xbox_texture_upload(entry, palette);
	}
	entry->watched_serial = serial;
	return entry->texture;
}

/* ---------- images */

/* XGPU_IMAGE_* (xgpu_images.h) */
enum
{
	_image_rgba,
	_image_coverage,
};

static GLuint image_get(const struct xgpu_stream_texture_stage *stage)
{
	struct entry **bucket = &cache.buckets[bucket_index(stage->resource, 0, 1)];
	struct entry *entry;
	const unsigned char *pixels = host_pointer(stage->data);
	uint32_t width = stage->width, height = stage->height;
	BOOL coverage = stage->resource_format == _image_coverage;

	if (!pixels || !width || !height || width > 16384 || height > 16384 ||
		!readable(stage->data, width * height * (coverage ? 1 : 4)))
		return 0;
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->kind == _entry_image && entry->data == stage->resource)
			break;
	}
	if (!entry)
	{
		entry = calloc(1, sizeof(*entry));
		if (!entry)
			return 0;
		entry->kind = _entry_image;
		entry->data = stage->resource;
		entry->target = GL_TEXTURE_2D;
		entry->next = *bucket;
		*bucket = entry;
	}
	entry->last_used = cache.frame;
	if (entry->texture && entry->format_word == stage->resource_size)
		return entry->texture;
	/* (its pixels changed, or it is another image of the number's) */
	if (!entry->texture)
		glGenTextures(1, &entry->texture);
	entry->format_word = stage->resource_size;
	entry->size_word = stage->resource_format;
	entry->description.width = width;
	entry->description.height = height;
	entry->description.levels = stage->levels;
	glBindTexture(GL_TEXTURE_2D, entry->texture);
	xgpu_gl_state_invalidate();
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, stage->levels > 1 ? (GLint)stage->levels - 1 : 0);
	if (coverage)
	{
		/* (white, the coverage its alpha, as the maps' fonts are: text_hires.c) */
		unsigned char *texels = malloc((size_t)width * height * 4);
		size_t index;

		if (!texels)
			return entry->texture;
		for (index = 0; index < (size_t)width * height; index++)
		{
			texels[index * 4 + 0] = texels[index * 4 + 1] = texels[index * 4 + 2] = 255;
			texels[index * 4 + 3] = pixels[index];
		}
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
		free(texels);
	}
	else
	{
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	}
	if (stage->levels > 1)
		glGenerateMipmap(GL_TEXTURE_2D);
	xgpu_gl_state_invalidate();
	return entry->texture;
}

/* ---------- the cache */

GLuint host_gl_textures_get(const struct xgpu_stream_texture_stage *stage, GLenum *target)
{
	*target = GL_TEXTURE_2D;
	if (stage->source == XGPU_STREAM_SOURCE_IMAGE)
		return image_get(stage);
	return stage->resource ? xbox_texture_get(stage, target) : 0;
}

/* (xgpu_gl.c's, at each frame's end) */
void xgpu_texture_cache_begin_frame(void)
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
				if (entry->texture)
				{
					xgpu_gl_texture_deleted(entry->texture);
					glDeleteTextures(1, &entry->texture);
					xgpu_gl_state_invalidate();
				}
				free(entry);
			}
			else
			{
				link = &entry->next;
			}
		}
	}
}
