/*
XGPU_METAL_TEST.M

The Metal backend's tests (ninja macos_metal_test; tools/test_macos_metal.py
runs them): each drives xgpu_metal.h's functions, or the command stream's
backend table, with calls written here, draws with shaders written here
(the MSL emitter's take their place when they come), reads the pixels back
and checks them.

	xgpu_metal_test [--archive file] [--expect-archive]
		the tests; with a pipeline archive (the second run of the same
		archive expects every pipeline from it)
	xgpu_metal_test --window seconds [--resize]
		a window: frames drawn as fast as the presentation takes them, and
		the pacing measured (with HALO_METAL_STATS for the presentation's
		own log)
*/

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

/* (host.h's host_page_size is Mach's too: not used here) */
#define host_page_size host_page_size_of_guest_memory
#include "host.h"
#undef host_page_size
#include "xgpu_metal.h"
#include "xgpu_metal_internal.h"
#if __has_include("host_render_stream.h")
#include "host_render_stream.h"
#include "xgpu_metal_stream.h"
#define TEST_STREAM 1
#endif

#include <SDL3/SDL.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ---------- what the host gives the backend, here */

uintptr_t host_guest_base;
void (*host_memory_remapped)(uint32_t address, uint32_t size);

void host_logf(int priority, const char *format, ...)
{
	va_list arguments;

	if (priority < HOST_LOG_WARN && !getenv("TEST_VERBOSE"))
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

static void *window_object;

void *host_sdl_handle_get(uint32_t handle, int type)
{
	(void)type;
	return handle == 1 ? window_object : NULL;
}

int host_memory_readable_extent(uint32_t address, uint32_t size, uint32_t limit_start, uint32_t limit_end,
	uint32_t *start, uint32_t *end);

/* the write tracking (host_memory_watch.c): nothing is protected here; a
test says its texels changed by counting a generation */
static uint32_t watch_generation = 1;

void host_memory_watch_protect(uint32_t address, uint32_t size)
{
	(void)address;
	(void)size;
}

uint32_t host_memory_watch_generation(uint32_t address, uint32_t size)
{
	(void)address;
	(void)size;
	return watch_generation;
}

uint32_t host_memory_watch_serial(void)
{
	return watch_generation;
}

#ifdef TEST_STREAM
void host_render_stream_set_backend(const struct host_render_backend *backend)
{
	(void)backend;
}

const struct xgpu_stream_ring *host_render_stream_ring(void)
{
	return NULL;
}

int host_render_stream_playing(void)
{
	return 0;
}
#endif

/* host_metal.m */
int host_metal_initialize(void);
int host_metal_window_open(uint32_t window);

/* ---------- checks */

static int failures, checks;
static const char *test_name;

#define CHECK(condition, ...) check((condition) != 0, __LINE__, __VA_ARGS__)

static void check(int passed, int line, const char *format, ...)
{
	va_list arguments;

	checks++;
	if (passed)
		return;
	failures++;
	fprintf(stderr, "FAIL %s (line %d): ", test_name, line);
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
}

static int near(uint32_t pixel, uint32_t expected, int tolerance)
{
	int shift;

	for (shift = 0; shift < 32; shift += 8)
	{
		int a = (int)((pixel >> shift) & 0xff), b = (int)((expected >> shift) & 0xff);

		if (abs(a - b) > tolerance)
			return 0;
	}
	return 1;
}

/* ---------- the test's shaders and draws */

/* the colour, all its channels */
#define CLEAR_COLOR (XGPU_METAL_CLEAR_COLOR | XGPU_METAL_CLEAR_CHANNELS)

#define SIZE 64

static uint32_t vertex_shader, pixel_shader, white;

struct vertex
{
	float x, y, z, w;
	/* D3DCOLOR: 0xAARRGGBB, bytes b, g, r, a */
	uint32_t color;
	float u, v;
};

struct uniforms
{
	float tint[4];
	/* x: sample texture 0 */
	float mode[4];
};

static NSString *shader_source(void)
{
	return [NSString stringWithFormat:
		@"#include <metal_stdlib>\n"
		"using namespace metal;\n"
		"struct vertex_in { float4 position [[attribute(0)]]; float4 color [[attribute(3)]]; float2 uv [[attribute(9)]]; };\n"
		"struct vertex_out { float4 position [[position]]; float4 color; float2 uv; };\n"
		"struct uniforms { float4 tint; float4 mode; };\n"
		"vertex vertex_out test_vertex(vertex_in in [[stage_in]], constant float4 *c [[buffer(%d)]])\n"
		"{\n"
		"\tvertex_out out;\n"
		"\tout.position = in.position + float4(c[0].xy, 0.0, 0.0);\n"
		"\tout.color = in.color;\n"
		"\tout.uv = in.uv;\n"
		"\treturn out;\n"
		"}\n"
		"fragment float4 test_pixel(vertex_out in [[stage_in]], constant uniforms &u [[buffer(%d)]],\n"
		"\ttexture2d<float> t [[texture(%d)]], sampler s [[sampler(%d)]])\n"
		"{\n"
		"\tfloat4 texel = u.mode.x > 0.5 ? t.sample(s, in.uv) : float4(1.0);\n"
		"\treturn texel * in.color * u.tint;\n"
		"}\n",
		XGPU_METAL_BUFFER_VERTEX_CONSTANTS, XGPU_METAL_BUFFER_DRAW_UNIFORMS, XGPU_METAL_TEXTURE_STAGE(0),
		XGPU_METAL_SAMPLER_STAGE(0)];
}

static void shaders_make(void)
{
	NSString *source = shader_source();

	vertex_shader = xgpu_metal_shader_create(source.UTF8String, strlen(source.UTF8String), "test_vertex");
	pixel_shader = xgpu_metal_shader_create(source.UTF8String, strlen(source.UTF8String), "test_pixel");
}

static uint32_t target_make(uint32_t format, uint32_t width, uint32_t height, uint32_t samples)
{
	struct xgpu_metal_texture_description description;

	memset(&description, 0, sizeof(description));
	description.type = XGPU_METAL_TEXTURE_2D;
	description.format = (uint8_t)format;
	description.usage = XGPU_METAL_USAGE_SAMPLED | XGPU_METAL_USAGE_TARGET;
	description.samples = (uint8_t)samples;
	description.width = (uint16_t)width;
	description.height = (uint16_t)height;
	description.depth_or_layers = 1;
	description.levels = 1;
	return xgpu_metal_texture_create(&description);
}

static uint32_t texture_make(uint32_t type, uint32_t format, uint32_t width, uint32_t height, uint32_t layers)
{
	struct xgpu_metal_texture_description description;

	memset(&description, 0, sizeof(description));
	description.type = (uint8_t)type;
	description.format = (uint8_t)format;
	description.usage = XGPU_METAL_USAGE_SAMPLED;
	description.width = (uint16_t)width;
	description.height = (uint16_t)height;
	description.depth_or_layers = (uint16_t)layers;
	description.levels = 1;
	return xgpu_metal_texture_create(&description);
}

static void pipeline_default(struct xgpu_metal_pipeline *pipeline, uint32_t depth_format)
{
	memset(pipeline, 0, sizeof(*pipeline));
	pipeline->vertex_shader = vertex_shader;
	pipeline->pixel_shader = pixel_shader;
	pipeline->attributes[0] = (struct xgpu_metal_vertex_attribute){ XGPU_METAL_ATTRIBUTE_FLOAT4, 0, 0 };
	pipeline->attributes[3] = (struct xgpu_metal_vertex_attribute){ XGPU_METAL_ATTRIBUTE_D3DCOLOR, 0, 16 };
	pipeline->attributes[9] = (struct xgpu_metal_vertex_attribute){ XGPU_METAL_ATTRIBUTE_FLOAT2, 0, 20 };
	pipeline->strides[0] = sizeof(struct vertex);
	pipeline->color_mask = XGPU_WRITE_RED | XGPU_WRITE_GREEN | XGPU_WRITE_BLUE | XGPU_WRITE_ALPHA;
	pipeline->color_format = XGPU_METAL_FORMAT_BGRA8;
	pipeline->depth_format = (uint8_t)depth_format;
	pipeline->depth_function = XGPU_COMPARE_LESS_EQUAL;
	pipeline->samples = 1;
}

static void dynamic_default(uint32_t width, uint32_t height)
{
	struct xgpu_metal_dynamic_state state;

	memset(&state, 0, sizeof(state));
	state.viewport[2] = (int32_t)width;
	state.viewport[3] = (int32_t)height;
	state.scissor[2] = (int32_t)width;
	state.scissor[3] = (int32_t)height;
	state.depth_range[1] = 1.0f;
	xgpu_metal_set_dynamic_state(&state);
}

static void uniforms_set(float r, float g, float b, float a, int textured)
{
	struct uniforms uniforms = { { r, g, b, a }, { textured ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f } };

	xgpu_metal_set_draw_uniforms(&uniforms, sizeof(uniforms));
}

static void sampler_point(struct xgpu_metal_sampler *sampler)
{
	memset(sampler, 0, sizeof(*sampler));
	sampler->address_u = sampler->address_v = sampler->address_w = XGPU_METAL_ADDRESS_CLAMP;
	sampler->max_anisotropy = 1;
}

/* a rectangle in the target's pixels as two triangles (clip space: y up) */
static void rectangle_vertices(struct vertex *vertices, float left, float top, float right, float bottom, float z,
	uint32_t color)
{
	float x0 = 2.0f * left / SIZE - 1.0f, x1 = 2.0f * right / SIZE - 1.0f;
	float y0 = 1.0f - 2.0f * top / SIZE, y1 = 1.0f - 2.0f * bottom / SIZE;
	struct vertex corners[4] = {
		{ x0, y0, z, 1.0f, color, 0.0f, 0.0f }, { x1, y0, z, 1.0f, color, 1.0f, 0.0f },
		{ x0, y1, z, 1.0f, color, 0.0f, 1.0f }, { x1, y1, z, 1.0f, color, 1.0f, 1.0f },
	};

	vertices[0] = corners[0];
	vertices[1] = corners[1];
	vertices[2] = corners[2];
	vertices[3] = corners[2];
	vertices[4] = corners[1];
	vertices[5] = corners[3];
}

static void draw_rectangle(float left, float top, float right, float bottom, float z, uint32_t color)
{
	struct vertex vertices[6];

	rectangle_vertices(vertices, left, top, right, bottom, z, color);
	xgpu_metal_set_stream(0, xgpu_metal_stream(vertices, sizeof(vertices)));
	xgpu_metal_draw(XGPU_METAL_PRIMITIVE_TRIANGLES, 6, 0, 0, (struct xgpu_metal_source){ 0, 0, 0, 0 });
}

static uint32_t pixels[SIZE * SIZE];

static void read_target(uint32_t texture)
{
	memset(pixels, 0, sizeof(pixels));
	CHECK(xgpu_metal_read_pixels(texture, 0, 0, 0, 0, SIZE, SIZE, pixels, SIZE * 4), "read the pixels");
}

static uint32_t pixel(int x, int y)
{
	return pixels[y * SIZE + x];
}

/* the statistics since the last call, added to the run's */
static struct xgpu_metal_statistics statistics_total;

static void statistics_take(struct xgpu_metal_statistics *statistics)
{
	xgpu_metal_statistics(statistics);
	statistics_total.pipelines_made += statistics->pipelines_made;
	statistics_total.pipelines_from_archive += statistics->pipelines_from_archive;
	statistics_total.pipeline_seconds += statistics->pipeline_seconds;
}

static void begin(const char *name)
{
	struct xgpu_metal_statistics statistics;

	test_name = name;
	statistics_take(&statistics);
}

static void frame_begin(uint32_t target, uint32_t depth)
{
	struct xgpu_metal_sampler sampler;

	xgpu_metal_frame_begin();
	xgpu_metal_set_targets(target, 0, 0, depth, 1);
	dynamic_default(SIZE, SIZE);
	sampler_point(&sampler);
	xgpu_metal_set_texture(0, white, &sampler);
	uniforms_set(1.0f, 1.0f, 1.0f, 1.0f, 0);
}

/* ---------- the tests */

static void test_clears(void)
{
	uint32_t target = target_make(XGPU_METAL_FORMAT_BGRA8, SIZE, SIZE, 0);
	uint32_t other = target_make(XGPU_METAL_FORMAT_BGRA8, SIZE, SIZE, 0);
	uint32_t depth = target_make(XGPU_METAL_FORMAT_D24S8, SIZE, SIZE, 0);
	float red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 }, blue[4] = { 0, 0, 1, 0.5f };
	int32_t one[1][4] = { { 8, 8, 16, 16 } };
	int32_t two[2][4] = { { 40, 0, 8, 8 }, { 56, 56, 100, 100 } };
	struct xgpu_metal_statistics statistics;
	float depths[SIZE * SIZE];

	begin("clears");
	frame_begin(target, depth);
	xgpu_metal_clear(NULL, 0, CLEAR_COLOR | XGPU_METAL_CLEAR_DEPTH | XGPU_METAL_CLEAR_STENCIL, red, 1.0f, 0);
	xgpu_metal_clear((const int32_t (*)[4])one, 1, CLEAR_COLOR | XGPU_METAL_CLEAR_DEPTH, green, 0.25f, 0);
	xgpu_metal_clear((const int32_t (*)[4])two, 2, CLEAR_COLOR, blue, 0.0f, 0);
	/* a whole clear of a target nothing is drawn into before the targets
	change is still done */
	xgpu_metal_set_targets(other, 0, 0, 0, 1);
	xgpu_metal_clear(NULL, 0, CLEAR_COLOR, green, 0.0f, 0);
	xgpu_metal_set_targets(target, 0, 0, depth, 1);
	xgpu_metal_frame_wait(xgpu_metal_frame_end(0));
	read_target(target);
	CHECK(pixel(0, 0) == 0xffff0000, "the whole clear: %08x", pixel(0, 0));
	CHECK(pixel(8, 8) == 0xff00ff00 && pixel(23, 23) == 0xff00ff00, "the rectangle: %08x %08x", pixel(8, 8), pixel(23, 23));
	CHECK(pixel(24, 24) == 0xffff0000 && pixel(7, 8) == 0xffff0000, "outside it: %08x %08x", pixel(24, 24), pixel(7, 8));
	CHECK(near(pixel(44, 4), 0x800000ff, 1) && near(pixel(63, 63), 0x800000ff, 1) && pixel(55, 63) == 0xffff0000,
		"two rectangles, one past the edge: %08x %08x %08x", pixel(44, 4), pixel(63, 63), pixel(55, 63));
	CHECK(xgpu_metal_read_pixels(depth, 0, 0, 0, 0, SIZE, SIZE, depths, SIZE * 4), "read the depth");
	CHECK(depths[0] == 1.0f && fabsf(depths[10 * SIZE + 10] - 0.25f) < 1e-6f, "the depth: %f %f", depths[0],
		depths[10 * SIZE + 10]);
	read_target(other);
	CHECK(pixel(30, 30) == 0xff00ff00, "the target cleared without a draw: %08x", pixel(30, 30));
	/* a channel alone (the game clears alpha so): drawn even whole */
	frame_begin(target, 0);
	xgpu_metal_clear(NULL, 0, XGPU_METAL_CLEAR_COLOR | XGPU_METAL_CLEAR_ALPHA, (float[4]){ 0, 1, 0, 0.5f }, 0, 0);
	xgpu_metal_frame_wait(xgpu_metal_frame_end(0));
	read_target(target);
	CHECK(near(pixel(0, 0), 0x80ff0000, 1) && near(pixel(10, 10), 0x8000ff00, 1), "alpha alone: %08x %08x", pixel(0, 0),
		pixel(10, 10));
	statistics_take(&statistics);
	CHECK(statistics.clears_as_loads == 2 && statistics.clears_as_quads == 4, "loads %u, quads %u",
		statistics.clears_as_loads, statistics.clears_as_quads);
	xgpu_metal_texture_destroy(target);
	xgpu_metal_texture_destroy(other);
	xgpu_metal_texture_destroy(depth);
}

