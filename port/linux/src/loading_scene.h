/* The main menu's scene as the loading screen draws it: the meshes of the
   ui map's sky and objects (the ring) with their textures, and the menu's
   camera, read from the player's own map as the menu showed it
   (port/linux/game/loading_scene.c) and handed to the presenter in one
   block (xgpu_backend_loading_scene). Nothing of the map's is kept in the
   port or its releases. The presenter reads the block as untrusted bytes:
   loading_scene_validate checks every count and offset before use. */
#ifndef LOADING_SCENE_H
#define LOADING_SCENE_H

#include <stddef.h>
#include <stdint.h>

#define LOADING_SCENE_MAGIC 0x4e43534cu /* 'LSCN' */
#define LOADING_SCENE_VERSION 1u
#define LOADING_SCENE_MAXIMUM_BYTES (6u << 20)
#define LOADING_SCENE_MAXIMUM_PARTS 48u
#define LOADING_SCENE_MAXIMUM_TEXTURES 24u
#define LOADING_SCENE_MAXIMUM_VERTICES 65535u
#define LOADING_SCENE_MAXIMUM_TRIANGLES 65536u
#define LOADING_SCENE_NO_TEXTURE 0xffffffffu
#define LOADING_SCENE_PART_MAPS 4u

/* where a part is drawn: about the camera at infinity (the sky's), or in
   the world, depth tested (the objects') */
enum
{
	LOADING_SCENE_LAYER_SKY = 0,
	LOADING_SCENE_LAYER_WORLD,
};

/* how a part's colour meets what is drawn behind it: opaque, or one of the
   game's framebuffer blend functions (shaders: alpha blend, multiply,
   double multiply, add, subtract, min, max, alpha-multiply add) */
enum
{
	LOADING_SCENE_BLEND_OPAQUE = 0,
	LOADING_SCENE_BLEND_ALPHA,
	LOADING_SCENE_BLEND_MULTIPLY,
	LOADING_SCENE_BLEND_DOUBLE_MULTIPLY,
	LOADING_SCENE_BLEND_ADD,
	LOADING_SCENE_BLEND_SUBTRACT,
	LOADING_SCENE_BLEND_MINIMUM,
	LOADING_SCENE_BLEND_MAXIMUM,
	LOADING_SCENE_BLEND_ALPHA_MULTIPLY_ADD,
	LOADING_SCENE_BLENDS,
};

/* how a map's colour meets the next map's (a chicago shader's map
   functions): the first map's colour, then each function in turn with the
   next map's */
enum
{
	LOADING_SCENE_MAP_CURRENT = 0,
	LOADING_SCENE_MAP_NEXT,
	LOADING_SCENE_MAP_MULTIPLY,
	LOADING_SCENE_MAP_DOUBLE_MULTIPLY,
	LOADING_SCENE_MAP_ADD,
	LOADING_SCENE_MAP_ADD_SIGNED_CURRENT,
	LOADING_SCENE_MAP_ADD_SIGNED_NEXT,
	LOADING_SCENE_MAP_SUBTRACT_CURRENT,
	LOADING_SCENE_MAP_SUBTRACT_NEXT,
	LOADING_SCENE_MAP_BLEND_CURRENT_ALPHA,
	LOADING_SCENE_MAP_BLEND_CURRENT_ALPHA_INVERSE,
	LOADING_SCENE_MAP_BLEND_NEXT_ALPHA,
	LOADING_SCENE_MAP_BLEND_NEXT_ALPHA_INVERSE,
	LOADING_SCENE_MAP_FUNCTIONS,
};

/* a part's flags */
enum
{
	/* lit by the scene's light (a model shader's surface), else unlit */
	LOADING_SCENE_PART_LIT = 1u << 0,
	/* both faces drawn (a transparent shader's, the sky's) */
	LOADING_SCENE_PART_TWO_SIDED = 1u << 1,
	/* the texture's alpha is not coverage (a model's multipurpose bits) */
	LOADING_SCENE_PART_OPAQUE_ALPHA = 1u << 2,
};

struct loading_scene_vertex
{
	float position[3];
	float normal[3];
	float texcoord[2];
};

/* a texture a part is drawn with: its index (or LOADING_SCENE_NO_TEXTURE),
   how it meets the next map, and the vertices' texture coordinates' scale
   and offset for it */
struct loading_scene_map
{
	uint32_t texture, function;
	float scale[2], offset[2];
};

