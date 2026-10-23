/*
XGPU_ASTC_ENCODE.CPP

The ASTC encoder (and decoder) of xgpu_astc.h: ARM's astc-encoder
(port/third_party/astc-encoder), for the DXT blocks transcoding leaves.
The blocks are not a texture's whole: they are laid out side by side in an
image of their own (rows of up to 64 blocks, the last row's rest the last
block again), which encodes each as it would in the texture, since every
ASTC block is encoded alone. The only C++ of the hosts.
*/

#include "xgpu_astc.h"

#include "astcenc.h"

#include <stdlib.h>
#include <string.h>

/* the blocks of a row of the image */
#define ROW_BLOCKS 64

struct xgpu_astc_encoder
{
	astcenc_context *context;
};

extern "C" struct xgpu_astc_encoder *xgpu_astc_encoder_create(float quality)
{
	astcenc_config config;
	struct xgpu_astc_encoder *encoder;

	if (astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, quality, 0, &config) != ASTCENC_SUCCESS)
		return NULL;
	encoder = (struct xgpu_astc_encoder *)calloc(1, sizeof(*encoder));
	if (!encoder)
		return NULL;
	if (astcenc_context_alloc(&config, 1, &encoder->context, NULL) != ASTCENC_SUCCESS)
	{
		free(encoder);
		return NULL;
	}
	return encoder;
}

extern "C" void xgpu_astc_encoder_destroy(struct xgpu_astc_encoder *encoder)
{
	if (!encoder)
		return;
	astcenc_context_free(encoder->context);
	free(encoder);
}

/* the blocks' image's columns and rows of blocks */
static void image_blocks(uint32_t count, uint32_t *columns, uint32_t *rows)
{
	*columns = count < ROW_BLOCKS ? count : ROW_BLOCKS;
	*rows = *columns ? (count + *columns - 1) / *columns : 0;
}

extern "C" int xgpu_astc_encode(struct xgpu_astc_encoder *encoder, const uint8_t *rgba, uint32_t count, uint8_t *astc)
{
	uint32_t columns, rows, width, height, block, row;
	uint8_t *texels, *encoded;
	void *slices[1];
	astcenc_image image;
	astcenc_swizzle swizzle = { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };
	astcenc_error error;

	if (!count)
		return 1;
	image_blocks(count, &columns, &rows);
	width = columns * 4;
	height = rows * 4;
	texels = (uint8_t *)malloc((size_t)width * height * 4);
	encoded = (uint8_t *)malloc((size_t)columns * rows * XGPU_ASTC_BLOCK_BYTES);
	if (!texels || !encoded)
	{
		free(texels);
		free(encoded);
		return 0;
	}
	for (block = 0; block < columns * rows; block++)
	{
		const uint8_t *source = rgba + (size_t)(block < count ? block : count - 1) * 64;
		uint32_t x = block % columns * 4, y = block / columns * 4;

		for (row = 0; row < 4; row++)
			memcpy(texels + ((size_t)(y + row) * width + x) * 4, source + row * 16, 16);
	}
	slices[0] = texels;
	image.dim_x = width;
	image.dim_y = height;
	image.dim_z = 1;
	image.data_type = ASTCENC_TYPE_U8;
	image.data = slices;
	error = astcenc_compress_image(encoder->context, &image, &swizzle, encoded, (size_t)columns * rows *
		XGPU_ASTC_BLOCK_BYTES, 0);
	astcenc_compress_reset(encoder->context);
	if (error == ASTCENC_SUCCESS)
		memcpy(astc, encoded, (size_t)count * XGPU_ASTC_BLOCK_BYTES);
	free(texels);
	free(encoded);
	return error == ASTCENC_SUCCESS;
}

extern "C" int xgpu_astc_decode(struct xgpu_astc_encoder *encoder, const uint8_t *astc, uint32_t count, uint8_t *rgba)
{
	uint32_t columns, rows, width, height, block, row;
	uint8_t *texels, *blocks;
	void *slices[1];
	astcenc_image image;
	astcenc_swizzle swizzle = { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };
	astcenc_error error;

	if (!count)
		return 1;
	image_blocks(count, &columns, &rows);
	width = columns * 4;
	height = rows * 4;
	texels = (uint8_t *)malloc((size_t)width * height * 4);
	blocks = (uint8_t *)malloc((size_t)columns * rows * XGPU_ASTC_BLOCK_BYTES);
	if (!texels || !blocks)
	{
		free(texels);
		free(blocks);
		return 0;
	}
	memcpy(blocks, astc, (size_t)count * XGPU_ASTC_BLOCK_BYTES);
	/* (the last row's rest: the last block again) */
	for (block = count; block < columns * rows; block++)
		memcpy(blocks + (size_t)block * XGPU_ASTC_BLOCK_BYTES, astc + (size_t)(count - 1) * XGPU_ASTC_BLOCK_BYTES,
			XGPU_ASTC_BLOCK_BYTES);
	slices[0] = texels;
	image.dim_x = width;
	image.dim_y = height;
	image.dim_z = 1;
	image.data_type = ASTCENC_TYPE_U8;
	image.data = slices;
	error = astcenc_decompress_image(encoder->context, blocks, (size_t)columns * rows * XGPU_ASTC_BLOCK_BYTES, &image,
		&swizzle, 0);
	astcenc_decompress_reset(encoder->context);
	if (error == ASTCENC_SUCCESS)
	{
		for (block = 0; block < count; block++)
		{
			uint32_t x = block % columns * 4, y = block / columns * 4;

			for (row = 0; row < 4; row++)
				memcpy(rgba + (size_t)block * 64 + row * 16, texels + ((size_t)(y + row) * width + x) * 4, 16);
		}
	}
	free(texels);
	free(blocks);
	return error == ASTCENC_SUCCESS;
}
