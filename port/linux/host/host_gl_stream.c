/*
HOST_GL_STREAM.C

The OpenGL backend in the host: the renderer's command stream
(port/linux/src/xgpu_stream.h, port/android/host/host_render_stream.h)
played into the guest's OpenGL backend (port/linux/src/xgpu_gl.c), built
into the host as it is (XGPU_GL_HOST, with the host's stand-ins for the
guest's headers, port/linux/host/gl). Each record's entry here makes the
backend call the guest's front end made (xgpu_backend.h), its structures
filled from the record's fields, and its guest addresses the host's.

The front end makes some calls before every draw that the stream sends
only when they change: set_textures (in bind_textures, just before
set_targets) and draw_state (last before the draw's vertices). In the
backend they do more than set what they are given: set_textures looks at
the textures again (their memory written since) and resolves the
multisampled targets drawn into since, draw_state applies the pipeline
chosen and uploads the constants set since. So where the stream left one
out, it is made again here in its place: set_textures as the targets are
set, draw_state before the draw.

The game's window is the guest's (sdl_platform.c makes it); its OpenGL
context is the host's, made when the guest hands the window over
(host_gl_window) and current on the render thread, which draws, presents
and waits for the GPU. The guest never calls OpenGL.

What the backend answers through the stream: its capabilities at start-up;
a pipeline's success (by the decoder: XGPU_STREAM_PIPELINE_*); the
visibility tests' counts, written into the guest's memory at each frame's
end for the slots that have had a test; a target's pixels.

The counts are those the guest's own backend gives the next frame: what
the GPU has written by the frame's end (xgpu_gl.c, visibility_copy_batch).
The guest reads them while the host plays its frames, so its next frame can
find those of the frame before (the guest's own backend's are as early or
as late as the GPU is, against the frame's pacing). The textures
are the host's cache's (host_gl_textures.c), found by the stage's record.
*/

#include "host.h"
#include "host_render_stream.h"
#include "host_gl_renderer.h"
#include "xgpu_backend.h"
#include "xgpu_shader.h"
#include "sdl_platform.h"
#include "port_config.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the interface's pieces the stream carries as their bytes, alike here */
_Static_assert(sizeof(struct xgpu_vertex_instruction) == XGPU_STREAM_VERTEX_INSTRUCTION_SIZE, "vertex instruction");
_Static_assert(sizeof(struct xgpu_texture_stage) == XGPU_STREAM_TEXTURE_STAGE_SIZE, "texture stage");
_Static_assert(sizeof(struct xgpu_combiner_stage) == XGPU_STREAM_COMBINER_STAGE_SIZE, "combiner stage");
_Static_assert(sizeof(struct xgpu_final_combiner) == XGPU_STREAM_FINAL_COMBINER_SIZE, "final combiner");
_Static_assert(sizeof(struct xgpu_vertex_element) == sizeof(struct xgpu_stream_vertex_element), "vertex element");
_Static_assert(sizeof(struct xgpu_draw_uniforms) == sizeof(struct xgpu_stream_draw_uniforms), "draw uniforms");
_Static_assert(sizeof(struct xgpu_dynamic_lights_block) == sizeof(struct xgpu_stream_dynamic_lights_block),
	"dynamic lights block");
_Static_assert(XGPU_VERTEX_ATTRIBUTE_COUNT == 16, "vertex attributes");
_Static_assert(XGPU_MIP_LEVELS == XGPU_STREAM_MIP_LEVELS, "mip levels");
_Static_assert(XGPU_VISIBILITY_TEST_SLOTS == XGPU_STREAM_VISIBILITY_SLOTS, "visibility slots");
_Static_assert(D3DRS_PS_MAX == XGPU_STREAM_COMBINER_STATES, "combiner states");

/* the fences the stream asks for, by their number (the render thread's
queue holds fewer) */
#define FENCES 128

/* (xgpu.h: the VR mode's foveated eye pass, host_gl_vr.c) */
int xgpu_foveated;

static struct
{
	/* the window and the context, made on the thread that handed the
	window over and current on the render thread from the stream's first
	record */
	SDL_Window *window;
	SDL_GLContext context;
	volatile BOOL current;
	/* the swap interval the guest asks for (host_gl_window), and the one
	set */
	volatile int interval;
	int interval_set;

	/* the guest's visibility counts; the slots that have had a test */
	volatile uint32_t *visibility;
	unsigned char visibility_used[XGPU_STREAM_VISIBILITY_SLOTS];
	unsigned long visibility_most;

