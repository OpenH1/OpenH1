/*
ASTC_TEST.C

The DXT textures as ASTC (port/linux/astc/xgpu_astc.h), on the build
machine with astc-encoder, without a GPU or game data (tools/test_astc.py):

	astc_test transcode   DXT1, DXT3 and DXT5 blocks transcoded exactly where
	                      they can be (decoded by astc-encoder within a third
	                      of a step of their DXT texels), the others flagged
	                      and encoded within bounds
	astc_test cache <dir> the workers and the disk cache: keyed by the
	                      blocks, files invalidated, its size bounded, a
	                      texture better decoded left so
	astc_test choice      display.astc_textures' choice for a GPU
*/

#include "xgpu_astc.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition, ...) \
	do { if (!(condition)) { fprintf(stderr, "%s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); exit(1); } } while (0)

static struct xgpu_astc_encoder *encoder;
static uint32_t random_state = 12345;

static uint32_t random_next(void)
{
	random_state = random_state * 1664525u + 1013904223u;
	return random_state >> 8;
}

/* ---------- blocks */

static void color_block(uint8_t *block, unsigned c0, unsigned c1, uint32_t indices)
{
	block[0] = (uint8_t)c0;
	block[1] = (uint8_t)(c0 >> 8);
	block[2] = (uint8_t)c1;
	block[3] = (uint8_t)(c1 >> 8);
	block[4] = (uint8_t)indices;
	block[5] = (uint8_t)(indices >> 8);
	block[6] = (uint8_t)(indices >> 16);
	block[7] = (uint8_t)(indices >> 24);
}

/* 16 indices of 2 bits from a pattern of index values (each texel's from
choices, at random) */
static uint32_t indices_from(const unsigned *choices, int count)
{
	uint32_t indices = 0;
	int texel;

	for (texel = 0; texel < 16; texel++)
		indices |= (uint32_t)choices[random_next() % count] << (texel * 2);
	return indices;
}

/* the largest difference of an ASTC block's texels (decoded by
astc-encoder) from the DXT block's */
static int block_error(int kind, const uint8_t *dxt, const uint8_t *astc)
{
	uint8_t decoded[64], reference[64];
	int texel, most = 0;

	CHECK(xgpu_astc_decode(encoder, astc, 1, decoded), "astc-encoder cannot decode a block");
	xgpu_astc_decode_dxt(kind, dxt, reference);
	for (texel = 0; texel < 64; texel++)
	{
		int difference = abs((int)decoded[texel] - reference[texel]);

		if (difference > most)
			most = difference;
	}
	return most;
}

/* a block transcoded: exactly (expected), within a third of a step a
channel (2 at most: ASTC's 21/64 and 43/64 for DXT's thirds, and DXT's own
rounding down), or flagged */
static void transcoded(const char *name, int kind, const uint8_t *dxt, int exact)
{
	uint8_t astc[XGPU_ASTC_BLOCK_BYTES], encode = 9;
	uint32_t left = xgpu_astc_transcode(kind, dxt, 1, astc, &encode);

	CHECK(left == (uint32_t)!exact && encode == !exact, "%s: %s, not %s", name, left ? "flagged" : "transcoded",
		exact ? "transcoded" : "flagged");
	if (exact)
		CHECK(block_error(kind, dxt, astc) <= 2, "%s: transcoded %d away", name, block_error(kind, dxt, astc));
}

/* indices of these choices, the first texels' set */
static uint32_t indices_with(const unsigned *choices, int count, const unsigned *first, int firsts)
{
	uint32_t indices = indices_from(choices, count);
	int texel;

	for (texel = 0; texel < firsts; texel++)
		indices = (indices & ~(3u << (texel * 2))) | (uint32_t)first[texel] << (texel * 2);
	return indices;
}

static void test_dxt1(void)
{
	static const unsigned four[4] = { 0, 1, 2, 3 }, three[3] = { 0, 1, 2 };
	static const unsigned with_c0[2] = { 3, 0 }, with_c1[2] = { 3, 1 }, with_mid[2] = { 3, 2 };
	static const unsigned with_two[3] = { 3, 0, 1 };
	uint8_t block[8];
	int round;

	for (round = 0; round < 2000; round++)
	{
		unsigned a = random_next() & 0xffff, b = random_next() & 0xffff;
		unsigned high = a > b ? a : b, low = a > b ? b : a;

		if (high == low)
			high = low ^ 1;
		if (high < low)
		{
			unsigned swap = high;

			high = low;
			low = swap;
		}
		/* four colors (c0 > c1) */
		color_block(block, high, low, indices_from(four, 4));
		transcoded("DXT1 four colors", XGPU_ASTC_DXT1, block, 1);
		/* three colors, no transparent texel */
		color_block(block, low, high, indices_from(three, 3));
		transcoded("DXT1 three colors", XGPU_ASTC_DXT1, block, 1);
		/* 1-bit alpha: transparent black and one color, each of the three */
		color_block(block, low, high, indices_with(with_c0, 2, with_c0, 2));
		transcoded("DXT1 transparent and c0", XGPU_ASTC_DXT1, block, 1);
		color_block(block, low, high, indices_with(with_c1, 2, with_c1, 2));
		transcoded("DXT1 transparent and c1", XGPU_ASTC_DXT1, block, 1);
		color_block(block, low, high, indices_with(with_mid, 2, with_mid, 2));
		transcoded("DXT1 transparent and the mid color", XGPU_ASTC_DXT1, block, 1);
		/* transparent black and two colors: the encoder's */
		color_block(block, low, high, indices_with(with_two, 3, with_two, 3));
		transcoded("DXT1 transparent and two colors", XGPU_ASTC_DXT1, block, 0);
	}
	/* wholly transparent; transparent and black */
	color_block(block, 0x0000, 0x0001, 0xffffffffu);
	transcoded("DXT1 wholly transparent", XGPU_ASTC_DXT1, block, 1);
	color_block(block, 0x0000, 0x0001, 0xf0f0f0f0u);
	transcoded("DXT1 transparent and black", XGPU_ASTC_DXT1, block, 1);
	/* the extremes: white to black, and its thirds */
	color_block(block, 0xffff, 0x0000, 0xe4e4e4e4u);
	transcoded("DXT1 white to black", XGPU_ASTC_DXT1, block, 1);
}

static void alpha_dxt3(uint8_t *block, const uint8_t alpha[16])
{
	int texel;

	memset(block, 0, 8);
	for (texel = 0; texel < 16; texel++)
		block[texel / 2] |= (uint8_t)((alpha[texel] & 15) << ((texel & 1) * 4));
}

static void alpha_dxt5(uint8_t *block, unsigned a0, unsigned a1, const uint8_t indices[16])
{
	uint64_t bits = 0;
	int texel;

	for (texel = 0; texel < 16; texel++)
		bits |= (uint64_t)(indices[texel] & 7) << (texel * 3);
	block[0] = (uint8_t)a0;
	block[1] = (uint8_t)a1;
	for (texel = 0; texel < 6; texel++)
		block[2 + texel] = (uint8_t)(bits >> (texel * 8));
}

static void test_dxt3_dxt5(void)
{
	static const unsigned four[4] = { 0, 1, 2, 3 };
	uint8_t block[16], alpha[16], indices[16];
	int round, texel;

	for (round = 0; round < 2000; round++)
	{
		unsigned c0 = random_next() & 0xffff, c1 = random_next() & 0xffff;
		unsigned level = random_next() & 15, a0 = random_next() & 255, a1 = random_next() & 255;

		/* (either order: DXT3's and DXT5's colors are always four) */
		color_block(block + 8, c0, c1, indices_from(four, 4));
		for (texel = 0; texel < 16; texel++)
			alpha[texel] = (uint8_t)level;
		alpha_dxt3(block, alpha);
		transcoded("DXT3 alpha the same", XGPU_ASTC_DXT3, block, 1);
		alpha[random_next() % 16] = (uint8_t)((level + 1 + random_next() % 15) & 15);
		alpha_dxt3(block, alpha);
		transcoded("DXT3 alpha varying", XGPU_ASTC_DXT3, block, 0);
		/* DXT5: one index throughout (eight-level mode and six-level, with
		its 0 and 255) */
		memset(indices, (int)(random_next() & 7), sizeof(indices));
		alpha_dxt5(block, a0, a1, indices);
		transcoded("DXT5 alpha the same", XGPU_ASTC_DXT5, block, 1);
		if (a0 != a1)
		{
			indices[random_next() % 16] = (uint8_t)((indices[0] + 1) & 1);
			indices[0] = 0;
			indices[1] = 1;
			alpha_dxt5(block, a0, a1, indices);
			transcoded("DXT5 alpha varying", XGPU_ASTC_DXT5, block, 0);
		}
	}
}

/* the PSNR of a texture's ASTC blocks against its DXT texels */
static double psnr(int kind, const uint8_t *dxt, const uint8_t *astc, uint32_t count)
{
	unsigned block_bytes = xgpu_astc_dxt_block_bytes(kind);
	uint8_t decoded[64], reference[64];
	double squares = 0.0;
	uint32_t index;
	int texel;

	for (index = 0; index < count; index++)
	{
		CHECK(xgpu_astc_decode(encoder, astc + (size_t)index * 16, 1, decoded), "cannot decode");
		xgpu_astc_decode_dxt(kind, dxt + (size_t)index * block_bytes, reference);
		for (texel = 0; texel < 64; texel++)
			squares += (double)(decoded[texel] - reference[texel]) * (decoded[texel] - reference[texel]);
	}
	return squares ? 10.0 * log10(65025.0 * count * 64.0 / squares) : 99.0;
}

/* a 64x64 texture of smooth gradients (DXT's own content: color along a
line in each block, alpha ramps), of the kind; its blocks */
static uint8_t *gradient_texture(int kind, uint32_t count)
{
	unsigned block_bytes = xgpu_astc_dxt_block_bytes(kind);
	uint8_t *dxt = malloc((size_t)count * block_bytes);
	uint32_t index;
	int texel;

	for (index = 0; index < count; index++)
	{
		uint8_t *block = dxt + (size_t)index * block_bytes;
		unsigned x = index % 16, y = index / 16;
		unsigned c0 = ((31 - x) << 11) | ((y * 4) << 5) | x * 2, c1 = ((x * 2) << 11) | ((63 - y * 4) << 5) | (31 - x);
		uint8_t alpha[16], indices[16];

		if (kind == XGPU_ASTC_DXT1)
		{
			/* (with 1-bit alpha: a punched-out circle) */
			uint32_t bits = 0;

			for (texel = 0; texel < 16; texel++)
			{
				int px = (int)(x * 4) + texel % 4 - 32, py = (int)(y * 4) + texel / 4 - 32;

				bits |= (uint32_t)(px * px + py * py < 400 ? 3 : (texel % 4 + texel / 4) % 3) << (texel * 2);
			}
			color_block(block, c0 < c1 ? c0 : c1, c0 < c1 ? c1 : c0, bits);
			continue;
		}
		color_block(block + 8, c0, c1, 0x1b1b1b1bu ^ (index * 0x01010101u));
		for (texel = 0; texel < 16; texel++)
		{
			alpha[texel] = (uint8_t)((x + texel % 4) * 15 / 18);
			indices[texel] = (uint8_t)((texel % 4 + y) % 8);
		}
		if (kind == XGPU_ASTC_DXT3)
			alpha_dxt3(block, alpha);
		else
			alpha_dxt5(block, 255 - y * 8, y * 8, indices);
	}
	return dxt;
}

static void test_encoded(void)
{
	int kind;

	for (kind = XGPU_ASTC_DXT1; kind <= XGPU_ASTC_DXT5; kind++)
	{
		uint32_t count = 256;
		uint8_t *dxt = gradient_texture(kind, count), *astc = malloc((size_t)count * 16), *encode = malloc(count);
		long encoded = xgpu_astc_convert(encoder, kind, dxt, count, astc, encode);
		double quality = psnr(kind, dxt, astc, count);

		CHECK(encoded > 0, "kind %d: nothing encoded", kind);
		/* (DXT1 punched out: a few blocks encoded, the rest exact; DXT3 and DXT5
		alpha ramps across every block: 38.5 to 39.7 dB measured, the stock
		maps' textures' encoded blocks 39 to 43) */
		CHECK(quality >= (kind == XGPU_ASTC_DXT1 ? 50.0 : 36.0), "kind %d: %.2f dB", kind, quality);
		printf("kind %d: %ld of %u blocks encoded, %.2f dB\n", kind, encoded, count, quality);
		CHECK(xgpu_astc_convert(NULL, kind, dxt, count, astc, NULL) == -1, "no encoder, and blocks left");
		free(dxt);
		free(astc);
		free(encode);
	}
}

/* ---------- the cache */

static uint64_t folder_bytes(const char *folder, int *files)
{
	DIR *directory = opendir(folder);
	struct dirent *found;
	uint64_t total = 0;

	*files = 0;
	CHECK(directory, "no folder %s", folder);
	while ((found = readdir(directory)) != NULL)
	{
		char path[1200];
		struct stat status;

		if (found->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", folder, found->d_name);
		if (!stat(path, &status))
		{
			total += (uint64_t)status.st_size;
			(*files)++;
		}
	}
	closedir(directory);
	return total;
}

/* a job's blocks, waited for */
static struct xgpu_astc_job *job_run(int kind, const uint8_t *dxt, uint32_t count, void *owner)
{
	struct xgpu_astc_job *job = xgpu_astc_submit(kind, dxt, count, owner), *finished;

	CHECK(job, "no job");
	xgpu_astc_cache_wait();
	finished = xgpu_astc_finished();
	CHECK(finished == job && xgpu_astc_job_owner(job) == owner, "not the job finished");
	CHECK(!xgpu_astc_finished(), "a job finished twice");
	return job;
}

static void check_blocks(const struct xgpu_astc_job *job, int kind, const uint8_t *dxt, uint32_t count)
{
	const uint8_t *blocks = xgpu_astc_job_blocks(job);
	uint8_t *expected = malloc((size_t)count * 16);

	CHECK(blocks, "the job failed");
	CHECK(xgpu_astc_convert(encoder, kind, dxt, count, expected, NULL) > 0, "cannot convert");
	CHECK(!memcmp(blocks, expected, (size_t)count * 16), "the job's blocks are not the encoder's");
	free(expected);
}

static void test_cache(const char *folder)
{
	struct xgpu_astc_statistics statistics;
	uint32_t count = 256;
	uint8_t *dxt = gradient_texture(XGPU_ASTC_DXT3, count), *noise;
	struct xgpu_astc_job *job;
	char path[1200];
	DIR *directory;
	struct dirent *found;
	int files, index;
	uint64_t bytes;

	/* the first time encoded and written, the second read */
	xgpu_astc_cache_initialize(folder, 1u << 30);
	job = job_run(XGPU_ASTC_DXT3, dxt, count, &count);
	check_blocks(job, XGPU_ASTC_DXT3, dxt, count);
	xgpu_astc_job_free(job);
	xgpu_astc_statistics(&statistics);
	CHECK(statistics.jobs == 1 && statistics.cache_hits == 0 && statistics.blocks_encoded > 0, "not encoded");
	bytes = folder_bytes(folder, &files);
	CHECK(files == 1 && bytes == statistics.bytes_written, "%d files, %llu bytes", files, (unsigned long long)bytes);
	job = job_run(XGPU_ASTC_DXT3, dxt, count, NULL);
	check_blocks(job, XGPU_ASTC_DXT3, dxt, count);
	xgpu_astc_job_free(job);
	xgpu_astc_statistics(&statistics);
	CHECK(statistics.jobs == 2 && statistics.cache_hits == 1, "not read from the cache");

	/* keyed by the blocks: one byte changed is another texture; the same
	blocks as DXT5 another */
	dxt[37] ^= 0x40;
	job = job_run(XGPU_ASTC_DXT3, dxt, count, NULL);
	check_blocks(job, XGPU_ASTC_DXT3, dxt, count);
	xgpu_astc_job_free(job);
	job = job_run(XGPU_ASTC_DXT5, dxt, count, NULL);
	xgpu_astc_job_free(job);
	xgpu_astc_statistics(&statistics);
	CHECK(statistics.cache_hits == 1 && folder_bytes(folder, &files) && files == 3, "%d files after 3 textures", files);

	/* a damaged file: encoded again, and written again */
	directory = opendir(folder);
	while ((found = readdir(directory)) != NULL)
	{
		FILE *file;

		if (found->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", folder, found->d_name);
		file = fopen(path, "r+b");
		fseek(file, -5, SEEK_END);
		fputc(0x5a, file);
		fclose(file);
	}
	closedir(directory);
	job = job_run(XGPU_ASTC_DXT3, dxt, count, NULL);
	check_blocks(job, XGPU_ASTC_DXT3, dxt, count);
	xgpu_astc_job_free(job);
	xgpu_astc_statistics(&statistics);
	CHECK(statistics.cache_hits == 1, "a damaged file read");
	job = job_run(XGPU_ASTC_DXT3, dxt, count, NULL);
	xgpu_astc_job_free(job);
	xgpu_astc_statistics(&statistics);
	CHECK(statistics.cache_hits == 2, "the file not written again");

	/* a cancelled job: never returned */
	xgpu_astc_cancel(xgpu_astc_submit(XGPU_ASTC_DXT3, dxt, count, NULL));
	xgpu_astc_cache_wait();
	CHECK(!xgpu_astc_finished(), "a cancelled job returned");

	/* noise: better left decoded, and the file says so */
	noise = malloc((size_t)count * 16);
	for (index = 0; index < (int)count * 16; index++)
		noise[index] = (uint8_t)random_next();
	job = job_run(XGPU_ASTC_DXT3, noise, count, NULL);
	CHECK(!xgpu_astc_job_blocks(job), "noise not left decoded");
	xgpu_astc_job_free(job);
	job = job_run(XGPU_ASTC_DXT3, noise, count, NULL);
	CHECK(!xgpu_astc_job_blocks(job), "noise not left decoded the second time");
	xgpu_astc_job_free(job);
	xgpu_astc_statistics(&statistics);
	CHECK(statistics.cache_hits == 3, "the decoded verdict not kept");
	xgpu_astc_cache_shutdown();

	/* the size bound: a small cache keeps the most recent files under it */
	xgpu_astc_cache_initialize(folder, 16384);
	for (index = 0; index < 24; index++)
	{
		dxt[5] = (uint8_t)index;
		xgpu_astc_job_free(job_run(XGPU_ASTC_DXT3, dxt, count, NULL));
		usleep(20000);
	}
	bytes = folder_bytes(folder, &files);
	xgpu_astc_statistics(&statistics);
	CHECK(bytes <= 16384 && statistics.evicted > 0, "%llu bytes kept, %u deleted", (unsigned long long)bytes,
		statistics.evicted);
	/* (the most recent kept) */
	xgpu_astc_job_free(job_run(XGPU_ASTC_DXT3, dxt, count, NULL));
	xgpu_astc_statistics(&statistics);
	CHECK(statistics.cache_hits == 1, "the most recent file deleted");
	printf("cache: %d files, %llu bytes kept, %u deleted\n", files, (unsigned long long)bytes, statistics.evicted);
	xgpu_astc_cache_shutdown();

	/* no folder: encoded every time */
	xgpu_astc_cache_initialize(NULL, 0);
	job = job_run(XGPU_ASTC_DXT3, dxt, count, NULL);
	check_blocks(job, XGPU_ASTC_DXT3, dxt, count);
	xgpu_astc_job_free(job);
	xgpu_astc_cache_shutdown();
	free(noise);
	free(dxt);
}

/* ---------- the choice */

static void test_choice(void)
{
	/* setting, BC, ASTC: chosen */
	static const struct { const char *setting; int bc, astc, chosen; } cases[] =
	{
		{ "auto", 0, 1, 1 }, { "auto", 1, 1, 0 }, { "auto", 0, 0, 0 }, { "auto", 1, 0, 0 },
		{ "always", 0, 1, 1 }, { "always", 1, 1, 1 }, { "always", 1, 0, 0 },
		{ "off", 0, 1, 0 }, { "off", 1, 1, 0 },
		/* (in any case, as the game's words) */
		{ "Always", 1, 1, 1 }, { "OFF", 0, 1, 0 },
		/* (anything else is auto) */
		{ "", 0, 1, 1 }, { "yes", 1, 1, 0 }, { NULL, 0, 1, 1 },
	};
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
		CHECK(xgpu_astc_chosen(cases[index].setting, cases[index].bc, cases[index].astc) == cases[index].chosen,
			"\"%s\", BC %d, ASTC %d", cases[index].setting ? cases[index].setting : "(none)", cases[index].bc,
			cases[index].astc);
}

int main(int argc, char **argv)
{
	encoder = xgpu_astc_encoder_create(XGPU_ASTC_ENCODER_QUALITY);
	CHECK(encoder, "no encoder");
	if (argc >= 2 && !strcmp(argv[1], "transcode"))
	{
		test_dxt1();
		test_dxt3_dxt5();
		test_encoded();
	}
	else if (argc >= 3 && !strcmp(argv[1], "cache"))
	{
		test_cache(argv[2]);
	}
	else if (argc >= 2 && !strcmp(argv[1], "choice"))
	{
		test_choice();
	}
	else
	{
		fprintf(stderr, "astc_test transcode | cache <folder> | choice\n");
		return 2;
	}
	xgpu_astc_encoder_destroy(encoder);
	printf("ok\n");
	return 0;
}
