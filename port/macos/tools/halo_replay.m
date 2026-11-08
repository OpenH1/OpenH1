/*
HALO_REPLAY.M

The renderer's command stream replayed from a recording with the Metal
backend (HALO_RENDER_RECORD: port/android/host/host_render_record.h): the
replay of port/tools/replay/host_render_replay.h, its options and its
service (--serve), with Metal's own:

halo_replay <file> ... [--window] [--cold] [--archive path]

--window: in a window, paced by the display unless --unpaced; --cold: in a
throwaway app of a new bundle id (a new Metal shader cache) with an empty
pipeline archive; --archive: the pipelines' archive (default:
<file>.pipelines).
*/

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

/* (host.h's host_page_size is Mach's too: not used here) */
#define host_page_size host_page_size_of_guest_memory
#include "host.h"
#undef host_page_size
#include "host_render_record.h"
#include "host_render_replay.h"
#include "host_render_stream.h"
#include "xgpu_metal.h"
#include "xgpu_metal_internal.h"
#include "xgpu_metal_stream.h"
#include "xgpu_metal_textures.h"
#include "xgpu_astc.h"

#include <SDL3/SDL.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* host_metal.m */
int host_metal_initialize(void);
int host_metal_window_open(uint32_t window);
void host_metal_set_unpaced(int unpaced);

/* ---------- what the host gives the backend, here */

/* display.astc_textures, from HALO_ASTC_TEXTURES as the host reads it (main) */
char host_astc_textures[16] = "auto";

static SDL_Window *window;

void *host_sdl_handle_get(uint32_t handle, int type)
{
	(void)type;
	return handle == 1 ? (void *)window : NULL;
}

/* ---------- the platform */

/* the textures' cache over the loop: its bytes at the most and at the end
(all, ASTC, DXT decoded), and its counts (xgpu_metal_textures.h) */
static struct xgpu_metal_texture_statistics textures, textures_most;

static void metal_frame_statistics(struct host_render_replay_statistics *to)
{
	struct xgpu_metal_statistics statistics;
	struct xgpu_metal_texture_statistics now;

	xgpu_metal_statistics(&statistics);
	to->cpu_ms = statistics.render_cpu_seconds * 1000.0;
	to->gpu_ms = statistics.gpu_seconds * 1000.0;
	to->compile_ms = statistics.pipeline_seconds * 1000.0;
	to->draws = statistics.draws;
	to->compiles = statistics.pipelines_made;
	to->shader_ms = statistics.shader_seconds * 1000.0;
	to->shaders = statistics.shaders_made;
	xgpu_metal_textures_statistics(&now);
	textures.astc_transcoded += now.astc_transcoded;
	textures.astc_encoded += now.astc_encoded;
	textures.astc_left_decoded += now.astc_left_decoded;
	textures.resident = now.resident;
	textures.resident_astc = now.resident_astc;
	textures.resident_decoded = now.resident_decoded;
	textures.astc_waiting = now.astc_waiting;
	if (now.resident > textures_most.resident)
		textures_most = now;
}

static void metal_report(void)
{
	struct xgpu_astc_statistics astc;

	xgpu_astc_statistics(&astc);
	printf("  textures: at the most %.1f MB (DXT as ASTC %.1f MB, DXT decoded %.1f MB), at the end %.1f MB "
		"(%.1f, %.1f); DXT textures transcoded to ASTC %u, encoded blocks applied %u, left decoded %u, waiting %u; "
		"the encoder: %u jobs, %u from the disk cache, %llu blocks in %.2f s\n",
		textures_most.resident / 1048576.0, textures_most.resident_astc / 1048576.0,
		textures_most.resident_decoded / 1048576.0, textures.resident / 1048576.0,
		textures.resident_astc / 1048576.0, textures.resident_decoded / 1048576.0,
		textures.astc_transcoded, textures.astc_encoded, textures.astc_left_decoded,
		textures.astc_waiting, astc.jobs, astc.cache_hits, (unsigned long long)astc.blocks_encoded,
		astc.encode_seconds);
}