static void test_textured_quad(void)
{
	uint32_t target = target_make(XGPU_METAL_FORMAT_BGRA8, SIZE, SIZE, 0);
	uint32_t texture = texture_make(XGPU_METAL_TEXTURE_2D, XGPU_METAL_FORMAT_BGRA8, 4, 4, 1);
	uint32_t texels[16], x, y;
	struct xgpu_metal_pipeline pipeline;
	struct xgpu_metal_sampler sampler;
	float offset[4] = { 0, 0, 0, 0 };
	int good = 1;

	begin("textured quad");
	for (y = 0; y < 4; y++)
	{
		for (x = 0; x < 4; x++)
			texels[y * 4 + x] = 0xff000000u | (x * 0x40u << 16) | (y * 0x40u << 8) | 0x20u;
	}
	xgpu_metal_texture_upload(texture, 0, 0, 0, 0, 0, 4, 4, 1, texels, 16, 0);
	frame_begin(target, 0);
	pipeline_default(&pipeline, 0);
	xgpu_metal_set_pipeline(&pipeline);
	xgpu_metal_set_vertex_constants(0, 1, offset);
	sampler_point(&sampler);
	xgpu_metal_set_texture(0, texture, &sampler);
	uniforms_set(1, 1, 1, 1, 1);
	draw_rectangle(0, 0, SIZE, SIZE, 0.5f, 0xffffffff);
	xgpu_metal_frame_wait(xgpu_metal_frame_end(0));
	read_target(target);
	for (y = 0; y < 4; y++)
	{
		for (x = 0; x < 4; x++)
		{
			if (pixel((int)x * 16 + 8, (int)y * 16 + 8) != texels[y * 4 + x])
				good = 0;
		}
	}
	CHECK(good, "each texel covers its 16x16 square: %08x %08x", pixel(8, 8), pixel(56, 56));

	/* the vertex constants move it; the draw uniforms tint it; D3DCOLOR's
	bytes are read as BGRA */
	frame_begin(target, 0);
	xgpu_metal_clear(NULL, 0, CLEAR_COLOR, (float[4]){ 0, 0, 0, 1 }, 0, 0);
	xgpu_metal_set_pipeline(&pipeline);
	offset[0] = 0.5f;
	xgpu_metal_set_vertex_constants(0, 1, offset);
	uniforms_set(1, 0.5f, 1, 1, 0);
	draw_rectangle(0, 0, 32, SIZE, 0.5f, 0xff4080c0);
	xgpu_metal_frame_wait(xgpu_metal_frame_end(0));
	read_target(target);
	CHECK(pixel(8, 8) == 0xff000000 && near(pixel(24, 8), 0xff4040c0, 1) && near(pixel(40, 8), 0xff4040c0, 1) &&
		pixel(56, 8) == 0xff000000, "moved by c[0] and tinted: %08x %08x %08x %08x", pixel(8, 8), pixel(24, 8),
		pixel(40, 8), pixel(56, 8));
	offset[0] = 0.0f;
	xgpu_metal_set_vertex_constants(0, 1, offset);
	xgpu_metal_texture_destroy(texture);
	xgpu_metal_texture_destroy(target);
}

/* each format: its texels, and the colour a texel reads as (0xAARRGGBB) */
static void test_formats(void)
{
	static const struct
	{
		const char *name;
		uint32_t format;
		uint32_t width, bytes;
		unsigned char data[16];
		uint32_t expected;
	} cases[] = {
		{ "BGRA8", XGPU_METAL_FORMAT_BGRA8, 1, 4, { 0x30, 0x60, 0x90, 0x80 }, 0x80906030 },
		{ "BGRX8", XGPU_METAL_FORMAT_BGRX8, 1, 4, { 0x30, 0x60, 0x90, 0x00 }, 0xff906030 },
		{ "R5G6B5 red", XGPU_METAL_FORMAT_R5G6B5, 1, 2, { 0x00, 0xf8 }, 0xffff0000 },
		{ "R5G6B5 blue", XGPU_METAL_FORMAT_R5G6B5, 1, 2, { 0x1f, 0x00 }, 0xff0000ff },
		{ "A1R5G5B5", XGPU_METAL_FORMAT_A1R5G5B5, 1, 2, { 0x00, 0x7c }, 0x00ff0000 },
		{ "A1R5G5B5 alpha", XGPU_METAL_FORMAT_A1R5G5B5, 1, 2, { 0x1f, 0x80 }, 0xff0000ff },
		{ "X1R5G5B5", XGPU_METAL_FORMAT_X1R5G5B5, 1, 2, { 0xe0, 0x03 }, 0xff00ff00 },
		{ "A4R4G4B4", XGPU_METAL_FORMAT_A4R4G4B4, 1, 2, { 0x00, 0x8f }, 0x88ff0000 },
		{ "A4R4G4B4 green", XGPU_METAL_FORMAT_A4R4G4B4, 1, 2, { 0xf0, 0xf0 }, 0xff00ff00 },
		{ "L8", XGPU_METAL_FORMAT_L8, 1, 1, { 0x80 }, 0xff808080 },
		{ "A8", XGPU_METAL_FORMAT_A8, 1, 1, { 0x40 }, 0x40000000 },
		{ "A8L8", XGPU_METAL_FORMAT_A8L8, 1, 2, { 0xc0, 0x40 }, 0x40c0c0c0 },
		/* a 4x4 block: colour 0 red, colour 1 blue, every index 0 */
		{ "DXT1", XGPU_METAL_FORMAT_DXT1, 4, 8, { 0x00, 0xf8, 0x1f, 0x00, 0, 0, 0, 0 }, 0xffff0000 },
		/* explicit alpha 8 of 15 everywhere */
		{ "DXT3", XGPU_METAL_FORMAT_DXT3, 4, 16,
			{ 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x00, 0xf8, 0x1f, 0x00, 0, 0, 0, 0 }, 0x88ff0000 },
		/* alpha 0 0x80, every alpha index 0 */
		{ "DXT5", XGPU_METAL_FORMAT_DXT5, 4, 16,
			{ 0x80, 0x00, 0, 0, 0, 0, 0, 0, 0x00, 0xf8, 0x1f, 0x00, 0, 0, 0, 0 }, 0x80ff0000 },
	};
	uint32_t target = target_make(XGPU_METAL_FORMAT_BGRA8, SIZE, SIZE, 0);
	struct xgpu_metal_pipeline pipeline;
	struct xgpu_metal_sampler sampler;
	unsigned int index;

	begin("formats");
	pipeline_default(&pipeline, 0);
	sampler_point(&sampler);
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
	{
		unsigned char texels[64];
		uint32_t texture, row = cases[index].width == 4 ? cases[index].bytes : cases[index].bytes * 4, texel;

		/* 4x4 texels of the same (or one block) */
		for (texel = 0; texel < 16 && cases[index].width == 1; texel++)
			memcpy(texels + texel * cases[index].bytes, cases[index].data, cases[index].bytes);
		if (cases[index].width == 4)
			memcpy(texels, cases[index].data, cases[index].bytes);
		texture = texture_make(XGPU_METAL_TEXTURE_2D, cases[index].format, 4, 4, 1);
		CHECK(texture, "make %s", cases[index].name);
		xgpu_metal_texture_upload(texture, 0, 0, 0, 0, 0, 4, 4, 1, texels, row, 0);
		frame_begin(target, 0);
		xgpu_metal_set_pipeline(&pipeline);
		xgpu_metal_set_texture(0, texture, &sampler);
		uniforms_set(1, 1, 1, 1, 1);
		draw_rectangle(0, 0, SIZE, SIZE, 0.5f, 0xffffffff);
		xgpu_metal_frame_wait(xgpu_metal_frame_end(0));
		read_target(target);
		CHECK(near(pixel(30, 30), cases[index].expected, 2), "%s reads as %08x, not %08x", cases[index].name,
			pixel(30, 30), cases[index].expected);
		xgpu_metal_texture_destroy(texture);
	}
	xgpu_metal_texture_destroy(target);
}

