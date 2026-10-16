/*
HOST_VULKAN.C

The game's window drawn with Vulkan (the Linux desktop's Vulkan build,
HALO_VULKAN): the backend (xgpu_vulkan.c) made for the window, and the
command stream given it.

The guest makes the window with SDL_WINDOW_VULKAN (sdl_platform.c) and hands
it over (host_vulkan_window_open, a host import, before the renderer's
device is made): the backend's instance has the extensions SDL's surface
needs, the surface is SDL's, and the frames are shown through a swapchain.
A window SDL cannot draw with Vulkan (its offscreen driver, where the
device has no headless surfaces) is drawn without one: the frames are
drawn and read back (screenshots) but not shown.

HALO_VULKAN_VALIDATION=1 turns on the validation layers and logs their
messages (the layers must be installed, or found through VK_ADD_LAYER_PATH).
The pipeline cache is $XDG_CACHE_HOME/opence/vulkan_pipelines.bin (or
~/.cache's); HALO_VULKAN_PIPELINE_CACHE names another file, or none (empty).
*/

#include "host.h"
#include "host_render_stream.h"
#include "xgpu_vulkan.h"
#include "xgpu_vulkan_stream.h"
#ifdef HALO_VR
#include "host_vr_vulkan.h"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static SDL_Window *vulkan_window;

static VkSurfaceKHR surface_create(VkInstance instance, void *context)
{
	VkSurfaceKHR surface = VK_NULL_HANDLE;

	(void)context;
	if (!SDL_Vulkan_CreateSurface(vulkan_window, instance, NULL, &surface))
	{
		host_logf(HOST_LOG_WARN, "vulkan: SDL makes no surface for the window: %s", SDL_GetError());
		return VK_NULL_HANDLE;
	}
	return surface;
}

/* (the thread SDL reports the window's events on) */
static bool SDLCALL window_event(void *userdata, SDL_Event *event)
{
	(void)userdata;
	if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
		SDL_GetWindowFromID(event->window.windowID) == vulkan_window)
		xgpu_vulkan_window_resized((uint32_t)event->window.data1, (uint32_t)event->window.data2);
	return true;
}

/* the pipeline cache's file: made folders, or NULL */
static const char *cache_path(char *path, size_t size)
{
	const char *chosen = getenv("HALO_VULKAN_PIPELINE_CACHE");
	const char *cache = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
	char folder[1024];

	if (chosen)
		return *chosen ? chosen : NULL;
#ifdef __ANDROID__
	/* The app has no HOME; keep the driver cache in its private cache. */
	if (!cache || !*cache)
		cache = SDL_GetAndroidCachePath();
#endif
	if (cache && *cache)
		snprintf(folder, sizeof(folder), "%s/opence", cache);
	else if (home && *home)
		snprintf(folder, sizeof(folder), "%s/.cache/opence", home);
	else
		return NULL;
	{
		char *slash;

		/* (each folder of it made, as mkdir -p) */
		for (slash = strchr(folder + 1, '/'); slash; slash = strchr(slash + 1, '/'))
		{
			*slash = 0;
			mkdir(folder, 0755);
			*slash = '/';
		}
		if (mkdir(folder, 0755) && errno != EEXIST)
			return NULL;
	}
	snprintf(path, size, "%s/vulkan_pipelines.bin", folder);
	return path;
}

static void vulkan_shutdown(void)
{
	static int stopped;

	if (stopped)
		return;
	stopped = 1;
	/* host_exit uses _exit, so the backend's exiting hook must also save
	the cache. Finish the committed stream before touching its device. */
	host_render_stream_close();
#ifdef HALO_VR
	xgpu_vulkan_wait(xgpu_vulkan_fence());
	host_vr_vulkan_shutdown();
#endif
	xgpu_vulkan_shutdown();
}

static void vulkan_exiting(void *context)
{
	(void)context;
	vulkan_shutdown();
}

/* (a host import: the guest's sdl_platform.c, on its main thread) */
int host_vulkan_window_open(uint32_t window)
{
	static int opened;
	SDL_Window *object = host_sdl_handle_get(window, _handle_window);
	struct xgpu_vulkan_options options;
	static struct host_render_backend backend;
	char path[1100];
	int width = 0, height = 0;

	if (!object || opened)
		return 0;
	vulkan_window = object;
	memset(&options, 0, sizeof(options));
	if (SDL_GetWindowFlags(object) & SDL_WINDOW_VULKAN)
	{
		Uint32 count = 0;
		const char *const *extensions = SDL_Vulkan_GetInstanceExtensions(&count);

		if (extensions)
		{
			options.instance_extensions = extensions;
			options.instance_extension_count = count;
			options.surface_create = surface_create;
		}
	}
	else
	{
		host_logf(HOST_LOG_INFO, "vulkan: the window is not one SDL draws with Vulkan: its frames are not shown");
	}
#ifdef HALO_VR
	if (host_vr_vulkan_requested() && host_vr_vulkan_prepare())
	{
		/* The headset's runtime chooses the physical device; its images
		are presented through OpenXR, not the desktop surface. */
		options.instance_extensions = NULL;
		options.instance_extension_count = 0;
		options.surface_create = NULL;
		options.create_instance = host_vr_vulkan_create_instance;
		options.choose_physical = host_vr_vulkan_physical_device;
		options.create_device = host_vr_vulkan_create_device;
		options.device_ready = host_vr_vulkan_bind;
	}
#endif
	options.cache_path = cache_path(path, sizeof(path));
	options.validation = getenv("HALO_VULKAN_VALIDATION") && atoi(getenv("HALO_VULKAN_VALIDATION"));
	if (!xgpu_vulkan_initialize(&options))
		return 0;
#ifdef HALO_VR
	host_vr_vulkan_set_depth_format(xgpu_vulkan_depth_format());
#endif
	SDL_GetWindowSizeInPixels(object, &width, &height);
	xgpu_vulkan_window_resized((uint32_t)width, (uint32_t)height);
	SDL_AddEventWatch(window_event, NULL);
	backend = *xgpu_vulkan_stream_backend();
	backend.exiting = vulkan_exiting;
	host_render_stream_set_backend(&backend);
	atexit(vulkan_shutdown);
	opened = 1;
	return 1;
}