static int metal_initialize(int windowed, int width, int height)
{
	if (!windowed)
		return host_metal_initialize();
	if (!SDL_Init(SDL_INIT_VIDEO) ||
		!(window = SDL_CreateWindow("halo_replay", width / 2 > 640 ? width / 2 : width,
			width / 2 > 640 ? height / 2 : height, SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY)) ||
		!host_metal_window_open(1))
	{
		fprintf(stderr, "halo_replay: no window (%s)\n", SDL_GetError());
		return 0;
	}
	return 1;
}

static void metal_pump(void)
{
	SDL_Event event;

	@autoreleasepool
	{
		while (SDL_PollEvent(&event))
			;
	}
}

static unsigned char *metal_read_target(uint32_t target, uint32_t *width, uint32_t *height)
{
	@autoreleasepool
	{
		return xgpu_metal_stream_read(target, width, height);
	}
}

static const struct host_render_replay_platform metal_platform =
{
	.backend = "metal",
	.window_supported = 1,
	.initialize = metal_initialize,
	.set_unpaced = host_metal_set_unpaced,
	.pump = metal_pump,
	.shutdown = xgpu_metal_shutdown,
	.frame_statistics = metal_frame_statistics,
	.report = metal_report,
	.read_target = metal_read_target,
};

/* ---------- --cold: this program again, in an app of a new bundle id

Metal keeps its compiled shaders by the app's bundle id (the user's cache
folder's <bundle id>/com.apple.metal); a program outside an app shares
one cache with every other (a copy under another name compiles nothing). A
throwaway app of a new id, with an empty pipeline archive, starts with
none; it and its cache are removed afterwards. */

static int cold_run(int argc, char *argv[])
{
	char self[4096], source_library[4200], identifier[128], cache[4400];
	uint32_t size = sizeof(self);
	char **arguments = calloc((size_t)argc + 4, sizeof(char *));
	int index, count = 0, has_archive = 0, status = 2;
	pid_t child;
	char *slash;
	NSFileManager *files = [NSFileManager defaultManager];
	NSString *app, *macos, *copy, *archive, *plist;

	if (_NSGetExecutablePath(self, &size) || !arguments)
		return 2;
	snprintf(identifier, sizeof(identifier), "com.openh1.halo-replay.cold-%ld-%d", (long)time(NULL), getpid());
	app = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"%s.app", identifier]];
	macos = [app stringByAppendingPathComponent:@"Contents/MacOS"];
	copy = [macos stringByAppendingPathComponent:@"halo_replay"];
	archive = [app stringByAppendingPathComponent:@"pipelines.bin"];
	snprintf(source_library, sizeof(source_library), "%s", self);
	slash = strrchr(source_library, '/');
	if (slash)
		snprintf(slash + 1, sizeof(source_library) - (size_t)(slash + 1 - source_library), "libSDL3.0.dylib");
	plist = [NSString stringWithFormat:@"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>"
		"<key>CFBundleIdentifier</key><string>%s</string><key>CFBundleExecutable</key><string>halo_replay</string>"
		"<key>CFBundlePackageType</key><string>APPL</string></dict></plist>\n", identifier];
	if (![files createDirectoryAtPath:macos withIntermediateDirectories:YES attributes:nil error:nil] ||
		![plist writeToFile:[app stringByAppendingPathComponent:@"Contents/Info.plist"] atomically:NO
			encoding:NSUTF8StringEncoding error:nil] ||
		![files copyItemAtPath:@(self) toPath:copy error:nil] ||
		![files copyItemAtPath:@(source_library) toPath:[macos stringByAppendingPathComponent:@"libSDL3.0.dylib"]
			error:nil])
	{
		fprintf(stderr, "halo_replay: cannot make %s\n", app.UTF8String);
		return 2;
	}
	/* (the shaders built ahead of time, as the game's app has them) */
	if (slash)
		snprintf(slash + 1, sizeof(source_library) - (size_t)(slash + 1 - source_library), "shaders.metallib");
	if (slash && [files fileExistsAtPath:@(source_library)])
	{
		NSString *resources = [app stringByAppendingPathComponent:@"Contents/Resources"];

		[files createDirectoryAtPath:resources withIntermediateDirectories:YES attributes:nil error:nil];
		[files copyItemAtPath:@(source_library) toPath:[resources stringByAppendingPathComponent:@"shaders.metallib"]
			error:nil];
	}
	arguments[count++] = (char *)copy.UTF8String;
	for (index = 1; index < argc; index++)
	{
		if (!strcmp(argv[index], "--cold"))
			continue;
		if (!strcmp(argv[index], "--archive"))
			has_archive = 1;
		arguments[count++] = argv[index];
	}
	if (!has_archive)
	{
		arguments[count++] = "--archive";
		arguments[count++] = (char *)archive.UTF8String;
	}
	printf("cold: in %s (bundle id %s: a new Metal cache; %s pipeline archive)\n", app.UTF8String, identifier,
		has_archive ? "the given" : "an empty");
	fflush(stdout);
	child = fork();
	if (child == 0)
	{
		execv(arguments[0], arguments);
		_exit(127);
	}
	if (child > 0 && waitpid(child, &status, 0) == child)
		status = WIFEXITED(status) ? WEXITSTATUS(status) : 2;
	[files removeItemAtPath:app error:nil];
	/* (the user's cache folder: confstr's, the folder Metal's caches are in) */
	{
		char directory[4096];

		if (confstr(_CS_DARWIN_USER_CACHE_DIR, directory, sizeof(directory)))
		{
			snprintf(cache, sizeof(cache), "%s/%s", directory, identifier);
			[files removeItemAtPath:@(cache) error:nil];
		}
	}
	return status;
}

