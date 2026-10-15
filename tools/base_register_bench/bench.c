/*
BENCH.C

CPU-bound work for measuring the cost of the guest's base register
(tools/guest_base_register.py), built by tools/base_register_bench/run.py
three ways: as the ILP32 guest is built, with and without the base
register, and natively for comparison. Freestanding: no C library, no
system calls, all memory static.

The kernels are the kinds of work the game does, the libraries the port
ships among them:

- tree: lookups in a binary search tree (pointer chasing);
- list: a walk of a shuffled linked list (pointer chasing, cache misses);
- sort: a sort of records through a comparison function pointer (indirect
  calls, loads at offsets);
- inflate: zlib's inflate (port/third_party/zlib), byte loads and stores;
- hash: BLAKE2b (port/third_party/monocypher), arithmetic;
- objects: game-like updates of objects that point at their parents and
  transforms (floats at offsets through pointers).
*/

#include <stddef.h>
#include <stdint.h>

#include "zlib.h"
#include "monocypher.h"

/* the compressed input of the inflate kernel (bench_data.c, made by run.py) */
extern const unsigned char bench_deflated[];
extern const unsigned int bench_deflated_size;
extern const unsigned int bench_inflated_size;

/* ---------- what the compiler and the libraries call */

void *memcpy(void *destination, const void *source, size_t size)
{
	unsigned char *to = destination;
	const unsigned char *from = source;

	while (size--)
		*to++ = *from++;
	return destination;
}

void *memset(void *destination, int value, size_t size)
{
	unsigned char *to = destination;

	while (size--)
		*to++ = (unsigned char)value;
	return destination;
}

void *memmove(void *destination, const void *source, size_t size)
{
	unsigned char *to = destination;
	const unsigned char *from = source;

	if (to < from)
		return memcpy(destination, source, size);
	while (size--)
		to[size] = from[size];
	return destination;
}

int memcmp(const void *a, const void *b, size_t size)
{
	const unsigned char *x = a, *y = b;

	for (; size; size--, x++, y++)
	{
		if (*x != *y)
			return *x - *y;
	}
	return 0;
}

size_t strlen(const char *text)
{
	size_t length = 0;

	while (text[length])
		length++;
	return length;
}

static uint32_t random_state;