static void test_pipelines(void)
{
	uint32_t target = target_make(XGPU_METAL_FORMAT_BGRA8, SIZE, SIZE, 0);
	uint32_t depth = target_make(XGPU_METAL_FORMAT_D24S8, SIZE, SIZE, 0);
	struct xgpu_metal_pipeline pipeline;
	struct xgpu_metal_statistics statistics;
	float black[4] = { 0, 0, 0, 1 };

	begin("pipelines");
	statistics_take(&statistics);
	frame_begin(target, depth);
	xgpu_metal_clear(NULL, 0, CLEAR_COLOR | XGPU_METAL_CLEAR_DEPTH | XGPU_METAL_CLEAR_STENCIL, black, 1.0f, 0);
	pipeline_default(&pipeline, XGPU_METAL_FORMAT_D24S8);
	pipeline.depth_test = 1;
	pipeline.depth_write = 1;
	pipeline.depth_function = XGPU_COMPARE_LESS;
	xgpu_metal_set_pipeline(&pipeline);
	/* the nearer first: the farther fails the depth test */
	draw_rectangle(0, 0, 32, 32, 0.25f, 0xffff0000);
	draw_rectangle(0, 0, 32, 32, 0.75f, 0xff00ff00);
	/* raster state is the encoder's: the same pipeline state */
	pipeline.cull = 1;
	pipeline.depth_bias = 1;
	xgpu_metal_set_pipeline(&pipeline);
	draw_rectangle(32, 0, 64, 32, 0.5f, 0xff0000ff);
	/* blending is the pipeline state's: another; with depth off, another
	depth-stencil state */
	pipeline.cull = 0;
	pipeline.depth_test = 0;
	pipeline.depth_write = 0;
	pipeline.blend = 1;
	pipeline.blend_source = XGPU_BLEND_ONE;
	pipeline.blend_destination = XGPU_BLEND_ONE;
	xgpu_metal_set_pipeline(&pipeline);
	draw_rectangle(0, 32, 64, 64, 0.5f, 0xff800000);
	draw_rectangle(0, 32, 32, 64, 0.5f, 0xff008000);
	/* a write mask */
	pipeline.blend = 0;
	pipeline.color_mask = XGPU_WRITE_BLUE;
	xgpu_metal_set_pipeline(&pipeline);
	draw_rectangle(48, 48, 64, 64, 0.5f, 0xffffffff);
	xgpu_metal_frame_wait(xgpu_metal_frame_end(0));
	read_target(target);
	statistics_take(&statistics);
	CHECK(pixel(8, 8) == 0xffff0000, "the depth test: %08x", pixel(8, 8));
	CHECK(pixel(40, 8) == 0xff0000ff, "culled (back) but facing: %08x", pixel(40, 8));
	CHECK(near(pixel(8, 40), 0xff808000, 1) && near(pixel(40, 40), 0xff800000, 1), "added: %08x %08x", pixel(8, 40),
		pixel(40, 40));
	CHECK(near(pixel(56, 56), 0xff8000ff, 1), "blue alone written: %08x", pixel(56, 56));
	CHECK(statistics.pipelines_made == 3, "pipeline states: %u made, %u of them from the archive",
		statistics.pipelines_made, statistics.pipelines_from_archive);
	CHECK(statistics.depth_stencil_states_made <= 2, "depth-stencil states: %u", statistics.depth_stencil_states_made);
	xgpu_metal_texture_destroy(target);
	xgpu_metal_texture_destroy(depth);
}

/* the winding: the Xbox screen's, rows from the top, as Metal has it */
static void test_winding(void)
{
	uint32_t target = target_make(XGPU_METAL_FORMAT_BGRA8, SIZE, SIZE, 0);
	struct xgpu_metal_pipeline pipeline;
	/* top left, top right, bottom left: clockwise on the screen */
	struct vertex clockwise[3] = {
		{ -1, 1, 0.5f, 1, 0xffffffff, 0, 0 }, { 1, 1, 0.5f, 1, 0xffffffff, 0, 0 }, { -1, -1, 0.5f, 1, 0xffffffff, 0, 0 },
	};
	int cases, drawn[4];
	float black[4] = { 0, 0, 0, 1 };

	begin("winding");
	for (cases = 0; cases < 4; cases++)
	{
		frame_begin(target, 0);
		xgpu_metal_clear(NULL, 0, CLEAR_COLOR, black, 0, 0);
		pipeline_default(&pipeline, 0);
		pipeline.cull = 1;
		/* fronts clockwise or not; the backs culled or the fronts */
		pipeline.front_counter_clockwise = (uint8_t)(cases & 1);
		pipeline.cull_front = (uint8_t)(cases >> 1);
		xgpu_metal_set_pipeline(&pipeline);
		xgpu_metal_set_stream(0, xgpu_metal_stream(clockwise, sizeof(clockwise)));
		xgpu_metal_draw(XGPU_METAL_PRIMITIVE_TRIANGLES, 3, 0, 0, (struct xgpu_metal_source){ 0, 0, 0, 0 });
		xgpu_metal_frame_wait(xgpu_metal_frame_end(0));
		read_target(target);
		drawn[cases] = pixel(4, 4) == 0xffffffff;
	}
	CHECK(drawn[0] && !drawn[1] && !drawn[2] && drawn[3],
		"a clockwise triangle, fronts clockwise: drawn with the backs culled, not the fronts (%d %d %d %d)", drawn[0],
		drawn[1], drawn[2], drawn[3]);
	xgpu_metal_texture_destroy(target);
}

static void test_visibility(void)
{
	uint32_t target = target_make(XGPU_METAL_FORMAT_BGRA8, SIZE, SIZE, 0);
	uint32_t other = target_make(XGPU_METAL_FORMAT_BGRA8, SIZE, SIZE, 0);
	uint32_t depth = target_make(XGPU_METAL_FORMAT_D24S8, SIZE, SIZE, 0);
	uint32_t multisampled = target_make(XGPU_METAL_FORMAT_BGRA8, SIZE, SIZE, 4);
	struct xgpu_metal_pipeline pipeline;
	uint32_t samples = 0;
	uint64_t frame, fence;
	float black[4] = { 0, 0, 0, 1 };
	volatile uint32_t output[XGPU_METAL_VISIBILITY_SLOTS];

	begin("visibility");
	memset((void *)output, 0, sizeof(output));
	xgpu_metal_set_visibility_output(output);
	frame_begin(target, depth);
	xgpu_metal_clear(NULL, 0, CLEAR_COLOR | XGPU_METAL_CLEAR_DEPTH, black, 0.5f, 0);
	pipeline_default(&pipeline, XGPU_METAL_FORMAT_D24S8);
	pipeline.depth_test = 1;
	pipeline.depth_function = XGPU_COMPARE_LESS;
	xgpu_metal_set_pipeline(&pipeline);
	/* 16x16 pixels in front (the later passes' tests must leave its count
	as it is) */
	xgpu_metal_visibility_begin();
	draw_rectangle(0, 0, 16, 16, 0.25f, 0xffffffff);
	xgpu_metal_visibility_end(7, 1.0f);
	/* behind the depth: none */
	xgpu_metal_visibility_begin();
	draw_rectangle(0, 0, 16, 16, 0.75f, 0xffffffff);
	xgpu_metal_visibility_end(8, 1.0f);
	/* across a change of targets: 8x8 here and 8x4 there */
	xgpu_metal_visibility_begin();
	draw_rectangle(32, 32, 40, 40, 0.25f, 0xffffffff);
	xgpu_metal_set_targets(other, 0, 0, 0, 1);
	pipeline.depth_format = 0;
	pipeline.depth_test = 0;
	xgpu_metal_set_pipeline(&pipeline);
	draw_rectangle(0, 0, 8, 4, 0.25f, 0xffffffff);
	xgpu_metal_visibility_end(9, 1.0f);
	/* a test begun before a clear that begins its pass, and a clear
	inside it, which counts nothing */
	xgpu_metal_set_targets(target, 0, 0, 0, 1);
	xgpu_metal_visibility_begin();
	xgpu_metal_clear((const int32_t (*)[4])(int32_t[1][4]){ { 0, 0, 8, 8 } }, 1, CLEAR_COLOR, black, 0, 0);
	draw_rectangle(0, 0, 2, 2, 0.25f, 0xffffffff);
	xgpu_metal_visibility_end(10, 1.0f);
	/* multisampled: samples are counted, 4 a pixel */
	xgpu_metal_set_targets(multisampled, 0, 0, 0, 1);
	pipeline.samples = 4;
	xgpu_metal_set_pipeline(&pipeline);
	xgpu_metal_visibility_begin();
	draw_rectangle(0, 0, 4, 4, 0.25f, 0xffffffff);
	xgpu_metal_visibility_end(11, 1.0f);
	fence = xgpu_metal_frame_end(0);
	xgpu_metal_frame_wait(fence);
	frame = xgpu_metal_visibility_result(7, &samples);
	CHECK(frame && samples == 256, "in front: %u samples (frame %llu)", samples, (unsigned long long)frame);
	xgpu_metal_visibility_result(8, &samples);
	CHECK(samples == 0, "behind: %u", samples);
	xgpu_metal_visibility_result(9, &samples);
	CHECK(samples == 96, "across two passes: %u", samples);
	xgpu_metal_visibility_result(10, &samples);
	CHECK(samples == 4, "with a clear inside: %u", samples);
	xgpu_metal_visibility_result(11, &samples);
	CHECK(samples == 64, "multisampled: %u", samples);
	CHECK(output[7] == 256 && output[9] == 96, "written for the guest: %u %u", output[7], output[9]);
	xgpu_metal_set_visibility_output(NULL);
	xgpu_metal_texture_destroy(target);
	xgpu_metal_texture_destroy(other);
	xgpu_metal_texture_destroy(depth);
	xgpu_metal_texture_destroy(multisampled);
}

