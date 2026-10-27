/*
SHADERS.C

display.hdr's draws as both backends' emitters write them
(tools/test_hdr_backends.py): one pixel program, its colour the diffuse
colour (v0) as the game's shaders hand it to the blend, tagged with each
blend the game's draws have and each tag (xgpu_hdr.h), written as the
Vulkan backend's SPIR-V (xgpu_spirv.c, reading the pixel drawn over as an
input attachment) and the Metal backend's MSL (xgpu_msl.c, reading it as
[[color(0)]]). The test makes the SPIR-V Metal's with SPIRV-Cross and runs
both on the Mac's GPU over the same pixels (harness.m): the two backends'
blends must agree.

  shaders <out dir>

writes <name>.spv and <name>.metal for each, and index.txt: a line of the
uniforms' blend colour's offsets (the SPIR-V's block, the MSL's), then a
line for each draw: its name and its tag.

Built against a source tree's emitters with msl_corpus's stand-in platform
header (tools/msl_corpus/stub), as tools/spirv_corpus is.
*/

#include "xgpu.h"
#include "xgpu_backend.h"
#include "xgpu_hdr.h"
#include "xgpu_shader.h"
#include "xgpu_spirv.h"
#include "xgpu_msl.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>

void platform_log(const char *format, ...) { (void)format; }

/* the game's blends that a tag blends in the shader (xgpu_hdr_classify's
overflowing and underflowing ones, and the others a glow may have) */
static const unsigned char blends[][3] =
{
	{ XGPU_BLEND_ONE, XGPU_BLEND_ONE, XGPU_BLEND_ADD },
	{ XGPU_BLEND_SOURCE_ALPHA, XGPU_BLEND_ONE, XGPU_BLEND_ADD },
	{ XGPU_BLEND_DESTINATION_COLOR, XGPU_BLEND_SOURCE_COLOR, XGPU_BLEND_ADD },
	{ XGPU_BLEND_SOURCE_ALPHA, XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA, XGPU_BLEND_ADD },
	{ XGPU_BLEND_ONE_MINUS_DESTINATION_COLOR, XGPU_BLEND_ONE, XGPU_BLEND_ADD },
	{ XGPU_BLEND_CONSTANT_COLOR, XGPU_BLEND_ONE_MINUS_CONSTANT_ALPHA, XGPU_BLEND_ADD },
	{ XGPU_BLEND_SOURCE_ALPHA_SATURATE, XGPU_BLEND_DESTINATION_ALPHA, XGPU_BLEND_ADD },
	{ XGPU_BLEND_ONE, XGPU_BLEND_ONE, XGPU_BLEND_REVERSE_SUBTRACT },
	{ XGPU_BLEND_ONE, XGPU_BLEND_ONE, XGPU_BLEND_SUBTRACT },
	{ XGPU_BLEND_ONE, XGPU_BLEND_ONE, XGPU_BLEND_MIN },
	{ XGPU_BLEND_ONE, XGPU_BLEND_ONE, XGPU_BLEND_MAX },
};

static const unsigned short tags[] =
{
	XGPU_HDR_CEILING(XGPU_HDR_CEILING_PAPER_WHITE),
	XGPU_HDR_CEILING(XGPU_HDR_CEILING_PAPER_WHITE) | XGPU_HDR_ALPHA_LIMIT,
	XGPU_HDR_ALPHA_LIMIT,
	XGPU_HDR_CEILING(XGPU_HDR_CEILING_WEAPON),
	XGPU_HDR_CEILING(XGPU_HDR_CEILING_HUD) | XGPU_HDR_ALPHA_LIMIT,
	XGPU_HDR_CEILING(XGPU_HDR_CEILING_LIGHT),
};

static int write_file(const char *dir, const char *name, const void *data, size_t size)
{
	char path[1024];
	FILE *file;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (!(file = fopen(path, "wb")))
	{
		perror(path);
		return 0;
	}
	fwrite(data, 1, size, file);
	fclose(file);
	return 1;
}

int main(int argc, char **argv)
{
	struct xgpu_pixel_program program;
	struct xgpu_spirv_options spirv;
	struct xgpu_msl_options msl;
	struct xgpu_spirv_module module;
	char name[64], file[80], path[1024];
	unsigned int blend, tag;
	FILE *index;
	char *source;

	if (argc != 2)
	{
		fprintf(stderr, "usage: shaders <out dir>\n");
		return 2;
	}
	if (mkdir(argv[1], 0777) && errno != EEXIST)
	{
		perror(argv[1]);
		return 1;
	}
	snprintf(path, sizeof(path), "%s/index.txt", argv[1]);
	if (!(index = fopen(path, "w")))
	{
		perror(path);
		return 1;
	}
	fprintf(index, "blend_constant %u %u\n", (unsigned)offsetof(struct xgpu_spirv_uniforms, blend_constant),
		(unsigned)offsetof(struct xgpu_msl_uniforms, blend_constant));
	memset(&spirv, 0, sizeof(spirv));
	spirv.input_samples = 1;
	memset(&msl, 0, sizeof(msl));
	for (blend = 0; blend < sizeof(blends) / sizeof(blends[0]); blend++)
	{
		for (tag = 0; tag < sizeof(tags) / sizeof(tags[0]); tag++)
		{
			/* (the final combiner's A B + (1 - A) C + D, with D the diffuse
			colour and the rest zero; its alpha the diffuse alpha) */
			memset(&program, 0, sizeof(program));
			program.final.present = 1;
			program.final.inputs[3].reg = XGPU_REGISTER_V0;
			program.final.inputs[6].reg = XGPU_REGISTER_V0;
			program.final.inputs[6].alpha = 1;
			program.hdr = (unsigned short)(tags[tag] | XGPU_HDR_BLEND(blends[blend][0], blends[blend][1],
				blends[blend][2]));
			snprintf(name, sizeof(name), "draw-blend%u-tag%u", blend, tag);
			if (!xgpu_spirv_pixel_shader(&program, &spirv, &module))
				return 1;
			snprintf(file, sizeof(file), "%s.spv", name);
			if (!write_file(argv[1], file, module.words, module.count * 4))
				return 1;
			free(module.words);
			source = xgpu_msl_pixel_shader(&program, &msl);
			if (!source)
				return 1;
			snprintf(file, sizeof(file), "%s.metal", name);
			if (!write_file(argv[1], file, source, strlen(source)))
				return 1;
			free(source);
			fprintf(index, "%s %u\n", name, (unsigned)program.hdr);
		}
	}
	fclose(index);
	return 0;
}
