/*
XBOX_TEXTURES.C

The OpenGL texture cache of the Xbox's textures, and the high-res
replacements drawn in their place; the decoding is xbox_texture_decode.c.

An Xbox texture is a Direct3D header - Common, Data (physical address),
Lock, Format and Size - over texels in guest memory. Power-of-two textures
are swizzled (Morton order, one level after another); textures with a Size
field are linear, with a pitch, and are addressed with texel coordinates.
DXT textures are stored as plain 4x4 blocks. Everything except DXT is
converted to 32-bit BGRA on upload.

A cached texture stays valid until any page it was read from is written;
memory_watch.c detects that by write-protecting the pages.

Where the renderer's backend is on the host (HALO_RENDER_STREAM), the host
caches the textures (port/macos/xgpu_metal_textures.c), and here are only
the replacements, as images (xgpu_images.h), and the Custom Edition channel
orders, which the command stream hands it.
*/

#ifdef HALO_RENDER_STREAM
#include "xgpu_images.h"
#else
#include "xgpu_gl.h"
#endif
#include "hud_hires.h"
#include "menu_files.h"
#include "text_hires.h"
#include "port_config.h"
#include "../game/cache_file_formats.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#ifdef HALO_GLES
#define GL_BGRA GL_RGBA
#endif
#include <stdlib.h>
#include <string.h>

/* Draws query this for each texture; read the setting only when it changes. */
static int hud_upscale_enabled(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static int enabled;

	if (read_at != config_changes())
	{
		enabled = config_boolean("display.high_res_hud");
		read_at = config_changes();
	}
	return enabled;
}

#ifndef HALO_RENDER_STREAM
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83f1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83f2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83f3
#endif


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

/* ---------- upload */

/* debug.texture_dump_directory writes level 0 of every upload as a TGA, read back from GL */
static void texture_dump(GLenum target, const struct xgpu_texture_description *description)
{
#ifdef HALO_GLES
	/* ES cannot read textures back */
	(void)target;
	(void)description;
}
#else
	static unsigned long dump_index = 0;
	const char *directory = *config_string("debug.texture_dump_directory") ?
		config_string("debug.texture_dump_directory") : NULL;
	unsigned long width = description->width, height = description->height;
	unsigned char header[18];
	unsigned char *pixels;
	char path[512];
	FILE *file;

	if (!directory || target != GL_TEXTURE_2D)
		return;
	pixels = malloc(width * height * 4);
	if (!pixels)
		return;
	glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
	snprintf(path, sizeof(path), "%s/tex%05lu_fmt%02x_%lux%lu.tga", directory, dump_index++,
		(unsigned)description->format, width, height);
	file = fopen(path, "wb");
	if (file)
	{
		memset(header, 0, sizeof(header));
		header[2] = 2;
		header[12] = (unsigned char)width; header[13] = (unsigned char)(width >> 8);
		header[14] = (unsigned char)height; header[15] = (unsigned char)(height >> 8);
		header[16] = 32; header[17] = 0x28;
		fwrite(header, 1, sizeof(header), file);
		fwrite(pixels, 4, width * height, file);
		fclose(file);
	}
	free(pixels);
}
#endif
#endif

/* ---------- Custom Edition channel orders

Halo PC keeps what some textures hold in other channels than the game reads
it from (enum custom_edition_channel_order): a model shader's multipurpose
masks, and a HUD meter's shape and fill order. The Custom Edition map
loading says which texels hold which order as they arrive
(port/linux/game/custom_edition_bitmaps.c), and textures made of them are
sampled with each channel taken from where Halo PC keeps it, which leaves
them as compressed as they were. Addresses stay listed until other texels
arrive there, which the loading also says, or the map goes; the game and the
renderer share a thread. */

/* for each order, the channel (red, green, blue, alpha) of the texels each
channel is sampled from */
static const unsigned char custom_edition_channel_sources[NUMBER_OF_CUSTOM_EDITION_CHANNEL_ORDERS][4] =
{
	{ 0, 1, 2, 3 },
	/* specular, self-illumination, color change and the auxiliary mask */
	{ 2, 1, 3, 0 },
	/* the fill order in color, the shape in alpha */
	{ 3, 3, 3, 0 },
};