struct loading_scene_part
{
	uint32_t layer, blend, flags, map_count;
	uint32_t vertex_offset, vertex_count;     /* bytes from the block's start; vertices */
	uint32_t triangle_offset, triangle_count; /* bytes; triangles of three uint16_t */
	struct loading_scene_map maps[LOADING_SCENE_PART_MAPS];
	float tint[4];
	/* the part's space to the world's: rows of a 3x4 (rotation, then
	   translation in the last column); a sky part's translation is ignored */
	float matrix[3][4];
};

/* an Xbox texture as it lay in the game's memory: its D3D format and size
   words and its texels, every level (xbox_texture_decode.c reads them) */
struct loading_scene_texture
{
	uint32_t format, size;
	uint32_t texel_offset, texel_bytes;
};

struct loading_scene_header
{
	uint32_t magic, version, bytes;
	uint32_t part_count, texture_count;
	uint32_t part_offset, texture_offset;
	uint32_t reserved;
	/* the menu's camera as the map change began */
	float camera_position[3], camera_forward[3], camera_up[3];
	float camera_field_of_view; /* horizontal, radians */
	/* its motion: world units a second */
	float camera_velocity[3];
	/* toward the scene's light (its sun; zero: none known) */
	float light_direction[3];
	uint32_t reserved2[5];
};

static inline int loading_scene_range(uint32_t bytes, uint32_t offset, uint64_t size, uint32_t alignment)
{
	return offset % alignment == 0 && (uint64_t)offset + size <= bytes;
}

/* the block is one the presenter can draw: every offset and count within
   it, every index within its part's vertices */
static inline const struct loading_scene_header *loading_scene_validate(const void *data, size_t bytes)
{
	const struct loading_scene_header *header = data;
	const unsigned char *base = data;
	const struct loading_scene_part *parts;
	const struct loading_scene_texture *textures;
	uint32_t index, triangle;

	if (!data || bytes < sizeof(*header) || bytes > LOADING_SCENE_MAXIMUM_BYTES || ((uintptr_t)data & 3))
		return NULL;
	if (header->magic != LOADING_SCENE_MAGIC || header->version != LOADING_SCENE_VERSION ||
		header->bytes != bytes || header->part_count > LOADING_SCENE_MAXIMUM_PARTS ||
		header->texture_count > LOADING_SCENE_MAXIMUM_TEXTURES || !header->part_count)
		return NULL;
	if (!loading_scene_range((uint32_t)bytes, header->part_offset,
			(uint64_t)header->part_count * sizeof(*parts), 4) ||
		!loading_scene_range((uint32_t)bytes, header->texture_offset,
			(uint64_t)header->texture_count * sizeof(*textures), 4))
		return NULL;
	parts = (const struct loading_scene_part *)(base + header->part_offset);
	textures = (const struct loading_scene_texture *)(base + header->texture_offset);
	for (index = 0; index < header->texture_count; index++)
	{
		if (!textures[index].texel_bytes ||
			!loading_scene_range((uint32_t)bytes, textures[index].texel_offset, textures[index].texel_bytes, 4))
			return NULL;
	}
	for (index = 0; index < header->part_count; index++)
	{
		const struct loading_scene_part *part = &parts[index];
		const uint16_t *triangles;
		uint32_t map;

		if (part->map_count > LOADING_SCENE_PART_MAPS)
			return NULL;
		for (map = 0; map < part->map_count; map++)
		{
			if ((part->maps[map].texture != LOADING_SCENE_NO_TEXTURE && part->maps[map].texture >= header->texture_count) ||
				part->maps[map].function >= LOADING_SCENE_MAP_FUNCTIONS)
				return NULL;
		}
		if (part->layer > LOADING_SCENE_LAYER_WORLD || part->blend >= LOADING_SCENE_BLENDS ||
			!part->vertex_count || part->vertex_count > LOADING_SCENE_MAXIMUM_VERTICES ||
			!part->triangle_count || part->triangle_count > LOADING_SCENE_MAXIMUM_TRIANGLES)
			return NULL;
		if (!loading_scene_range((uint32_t)bytes, part->vertex_offset,
				(uint64_t)part->vertex_count * sizeof(struct loading_scene_vertex), 4) ||
			!loading_scene_range((uint32_t)bytes, part->triangle_offset,
				(uint64_t)part->triangle_count * 6, 2))
			return NULL;
		triangles = (const uint16_t *)(base + part->triangle_offset);
		for (triangle = 0; triangle < part->triangle_count * 3; triangle++)
		{
			if (triangles[triangle] >= part->vertex_count)
				return NULL;
		}
	}
	return header;
}

#endif