static void test_multisampling(void)
{
	uint32_t target = target_make(XGPU_METAL_FORMAT_BGRA8, SIZE, SIZE, 4);
	struct xgpu_metal_pipeline pipeline;
	struct vertex triangle[3] = {
		{ -1, 1, 0.5f, 1, 0xffffffff, 0, 0 }, { 1, 1, 0.5f, 1, 0xffffffff, 0, 0 }, { -1, -1, 0.5f, 1, 0xffffffff, 0, 0 },
	};
	float black[4] = { 0, 0, 0, 1 };
	int x, partial = 0, inside = 1;

	begin("multisampling");
	frame_begin(target, 0);
	xgpu_metal_clear(NULL, 0, CLEAR_COLOR, black, 0, 0);
	pipeline_default(&pipeline, 0);
	pipeline.samples = 4;
	xgpu_metal_set_pipeline(&pipeline);
	xgpu_metal_set_stream(0, xgpu_metal_stream(triangle, sizeof(triangle)));
	xgpu_metal_draw(XGPU_METAL_PRIMITIVE_TRIANGLES, 3, 0, 0, (struct xgpu_metal_source){ 0, 0, 0, 0 });
	xgpu_metal_frame_wait(xgpu_metal_frame_end(0));
	read_target(target);
	/* the diagonal's pixels are partly covered */
	for (x = 0; x < SIZE; x++)
	{
		uint32_t green = (pixel(x, SIZE - 1 - x) >> 8) & 0xff;

		if (green > 0 && green < 255)
			partial++;
	}
	for (x = 0; x < 20; x++)
		inside = inside && pixel(x, 2) == 0xffffffff;
	CHECK(partial > SIZE / 2 && inside, "resolved: %d partly covered pixels on the diagonal", partial);

	/* the same target drawn without its multisampled attachment (samples
	0 in set_targets): straight into its texture, no partial pixels */
	xgpu_metal_frame_begin();
	xgpu_metal_set_targets(target, 0, 0, 0, 0);
	xgpu_metal_clear(NULL, 0, CLEAR_COLOR, black, 0, 0);
	pipeline.samples = 1;
	xgpu_metal_set_pipeline(&pipeline);
	xgpu_metal_set_stream(0, xgpu_metal_stream(triangle, sizeof(triangle)));
	xgpu_metal_draw(XGPU_METAL_PRIMITIVE_TRIANGLES, 3, 0, 0, (struct xgpu_metal_source){ 0, 0, 0, 0 });
	xgpu_metal_frame_wait(xgpu_metal_frame_end(0));
	read_target(target);
	for (partial = 0, x = 0; x < SIZE; x++)
	{
		uint32_t green = (pixel(x, SIZE - 1 - x) >> 8) & 0xff;

		if (green > 0 && green < 255)
			partial++;
	}
	CHECK(partial == 0 && pixel(2, 2) == 0xffffffff, "not multisampled: %d partly covered pixels", partial);
	xgpu_metal_texture_destroy(target);
}

static void test_upload_order(void)
{
	uint32_t target = target_make(XGPU_METAL_FORMAT_BGRA8, SIZE, SIZE, 0);
	uint32_t texture = texture_make(XGPU_METAL_TEXTURE_2D, XGPU_METAL_FORMAT_BGRA8, 1, 1, 1);
	uint32_t red = 0xffff0000, blue = 0xff0000ff, green = 0xff00ff00;
	struct xgpu_metal_pipeline pipeline;
	struct xgpu_metal_sampler sampler;
	struct xgpu_metal_statistics statistics;

	begin("upload order");
	xgpu_metal_texture_upload(texture, 0, 0, 0, 0, 0, 1, 1, 1, &red, 4, 0);
	frame_begin(target, 0);
	pipeline_default(&pipeline, 0);
	xgpu_metal_set_pipeline(&pipeline);
	sampler_point(&sampler);
	xgpu_metal_set_texture(0, texture, &sampler);
	uniforms_set(1, 1, 1, 1, 1);
	statistics_take(&statistics);
	draw_rectangle(0, 0, 32, SIZE, 0.5f, 0xffffffff);
	/* the draw before it still reads red */
	xgpu_metal_texture_upload(texture, 0, 0, 0, 0, 0, 1, 1, 1, &blue, 4, 0);
	draw_rectangle(32, 0, SIZE, SIZE, 0.5f, 0xffffffff);
	xgpu_metal_frame_wait(xgpu_metal_frame_end(0));
	read_target(target);
	CHECK(pixel(8, 8) == red && pixel(40, 8) == blue, "in the frame's order: %08x %08x", pixel(8, 8), pixel(40, 8));
	/* nothing reads it any more: written at once */
	xgpu_metal_texture_upload(texture, 0, 0, 0, 0, 0, 1, 1, 1, &green, 4, 0);
	statistics_take(&statistics);
	CHECK(statistics.uploads_ordered == 1 && statistics.uploads_direct == 1, "ordered %u, direct %u",
		statistics.uploads_ordered, statistics.uploads_direct);
	xgpu_metal_texture_destroy(texture);
	xgpu_metal_texture_destroy(target);
}

static void test_texture_kinds(void)
{
	uint32_t cube = texture_make(XGPU_METAL_TEXTURE_CUBE, XGPU_METAL_FORMAT_BGRA8, 4, 4, 1);
	uint32_t array = texture_make(XGPU_METAL_TEXTURE_2D_ARRAY, XGPU_METAL_FORMAT_BGRA8, 4, 4, 3);
	uint32_t volume = texture_make(XGPU_METAL_TEXTURE_3D, XGPU_METAL_FORMAT_BGRA8, 4, 4, 4);
	uint32_t texels[64], read[16];
	int face, good = 1;

	begin("cubes, arrays, volumes");
	CHECK(cube && array && volume, "made");
	for (face = 0; face < 6; face++)
	{
		int texel;

		for (texel = 0; texel < 16; texel++)
			texels[texel] = 0xff000000u | (uint32_t)face * 0x101010u;
		xgpu_metal_texture_upload(cube, 0, (uint32_t)face, 0, 0, 0, 4, 4, 1, texels, 16, 0);
		if (face < 3)
			xgpu_metal_texture_upload(array, 0, (uint32_t)face, 0, 0, 0, 4, 4, 1, texels, 16, 0);
	}
	for (face = 0; face < 6; face++)
	{
		xgpu_metal_read_pixels(cube, 0, (uint32_t)face, 0, 0, 4, 4, read, 16);
		good = good && read[5] == (0xff000000u | (uint32_t)face * 0x101010u);
		if (face < 3)
		{
			xgpu_metal_read_pixels(array, 0, (uint32_t)face, 0, 0, 4, 4, read, 16);
			good = good && read[5] == (0xff000000u | (uint32_t)face * 0x101010u);
		}
	}
	CHECK(good, "each face and layer its own");
	memset(texels, 0x7f, sizeof(texels));
	xgpu_metal_texture_upload(volume, 0, 0, 0, 0, 0, 4, 4, 4, texels, 16, 64);
	xgpu_metal_read_pixels(volume, 0, 0, 0, 0, 4, 4, read, 16);
	CHECK(read[0] == 0x7f7f7f7f, "the volume's first slice: %08x", read[0]);
	xgpu_metal_texture_destroy(cube);
	xgpu_metal_texture_destroy(array);
	xgpu_metal_texture_destroy(volume);
}

/* ---------- the guest's memory, read in place */

#define GUEST_START 0x01000000u
#define GUEST_SIZE (64u << 20)
static unsigned char *guest_memory;
static size_t guest_page;
/* readable or not, by host page */
static unsigned char guest_mapped[GUEST_SIZE / 0x4000];

/* host.h's host_page_size (Mach has one by that name too: renamed above),
which the textures' cache asks */
size_t guest_host_page_size(void) __asm__("_host_page_size");

size_t guest_host_page_size(void)
{
	return (size_t)getpagesize();
}

/* the contiguous memory test_stream_emitter maps (its textures) */
#define CONTIGUOUS 0x80000000u
#define CONTIGUOUS_MAPPED 0x10000u
static int contiguous_mapped;

int host_memory_readable_extent(uint32_t address, uint32_t size, uint32_t limit_start, uint32_t limit_end,
	uint32_t *start, uint32_t *end)
{
	uint64_t first = address & ~(uint64_t)(guest_page - 1);
	uint64_t last = ((uint64_t)address + size + guest_page - 1) & ~(uint64_t)(guest_page - 1);
	uint64_t page;

	if (contiguous_mapped && address >= CONTIGUOUS && (uint64_t)address + size <= CONTIGUOUS + CONTIGUOUS_MAPPED)
	{
		*start = first > limit_start ? (uint32_t)first : limit_start;
		*end = last < limit_end ? (uint32_t)last : limit_end;
		return 1;
	}

	if (first < GUEST_START || last > GUEST_START + GUEST_SIZE || first < limit_start || last > limit_end)
		return 0;
	for (page = first; page < last; page += guest_page)
	{
		if (!guest_mapped[(page - GUEST_START) / guest_page])
			return 0;
	}
	while (first > limit_start && first > GUEST_START && guest_mapped[(first - guest_page - GUEST_START) / guest_page])
		first -= guest_page;
	while (last < limit_end && last < GUEST_START + GUEST_SIZE && guest_mapped[(last - GUEST_START) / guest_page])
		last += guest_page;
	*start = (uint32_t)first;
	*end = (uint32_t)last;
	return 1;
}