struct custom_edition_texels
{
	unsigned long address;
	unsigned char channel_order;
};

static struct custom_edition_texels *custom_edition_texels;
static unsigned long custom_edition_texel_count;
static unsigned long custom_edition_texel_capacity;

/* the order of the texels at address */
static unsigned char custom_edition_texels_order(unsigned long address)
{
	unsigned long index;

	for (index = 0; index < custom_edition_texel_count; index++)
	{
		if (custom_edition_texels[index].address == address)
			return custom_edition_texels[index].channel_order;
	}
	return _custom_edition_channels_xbox;
}

void halo_custom_edition_texels_channels(const void *texels, unsigned char channel_order)
{
	unsigned long address = (unsigned long)texels;
	unsigned long index;

	if (channel_order >= NUMBER_OF_CUSTOM_EDITION_CHANNEL_ORDERS)
		channel_order = _custom_edition_channels_xbox;
	for (index = 0; index < custom_edition_texel_count && custom_edition_texels[index].address != address; index++)
	{
	}
	if (index < custom_edition_texel_count)
	{
		if (channel_order == _custom_edition_channels_xbox)
			custom_edition_texels[index] = custom_edition_texels[--custom_edition_texel_count];
		else
			custom_edition_texels[index].channel_order = channel_order;
	}
	else if (channel_order != _custom_edition_channels_xbox)
	{
		if (custom_edition_texel_count == custom_edition_texel_capacity)
		{
			unsigned long capacity = custom_edition_texel_capacity ? custom_edition_texel_capacity * 2 : 64;
			struct custom_edition_texels *grown = realloc(custom_edition_texels, capacity * sizeof(*grown));

			if (!grown)
			{
				platform_log("no memory to list the texels at %08lx: they are sampled in Halo PC's channel order",
					address);
				return;
			}
			custom_edition_texels = grown;
			custom_edition_texel_capacity = capacity;
		}
		custom_edition_texels[custom_edition_texel_count].address = address;
		custom_edition_texels[custom_edition_texel_count].channel_order = channel_order;
		custom_edition_texel_count++;
	}
}

void halo_custom_edition_texels_forget(void)
{
	free(custom_edition_texels);
	custom_edition_texels = NULL;
	custom_edition_texel_count = 0;
	custom_edition_texel_capacity = 0;
}

#ifdef HALO_RENDER_STREAM
unsigned long xgpu_texture_channels(unsigned long address)
{
	unsigned char order = custom_edition_texels_order(address);
	const unsigned char *sources = custom_edition_channel_sources[order];

	if (order == _custom_edition_channels_xbox)
		return 0;
	return 0x100 | sources[0] | (sources[1] << 2) | (sources[2] << 4) | ((unsigned long)sources[3] << 6);
}
#else
static void upload(GLuint texture, GLenum target, const struct xgpu_texture_description *description,
	const unsigned char *base, const D3DCOLOR *palette, unsigned char channel_order)
{
	unsigned long face_count = description->cube_map ? 6 : 1;
	unsigned long face_size = xgpu_texture_face_size(description);
	unsigned long largest = description->width * description->height * description->depth;
	BOOL decode_compressed = FALSE;
	DWORD *converted;
	unsigned long face, level;

#ifdef HALO_GLES
	decode_compressed = description->compressed && !xgpu_capabilities.s3tc;
#endif
#ifdef HALO_WEB
	/* WebGL's S3TC takes 2D textures whose sides are multiples of 4; and a
	texture whose channels are where Halo PC keeps them is decoded, to put
	them in order (WebGL 2 has no texture swizzle) */
	decode_compressed |= description->compressed &&
		(target == GL_TEXTURE_3D || (description->width & 3) || (description->height & 3) ||
			channel_order != _custom_edition_channels_xbox);
#endif
	converted = description->compressed && !decode_compressed ? NULL : malloc(largest * sizeof(DWORD));
	if (!converted && !(description->compressed && !decode_compressed))
	{
		platform_log("textures: no memory to convert a %lux%lux%lu texture; it is not drawn",
			description->width, description->height, description->depth);
		return;
	}
	glBindTexture(target, texture);
	xgpu_gl_state_invalidate();
	/* the channel of the texels each channel is sampled from, set on every
	upload: a texture object can be reused for other texels (WebGL 2 has no
	texture swizzle: the converted texels' channels are put in order before
	they are uploaded instead, below; there it is only mods' textures, as
	Custom Edition maps do not run there) */
#ifndef HALO_WEB
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
		if (channel_order != _custom_edition_channels_xbox)
		{
			GLint stored[4] = { channels[0], channels[1], channels[2], channels[3] };
			unsigned long channel;

			for (channel = 0; channel < 4; channel++)
				channels[channel] = stored[custom_edition_channel_sources[channel_order][channel]];
		}
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, channels[0]);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, channels[1]);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, channels[2]);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, channels[3]);
	}