static uint32_t random_next(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

/* ---------- tree */

#define TREE_NODES 65536

struct tree_node
{
	struct tree_node *left, *right;
	uint32_t key;
	uint32_t value;
};

static struct tree_node tree_nodes[TREE_NODES];

static uint32_t kernel_tree(uint32_t scale)
{
	struct tree_node *root = NULL;
	uint32_t index, round, sum = 0;

	random_state = 12345;
	for (index = 0; index < TREE_NODES; index++)
	{
		struct tree_node *node = &tree_nodes[index], **link = &root;

		node->key = random_next();
		node->value = index;
		node->left = node->right = NULL;
		while (*link)
			link = node->key < (*link)->key ? &(*link)->left : &(*link)->right;
		*link = node;
	}
	for (round = 0; round < scale * 4; round++)
	{
		random_state = 12345;
		for (index = 0; index < TREE_NODES; index++)
		{
			uint32_t key = random_next();
			struct tree_node *node = root;

			while (node && node->key != key)
				node = key < node->key ? node->left : node->right;
			sum += node ? node->value : 0;
		}
	}
	return sum;
}

/* ---------- list */

#define LIST_NODES (1 << 20)

struct list_node
{
	struct list_node *next;
	uint32_t value;
};

static struct list_node list_nodes[LIST_NODES];
static uint32_t list_order[LIST_NODES];

static uint32_t kernel_list(uint32_t scale)
{
	struct list_node *node;
	uint32_t index, round, sum = 0;

	random_state = 777;
	for (index = 0; index < LIST_NODES; index++)
		list_order[index] = index;
	for (index = LIST_NODES - 1; index > 0; index--)
	{
		uint32_t other = random_next() % (index + 1), swap = list_order[index];

		list_order[index] = list_order[other];
		list_order[other] = swap;
	}
	for (index = 0; index < LIST_NODES; index++)
	{
		list_nodes[list_order[index]].value = index;
		list_nodes[list_order[index]].next = index + 1 < LIST_NODES ? &list_nodes[list_order[index + 1]] : NULL;
	}
	for (round = 0; round < scale; round++)
	{
		for (node = &list_nodes[list_order[0]]; node; node = node->next)
			sum += node->value;
	}
	return sum;
}

/* ---------- sort */

#define SORT_RECORDS 65536

struct record
{
	uint32_t key;
	float weight;
	const char *name;
};

static struct record records[SORT_RECORDS];
static struct record scratch[SORT_RECORDS];

typedef int (*compare_function)(const struct record *, const struct record *);

static int compare_key(const struct record *a, const struct record *b)
{
	return a->key < b->key ? -1 : a->key > b->key;
}

static int compare_weight(const struct record *a, const struct record *b)
{
	return a->weight < b->weight ? -1 : a->weight > b->weight;
}

/* (volatile: called through memory, as the game's callbacks are) */
static compare_function volatile comparisons[2] = { compare_key, compare_weight };

static void merge_sort(struct record *items, struct record *temporary, uint32_t count, compare_function compare)
{
	uint32_t half = count / 2, left = 0, right = half, out = 0;

	if (count < 2)
		return;
	merge_sort(items, temporary, half, compare);
	merge_sort(items + half, temporary, count - half, compare);
	while (left < half && right < count)
		temporary[out++] = compare(&items[right], &items[left]) < 0 ? items[right++] : items[left++];
	while (left < half)
		temporary[out++] = items[left++];
	while (right < count)
		temporary[out++] = items[right++];
	memcpy(items, temporary, count * sizeof(*items));
}

static uint32_t kernel_sort(uint32_t scale)
{
	uint32_t round, index, sum = 0;

	for (round = 0; round < scale; round++)
	{
		random_state = 99 + round;
		for (index = 0; index < SORT_RECORDS; index++)
		{
			records[index].key = random_next();
			records[index].weight = (float)(random_next() & 0xffff) / 65536.0f;
			records[index].name = "record";
		}
		merge_sort(records, scratch, SORT_RECORDS, comparisons[round & 1]);
		sum += records[SORT_RECORDS / 2].key;
	}
	return sum;
}

/* ---------- inflate */

static unsigned char inflate_output[1 << 20];
static unsigned char inflate_arena[64 * 1024];
static uint32_t inflate_arena_used;

static voidpf arena_alloc(voidpf opaque, uInt items, uInt size)
{
	uint32_t bytes = (items * size + 15) & ~15u;
	void *result = inflate_arena + inflate_arena_used;

	(void)opaque;
	if (inflate_arena_used + bytes > sizeof(inflate_arena))
		return Z_NULL;
	inflate_arena_used += bytes;
	return result;
}

static void arena_free(voidpf opaque, voidpf address)
{
	(void)opaque;
	(void)address;
}

static uint32_t kernel_inflate(uint32_t scale)
{
	uint32_t round, sum = 0;

	for (round = 0; round < scale; round++)
	{
		z_stream stream;

		memset(&stream, 0, sizeof(stream));
		inflate_arena_used = 0;
		stream.zalloc = arena_alloc;
		stream.zfree = arena_free;
		if (inflateInit(&stream) != Z_OK)
			return 0;
		stream.next_in = (Bytef *)bench_deflated;
		stream.avail_in = bench_deflated_size;
		stream.next_out = inflate_output;
		stream.avail_out = sizeof(inflate_output);
		if (inflate(&stream, Z_FINISH) != Z_STREAM_END || stream.total_out != bench_inflated_size)
			return 0;
		inflateEnd(&stream);
		sum += inflate_output[round % bench_inflated_size];
	}
	return sum;
}

/* ---------- hash */

static uint8_t hash_input[1 << 18];

static uint32_t kernel_hash(uint32_t scale)
{
	uint8_t digest[64];
	uint32_t round, index, sum = 0;

	for (index = 0; index < sizeof(hash_input); index++)
		hash_input[index] = (uint8_t)(index * 31 + 7);
	for (round = 0; round < scale; round++)
	{
		hash_input[0] = (uint8_t)round;
		crypto_blake2b(digest, sizeof(digest), hash_input, sizeof(hash_input));
		sum += digest[round & 63];
	}
	return sum;
}

/* ---------- objects */

#define OBJECTS 16384

struct transform
{
	float rotation[3][3];
	float translation[3];
};

struct object
{
	struct object *parent;
	struct transform *transform;
	float position[3];
	float velocity[3];
	float world[3];
	uint32_t flags;
};

static struct object objects[OBJECTS];
static struct transform transforms[OBJECTS / 4];

static uint32_t kernel_objects(uint32_t scale)
{
	uint32_t index, round, sum = 0;

	random_state = 4242;
	for (index = 0; index < OBJECTS / 4; index++)
	{
		struct transform *transform = &transforms[index];
		int row, column;

		for (row = 0; row < 3; row++)
		{
			for (column = 0; column < 3; column++)
				transform->rotation[row][column] = row == column ? 0.999f : 0.01f * (float)(row - column);
			transform->translation[row] = (float)(random_next() & 255);
		}
	}
	for (index = 0; index < OBJECTS; index++)
	{
		struct object *object = &objects[index];

		object->parent = index ? &objects[random_next() % index] : NULL;
		object->transform = &transforms[random_next() % (OBJECTS / 4)];
		object->position[0] = (float)(random_next() & 1023);
		object->position[1] = (float)(random_next() & 1023);
		object->position[2] = (float)(random_next() & 1023);
		object->velocity[0] = object->velocity[1] = 0.5f;
		object->velocity[2] = -0.25f;
		object->flags = random_next() & 1;
	}
	for (round = 0; round < scale * 16; round++)
	{
		for (index = 0; index < OBJECTS; index++)
		{
			struct object *object = &objects[index];
			const struct transform *transform = object->transform;
			const float *origin = object->parent ? object->parent->world : transform->translation;
			int axis;

			if (object->flags)
			{
				for (axis = 0; axis < 3; axis++)
					object->position[axis] += object->velocity[axis] * (1.0f / 30.0f);
			}
			for (axis = 0; axis < 3; axis++)
			{
				object->world[axis] = origin[axis] + transform->rotation[axis][0] * object->position[0] +
					transform->rotation[axis][1] * object->position[1] + transform->rotation[axis][2] * object->position[2];
			}
		}
		sum += (uint32_t)objects[round % OBJECTS].world[0];
	}
	return sum;
}

/* ---------- entry */

typedef uint32_t (*kernel_function)(uint32_t);

static const kernel_function kernels[] =
{
	kernel_tree, kernel_list, kernel_sort, kernel_inflate, kernel_hash, kernel_objects,
};

/* runs kernel (an index of bench_kernel_names in run.py) with that scale;
returns a checksum, the same for every build */
uint32_t bench_run(uint32_t kernel, uint32_t scale)
{
	if (kernel >= sizeof(kernels) / sizeof(kernels[0]))
		return 0;
	return kernels[kernel](scale);
}