static void guest_map(uint32_t address, uint32_t size)
{
	uint32_t page;

	mmap(guest_memory + (address - GUEST_START), size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
	for (page = address; page < address + size; page += (uint32_t)guest_page)
		guest_mapped[(page - GUEST_START) / guest_page] = 1;
	/* (as host_memory_base.c tells the backend) */
	if (host_memory_remapped)
		host_memory_remapped(address, size);
}

static void test_guest_memory(void)
{
	uint32_t target = target_make(XGPU_METAL_FORMAT_BGRA8, SIZE, SIZE, 0);
	uint32_t vertices_at = GUEST_START + 0x10000 + 64, indices_at = GUEST_START + 0x20000;
	struct xgpu_metal_pipeline pipeline;
	struct xgpu_metal_statistics statistics;
	uint16_t indices[6] = { 0, 1, 2, 2, 1, 3 };
	struct vertex quad[6], corners[4];
	void (*hook)(uint32_t, uint32_t) = host_memory_remapped;

	begin("guest memory in place");
	guest_page = (size_t)getpagesize();
	/* (4 GB-aligned, as host_memory_base.c places the guest's memory: a
	guest address is a host address's low 32 bits) */
	{
		uintptr_t reservation = (uintptr_t)mmap(NULL, 0x200000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE,
			-1, 0);

		host_guest_base = (reservation + 0xffffffffull) & ~(uintptr_t)0xffffffffull;
		guest_memory = (unsigned char *)host_guest_base + GUEST_START;
	}
	host_memory_remapped = NULL;
	guest_map(GUEST_START, 0x40000);
	rectangle_vertices(quad, 0, 0, 32, 32, 0.5f, 0xffff0000);
	corners[0] = quad[0];
	corners[1] = quad[1];
	corners[2] = quad[2];
	corners[3] = quad[5];
	memcpy((void *)host_address(vertices_at), corners, sizeof(corners));
	memcpy((void *)host_address(indices_at), indices, sizeof(indices));
	/* (read-only, as the write tracking leaves pages) */
	mprotect((void *)host_address(GUEST_START), 0x40000, PROT_READ);
	host_memory_remapped = xgpu_metal_guest_remapped;
	statistics_take(&statistics);

	frame_begin(target, 0);
	xgpu_metal_clear(NULL, 0, CLEAR_COLOR, (float[4]){ 0, 0, 0, 1 }, 0, 0);
	pipeline_default(&pipeline, 0);
	xgpu_metal_set_pipeline(&pipeline);
	xgpu_metal_set_stream(0, xgpu_metal_guest(vertices_at, sizeof(corners)));
	xgpu_metal_draw(XGPU_METAL_PRIMITIVE_TRIANGLES, 6, 0, 0, xgpu_metal_guest(indices_at, sizeof(indices)));
	/* again, with a base vertex: the same buffer */
	xgpu_metal_set_stream(0, xgpu_metal_guest(vertices_at - sizeof(struct vertex), sizeof(corners) + sizeof(struct vertex)));
	xgpu_metal_draw(XGPU_METAL_PRIMITIVE_TRIANGLES, 6, 0, 1, xgpu_metal_guest(indices_at, sizeof(indices)));
	xgpu_metal_frame_wait(xgpu_metal_frame_end(0));
	read_target(target);
	statistics_take(&statistics);
	CHECK(pixel(8, 8) == 0xffff0000 && pixel(40, 40) == 0xff000000, "drawn from the guest's memory: %08x", pixel(8, 8));
	CHECK(statistics.guest_buffers_made == 1 && statistics.guest_bytes_copied == 0, "%u buffers made, %u bytes copied",
		statistics.guest_buffers_made, statistics.guest_bytes_copied);

	/* the guest maps the pages again with other vertices: the buffer made
	over the old pages goes, and a new one sees the new */
	guest_map(GUEST_START, 0x40000);
	for (int index = 0; index < 4; index++)
		corners[index].color = 0xff00ff00;
	memcpy((void *)host_address(vertices_at), corners, sizeof(corners));
	memcpy((void *)host_address(indices_at), indices, sizeof(indices));
	frame_begin(target, 0);
	xgpu_metal_set_pipeline(&pipeline);
	xgpu_metal_set_stream(0, xgpu_metal_guest(vertices_at, sizeof(corners)));
	xgpu_metal_draw(XGPU_METAL_PRIMITIVE_TRIANGLES, 6, 0, 0, xgpu_metal_guest(indices_at, sizeof(indices)));
	xgpu_metal_frame_wait(xgpu_metal_frame_end(0));
	read_target(target);
	statistics_take(&statistics);
	CHECK(pixel(8, 8) == 0xff00ff00, "the new pages: %08x", pixel(8, 8));
	CHECK(statistics.guest_buffers_made == 1, "a new buffer: %u", statistics.guest_buffers_made);

	/* pages that are not readable: copied (here they are readable, but the
	extent says not) */
	guest_mapped[(0x30000) / guest_page] = 0;
	xgpu_metal_guest_remapped(GUEST_START + 0x30000, (uint32_t)guest_page);
	frame_begin(target, 0);
	xgpu_metal_set_pipeline(&pipeline);
	memcpy((void *)host_address(GUEST_START + 0x30000), corners, sizeof(corners));
	xgpu_metal_set_stream(0, xgpu_metal_guest(GUEST_START + 0x30000, sizeof(corners)));
	xgpu_metal_draw(XGPU_METAL_PRIMITIVE_TRIANGLES, 6, 0, 0, xgpu_metal_guest(indices_at, sizeof(indices)));
	xgpu_metal_frame_wait(xgpu_metal_frame_end(0));
	statistics_take(&statistics);
	CHECK(statistics.guest_bytes_copied == sizeof(corners), "copied: %u bytes", statistics.guest_bytes_copied);
	host_memory_remapped = hook;
	xgpu_metal_texture_destroy(target);
}

/* ---------- frames in flight */

static void test_frames(void)
{
	uint32_t target = target_make(XGPU_METAL_FORMAT_BGRA8, SIZE, SIZE, 0);
	struct xgpu_metal_pipeline pipeline;
	uint64_t fences[40];
	int frame, ahead = 0, ordered = 1;

	begin("frames in flight");
	pipeline_default(&pipeline, 0);
	for (frame = 0; frame < 40; frame++)
	{
		int index;

		frame_begin(target, 0);
		/* at most two frames in flight: the one two before is done */
		if (frame >= 2 && xgpu_metal_frame_completed() < fences[frame - 2])
			ahead++;
		xgpu_metal_set_pipeline(&pipeline);
		for (index = 0; index < 200; index++)
			draw_rectangle((float)(index % 8) * 8, (float)(index / 8 % 8) * 8, (float)(index % 8) * 8 + 8,
				(float)(index / 8 % 8) * 8 + 8, 0.5f, 0xff000000u | (uint32_t)frame);
		fences[frame] = xgpu_metal_frame_end(0);
		if (frame && fences[frame] <= fences[frame - 1])
			ordered = 0;
	}
	xgpu_metal_frame_wait(fences[39]);
	read_target(target);
	CHECK(!ahead, "%d frames began with more than two in flight", ahead);
	CHECK(ordered, "the fences grow");
	CHECK(pixel(4, 4) == 0xff000027, "the last frame's: %08x", pixel(4, 4));
	xgpu_metal_texture_destroy(target);
}

/* ---------- the command stream's table */

#ifdef TEST_STREAM
/* xgpu_backend.h's values (its header is the guest's: it does not compile
on the host) */
enum
{
	XGPU_CLEAR_RED = 1, XGPU_CLEAR_GREEN = 2, XGPU_CLEAR_BLUE = 4, XGPU_CLEAR_ALPHA = 8,
	XGPU_CLEAR_COLOR = 16, XGPU_CLEAR_DEPTH = 32, XGPU_CLEAR_STENCIL = 64,
};
enum
{
	XGPU_POINTS, XGPU_LINES, XGPU_LINE_LOOP, XGPU_LINE_STRIP, XGPU_TRIANGLES, XGPU_TRIANGLE_STRIP,
	XGPU_TRIANGLE_FAN,
};
enum
{
	XGPU_VERTEX_FLOAT2 = 2, XGPU_VERTEX_FLOAT4 = 4, XGPU_VERTEX_COLOR = 5,
};

static int stream_shaders(void *context, const struct xgpu_stream_pipeline_define *definition,
	const struct xgpu_stream_vertex_program *program, const struct xgpu_stream_vertex_lighting *lighting,
	const struct xgpu_metal_stream_variant *variant, uint32_t *vertex, uint32_t *pixel)
{
	(void)context;
	(void)definition;
	(void)program;
	(void)lighting;
	(void)variant;
	*vertex = vertex_shader;
	*pixel = pixel_shader;
	return 1;
}

/* writes a record into the ring at *position (4-byte aligned, inline data
16) and returns it */
static void *ring_put(unsigned char *ring, uint32_t *position, const void *record, uint32_t size)
{
	void *at = ring + *position;

	memcpy(at, record, size);
	*position = (*position + size + 15) & ~15u;
	return at;
}

static void test_stream_table(void)
{
	const struct host_render_backend *backend = xgpu_metal_stream_backend();
	struct xgpu_metal_stream_shaders shaders = { NULL, stream_shaders, NULL };
	static volatile uint32_t visibility[XGPU_STREAM_VISIBILITY_SLOTS];
	struct xgpu_stream_capabilities capabilities;
	struct xgpu_stream_target_create create = { 0, 1, SIZE, SIZE, 0, 1, { 0, 0 } };
	struct xgpu_stream_target_create create_depth = { 0, 2, SIZE, SIZE, 1, 1, { 0, 0 } };
	struct xgpu_stream_set_targets targets = { 0, 1, 2, -1, 1, 0, 1 };
	struct xgpu_stream_pipeline_define definition;
	struct xgpu_stream_dynamic_state dynamic;
	struct xgpu_stream_draw_uniforms uniforms;
	struct host_render_vertex_input input;
	struct host_render_texture_stage stages[4];
	struct xgpu_stream_layout_define *layout;
	struct xgpu_stream_mirror_write mirror;
	struct xgpu_stream_draw draw;
	struct xgpu_stream_draw_indices *draw_indices;
	struct xgpu_stream_draw_indexed draw_indexed;
	struct xgpu_stream_draw_immediate immediate;
	struct xgpu_stream_visibility_end visibility_end;
	struct xgpu_stream_read_target read;
	struct vertex quad[6], fan[4], indexed[4];
	unsigned char *ring;
	uint32_t ring_size = 1u << 20, position = 0;
	uint16_t indices[6] = { 0, 1, 2, 2, 1, 3 }, indices_from_one[6] = { 1, 2, 3, 3, 2, 4 };
	float attributes[16][4], immediate_vertices[4][2][4];
	/* a clear record and its rectangle */
	union
	{
		struct xgpu_stream_clear clear;
		unsigned char bytes[sizeof(struct xgpu_stream_clear) + sizeof(int32_t[4])];
	} clear;
	int index;

	begin("the command stream's table");
	/* the ring in the guest's memory (as the stream's is) */
	ring = (unsigned char *)host_address(GUEST_START + 0x100000);
	mmap(ring, ring_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
	xgpu_metal_stream_set_shaders(&shaders);
	backend->open(NULL, ring, ring_size, visibility);
	backend->initialize(NULL, &capabilities);
	CHECK(capabilities.largest_target >= 8192 && capabilities.most_samples >= 4 && capabilities.bgra_vertex_colors &&
		capabilities.base_vertex, "the capabilities: %d, %d samples", capabilities.largest_target,
		capabilities.most_samples);
	backend->target_create(NULL, &create);
	backend->target_create(NULL, &create_depth);
	backend->set_targets(NULL, &targets);
	memset(&clear, 0, sizeof(clear));
	clear.clear.flags = XGPU_CLEAR_COLOR | XGPU_CLEAR_RED | XGPU_CLEAR_GREEN | XGPU_CLEAR_BLUE | XGPU_CLEAR_ALPHA |
		XGPU_CLEAR_DEPTH;
	clear.clear.color[3] = 1.0f;
	clear.clear.depth = 1.0f;
	backend->clear(NULL, &clear.clear);
	clear.clear.count = 1;
	clear.clear.rectangles[0][0] = 48;
	clear.clear.rectangles[0][1] = 48;
	clear.clear.rectangles[0][2] = 16;
	clear.clear.rectangles[0][3] = 16;
	clear.clear.color[2] = 1.0f;
	backend->clear(NULL, &clear.clear);

	/* a pipeline: colour BGRA8, depth D24S8, the depth tested */
	memset(&definition, 0, sizeof(definition));
	definition.key.color_mask = 15;
	definition.key.color_format = 1;
	definition.key.depth_format = 2;
	definition.key.depth_test = 1;
	definition.key.depth_function = XGPU_COMPARE_LESS_EQUAL;
	definition.key.samples = 1;
	CHECK(backend->pipeline(NULL, 5, &definition), "the pipeline is made");
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.viewport[2] = dynamic.viewport[3] = SIZE;
	dynamic.scissor[2] = dynamic.scissor[3] = SIZE;
	dynamic.depth_range[1] = 1.0f;
	memset(&uniforms, 0, sizeof(uniforms));
	/* (the test's pixel shader reads its tint from the uniforms' start) */
	uniforms.viewport_scale[0] = uniforms.viewport_scale[1] = uniforms.viewport_scale[2] = uniforms.viewport_scale[3] = 1.0f;
	backend->draw_state(NULL, &dynamic, &uniforms, 1);
	memset(stages, 0, sizeof(stages));
	backend->set_textures(NULL, stages);

	/* the layout: position (FLOAT4) in register 0, colour (D3DCOLOR) in 3;
	register 9 (the uv) unfed */
	layout = calloc(1, sizeof(*layout) + 2 * sizeof(struct xgpu_stream_vertex_element));
	layout->id = 1;
	layout->element_count = 2;
	layout->elements[0] = (struct xgpu_stream_vertex_element){ 0, 0, XGPU_VERTEX_FLOAT4, 16, 0 };
	layout->elements[1] = (struct xgpu_stream_vertex_element){ 3, 0, XGPU_VERTEX_COLOR, 4, 16 };
	memset(&input, 0, sizeof(input));
	input.layout = layout;
	input.present = 1;
	rectangle_vertices(quad, 0, 0, 16, 16, 0.5f, 0xffff0000);
	input.streams[0].segment = -1;
	input.streams[0].data = guest_address(ring_put(ring, &position, quad, sizeof(quad)));
	input.streams[0].bytes = sizeof(quad);
	input.streams[0].stride = sizeof(struct vertex);
	backend->set_vertices(NULL, &input);
	draw = (struct xgpu_stream_draw){ 0, XGPU_TRIANGLES, 6 };
	backend->visibility_begin(NULL);
	backend->draw(NULL, &draw);
	visibility_end = (struct xgpu_stream_visibility_end){ 0, 3, 1.0f, 1, 1 };
	backend->visibility_end(NULL, &visibility_end);

	/* a fan, made into triangles */
	rectangle_vertices(quad, 16, 0, 32, 16, 0.5f, 0xff00ff00);
	fan[0] = quad[0];
	fan[1] = quad[1];
	fan[2] = quad[5];
	fan[3] = quad[2];
	input.streams[0].data = guest_address(ring_put(ring, &position, fan, sizeof(fan)));
	input.streams[0].bytes = sizeof(fan);
	backend->set_vertices(NULL, &input);
	draw = (struct xgpu_stream_draw){ 0, XGPU_TRIANGLE_FAN, 4 };
	backend->draw(NULL, &draw);

	/* inline indices */
	rectangle_vertices(quad, 32, 0, 48, 16, 0.5f, 0xff0000ff);
	indexed[0] = quad[0];
	indexed[1] = quad[1];
	indexed[2] = quad[2];
	indexed[3] = quad[5];
	input.streams[0].data = guest_address(ring_put(ring, &position, indexed, sizeof(indexed)));
	input.streams[0].bytes = sizeof(indexed);
	backend->set_vertices(NULL, &input);
	{
		unsigned char record[sizeof(struct xgpu_stream_draw_indices)];

		draw_indices = (struct xgpu_stream_draw_indices *)record;
		draw_indices->primitive = XGPU_TRIANGLES;
		draw_indices->count = 6;
		draw_indices->indices = guest_address(ring_put(ring, &position, indices, sizeof(indices)));
		backend->draw_indices(NULL, draw_indices);
	}

	/* vertices and indices from the mirror's segment 1 */
	rectangle_vertices(quad, 0, 16, 16, 32, 0.5f, 0xffffff00);
	indexed[0] = quad[0];
	indexed[1] = quad[1];
	indexed[2] = quad[2];
	indexed[3] = quad[5];
	mirror = (struct xgpu_stream_mirror_write){ 0, 1, 0x1000, sizeof(indexed), 1,
		guest_address(ring_put(ring, &position, indexed, sizeof(indexed))) };
	backend->mirror_write(NULL, &mirror);
	/* (the indices count from the vertex before the quad's: minimum 1, the
	quad's first vertex the streams' first) */
	mirror = (struct xgpu_stream_mirror_write){ 0, 1, 0x2000, sizeof(indices_from_one), 1,
		guest_address(ring_put(ring, &position, indices_from_one, sizeof(indices_from_one))) };
	backend->mirror_write(NULL, &mirror);
	input.streams[0].segment = 1;
	input.streams[0].offset = 0x1000;
	backend->set_vertices(NULL, &input);
	draw_indexed = (struct xgpu_stream_draw_indexed){ 0, XGPU_TRIANGLES, 6, 1, 1, 0x2000, 0, 0 };
	backend->draw_indexed(NULL, &draw_indexed);

	/* immediate mode: registers 0 (position) and 3 (colour) written, two
	triangles of a strip; the colour a float4 here */
	for (index = 0; index < 4; index++)
	{
		float x = index & 1 ? 0.0f : -0.5f, y = index & 2 ? 0.0f : 0.5f;

		immediate_vertices[index][0][0] = x;
		immediate_vertices[index][0][1] = y;
		immediate_vertices[index][0][2] = 0.5f;
		immediate_vertices[index][0][3] = 1.0f;
		immediate_vertices[index][1][0] = 1.0f;
		immediate_vertices[index][1][1] = 0.0f;
		immediate_vertices[index][1][2] = 1.0f;
		immediate_vertices[index][1][3] = 1.0f;
	}
	memset(attributes, 0, sizeof(attributes));
	immediate = (struct xgpu_stream_draw_immediate){ 0, XGPU_TRIANGLE_STRIP, 4, (1 << 0) | (1 << 3), 0,
		guest_address(ring_put(ring, &position, immediate_vertices, sizeof(immediate_vertices))), 0, 0 };
	backend->draw_immediate(NULL, &immediate, (const float (*)[4])attributes);

	backend->present(NULL, 1);
	backend->frame_end(NULL);
	backend->wait(NULL, backend->fence(NULL));

	/* (the pixels go to guest memory) */
	read = (struct xgpu_stream_read_target){ 0, 1, -1, 0, GUEST_START + 0x200000, SIZE * SIZE * 4, 0 };
	mmap((void *)host_address(read.pixels), SIZE * SIZE * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED,
		-1, 0);
	CHECK(backend->read_target(NULL, &read), "read the target");
	memcpy(pixels, (void *)host_address(read.pixels), sizeof(pixels));
	CHECK(pixel(8, 8) == 0xffff0000, "drawn from inline vertices: %08x", pixel(8, 8));
	CHECK(pixel(24, 8) == 0xff00ff00 && pixel(17, 14) == 0xff00ff00 && pixel(30, 1) == 0xff00ff00,
		"a fan: %08x %08x %08x", pixel(24, 8), pixel(17, 14), pixel(30, 1));
	CHECK(pixel(40, 8) == 0xff0000ff, "inline indices: %08x", pixel(40, 8));
	CHECK(pixel(8, 24) == 0xffffff00, "the mirror's vertices and indices: %08x", pixel(8, 24));
	CHECK(pixel(20, 20) == 0xffff00ff, "immediate mode: %08x", pixel(20, 20));
	CHECK(pixel(56, 56) == 0xff0000ff && pixel(60, 30) == 0xff000000, "the clears: %08x %08x", pixel(56, 56),
		pixel(60, 30));
	CHECK(visibility[3] == 256, "the visibility count for the guest: %u", visibility[3]);
	free(layout);
	backend->target_delete(NULL, 1);
	backend->target_delete(NULL, 2);
}

/* ---------- the MSL emitter's shaders and the Xbox's textures, through the table

A pipeline made by the emitter's provider (xgpu_metal_shaders.c) from a
vertex program and a pixel program written here as the stream's bytes (the
intermediate form's structures, field by field: xgpu_shader.h), drawing a
texture's texels: an Xbox texture in the guest's memory (swizzled
A8R8G8B8), read again when its pages are written; an image (a high-res
replacement's RGBA); and a border colour Metal has not (the motion
tracker's), which the shader draws. */

/* a vertex program instruction's bytes (struct xgpu_vertex_instruction):
mac, ilu, operands a, b, c (file, index, swizzle, negate), relative, the
units' masks and temporaries, the output's mask, is_register, register,
address, from_ilu, writes_output, captures_clip, last */
#define INSTRUCTION_SIZE 36

static void instruction_move(unsigned char *instruction, int input, int output, int last)
{
	memset(instruction, 0, INSTRUCTION_SIZE);
	instruction[0] = 1;        /* XGPU_MAC_MOV */
	instruction[2] = 2;        /* a: XGPU_VERTEX_INPUT */
	instruction[3] = (unsigned char)input;
	instruction[4] = 0;
	instruction[5] = 1;
	instruction[6] = 2;
	instruction[7] = 3;
	instruction[28] = 0xf;     /* the output's mask: x, y, z, w */
	instruction[29] = 1;
	instruction[30] = (unsigned char)output;
	instruction[33] = 1;
	instruction[35] = (unsigned char)last;
}

/* the Xbox's 4x4 swizzled texel (x, y): x's bits are 0 and 2, y's 1 and 3 */
static uint32_t swizzled(uint32_t x, uint32_t y)
{
	return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2);
}

static void test_stream_emitter(void)
{
	const struct host_render_backend *backend = xgpu_metal_stream_backend();
	struct xgpu_stream_target_create create = { 0, 11, SIZE, SIZE, 0, 1, { 0, 0 } };
	struct xgpu_stream_set_targets targets = { 0, 11, 0, -1, 1, 0, 0 };
	struct xgpu_stream_pipeline_define definition;
	struct xgpu_stream_dynamic_state dynamic;
	struct xgpu_stream_draw_uniforms uniforms;
	struct host_render_vertex_input input;
	struct host_render_texture_stage stages[4];
	struct xgpu_stream_layout_define *layout;
	struct xgpu_stream_draw draw;
	struct xgpu_stream_read_target read;
	unsigned char program_record[sizeof(struct xgpu_stream_vertex_program) + 3 * INSTRUCTION_SIZE];
	struct xgpu_stream_vertex_program *program = (struct xgpu_stream_vertex_program *)program_record;
	unsigned char *ring = (unsigned char *)host_address(GUEST_START + 0x100000);
	uint32_t *texture_header, *texels, *image;
	uint32_t position = 0x80000, x, y;
	unsigned char *combiner;
	/* corners at the pixels' edges (the emitter adds the half pixel), uv
	from u0 to u1 */
	struct vertex corners[4];
	struct vertex quad[6];
	union
	{
		struct xgpu_stream_clear clear;
		unsigned char bytes[sizeof(struct xgpu_stream_clear) + sizeof(int32_t[4])];
	} clear;

	begin("the emitter's shaders and the Xbox's textures through the table");
	xgpu_metal_stream_set_shaders(xgpu_metal_shaders());
	/* the texture and the image in the contiguous memory */
	mmap((void *)host_address(CONTIGUOUS), CONTIGUOUS_MAPPED, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED,
		-1, 0);
	contiguous_mapped = 1;
	texture_header = (uint32_t *)host_address(CONTIGUOUS + 0x1000);
	texels = (uint32_t *)host_address(CONTIGUOUS + 0x2000);
	image = (uint32_t *)host_address(CONTIGUOUS + 0x3000);
	/* Common, Data (physical), Lock, Format (2D, A8R8G8B8 swizzled, one
	level, 4x4), Size (0: swizzled) */
	texture_header[0] = 0x00040001;
	texture_header[1] = 0x2000;
	texture_header[2] = 0;
	texture_header[3] = (2u << 4) | (0x06u << 8) | (1u << 16) | (2u << 20) | (2u << 24);
	texture_header[4] = 0;
	/* red, green above blue, white */
	for (y = 0; y < 4; y++)
	{
		for (x = 0; x < 4; x++)
			texels[swizzled(x, y)] = y < 2 ? (x < 2 ? 0xffff0000u : 0xff00ff00u) : (x < 2 ? 0xff0000ffu : 0xffffffffu);
	}
	/* the image: 2x2, RGBA bytes: magenta, cyan, yellow, grey */
	image[0] = 0xffff00ffu;
	image[1] = 0xffffff00u;
	image[2] = 0xff00ffffu;
	image[3] = 0xff808080u;

	backend->target_create(NULL, &create);
	backend->set_targets(NULL, &targets);
	memset(&clear, 0, sizeof(clear));
	clear.clear.flags = XGPU_CLEAR_COLOR | XGPU_CLEAR_RED | XGPU_CLEAR_GREEN | XGPU_CLEAR_BLUE | XGPU_CLEAR_ALPHA;
	backend->clear(NULL, &clear.clear);

	/* the vertex program: oPos = v0, oD0 = v3, oT0 = v9 */
	memset(program_record, 0, sizeof(program_record));
	program->header = XGPU_STREAM_HEADER(XGPU_STREAM_VERTEX_PROGRAM, sizeof(program_record));
	program->id = 1;
	program->hash[0] = 0x12345678;
	program->hash[1] = 0x9abcdef0;
	program->instruction_count = 3;
	instruction_move(program->instructions[0], 0, 0, 0);
	instruction_move(program->instructions[1], 3, 1, 0);
	instruction_move(program->instructions[2], 9, 7, 1);
	backend->vertex_program(NULL, program);

	/* the pixel program: stage 0 PROJECT2D of a 2D texture; one combiner
	stage, r0 = t0 * 1 (rgb and alpha); no final combiner: the result is r0 */
	memset(&definition, 0, sizeof(definition));
	definition.id = 9;
	definition.key.vertex_program[0] = program->hash[0];
	definition.key.vertex_program[1] = program->hash[1];
	definition.key.color_mask = 15;
	definition.key.color_format = 1;
	definition.key.samples = 1;
	definition.pixel_program.stages[0][0] = 1;
	definition.pixel_program.stages[0][1] = 1;
	definition.pixel_program.stages[0][2] = 1;
	definition.pixel_program.stages[0][4] = 0xff;
	for (x = 1; x < 4; x++)
		definition.pixel_program.stages[x][4] = (unsigned char)(x - 1);
	definition.pixel_program.combiner_count = 1;
	combiner = definition.pixel_program.combiners[0];
	/* (a portion: inputs A-D, register, alpha and mapping each; then
	ab_dot, cd_dot, mux, output_mapping, ab, cd, sum, the blue copies) */
	combiner[0] = 8;           /* rgb: A = t0 */
	combiner[3] = 0;           /* B = zero, inverted: 1 */
	combiner[5] = 1;
	combiner[16] = 12;         /* AB to r0 */
	combiner[21 + 0] = 8;      /* alpha: A = t0's alpha */
	combiner[21 + 1] = 1;
	combiner[21 + 5] = 1;
	combiner[21 + 16] = 12;
	backend->pipeline_define(NULL, &definition);
	CHECK(backend->pipeline(NULL, 9, &definition), "the emitter's pipeline is made");

	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.viewport[2] = dynamic.viewport[3] = SIZE;
	dynamic.scissor[2] = dynamic.scissor[3] = SIZE;
	dynamic.depth_range[1] = 1.0f;
	memset(&uniforms, 0, sizeof(uniforms));
	/* (the screen conversion the emitter undoes: pixels from the top left) */
	uniforms.viewport_scale[0] = SIZE / 2.0f;
	uniforms.viewport_scale[1] = -SIZE / 2.0f;
	uniforms.viewport_scale[2] = uniforms.viewport_scale[3] = 1.0f;
	uniforms.viewport_offset[0] = uniforms.viewport_offset[1] = SIZE / 2.0f;
	uniforms.point_size = 1.0f;
	for (x = 0; x < 4; x++)
		uniforms.texture_scale[x][0] = uniforms.texture_scale[x][1] = uniforms.texture_scale[x][2] =
			uniforms.texture_scale[x][3] = 1.0f;
	backend->draw_state(NULL, &dynamic, &uniforms, 2);

	layout = calloc(1, sizeof(*layout) + 3 * sizeof(struct xgpu_stream_vertex_element));
	layout->id = 2;
	layout->element_count = 3;
	layout->elements[0] = (struct xgpu_stream_vertex_element){ 0, 0, XGPU_VERTEX_FLOAT4, 16, 0 };
	layout->elements[1] = (struct xgpu_stream_vertex_element){ 3, 0, XGPU_VERTEX_COLOR, 4, 16 };
	layout->elements[2] = (struct xgpu_stream_vertex_element){ 9, 0, XGPU_VERTEX_FLOAT2, 8, 20 };
	memset(&input, 0, sizeof(input));
	input.layout = layout;
	input.present = 1;
	input.streams[0].segment = -1;
	input.streams[0].stride = sizeof(struct vertex);

#define QUAD(u0, u1) do { \
		corners[0] = (struct vertex){ -0.5f, -0.5f, 0.5f, 1.0f, 0xffffffffu, (u0), (u0) }; \
		corners[1] = (struct vertex){ SIZE - 0.5f, -0.5f, 0.5f, 1.0f, 0xffffffffu, (u1), (u0) }; \
		corners[2] = (struct vertex){ -0.5f, SIZE - 0.5f, 0.5f, 1.0f, 0xffffffffu, (u0), (u1) }; \
		corners[3] = (struct vertex){ SIZE - 0.5f, SIZE - 0.5f, 0.5f, 1.0f, 0xffffffffu, (u1), (u1) }; \
		quad[0] = corners[0]; quad[1] = corners[1]; quad[2] = corners[2]; \
		quad[3] = corners[2]; quad[4] = corners[1]; quad[5] = corners[3]; \
		input.streams[0].data = guest_address(ring_put(ring, &position, quad, sizeof(quad))); \
		input.streams[0].bytes = sizeof(quad); \
		backend->set_vertices(NULL, &input); \
		draw = (struct xgpu_stream_draw){ 0, XGPU_TRIANGLES, 6 }; \
		backend->draw(NULL, &draw); \
	} while (0)
#define READ() do { \
		backend->present(NULL, 11); \
		backend->frame_end(NULL); \
		backend->wait(NULL, backend->fence(NULL)); \
		read = (struct xgpu_stream_read_target){ 0, 11, -1, 0, GUEST_START + 0x200000, SIZE * SIZE * 4, 0 }; \
		CHECK(backend->read_target(NULL, &read), "read the target"); \
		memcpy(pixels, (void *)host_address(read.pixels), sizeof(pixels)); \
	} while (0)

	/* the Xbox texture, point sampled, clamped */
	memset(stages, 0, sizeof(stages));
	stages[0].stage.source = 1;      /* XGPU_SOURCE_TEXTURE */
	stages[0].stage.sampler_type = 1;
	stages[0].stage.sampled = 1;
	stages[0].stage.resource = CONTIGUOUS + 0x1000;
	stages[0].stage.resource_format = texture_header[3];
	stages[0].stage.resource_size = 0;
	stages[0].stage.sampler.minify = stages[0].stage.sampler.magnify = 1;
	stages[0].stage.sampler.address[0] = stages[0].stage.sampler.address[1] = stages[0].stage.sampler.address[2] = 2;
	backend->set_textures(NULL, stages);
	QUAD(0.0f, 1.0f);
	READ();
	CHECK(pixel(8, 8) == 0xffff0000u && pixel(56, 8) == 0xff00ff00u && pixel(8, 56) == 0xff0000ffu &&
		pixel(56, 56) == 0xffffffffu, "the Xbox texture, unswizzled: %08x %08x %08x %08x", pixel(8, 8), pixel(56, 8),
		pixel(8, 56), pixel(56, 56));

	/* its texels written (the write tracking says so): read again */
	texels[swizzled(0, 0)] = 0xffffff00u;
	watch_generation++;
	backend->set_targets(NULL, &targets);
	QUAD(0.0f, 1.0f);
	READ();
	CHECK(pixel(4, 4) == 0xffffff00u && pixel(20, 20) == 0xffff0000u, "the texture read again once written: %08x %08x",
		pixel(4, 4), pixel(20, 20));

	/* an image in its place (RGBA) */
	stages[0].stage.source = XGPU_STREAM_SOURCE_IMAGE;
	stages[0].stage.resource = 7;
	stages[0].stage.resource_format = 0;
	stages[0].stage.resource_size = 1;
	stages[0].stage.data = CONTIGUOUS + 0x3000;
	stages[0].stage.width = stages[0].stage.height = 2;
	stages[0].stage.levels = 1;
	backend->set_textures(NULL, stages);
	backend->set_targets(NULL, &targets);
	QUAD(0.0f, 1.0f);
	READ();
	CHECK(pixel(8, 8) == 0xffff00ffu && pixel(56, 8) == 0xff00ffffu && pixel(8, 56) == 0xffffff00u &&
		near(pixel(56, 56), 0xff808080u, 1), "the image, its RGBA: %08x %08x %08x %08x", pixel(8, 8), pixel(56, 8),
		pixel(8, 56), pixel(56, 56));

	/* the Xbox texture's border: Metal's opaque black, then the motion
	tracker's colour (0x46000000), which the shader draws */
	stages[0].stage.source = 1;
	stages[0].stage.resource = CONTIGUOUS + 0x1000;
	stages[0].stage.resource_format = texture_header[3];
	stages[0].stage.resource_size = 0;
	stages[0].stage.sampler.address[0] = stages[0].stage.sampler.address[1] = 3;
	stages[0].stage.sampler.border_color = 0xff000000u;
	backend->set_textures(NULL, stages);
	backend->set_targets(NULL, &targets);
	QUAD(-1.0f, 2.0f);
	READ();
	CHECK(pixel(4, 4) == 0xff000000u && pixel(32, 32) == 0xffffffffu, "a border colour Metal has: %08x %08x",
		pixel(4, 4), pixel(32, 32));
	stages[0].stage.sampler.border_color = 0x46000000u;
	backend->set_textures(NULL, stages);
	backend->set_targets(NULL, &targets);
	QUAD(-1.0f, 2.0f);
	READ();
	CHECK(pixel(4, 4) == 0x46000000u && pixel(60, 60) == 0x46000000u && pixel(32, 32) == 0xffffffffu,
		"the motion tracker's border colour: %08x %08x %08x", pixel(4, 4), pixel(60, 60), pixel(32, 32));