#endif
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
					glCompressedTexImage3D(image_target, (GLint)level, compressed_format(description->format), width, height, depth, 0,
						(GLsizei)xgpu_texture_level_size(description, level), source);
				else
					glCompressedTexImage2D(image_target, (GLint)level, compressed_format(description->format), width, height, 0,
						(GLsizei)xgpu_texture_level_size(description, level), source);
			}
			else
			{
#ifdef HALO_GLES
				if (decode_compressed)
					xgpu_texture_decode_dxt(description, level, source, converted);
				else
#endif
				if (!xgpu_texture_decode_level(description, level, source, palette, converted))
				{
					platform_log("textures: no memory to convert a %lux%lux%lu texture; it is not drawn",
						description->width, description->height, description->depth);
					free(converted);
					return;
				}
#ifdef HALO_WEB
				{
					/* BGRA (32-bit ARGB words) to the RGBA WebGL takes, each
					channel from where the texels keep it */
					unsigned long texel, count = (unsigned long)width * (unsigned long)height * (unsigned long)depth;

					for (texel = 0; texel < count; texel++)
					{
						unsigned long value = converted[texel];

						if (channel_order == _custom_edition_channels_xbox)
						{
							converted[texel] = (value & 0xff00ff00UL) | ((value >> 16) & 0xffUL) | ((value & 0xffUL) << 16);
						}
						else
						{
							/* (red, green, blue and alpha, as stored) */
							unsigned long stored[4];
							unsigned long channel;

							stored[0] = (value >> 16) & 0xffUL;
							stored[1] = (value >> 8) & 0xffUL;
							stored[2] = value & 0xffUL;
							stored[3] = (value >> 24) & 0xffUL;
							converted[texel] = 0;
							for (channel = 0; channel < 4; channel++)
								converted[texel] |= stored[custom_edition_channel_sources[channel_order][channel]] << (8 * channel);
						}
					}
				}
#endif
				if (target == GL_TEXTURE_3D)
					glTexImage3D(image_target, (GLint)level, GL_RGBA8, width, height, depth, 0, GL_BGRA, GL_UNSIGNED_BYTE, converted);
				else
					glTexImage2D(image_target, (GLint)level, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, converted);
			}
		}
	}
	free(converted);
	texture_dump(target, description);
}

/* ---------- cache */

struct texture_entry
{
	struct texture_entry *next;
	DWORD data, format_word, size_word;
	unsigned long palette_hash;
	GLuint texture;
	GLenum target;
	struct xgpu_texture_description description;
	unsigned long address, size;
	unsigned long generation;
	unsigned long last_used_frame;
	/* the high-res HUD texture drawn in its place (hud_hires.h), or -1 */
	long override;
	GLuint hud_upscale;
	unsigned long hud_upscale_levels;
	BOOL hud_upscale_checked;
	/* the newest generation of its pages (memory_watch_generation) as of the
	memory watch serial read before it was found: the same while no watched
	page has been written since (0: never found) */
	unsigned long watched_serial, watched_generation;
};

#define TEXTURE_BUCKET_COUNT 4096
#define TEXTURE_SWEEP_SECONDS 10
#define TEXTURE_IDLE_SECONDS 30
#define TEXTURE_SWEEP_COUNT (TEXTURE_IDLE_SECONDS / TEXTURE_SWEEP_SECONDS)
#define MAXIMUM_PALETTE_VARIANTS 8