	GLsync fences[FENCES];
	uint64_t fence_next;

	/* the stages as the records set them (the texture cache finds a
	stage's texture by its record), and as the backend takes them; set
	since the last draw */
	struct host_render_texture_stage stages[4];
	struct xgpu_texture_source sources[4];
	BOOL sources_made, textures_set;
	/* the latest draw state, and whether it was set since the last draw */
	struct xgpu_dynamic_state dynamic;
	const struct xgpu_draw_uniforms *uniforms;
	unsigned long uniforms_serial;
	BOOL draw_state_made, draw_state_set;
	/* each pipeline id's key, and the definition of the pipeline being
	made (its decoded pixel program) */
	struct xgpu_pipeline_key *keys[XGPU_STREAM_PIPELINES];
	const struct xgpu_stream_pipeline_define *definition;
	/* (an immediate draw's vertices, all 16 registers) */
	float *immediate;
	unsigned long immediate_count;
} gl;

/* ---------- the stand-ins' functions (port/linux/host/gl) */

void platform_log(const char *format, ...)
{
	char line[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	host_logf(HOST_LOG_INFO, "%s", line);
}

/* the backend's settings (all debugging ones), from their environment
variables alone (port_config.c's names and defaults) */
static const struct
{
	const char *name, *variable, *value;
	/* set means true, whatever its value */
	BOOL set_is_true;
} settings[] =
{
	{ "debug.gl_debug", "HALO_GL_DEBUG", "false", TRUE },
	{ "debug.gpu_flush_draws", "HALO_GPU_FLUSH_DRAWS", "-1", FALSE },
	{ "debug.gpu_dump_shaders", "HALO_GPU_DUMP_SHADERS", "", FALSE },
	{ "debug.gpu_debug_expression", "HALO_GPU_DEBUG_EXPR", "", FALSE },
	{ "debug.gpu_debug_texture0", "HALO_GPU_DEBUG_T0", "false", TRUE },
	{ "debug.gpu_debug_flat", "HALO_GPU_DEBUG_FLAT", "false", TRUE },
	{ "debug.screenshot_directory", "HALO_SCREENSHOT_DIR", "", FALSE },
	{ "debug.texture_log", "HALO_TEXTURE_LOG", "false", TRUE },
	{ "debug.vr_gpu_time", "HALO_VR_GPU_TIME", "false", FALSE },
	{ "debug.vr_foveation_show", "HALO_VR_FOVEATION_SHOW", "false", FALSE },
};

const char *config_string(const char *name)
{
	size_t index;

	for (index = 0; index < sizeof(settings) / sizeof(settings[0]); index++)
	{
		const char *value;

		if (strcmp(settings[index].name, name))
			continue;
		value = getenv(settings[index].variable);
		if (settings[index].set_is_true)
			return value ? "true" : "false";
		return value ? value : settings[index].value;
	}
	host_logf(HOST_LOG_WARN, "gl: no setting %s in the host", name);
	return "";
}

int config_boolean(const char *name)
{
	const char *value = config_string(name);

	return !strcmp(value, "true") || !strcmp(value, "1") || !strcmp(value, "yes") || !strcmp(value, "on");
}

long config_integer(const char *name)
{
	return strtol(config_string(name), NULL, 10);
}

void platform_video_drawable_size(int *width, int *height)
{
	*width = *height = 0;
	SDL_GetWindowSizeInPixels(gl.window, width, height);
}

static void interval_apply(void)
{
	int interval = gl.interval;

	if (interval == gl.interval_set)
		return;
	gl.interval_set = interval;
	if (!SDL_GL_SetSwapInterval(interval))
		host_logf(HOST_LOG_WARN, "gl: cannot set the swap interval %d: %s", interval, SDL_GetError());
}

void platform_video_swap(void)
{
	interval_apply();
	SDL_GL_SwapWindow(gl.window);
}

/* ---------- the window (a host import: the guest's sdl_platform.c) */

int gl_functions_load(void);

#ifdef __ANDROID__
/* Android: the window's surface goes as the app goes to the background, and
SDL keeps the GL context of the thread that pumps the events, the game's,
across it (SDL_androidevents.c's backup and restore, between the app's
events, which it tells event watches of on that thread): the render thread
hands the context over as the app goes to the background, and takes it back
as it comes to the front. A context lost in the meantime (SDL makes another)
is not made again: the game's GL objects went with it. */
static void context_release(void *argument)
{
	(void)argument;
	glFinish();
	SDL_GL_MakeCurrent(gl.window, NULL);
	gl.current = FALSE;
}

static void context_take(void *argument)
{
	(void)argument;
	gl.current = SDL_GL_MakeCurrent(gl.window, gl.context);
	if (!gl.current)
		host_logf(HOST_LOG_ERROR, "gl: the render thread cannot use the OpenGL context again: %s", SDL_GetError());
	gl.interval_set = -2;
}

static bool SDLCALL lifecycle_watch(void *user, SDL_Event *event)
{
	(void)user;
	if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND && gl.current)
	{
		host_render_stream_call(context_release, NULL);
		SDL_GL_MakeCurrent(gl.window, gl.context);
	}
	else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND && !gl.current)
	{
		if (SDL_GL_GetCurrentContext() != gl.context)
			host_logf(HOST_LOG_ERROR, "gl: the OpenGL context was lost in the background");
		SDL_GL_MakeCurrent(gl.window, NULL);
		host_render_stream_call(context_take, NULL);
	}
	return true;
}
#endif