#undef QUAD
#undef READ
	free(layout);
	backend->target_delete(NULL, 11);
}
#endif

/* ---------- the window */

/* a frame's picture given to the presentation (with a window only): the
next frame draws into another texture, which has the pictures of the last
where a frame draws or reads before it clears the whole of it */
static void test_handover(void)
{
	uint32_t target = target_make(XGPU_METAL_FORMAT_BGRA8, SIZE, SIZE, 0);
	float red[4] = { 1, 0, 0, 1 }, blue[4] = { 0, 0, 1, 1 };
	struct xgpu_metal_pipeline pipeline;
	struct xgpu_metal_statistics statistics;

	begin("handover");
	pipeline_default(&pipeline, 0);
	frame_begin(target, 0);
	xgpu_metal_clear(NULL, 0, CLEAR_COLOR, red, 0, 0);
	xgpu_metal_frame_wait(xgpu_metal_frame_end(target));
	/* drawn into without a clear: on the last frame's picture */
	frame_begin(target, 0);
	xgpu_metal_set_pipeline(&pipeline);
	draw_rectangle(0, 0, SIZE / 2, SIZE, 0.5f, 0xff00ff00);
	xgpu_metal_frame_wait(xgpu_metal_frame_end(target));
	statistics_take(&statistics);
	CHECK(statistics.pictures_restored == 1, "the picture restored once: %u", statistics.pictures_restored);
	/* read before anything is drawn: the last frame's picture */
	xgpu_metal_frame_begin();
	read_target(target);
	CHECK(pixel(4, 30) == 0xff00ff00 && pixel(60, 30) == 0xffff0000, "the last two frames' pictures: %08x %08x",
		pixel(4, 30), pixel(60, 30));
	xgpu_metal_frame_wait(xgpu_metal_frame_end(target));
	/* cleared whole: nothing restored */
	frame_begin(target, 0);
	xgpu_metal_clear(NULL, 0, CLEAR_COLOR, blue, 0, 0);
	xgpu_metal_set_pipeline(&pipeline);
	draw_rectangle(0, 0, 8, 8, 0.5f, 0xff00ff00);
	xgpu_metal_frame_wait(xgpu_metal_frame_end(target));
	statistics_take(&statistics);
	CHECK(statistics.pictures_restored == 1, "the reads' restore only: %u", statistics.pictures_restored);
	xgpu_metal_frame_begin();
	read_target(target);
	CHECK(pixel(4, 4) == 0xff00ff00 && pixel(30, 30) == 0xff0000ff, "the cleared frame: %08x %08x", pixel(4, 4),
		pixel(30, 30));
	xgpu_metal_frame_wait(xgpu_metal_frame_end(target));
	xgpu_metal_texture_destroy(target);
}