static struct texture_entry *texture_buckets[TEXTURE_BUCKET_COUNT];

/* Draws mostly bind the textures the draws before them bound. A lookup of a
texture that is not palettized is remembered with the memory watch serial it
started at: while no watched page has been written since, and no texture
has been dropped, the same lookup finds the same current texture. */
#define RECENT_TEXTURE_COUNT 512

static struct
{
	DWORD data, format_word, size_word;
	struct texture_entry *entry;
	unsigned long watch_serial;
	unsigned long drop_serial;
} recent_textures[RECENT_TEXTURE_COUNT];
static unsigned long texture_drop_serial = 1;
static unsigned long texture_frame = 0;
/* the frame of each of the last sweeps, a sweep apart, and when the next
is due */
static unsigned long texture_sweep_frames[TEXTURE_SWEEP_COUNT];
static unsigned long texture_sweep_count = 0;
static Uint64 texture_next_sweep = 0;

/* (every bit of the three mixed into the top ones: textures are aligned,
and few sizes and formats are common) */
static unsigned long bucket_index(DWORD data, DWORD format_word, DWORD size_word)
{
	unsigned long hash = (unsigned long)data * 2654435761UL ^ (unsigned long)format_word * 2246822519UL ^
		(unsigned long)size_word * 3266489917UL;

	return ((hash & 0xffffffffUL) >> 20) % TEXTURE_BUCKET_COUNT;
}

/* palettized textures are cached per palette contents: the game rewrites
palettes freely, and often cycles a texture through a few of them */
static unsigned long palette_hash(const D3DCOLOR *palette)
{
	unsigned long hash = 2166136261UL, index;

	if (!palette)
		return 0;
	for (index = 0; index < 256; index++)
		hash = (hash ^ palette[index]) * 16777619UL;
	return hash ? hash : 1;
}

/* an entry's GL texture and description: its replacement's, if it has one,
with the bitmap's own size (which its coordinates are in) */
static GLuint texture_entry_result(struct texture_entry *entry, GLenum *target,
	struct xgpu_texture_description *description)
{
	*target = entry->target;
	*description = entry->description;
	/* (the high-res text's atlas, for its placeholder bitmap: text_hires.h) */
	{
		GLuint atlas = text_hires_atlas_texture(entry->data);

		if (atlas)
		{
			description->levels = 1;
			return atlas;
		}
	}
	/* (a menu's bitmap: menu_files.h) */
	{
		unsigned long levels;
		GLuint art = menu_art_texture(entry->data, &levels);

		if (art)
		{
			description->levels = levels;
			description->hires = TRUE;
			return art;
		}
	}
	/* (a high-res texture drawn for some sprites, for the placeholder the
	game draws them from: hud_hires.h) */
	{
		GLuint texture = hud_hires_placeholder_texture(entry->data, &description->levels);

		if (texture)
		{
			description->hires = TRUE;
			return texture;
		}
	}
	if (entry->override >= 0)
	{
		GLuint texture = hud_hires_override_texture(entry->override, &description->levels);

		if (texture)
		{
			description->hires = TRUE;
			description->hires_coverage = hud_hires_override_coverage(entry->override);
			description->hires_point_threshold = hud_hires_override_point_threshold(entry->override);
			return texture;
		}
	}
	/* These sheets keep the map's art and coordinates. Decode through the
	 * normal format reader; the HUD module owns only the interpolation. */
	if (hud_upscale_enabled())
	{
		if (!entry->hud_upscale_checked)
		{
			long kind = hud_hires_upscale_at(entry->address, entry->description.width, entry->description.height);

			entry->hud_upscale_checked = TRUE;
			if (entry->size && !entry->description.compressed && entry->target == GL_TEXTURE_2D &&
				entry->description.format != 0x0b &&
				entry->description.width <= 256 && entry->description.height <= 256 &&
				kind &&
				custom_edition_texels_order(entry->address) == _custom_edition_channels_xbox &&
				platform_is_contiguous((void *)entry->address) &&
				platform_is_contiguous((void *)(entry->address + entry->size - 1)))
			{
				DWORD *pixels = malloc(entry->description.width * entry->description.height * 4);

				if (pixels && xgpu_texture_decode_level(&entry->description, 0, (const unsigned char *)entry->address, NULL, pixels))
					entry->hud_upscale = hud_hires_upscale_texture((const unsigned char *)pixels,
						entry->description.width, entry->description.height, kind, &entry->hud_upscale_levels);
				free(pixels);
			}
		}
		if (entry->hud_upscale)
		{
			description->hires = TRUE;
			description->levels = entry->hud_upscale_levels;
			return entry->hud_upscale;
		}
	}
	return entry->texture;
}