int host_gl_window(uint32_t window, int interval)
{
	SDL_Window *object = host_sdl_handle_get(window, _handle_window);

	gl.interval = interval;
	if (gl.window)
		return gl.window == object;
	if (!object)
		return 0;
	gl.context = SDL_GL_CreateContext(object);
#ifdef HALO_GLES
	/* ES 3.2 where the driver has it, otherwise the renderer makes do with
	3.0 plus extensions (as sdl_platform.c did) */
	if (!gl.context)
	{
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
		gl.context = SDL_GL_CreateContext(object);
	}
#endif
	if (!gl.context)
	{
		host_logf(HOST_LOG_ERROR, "gl: cannot create an OpenGL context: %s", SDL_GetError());
		return 0;
	}
	if (!SDL_GL_MakeCurrent(object, gl.context) || !gl_functions_load())
	{
		host_logf(HOST_LOG_ERROR, "gl: cannot use the OpenGL context: %s", SDL_GetError());
		SDL_GL_DestroyContext(gl.context);
		gl.context = NULL;
		return 0;
	}
	host_logf(HOST_LOG_INFO, "gl: OpenGL %s on %s, drawn on the host's render thread",
		(const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER));
	/* (the render thread makes it current: stream_initialize) */
	SDL_GL_MakeCurrent(object, NULL);
	gl.window = object;
	gl.interval_set = -2;
#ifdef __ANDROID__
	SDL_AddEventWatch(lifecycle_watch, NULL);
#endif
	host_render_stream_set_backend(host_gl_stream_backend());
	return 1;
}

/* ---------- the backend's life */

static void stream_open(void *context, void *ring, uint32_t ring_size, volatile uint32_t *visibility)
{
	(void)context;
	(void)ring;
	(void)ring_size;
	gl.visibility = visibility;
}

static uint64_t stream_fence(void *context)
{
	uint64_t fence = ++gl.fence_next;
	GLsync *slot = &gl.fences[fence % FENCES];

	(void)context;
	if (!gl.current)
		return fence;
	if (*slot)
		glDeleteSync(*slot);
	*slot = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	glFlush();
	return fence;
}