static int window_run(double seconds, int resize)
{
	SDL_Window *window;
	uint32_t target, frame = 0;
	struct xgpu_metal_pipeline pipeline;
	double start, last_log, resized = 0.0;
	uint32_t frames_since = 0;
	double frame_time_sum = 0.0, frame_time_most = 0.0;

	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 2;
	}
	window = SDL_CreateWindow("xgpu_metal_test", 960, 540, SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE |
		SDL_WINDOW_HIGH_PIXEL_DENSITY);
	window_object = window;
	if (!window || !host_metal_window_open(1))
	{
		fprintf(stderr, "no Metal window\n");
		return 2;
	}
	SDL_RaiseWindow(window);
	shaders_make();
	target = target_make(XGPU_METAL_FORMAT_BGRA8, 640, 480, 0);
	pipeline_default(&pipeline, 0);
	start = last_log = CACurrentMediaTime();
	while (CACurrentMediaTime() - start < seconds)
	{
		SDL_Event event;
		double frame_start = CACurrentMediaTime(), took;
		float color[4] = { 0.1f, 0.1f, 0.15f, 1.0f };
		struct xgpu_metal_dynamic_state state;
		float position = (float)(frame % 120) / 120.0f;

		while (SDL_PollEvent(&event))
		{
			if (event.type == SDL_EVENT_QUIT)
				seconds = 0.0;
		}
		if (resize && !resized && frame_start - start > seconds / 3.0)
		{
			SDL_SetWindowSize(window, 1280, 720);
			resized = frame_start;
		}
		if (resize && resized > 0.0 && frame_start - start > 2.0 * seconds / 3.0)
		{
			SDL_SetWindowFullscreen(window, true);
			resized = -1.0;
		}
		xgpu_metal_frame_begin();
		xgpu_metal_set_targets(target, 0, 0, 0, 1);
		xgpu_metal_clear(NULL, 0, CLEAR_COLOR, color, 0, 0);
		memset(&state, 0, sizeof(state));
		state.viewport[2] = 640;
		state.viewport[3] = 480;
		state.scissor[2] = 640;
		state.scissor[3] = 480;
		state.depth_range[1] = 1.0f;
		xgpu_metal_set_dynamic_state(&state);
		xgpu_metal_set_pipeline(&pipeline);
		{
			struct xgpu_metal_sampler sampler;

			sampler_point(&sampler);
			xgpu_metal_set_texture(0, white, &sampler);
		}
		uniforms_set(1, 1, 1, 1, 0);
		/* a bar that moves a twelfth of the way each frame: missed frames
		show as jumps */
		{
			struct vertex vertices[6];
			float left = -1.0f + 2.0f * position, right = left + 0.1f;
			struct vertex corners[4] = {
				{ left, 1, 0.5f, 1, 0xffffffff, 0, 0 }, { right, 1, 0.5f, 1, 0xffffffff, 0, 0 },
				{ left, -1, 0.5f, 1, 0xffffffff, 0, 0 }, { right, -1, 0.5f, 1, 0xffffffff, 0, 0 },
			};

			vertices[0] = corners[0];
			vertices[1] = corners[1];
			vertices[2] = corners[2];
			vertices[3] = corners[2];
			vertices[4] = corners[1];
			vertices[5] = corners[3];
			xgpu_metal_set_stream(0, xgpu_metal_stream(vertices, sizeof(vertices)));
			xgpu_metal_draw(XGPU_METAL_PRIMITIVE_TRIANGLES, 6, 0, 0, (struct xgpu_metal_source){ 0, 0, 0, 0 });
		}
		xgpu_metal_frame_end(target);
		took = CACurrentMediaTime() - frame_start;
		frame_time_sum += took;
		if (took > frame_time_most)
			frame_time_most = took;
		frame++;
		frames_since++;
		if (CACurrentMediaTime() - last_log >= 2.0)
		{
			double now = CACurrentMediaTime();
			int width, height;

			SDL_GetWindowSizeInPixels(window, &width, &height);
			printf("window: %.1f frames a second (each %.2f ms on average, %.2f ms at most, waits included), %dx%d pixels\n",
				frames_since / (now - last_log), frame_time_sum * 1000.0 / frames_since, frame_time_most * 1000.0, width,
				height);
			fflush(stdout);
			frames_since = 0;
			frame_time_sum = frame_time_most = 0.0;
			last_log = now;
		}
	}
	printf("window: %u frames in %.1f s\n", frame, CACurrentMediaTime() - start);
	test_handover();
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}