GLuint xgpu_texture_get(const DWORD *resource, const D3DCOLOR *palette, GLenum *target,
	struct xgpu_texture_description *description)
{
	DWORD data = resource[1], format_word = resource[3], size_word = resource[4];
	struct texture_entry **bucket = &texture_buckets[bucket_index(data, format_word, size_word)];
	struct texture_entry *entry;
	unsigned long generation;
	BOOL palettized = ((format_word & D3DFORMAT_FORMAT_MASK) >> D3DFORMAT_FORMAT_SHIFT) == 0x0b;
	unsigned long hash = palettized ? palette_hash(palette) : 0;

	struct texture_entry *oldest_variant = NULL;
	unsigned long variant_count = 0;
	static int no_cache = -1;
	unsigned long recent = bucket_index(data, format_word, size_word) % RECENT_TEXTURE_COUNT;
	unsigned long watch_serial = memory_watch_serial();

	/* (a texture that is not palettized has the one entry, until one is
	dropped: remembered, a page written since only means checking it) */
	entry = NULL;
	if (!palettized && recent_textures[recent].entry && recent_textures[recent].data == data &&
		recent_textures[recent].format_word == format_word && recent_textures[recent].size_word == size_word &&
		recent_textures[recent].drop_serial == texture_drop_serial)
	{
		entry = recent_textures[recent].entry;
		if (recent_textures[recent].watch_serial == watch_serial)
		{
			entry->last_used_frame = texture_frame;
			return texture_entry_result(entry, target, description);
		}
	}

	if (!entry)
	{
		for (entry = *bucket; entry; entry = entry->next)
		{
			if (entry->data == data && entry->format_word == format_word && entry->size_word == size_word)
			{
				if (entry->palette_hash == hash)
					break;
				variant_count++;
				if (!oldest_variant || entry->last_used_frame < oldest_variant->last_used_frame)
					oldest_variant = entry;
			}
		}
	}
	if (!entry && variant_count >= MAXIMUM_PALETTE_VARIANTS)
	{
		/* a palette that keeps changing reuses the stalest copy */
		entry = oldest_variant;
		entry->palette_hash = hash;
		entry->generation = 0;
	}
	if (!entry)
	{
		entry = calloc(1, sizeof(*entry));
		if (!entry)
		{
			xgpu_texture_describe(format_word, size_word, description);
			*target = GL_TEXTURE_2D;
			return 0;
		}
		entry->data = data;
		entry->format_word = format_word;
		entry->size_word = size_word;
		entry->palette_hash = hash;
		xgpu_texture_describe(format_word, size_word, &entry->description);
		entry->target = entry->description.cube_map ? GL_TEXTURE_CUBE_MAP :
			entry->description.depth > 1 ? GL_TEXTURE_3D : GL_TEXTURE_2D;
		entry->address = (unsigned long)PLATFORM_PHYSICAL_TO_VIRTUAL(data);
		/* (a size beyond D3DDevice_GetDeviceCaps' is never uploaded: its
		byte counts would not fit in 32 bits) */
		entry->size = xgpu_texture_size_supported(&entry->description) ?
			xgpu_texture_face_size(&entry->description) * (entry->description.cube_map ? 6 : 1) : 0;
		if (!entry->size)
		{
			platform_log("textures: a %lux%lux%lu texture is larger than the device takes; it is not drawn",
				entry->description.width, entry->description.height, entry->description.depth);
		}
		entry->generation = 0;
		entry->override = -1;
		glGenTextures(1, &entry->texture);
		entry->next = *bucket;
		*bucket = entry;
	}

