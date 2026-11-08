/*
HALO_REPLAY.C

The renderer's command stream replayed from a recording with the Vulkan
backend (HALO_RENDER_RECORD: port/android/host/host_render_record.h), on
the Linux builds' host: the replay of port/tools/replay/host_render_replay.h,
its options and its service (--serve), offscreen (no window: the backend
draws the frames and reads them back, as the game's does with SDL's
offscreen driver). ninja linux_replay builds it as build/<name>/halo_replay.

The pipeline cache is <file>.vkpipelines (HALO_VULKAN_PIPELINE_CACHE names
another file, or none: empty); HALO_VULKAN_VALIDATION=1 turns on the
validation layers.
*/

#include "host.h"
#include "host_render_record.h"
#include "host_render_replay.h"
#include "host_render_stream.h"
#include "xgpu_vulkan.h"
#include "xgpu_vulkan_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- what the host gives the backend, here */

/* (no window: the backend asks for none) */
void *host_sdl_handle_get(uint32_t handle, int type)
{
	(void)handle;
	(void)type;
	return NULL;
}

/* (no display: a 60 Hz one's) */
uint64_t host_vulkan_display_period(void)
{
	return 16666667;
}

/* ---------- the platform */

static int vulkan_initialize(int window, int width, int height)
{
	struct xgpu_vulkan_options options;
	static struct host_render_backend backend;
	const char *validation = getenv("HALO_VULKAN_VALIDATION"), *cache = getenv("HALO_VULKAN_PIPELINE_CACHE");

	(void)window;
	memset(&options, 0, sizeof(options));
	options.cache_path = cache && *cache ? cache : NULL;
	options.validation = validation && atoi(validation);
	/* (its frames' GPU times) */
	options.statistics = 1;
	if (!xgpu_vulkan_initialize(&options))
		return 0;
	xgpu_vulkan_window_resized((uint32_t)width, (uint32_t)height);
	backend = *xgpu_vulkan_stream_backend();
	host_render_stream_set_backend(&backend);
	host_render_record_set_shooter(xgpu_vulkan_stream_shot);
	return 1;
}

static void vulkan_frame_statistics(struct host_render_replay_statistics *to)
{
	struct xgpu_vulkan_statistics statistics;

	xgpu_vulkan_statistics(&statistics);
	to->gpu_ms = statistics.gpu_frames ? statistics.gpu_milliseconds / statistics.gpu_frames : 0.0;
	to->compile_ms = statistics.pipeline_seconds * 1000.0;
	to->draws = statistics.draws;
	to->compiles = statistics.pipelines_made;
}

static void vulkan_ids_draw(uint32_t index)
{
	xgpu_vulkan_stream_ids_draw(index);
}

static const struct host_render_replay_platform vulkan_platform =
{
	.backend = "vulkan",
	.initialize = vulkan_initialize,
	.shutdown = xgpu_vulkan_shutdown,
	.frame_statistics = vulkan_frame_statistics,
	.read_target = xgpu_vulkan_stream_read,
	.ids_begin = xgpu_vulkan_stream_ids_begin,
	.ids_draw = vulkan_ids_draw,
	.ids_read = xgpu_vulkan_stream_ids_read,
	.ids_end = xgpu_vulkan_stream_ids_end,
};

int main(int argc, char *argv[])
{
	char cache[4200];
	int index;

	/* (the pipeline cache beside the recording, as Metal's archive is) */
	for (index = 1; index < argc; index++)
	{
		if (!strcmp(argv[index], "--compare"))
			break;
		if (argv[index][0] != '-' && (index == 1 || argv[index - 1][0] != '-' || !strcmp(argv[index - 1], "--serve") ||
			!strcmp(argv[index - 1], "--quiet") || !strcmp(argv[index - 1], "--verbose") ||
			!strcmp(argv[index - 1], "--unpaced")))
		{
			snprintf(cache, sizeof(cache), "%s.vkpipelines", argv[index]);
			setenv("HALO_VULKAN_PIPELINE_CACHE", cache, 0);
			break;
		}
	}
	return host_render_replay_main(argc, argv, &vulkan_platform);
}