/* ---------- main */

/* the shared replay's options that take a value */
static int takes_value(const char *option)
{
	return !strcmp(option, "--frames") || !strcmp(option, "--shots") || !strcmp(option, "--shot-frames") ||
		!strcmp(option, "--loop") || !strcmp(option, "--hitch");
}

int main(int argc, char *argv[])
{
	const char *archive = NULL, *path = NULL;
	char archive_default[4200];
	char **arguments = calloc((size_t)argc + 1, sizeof(char *));
	int index, count = 0, cold = 0;

	if (getenv("HALO_ASTC_TEXTURES") && *getenv("HALO_ASTC_TEXTURES"))
		snprintf(host_astc_textures, sizeof(host_astc_textures), "%s", getenv("HALO_ASTC_TEXTURES"));
	if (!arguments)
		return 2;
	/* (Metal's own options taken out; the others are the shared replay's) */
	for (index = 0; index < argc; index++)
	{
		if (index && !strcmp(argv[index], "--cold"))
			cold = 1;
		else if (index && !strcmp(argv[index], "--archive") && index + 1 < argc)
			archive = argv[++index];
		else
		{
			arguments[count++] = argv[index];
			if (index && takes_value(argv[index]) && index + 1 < argc)
				arguments[count++] = argv[++index];
			else if (index && argv[index][0] != '-' && !path)
				path = argv[index];
		}
	}
	if (cold)
	{
		@autoreleasepool
		{
			return cold_run(argc, argv);
		}
	}
	if (path && !(argc > 1 && !strcmp(argv[1], "--compare")))
	{
		if (!archive)
		{
			snprintf(archive_default, sizeof(archive_default), "%s.pipelines", path);
			archive = archive_default;
		}
		setenv("HALO_METAL_ARCHIVE", archive, 1);
	}
	@autoreleasepool
	{
		return host_render_replay_main(count, arguments, &metal_platform);
	}
}