	if (no_cache < 0)
		no_cache = config_boolean("debug.texture_no_cache");
	/* (its pages' newest generation, found again only once a watched page
	has been written: a large texture's pages, scanned for every draw that
	bound it, were much of a frame with many objects) */
	if (entry->watched_serial && entry->watched_serial == watch_serial)
		generation = entry->watched_generation;
	else
	{
		generation = memory_watch_generation(entry->address, entry->size);
		entry->watched_serial = watch_serial;
		entry->watched_generation = generation;
	}
	if (!entry->generation || generation > entry->generation || no_cache)
	{
		/* protect first, so a write racing with the upload is noticed */
		memory_watch_protect(entry->address, entry->size);
		entry->generation = memory_watch_generation(entry->address, entry->size);
		if (!entry->generation)
			entry->generation = 1;
		/* (which bitmap is here may have changed with the pixels) */
		if (entry->hud_upscale)
		{
			glDeleteTextures(1, &entry->hud_upscale);
			entry->hud_upscale = 0;
			xgpu_gl_state_invalidate();
		}
		entry->hud_upscale_checked = FALSE;
		entry->override = -1;
		if (entry->size && !palettized && !entry->description.cube_map && entry->description.depth == 1)
		{
			unsigned long levels;

			entry->override = hud_hires_override_find(entry->address, entry->description.width,
				entry->description.height, entry->description.levels > 1 ?
				xgpu_texture_level_offset(&entry->description, 1) : xgpu_texture_face_size(&entry->description));
			if (entry->override >= 0 && !hud_hires_override_texture(entry->override, &levels))
				entry->override = -1;
		}
		if (entry->override < 0 && entry->size && platform_is_contiguous((void *)entry->address) &&
			platform_is_contiguous((void *)(entry->address + entry->size - 1)))
		{
			if (config_boolean("debug.texture_log"))
			{
				const unsigned char *bytes = (const unsigned char *)entry->address;
				unsigned long index, ones = 0, zeros = 0;

				for (index = 0; index < entry->size; index++)
				{
					ones += bytes[index] == 0xff;
					zeros += bytes[index] == 0;
				}
				platform_log("texture upload %08lx fmt %02lx %lux%lu size %lu gen %lu ff %lu%% 00 %lu%%",
					(unsigned long)data, (unsigned long)entry->description.format, entry->description.width,
					entry->description.height, entry->size, entry->generation,
					ones * 100 / entry->size, zeros * 100 / entry->size);
			}
			upload(entry->texture, entry->target, &entry->description, (const unsigned char *)entry->address, palette,
				custom_edition_texels_order(entry->address));
		}
	}
	entry->last_used_frame = texture_frame;
	if (!palettized && !no_cache)
	{
		recent_textures[recent].data = data;
		recent_textures[recent].format_word = format_word;
		recent_textures[recent].size_word = size_word;
		recent_textures[recent].entry = entry;
		recent_textures[recent].watch_serial = watch_serial;
		recent_textures[recent].drop_serial = texture_drop_serial;
	}
	return texture_entry_result(entry, target, description);
}

void xgpu_texture_cache_begin_frame(void)
{
	unsigned long index, idle_since;
	unsigned long changed = hud_hires_settings_changed();
	Uint64 now;

	/* Refresh only map bitmaps belonging to the setting that changed. Keep
	 * decoded replacement art and world textures; rebuild native pixels on
	 * demand too, since an override may have skipped their first upload. */
	if (changed)
	{
		for (index = 0; index < TEXTURE_BUCKET_COUNT; index++)
		{
			struct texture_entry *entry;

			for (entry = texture_buckets[index]; entry; entry = entry->next)
				if (hud_hires_bitmap_affected(entry->address,
					(long)entry->description.width, (long)entry->description.height, changed))
					entry->generation = 0;
		}
		/* Recent hits bypass the generation check. Forget lookups, not textures. */
		memset(recent_textures, 0, sizeof(recent_textures));
		xgpu_gl_state_invalidate();
	}
	texture_frame++;
	now = SDL_GetTicks();
	if (now < texture_next_sweep)
		return;
	texture_next_sweep = now + TEXTURE_SWEEP_SECONDS * 1000;
	/* drop the textures not used since the sweep half a minute ago (none
	before the first sweeps, which find frame 0). The time is counted in
	seconds, not frames: 1800 frames, half a minute at 60 frames a second, is
	five seconds at 360, and a texture out of sight that long was converted
	and uploaded again when it came back into view */
	idle_since = texture_sweep_frames[texture_sweep_count % TEXTURE_SWEEP_COUNT];
	texture_sweep_frames[texture_sweep_count % TEXTURE_SWEEP_COUNT] = texture_frame;
	texture_sweep_count++;
	for (index = 0; index < TEXTURE_BUCKET_COUNT; index++)
	{
		struct texture_entry **link = &texture_buckets[index];

		while (*link)
		{
			struct texture_entry *entry = *link;

			if (texture_frame - entry->last_used_frame > texture_frame - idle_since)
			{
				*link = entry->next;
				glDeleteTextures(1, &entry->texture);
				if (entry->hud_upscale)
					glDeleteTextures(1, &entry->hud_upscale);
				xgpu_gl_state_invalidate();
				texture_drop_serial++;
				free(entry);
			}
			else
			{
				link = &entry->next;
			}
		}
	}
}
#endif