static void stream_wait(void *context, uint64_t fence)
{
	GLsync *slot = &gl.fences[fence % FENCES];

	(void)context;
	if (!*slot)
		return;
	/* (a second at most: a lost context must not hang the game) */
	glClientWaitSync(*slot, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
	glDeleteSync(*slot);
	*slot = NULL;
}

static void stream_initialize(void *context, struct xgpu_stream_capabilities *capabilities)
{
	(void)context;
	if (!gl.current)
	{
		if (!SDL_GL_MakeCurrent(gl.window, gl.context))
		{
			host_logf(HOST_LOG_ERROR, "gl: the render thread cannot use the OpenGL context: %s", SDL_GetError());
			return;
		}
		gl.current = TRUE;
		interval_apply();
	}
	xgpu_backend_initialize();
	capabilities->largest_target = (int32_t)xgpu_backend_capabilities.largest_target;
	capabilities->most_samples = xgpu_backend_capabilities.most_samples;
	capabilities->most_anisotropy = (int32_t)xgpu_backend_capabilities.most_anisotropy;
	capabilities->sample_counting = xgpu_backend_capabilities.sample_counting != 0;
	capabilities->bgra_vertex_colors = xgpu_backend_capabilities.bgra_vertex_colors != 0;
	capabilities->base_vertex = xgpu_backend_capabilities.base_vertex != 0;
	capabilities->sample_mask = xgpu_backend_capabilities.sample_mask != 0;
	capabilities->mixed_constant_blend = xgpu_backend_capabilities.mixed_constant_blend != 0;
	capabilities->dynamic_lights = xgpu_backend_capabilities.dynamic_lights != 0;
	/* (whether the lights' block and cells can be made: tried, as the
	front end would at its first frame with them) */
	capabilities->dynamic_lights_preparable = xgpu_backend_capabilities.dynamic_lights != 0;
}

static void stream_invalidate(void *context)
{
	(void)context;
	xgpu_backend_invalidate();
}

static void stream_flush(void *context)
{
	(void)context;
	xgpu_backend_flush();
}

static void stream_anti_aliasing_prepare(void *context, uint32_t smaa)
{
	(void)context;
	xgpu_backend_anti_aliasing_prepare(smaa != 0);
}

static void stream_anti_alias(void *context, const struct xgpu_stream_anti_alias *record)
{
	long corners[4];
	int index;

	(void)context;
	for (index = 0; index < 4; index++)
		corners[index] = record->corners[index];
	xgpu_backend_anti_alias(record->target, record->smaa != 0, corners);
}

/* ---------- render targets */

static void stream_target_create(void *context, const struct xgpu_stream_target_create *record)
{
	struct xgpu_target_description description;

	(void)context;
	memset(&description, 0, sizeof(description));
	description.width = record->width;
	description.height = record->height;
	description.depth = record->depth != 0;
	description.layers = record->layers;
	xgpu_backend_target_create(record->target, &description);
}

static void stream_target_delete(void *context, uint32_t target)
{
	(void)context;
	xgpu_backend_target_delete(target);
}

static void stream_target_multisample(void *context, const struct xgpu_stream_target_multisample *record)
{
	(void)context;
	xgpu_backend_target_multisample(record->target, record->samples);
}

static void stream_target_resolve(void *context, uint32_t target)
{
	(void)context;
	xgpu_backend_target_resolve(target);
}

static void stream_set_targets(void *context, const struct xgpu_stream_set_targets *record)
{
	(void)context;
	/* (the textures as the front end set them again: above) */
	if (!gl.textures_set && gl.sources_made)
		xgpu_backend_set_textures(gl.sources);
	gl.textures_set = FALSE;
	xgpu_backend_set_targets(record->color, record->hud_group, record->depth, record->views, record->base,
		record->samples);
}

static void stream_clear(void *context, const struct xgpu_stream_clear *record)
{
	long rectangles[16][4];
	long (*all)[4] = rectangles;
	uint32_t index;
	int side;

	(void)context;
	if (record->count > 16 && !(all = malloc(record->count * sizeof(*all))))
		return;
	for (index = 0; index < record->count; index++)
	{
		for (side = 0; side < 4; side++)
			all[index][side] = record->rectangles[index][side];
	}
	xgpu_backend_clear(record->flags, record->color, record->depth, record->stencil, (const long (*)[4])all,
		record->count);
	if (all != rectangles)
		free(all);
}

static void stream_clear_target(void *context, const struct xgpu_stream_clear_target *record)
{
	(void)context;
	xgpu_backend_clear_target(record->target, record->color);
}

static int stream_read_target(void *context, const struct xgpu_stream_read_target *record)
{
	unsigned char *pixels;

	(void)context;
	pixels = xgpu_backend_read_target(record->target, record->layer, record->raw != 0);
	if (!pixels)
		return 0;
	memcpy(host_pointer(record->pixels), pixels, record->size);
	free(pixels);
	return 1;
}

/* ---------- textures */

static void stream_set_textures(void *context, const struct host_render_texture_stage stages[4])
{
	int stage;
	uint32_t level;

	(void)context;
	for (stage = 0; stage < 4; stage++)
	{
		const struct xgpu_stream_texture_stage *from = &stages[stage].stage;
		struct xgpu_texture_source *to = &gl.sources[stage];

		gl.stages[stage] = stages[stage];
		memset(to, 0, sizeof(*to));
		/* (an image is drawn as the Xbox texture it stands for: the cache
		finds it by the stage's record) */
		to->source = from->source == XGPU_STREAM_SOURCE_IMAGE ? XGPU_SOURCE_TEXTURE : from->source;
		to->sampler_type = from->sampler_type;
		to->sampled = from->sampled;
		to->resource = from->resource;
		to->palette = from->palette;
		to->target = from->target;
		to->drawn = from->drawn;
		to->data = from->data;
		to->width = from->width;
		to->height = from->height;
		to->levels = from->levels;
		to->rendered_levels = from->rendered_levels;
		for (level = 0; level < from->level_count && level < XGPU_MIP_LEVELS; level++)
		{
			to->level_targets[level] = stages[stage].levels[level].target;
			to->level_written[level] = stages[stage].levels[level].written;
		}
		to->sampler.minify = from->sampler.minify;
		to->sampler.mip = from->sampler.mip;
		to->sampler.magnify = from->sampler.magnify;
		memcpy(to->sampler.address, from->sampler.address, sizeof(to->sampler.address));
		to->sampler.lod_bias = from->sampler.lod_bias;
		to->sampler.first_level = from->sampler.first_level;
		to->sampler.anisotropy = from->sampler.anisotropy;
		to->sampler.border_color = from->sampler.border_color;
	}
	gl.sources_made = TRUE;
	gl.textures_set = TRUE;
	xgpu_backend_set_textures(gl.sources);
}

GLuint xgpu_host_texture_get(int stage, GLenum *target)
{
	*target = GL_TEXTURE_2D;
	if (stage < 0 || stage >= 4)
		return 0;
	return host_gl_textures_get(&gl.stages[stage].stage, target);
}

/* ---------- pipelines */

static void stream_set_constants(void *context, const struct xgpu_stream_constants *record)
{
	(void)context;
	xgpu_backend_set_constants(record->first, (const float (*)[4])record->values, record->count);
}

static void stream_vertex_program(void *context, const struct xgpu_stream_vertex_program *record)
{
	static struct xgpu_vertex_program program;

	(void)context;
	memset(&program, 0, sizeof(program));
	program.instruction_count = record->instruction_count;
	memcpy(program.instructions, record->instructions, record->instruction_count * sizeof(program.instructions[0]));
	program.hash = record->hash[0] | (unsigned long long)record->hash[1] << 32;
	program.projects = (unsigned char)record->projects;
	xgpu_backend_vertex_program(record->id, &program);
}

static void stream_vertex_lighting(void *context, const struct xgpu_stream_vertex_lighting *record)
{
	struct nv2a_vertex_lighting lighting;

	(void)context;
	memset(&lighting, 0, sizeof(lighting));
	lighting.lights = record->lights;
	lighting.normal_instruction = record->normal_instruction;
	lighting.normal_register = record->normal_register;
	lighting.position_instruction = record->position_instruction;
	lighting.position_register = record->position_register;
	lighting.world_found = record->world_found != 0;
	lighting.world_instruction = record->world_instruction;
	lighting.world_register = record->world_register;
	xgpu_backend_vertex_lighting(record->hash[0] | (unsigned long long)record->hash[1] << 32, &lighting);
}

static void pixel_key_convert(const struct xgpu_stream_pixel_key *wire, struct nv2a_pixel_shader_key *key)
{
	int index;

	for (index = 0; index < XGPU_STREAM_COMBINER_STATES; index++)
		key->combiner_state[index] = wire->combiner_state[index];
	key->texture_modes = wire->texture_modes;
	memcpy(key->sampler_type, wire->sampler_type, 4);
	memcpy(key->alpha_kill, wire->alpha_kill, 4);
	memcpy(key->color_sign, wire->color_sign, 4);
	key->alpha_test_function = wire->alpha_test_function;
	key->fog_enable = wire->fog_enable;
	key->fog_table_mode = wire->fog_table_mode;
	key->count_samples = wire->count_samples;
	key->coverage_alpha = wire->coverage_alpha;
	key->point_threshold = wire->point_threshold;
	key->per_pixel_lighting = wire->per_pixel_lighting;
	key->alpha_test_samples = wire->alpha_test_samples;
	key->multiview = wire->multiview;
	key->source_blend = wire->source_blend;
	key->dynamic_lights = wire->dynamic_lights;
}

/* (zeroed first: the backend hashes and compares keys whole) */
static void pipeline_key_convert(const struct xgpu_stream_pipeline_key *wire, struct xgpu_pipeline_key *key)
{
	memset(key, 0, sizeof(*key));
	key->vertex_program = wire->vertex_program[0] | (unsigned long long)wire->vertex_program[1] << 32;
	key->packed_attributes = wire->packed_attributes;
	key->stencil_read_mask = wire->stencil_read_mask;
	key->stencil_write_mask = wire->stencil_write_mask;
	key->immediate = wire->immediate;
	key->lighting = wire->lighting;
	key->views = wire->views;
	key->blend = wire->blend;
	key->blend_source = wire->blend_source;
	key->blend_destination = wire->blend_destination;
	key->blend_operation = wire->blend_operation;
	key->color_mask = wire->color_mask;
	key->depth_test = wire->depth_test;
	key->depth_function = wire->depth_function;
	key->depth_write = wire->depth_write;
	key->stencil_test = wire->stencil_test;
	key->stencil_function = wire->stencil_function;
	key->stencil_fail = wire->stencil_fail;
	key->stencil_depth_fail = wire->stencil_depth_fail;
	key->stencil_pass = wire->stencil_pass;
	key->cull = wire->cull;
	key->front_counter_clockwise = wire->front_counter_clockwise;
	key->cull_front = wire->cull_front;
	key->fill = wire->fill;
	key->depth_bias = wire->depth_bias;
	key->color_format = wire->color_format;
	key->depth_format = wire->depth_format;
	key->samples = wire->samples;
	pixel_key_convert(&wire->pixel, &key->pixel);
}

static void stream_pipeline_define(void *context, const struct xgpu_stream_pipeline_define *record)
{
	struct xgpu_pipeline_key **key = &gl.keys[record->id];

	(void)context;
	if (!*key && !(*key = malloc(sizeof(**key))))
		return;
	pipeline_key_convert(&record->key, *key);
}

static int stream_pipeline(void *context, uint32_t id, const struct xgpu_stream_pipeline_define *definition)
{
	BOOL made;

	(void)context;
	if (!gl.keys[id])
		return 0;
	gl.definition = definition;
	made = xgpu_backend_pipeline(gl.keys[id]);
	gl.definition = NULL;
	return made != FALSE;
}

void xgpu_host_pixel_program(const struct nv2a_pixel_shader_key *key, struct xgpu_pixel_program *program)
{
	const struct xgpu_stream_pixel_program *wire;

	(void)key;
	memset(program, 0, sizeof(*program));
	if (!gl.definition)
	{
		host_logf(HOST_LOG_ERROR, "gl: a pixel shader made outside a pipeline");
		return;
	}
	wire = &gl.definition->pixel_program;
	memcpy(program->stages, wire->stages, sizeof(program->stages));
	program->mux_msb = wire->mux_msb;
	memcpy(&program->final, wire->final, sizeof(program->final));
	program->fog = wire->fog;
	program->fog_mode = wire->fog_mode;
	program->combiner_count = wire->combiner_count;
	memcpy(program->combiners, wire->combiners, sizeof(program->combiners));
	program->alpha_test = wire->alpha_test;
	program->alpha_test_samples = wire->alpha_test_samples;
	program->count_samples = wire->count_samples;
	program->coverage_alpha = wire->coverage_alpha;
	program->point_threshold = wire->point_threshold;
	program->per_pixel_lighting = wire->per_pixel_lighting;
	program->dynamic_lights = wire->dynamic_lights;
	program->multiview = wire->multiview;
	program->source_blend = wire->source_blend;
}

static void stream_draw_state(void *context, const struct xgpu_stream_dynamic_state *wire,
	const struct xgpu_stream_draw_uniforms *uniforms, uint32_t uniforms_serial)
{
	struct xgpu_dynamic_state *dynamic = &gl.dynamic;
	int index;

	(void)context;
	for (index = 0; index < 4; index++)
	{
		dynamic->viewport[index] = wire->viewport[index];
		dynamic->scissor[index] = wire->scissor[index];
		dynamic->blend_color[index] = wire->blend_color[index];
	}
	dynamic->depth_range[0] = wire->depth_range[0];
	dynamic->depth_range[1] = wire->depth_range[1];
	dynamic->stencil_reference = wire->stencil_reference;
	dynamic->depth_bias[0] = wire->depth_bias[0];
	dynamic->depth_bias[1] = wire->depth_bias[1];
	/* (the decoder's: the latest uniforms, in place) */
	gl.uniforms = (const struct xgpu_draw_uniforms *)uniforms;
	gl.uniforms_serial = uniforms_serial;
	gl.draw_state_made = TRUE;
	gl.draw_state_set = TRUE;
	xgpu_backend_draw_state(dynamic, gl.uniforms, gl.uniforms_serial);
}

/* a draw follows: the draw state as the front end set it again (above) */
static void draw_state_again(void)
{
	if (!gl.draw_state_set && gl.draw_state_made)
		xgpu_backend_draw_state(&gl.dynamic, gl.uniforms, gl.uniforms_serial);
	gl.draw_state_set = FALSE;
}

/* ---------- vertices and draws */

static void stream_mirror_write(void *context, const struct xgpu_stream_mirror_write *record)
{
	(void)context;
	xgpu_backend_mirror_write(record->segment, record->offset, record->size, host_pointer(record->data),
		record->unused != 0);
}

static void stream_set_vertices(void *context, const struct host_render_vertex_input *input)
{
	static struct xgpu_vertex_input to;
	int stream;

	(void)context;
	to.declaration = input->layout->declaration;
	to.element_count = input->layout->element_count;
	memcpy(to.elements, input->layout->elements, input->layout->element_count * sizeof(to.elements[0]));
	to.packed_mask = input->layout->packed_mask;
	for (stream = 0; stream < 16; stream++)
	{
		const struct xgpu_stream_vertex_stream *from = &input->streams[stream];

		memset(&to.streams[stream], 0, sizeof(to.streams[stream]));
		if (!(input->present & (1u << stream)))
			continue;
		to.streams[stream].present = TRUE;
		to.streams[stream].segment = from->segment;
		to.streams[stream].offset = from->offset;
		to.streams[stream].data = from->segment < 0 ? (unsigned long)(uintptr_t)host_pointer(from->data) : 0;
		to.streams[stream].bytes = from->bytes;
		to.streams[stream].stride = from->stride;
	}
	memcpy(to.attributes, input->attributes, sizeof(to.attributes));
	xgpu_backend_set_vertices(&to);
}

static void stream_draw(void *context, const struct xgpu_stream_draw *record)
{
	(void)context;
	draw_state_again();
	xgpu_backend_draw((int)record->primitive, record->count);
}

static void stream_draw_indices(void *context, const struct xgpu_stream_draw_indices *record)
{
	(void)context;
	draw_state_again();
	xgpu_backend_draw_indices((int)record->primitive, host_pointer(record->indices), record->count);
}

static void stream_draw_indexed(void *context, const struct xgpu_stream_draw_indexed *record)
{
	(void)context;
	draw_state_again();
	xgpu_backend_draw_indexed((int)record->primitive, record->segment < 0 ? host_pointer(record->indices) : NULL,
		record->count, record->minimum, record->segment, record->offset, record->temporary != 0);
}

static void stream_draw_immediate(void *context, const struct xgpu_stream_draw_immediate *record,
	const float (*attributes)[4])
{
	const float *vertices = host_pointer(record->vertices);
	unsigned long vertex, place = 0;
	int reg;

	(void)context;
	/* (the registers written, in their places among the 16: the backend
	takes the written ones from there) */
	if (record->count > gl.immediate_count)
	{
		float *grown = realloc(gl.immediate, (size_t)record->count * 16 * 4 * sizeof(float));

		if (!grown)
			return;
		gl.immediate = grown;
		gl.immediate_count = record->count;
	}
	for (vertex = 0; vertex < record->count; vertex++)
	{
		for (reg = 0; reg < 16; reg++)
		{
			if (!(record->written & (1u << reg)))
				continue;
			memcpy(gl.immediate + (vertex * 16 + reg) * 4, vertices + place * 4, 4 * sizeof(float));
			place++;
		}
	}
	draw_state_again();
	xgpu_backend_draw_immediate((int)record->primitive, gl.immediate, record->count, record->written, attributes,
		record->index_count ? host_pointer(record->indices) : NULL, record->index_count);
}

/* ---------- visibility tests */

static void stream_visibility_begin(void *context)
{
	(void)context;
	xgpu_backend_visibility_begin();
}

static void stream_visibility_end(void *context, const struct xgpu_stream_visibility_end *record)
{
	(void)context;
	if (!gl.visibility_used[record->slot])
	{
		gl.visibility_used[record->slot] = 1;
		if (record->slot + 1 > gl.visibility_most)
			gl.visibility_most = record->slot + 1;
	}
	xgpu_backend_visibility_end(record->slot, record->area, record->views, record->samples);
}

static void stream_visibility_frame(void *context)
{
	(void)context;
	xgpu_backend_visibility_frame();
}

/* the latest counts known, for the front end (xgpu_backend_visibility_result) */
static void visibility_answer(void)
{
	unsigned long slot;

	if (!gl.visibility)
		return;
	for (slot = 0; slot < gl.visibility_most; slot++)
	{
		if (gl.visibility_used[slot])
			__atomic_store_n(&gl.visibility[slot], (uint32_t)xgpu_backend_visibility_result(slot), __ATOMIC_RELAXED);
	}
}

/* ---------- frames */

static void stream_present(void *context, uint32_t back_buffer)
{
	(void)context;
	xgpu_backend_present(back_buffer);
}

static void stream_frame_end(void *context)
{
	(void)context;
	xgpu_backend_frame_end();
	visibility_answer();
}

/* ---------- the enhanced dynamic lights */

static int stream_dynamic_lights_prepare(void *context)
{
	(void)context;
	return xgpu_backend_dynamic_lights_prepare() != FALSE;
}

static void stream_dynamic_lights_upload(void *context, const struct xgpu_stream_dynamic_lights_upload *record)
{
	(void)context;
	xgpu_backend_dynamic_lights_upload((const struct xgpu_dynamic_lights_block *)&record->block,
		host_pointer(record->cells));
}

static void stream_dynamic_lights_halos(void *context, const struct xgpu_stream_dynamic_lights_halos *record)
{
	static struct xgpu_dynamic_lights_halos halos;

	(void)context;
	halos.count = record->count;
	memcpy(halos.halos, record->halos, (size_t)record->count * 2 * sizeof(halos.halos[0]));
	memcpy(halos.camera_right, record->camera_right, sizeof(halos.camera_right));
	memcpy(halos.camera_up, record->camera_up, sizeof(halos.camera_up));
	memcpy(halos.projection, record->projection, sizeof(halos.projection));
	memcpy(halos.viewport_scale, record->viewport_scale, sizeof(halos.viewport_scale));
	memcpy(halos.viewport_offset, record->viewport_offset, sizeof(halos.viewport_offset));
	memcpy(halos.camera, record->camera, sizeof(halos.camera));
	halos.softness = record->softness;
	halos.screen_offset = record->screen_offset;
	xgpu_backend_dynamic_lights_halos(record->color, record->depth, record->layers, &halos);
}

/* ---------- the table */

const struct host_render_backend *host_gl_stream_backend(void)
{
	static struct host_render_backend backend =
	{
		.open = stream_open,
		.fence = stream_fence,
		.wait = stream_wait,
		.initialize = stream_initialize,
		.invalidate = stream_invalidate,
		.flush = stream_flush,
		.anti_aliasing_prepare = stream_anti_aliasing_prepare,
		.anti_alias = stream_anti_alias,
		.target_create = stream_target_create,
		.target_delete = stream_target_delete,
		.target_multisample = stream_target_multisample,
		.target_resolve = stream_target_resolve,
		.set_targets = stream_set_targets,
		.clear = stream_clear,
		.clear_target = stream_clear_target,
		.read_target = stream_read_target,
		.set_textures = stream_set_textures,
		.set_constants = stream_set_constants,
		.vertex_program = stream_vertex_program,
		.vertex_lighting = stream_vertex_lighting,
		.pipeline_define = stream_pipeline_define,
		.pipeline = stream_pipeline,
		.draw_state = stream_draw_state,
		.mirror_write = stream_mirror_write,
		.set_vertices = stream_set_vertices,
		.draw = stream_draw,
		.draw_indices = stream_draw_indices,
		.draw_indexed = stream_draw_indexed,
		.draw_immediate = stream_draw_immediate,
		.visibility_begin = stream_visibility_begin,
		.visibility_end = stream_visibility_end,
		.visibility_frame = stream_visibility_frame,
		.present = stream_present,
		.frame_end = stream_frame_end,
		.dynamic_lights_prepare = stream_dynamic_lights_prepare,
		.dynamic_lights_upload = stream_dynamic_lights_upload,
		.dynamic_lights_halos = stream_dynamic_lights_halos,
	};

#ifdef HALO_VR
	static int made;

	if (!made)
		host_gl_vr_backend(&backend);
	made = 1;
#endif
	return &backend;
}