int main(int argc, char *argv[])
{
	const char *archive = NULL;
	int expect_archive = 0, index, resize = 0;
	double window_seconds = 0.0;
	struct xgpu_metal_options options;
	struct xgpu_metal_statistics statistics;
	uint32_t one = 0xffffffff;

	for (index = 1; index < argc; index++)
	{
		if (!strcmp(argv[index], "--archive") && index + 1 < argc)
			archive = argv[++index];
		else if (!strcmp(argv[index], "--expect-archive"))
			expect_archive = 1;
		else if (!strcmp(argv[index], "--window") && index + 1 < argc)
			window_seconds = atof(argv[++index]);
		else if (!strcmp(argv[index], "--resize"))
			resize = 1;
	}
	@autoreleasepool
	{
		if (window_seconds > 0.0)
		{
			/* (host_metal.m sets the backend up, as the game's host does) */
			if (archive)
				setenv("HALO_METAL_ARCHIVE", archive, 1);
			else
				setenv("HALO_METAL_ARCHIVE", "", 1);
			if (!host_metal_initialize())
				return 2;
			white = texture_make(XGPU_METAL_TEXTURE_2D, XGPU_METAL_FORMAT_BGRA8, 1, 1, 1);
			xgpu_metal_texture_upload(white, 0, 0, 0, 0, 0, 1, 1, 1, &one, 4, 0);
			return window_run(window_seconds, resize);
		}
		memset(&options, 0, sizeof(options));
		options.archive_path = archive;
		options.guest_extent = host_memory_readable_extent;
		if (!xgpu_metal_initialize(&options))
		{
			fprintf(stderr, "no Metal device\n");
			return 2;
		}
		shaders_make();
		if (!vertex_shader || !pixel_shader)
		{
			fprintf(stderr, "the test's shaders did not compile\n");
			return 2;
		}
		white = texture_make(XGPU_METAL_TEXTURE_2D, XGPU_METAL_FORMAT_BGRA8, 1, 1, 1);
		xgpu_metal_texture_upload(white, 0, 0, 0, 0, 0, 1, 1, 1, &one, 4, 0);
		statistics_take(&statistics);

		test_clears();
		test_textured_quad();
		test_formats();
		test_pipelines();
		test_winding();
		test_visibility();
		test_multisampling();
		test_upload_order();
		test_texture_kinds();
		test_guest_memory();
		test_frames();
#ifdef TEST_STREAM
		test_stream_table();
		test_stream_emitter();
#else
		printf("(the command stream's table: not built, its header is not in the tree)\n");
#endif
		/* the archive: the second run makes nothing that the first added */
		{
			struct xgpu_metal_statistics totals;
			double start = CACurrentMediaTime();

			totals = statistics_total;
			xgpu_metal_shutdown();
			printf("pipelines: %u made, %u from the archive; %.1f ms making them; archive saved in %.1f ms\n",
				totals.pipelines_made, totals.pipelines_from_archive, totals.pipeline_seconds * 1000.0,
				(CACurrentMediaTime() - start) * 1000.0);
			test_name = "archive";
			if (expect_archive)
				CHECK(totals.pipelines_from_archive > 0 && totals.pipelines_made == totals.pipelines_from_archive,
					"every pipeline from the archive: %u of %u", totals.pipelines_from_archive, totals.pipelines_made);
		}
	}
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