#ifdef HALO_RENDER_STREAM
/* ---------- the replacements, where the renderer is on the host

What texture_entry_result finds for the OpenGL cache: the high-res text's
atlas, a menu's art, a high-res HUD texture for a placeholder bitmap, or
for the bitmap whose pixels these are. Which HUD texture stands for a
bitmap's pixels is found again whenever they change (the write tracking),
for each texture by its Data, Format and Size. */

#define REPLACEMENT_BUCKET_COUNT 1024
#define REPLACEMENT_IDLE_FRAMES 1800

struct replacement_entry
{
	struct replacement_entry *next;
	DWORD data, format_word, size_word;
	/* the high-res HUD texture (hud_hires.h), or -1 */
	long override;
	unsigned int hud_upscale;
	unsigned long hud_upscale_levels, last_used_frame;
	BOOL hud_upscale_checked;
	/* its pages' newest generation when it was found, and the memory
	watch serial as of which that is known (as texture_entry's) */
	unsigned long generation, watched_serial;
};

static struct replacement_entry *replacement_buckets[REPLACEMENT_BUCKET_COUNT];
static unsigned long replacement_frame;

void xgpu_texture_cache_begin_frame(void)
{
	unsigned long changed = hud_hires_settings_changed();
	unsigned long index;

	replacement_frame++;
	if (!changed && replacement_frame % 600)
		return;
	for (index = 0; index < REPLACEMENT_BUCKET_COUNT; index++)
	{
		struct replacement_entry **link = &replacement_buckets[index];

		while (*link)
		{
			struct replacement_entry *entry = *link;
			struct xgpu_texture_description description;
			unsigned long address = (unsigned long)PLATFORM_PHYSICAL_TO_VIRTUAL(entry->data);

			if (replacement_frame - entry->last_used_frame > REPLACEMENT_IDLE_FRAMES)
			{
				*link = entry->next;
				xgpu_image_delete(entry->hud_upscale);
				free(entry);
				continue;
			}
			if (changed)
			{
				xgpu_texture_describe(entry->format_word, entry->size_word, &description);
				if (hud_hires_bitmap_affected(address, (long)description.width, (long)description.height, changed))
				{
					/* Find the replacement again even when its pixels have not
					changed. Keep the images used by frames still in flight. */
					entry->generation = 0;
					entry->watched_serial = 0;
				}
			}
			link = &entry->next;
		}
	}
}

static struct replacement_entry *replacement_get(const DWORD *resource, const struct xgpu_texture_description *description)
{
	DWORD data = resource[1], format_word = resource[3], size_word = resource[4];
	unsigned long hash = ((unsigned long)data * 2654435761UL ^ (unsigned long)format_word * 2246822519UL ^
		(unsigned long)size_word * 3266489917UL) >> 20;
	struct replacement_entry **bucket = &replacement_buckets[hash % REPLACEMENT_BUCKET_COUNT];
	struct replacement_entry *entry;
	unsigned long address, size, generation, watch_serial = memory_watch_serial();

	if (description->format == 0x0b || description->cube_map || description->depth > 1 ||
		!xgpu_texture_size_supported(description))
		return NULL;
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->data == data && entry->format_word == format_word && entry->size_word == size_word)
			break;
	}
	if (!entry)
	{
		entry = calloc(1, sizeof(*entry));
		if (!entry)
			return NULL;
		entry->data = data;
		entry->format_word = format_word;
		entry->size_word = size_word;
		entry->override = -1;
		entry->next = *bucket;
		*bucket = entry;
	}
	entry->last_used_frame = replacement_frame;
	if (entry->watched_serial && entry->watched_serial == watch_serial)
		return entry;
	address = (unsigned long)PLATFORM_PHYSICAL_TO_VIRTUAL(data);
	size = xgpu_texture_face_size(description);
	generation = memory_watch_generation(address, size);
	if (!entry->generation || generation > entry->generation)
	{
		unsigned long levels;

		/* protect first, so a write racing with the search is noticed */
		memory_watch_protect(address, size);
		entry->generation = memory_watch_generation(address, size);
		if (!entry->generation)
			entry->generation = 1;
		xgpu_image_delete(entry->hud_upscale);
		entry->hud_upscale = 0;
		entry->hud_upscale_checked = FALSE;
		entry->override = hud_hires_override_find(address, description->width, description->height,
			description->levels > 1 ? xgpu_texture_level_offset(description, 1) : size);
		if (entry->override >= 0 && !hud_hires_override_texture(entry->override, &levels))
			entry->override = -1;
	}
	entry->watched_serial = watch_serial;
	return entry;
}

unsigned int xgpu_texture_replacement(DWORD resource, struct xgpu_texture_description *description)
{
	const DWORD *header = (const DWORD *)resource;
	unsigned long levels;
	unsigned int image;
	long override;
	struct replacement_entry *entry;

	/* (the high-res text's atlas, for its placeholder bitmap: text_hires.h) */
	if ((image = text_hires_atlas_texture(header[1])) != 0)
	{
		description->levels = 1;
		return image;
	}
	/* (a menu's bitmap: menu_files.h) */
	if ((image = menu_art_texture(header[1], &levels)) != 0)
	{
		description->levels = levels;
		description->hires = TRUE;
		return image;
	}
	/* (a high-res texture drawn for some sprites, for the placeholder the
	game draws them from: hud_hires.h) */
	if ((image = hud_hires_placeholder_texture(header[1], &levels)) != 0)
	{
		description->levels = levels;
		description->hires = TRUE;
		return image;
	}
	entry = replacement_get(header, description);
	if (!entry)
		return 0;
	override = entry->override;
	if (override >= 0 && (image = hud_hires_override_texture(override, &levels)) != 0)
	{
		description->levels = levels;
		description->hires = TRUE;
		description->hires_coverage = hud_hires_override_coverage(override);
		description->hires_point_threshold = hud_hires_override_point_threshold(override);
		return image;
	}
	if (hud_upscale_enabled())
	{
		if (!entry->hud_upscale_checked)
		{
			unsigned long address = (unsigned long)PLATFORM_PHYSICAL_TO_VIRTUAL(header[1]);
			unsigned long size = xgpu_texture_face_size(description);
			long kind = hud_hires_upscale_at(address, description->width, description->height);

			entry->hud_upscale_checked = TRUE;
			if (kind && size && !description->compressed && description->width <= 256 && description->height <= 256 &&
				custom_edition_texels_order(address) == _custom_edition_channels_xbox &&
				platform_is_contiguous((void *)address) && platform_is_contiguous((void *)(address + size - 1)))
			{
				DWORD *pixels = malloc(description->width * description->height * 4);

				if (pixels && xgpu_texture_decode_level(description, 0, (const unsigned char *)address, NULL, pixels))
					entry->hud_upscale = hud_hires_upscale_texture((const unsigned char *)pixels,
						description->width, description->height, kind, &entry->hud_upscale_levels);
				free(pixels);
			}
		}
		if (entry->hud_upscale)
		{
			description->hires = TRUE;
			description->levels = entry->hud_upscale_levels;
			return entry->hud_upscale;
		}
	}
	return 0;
}
#endif
