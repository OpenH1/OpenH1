/*
HOST_METAL.M

The game's window on macOS, drawn with Metal: its layer, the Metal backend
(xgpu_metal.m) set up for it, and the presentation of frames, paced by the
display.

The guest makes the window with SDL_WINDOW_METAL (sdl_platform.c) and hands
it over (host_metal_window_open, on the main thread, which Cocoa's views
need): SDL makes a view with a CAMetalLayer, whose drawables are the
window's pixels (Retina's included; resized with the window, and in
fullscreen).

Presentation runs on a thread of its own, driven by a CAMetalDisplayLink on
its run loop: at each of the display's refreshes the link hands over a
drawable, and the thread draws the oldest frame queued (letterboxed to the
frame's shape) into it and presents it. A ProMotion display's rate follows
the frames, up to its highest (120 Hz); a fixed one's is its own. A frame's
picture is copied into one of three presentation images when the frame
ends (host_metal_present_exchange): the back buffer's texture itself, whose
place the image it replaces takes for the next frame (xgpu_metal.m). The
images are the pacing: with all three queued or on screen, the next frame
waits, so the game runs no faster than the display shows frames.

Until frames come (the null renderer draws none), each refresh clears the
drawable to black and presents it.

HALO_METAL_STATS=<seconds> logs the pacing that often: the link's
refreshes, the frames presented, the intervals between the times the
drawables reached the screen.
*/

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

/* (host.h's host_page_size is Mach's too: not used here) */
#define host_page_size host_page_size_of_guest_memory
#include "host.h"
#undef host_page_size
#include "xgpu_metal_internal.h"
#include "host_render_record.h"
#include "host_render_stream.h"
#include "xgpu_metal_stream.h"
#include "xgpu_metal_textures.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <math.h>
#include <pthread.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PRESENT_IMAGES 3
/* the longest a frame waits for an image before the oldest queued frame is
dropped (100 ms: six refreshes at 60 Hz) */
#define PRESENT_WAIT_MAXIMUM_NS 100000000L

enum
{
	_image_free,
	_image_queued,
	/* drawn into a drawable, until that command buffer is done */
	_image_showing,
};

static struct
{
	SDL_Window *window;
	SDL_MetalView view;
	CAMetalLayer *layer;
	CAMetalDisplayLink *link;
	id<MTLRenderPipelineState> present_pipeline;
	id<MTLSamplerState> present_sampler;
	_Atomic int active;
	/* the window's size in pixels, as SDL last reported it */
	_Atomic uint32_t pixel_width, pixel_height;
	uint32_t drawable_width, drawable_height;
	float refresh_rate;

	pthread_mutex_t lock;
	pthread_cond_t freed;
	id<MTLTexture> images[PRESENT_IMAGES];
	int image_states[PRESENT_IMAGES];
	/* queued images, oldest first */
	int queue[PRESENT_IMAGES];
	int queued;
	BOOL frames_seen;
	/* a frame waits for an image (host_metal_present_exchange) */
	_Atomic int image_waiting;
	BOOL unpaced;

	/* pacing, since the last log */
	double stats_interval, stats_since;
	uint32_t ticks, frames_presented, idle_presented, skipped, waits, dropped;
	/* the refreshes without a new frame by what the render thread was doing:
	waiting for the GPU to finish a frame, for an image, playing the stream,
	or waiting for the game's */
	uint32_t skipped_gpu, skipped_image, skipped_playing, skipped_game;
	_Atomic uint32_t shown, late, intervals;
	_Atomic uint64_t interval_sum_us, interval_minimum_us, interval_maximum_us;
	/* the presentations' GPU time, their wait from commit to the GPU's
	start, and those shown after the refresh the link drew them for */
	_Atomic uint64_t present_gpu_ns, present_queued_ns, present_queued_most_ns;
	_Atomic uint32_t present_buffers, missed;
	/* presentations whose GPU work ended after their target time less a
	millisecond (the display's latch), and the least slack */
	_Atomic uint32_t present_gpu_late;
	_Atomic int64_t present_slack_least_us;
	double last_presented;
	/* the link's callbacks more than 1.5 refreshes after the last: with
	every drawable still to reach the screen, with some free, and those
	right after another such */
	double last_tick, last_target;
	/* of those, the ones whose target time skipped a refresh too (the link's
	choice), and those for the next refresh (delivered late) */
	uint32_t gaps_target_skipped, gaps_delivered_late;
	uint32_t gaps_starved, gaps_free, gaps_following, gaps_after_slow;
	/* the last callback's time from its start to its return, and the most */
	double last_callback_seconds, callback_most_seconds;
	BOOL last_gap;
	_Atomic int drawables_pending;
} presentation = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.freed = PTHREAD_COND_INITIALIZER,
};

int host_metal_window_active(void)
{
	return atomic_load(&presentation.active);
}

/* ---------- the presentation images */

id<MTLTexture> host_metal_present_exchange(id<MTLTexture> picture)
{
	int index, found = -1;
	id<MTLTexture> replaced;

	pthread_mutex_lock(&presentation.lock);
	for (;;)
	{
		struct timespec until;

		for (index = 0; index < PRESENT_IMAGES && found < 0; index++)
		{
			if (presentation.image_states[index] == _image_free)
				found = index;
		}
		if (found >= 0)
			break;
		/* (HALO_METAL_UNPACED: the oldest queued is dropped at once, so that
		the game draws as fast as the GPU and the render thread can: a
		measure of their work at their full speed) */
		if (presentation.unpaced && presentation.queued)
		{
			found = presentation.queue[0];
			memmove(presentation.queue, presentation.queue + 1, sizeof(int) * (size_t)(presentation.queued - 1));
			presentation.queued--;
			presentation.dropped++;
			break;
		}
		presentation.waits++;
		atomic_store(&presentation.image_waiting, 1);
		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_nsec += PRESENT_WAIT_MAXIMUM_NS;
		if (until.tv_nsec >= 1000000000L)
		{
			until.tv_sec++;
			until.tv_nsec -= 1000000000L;
		}
		if (pthread_cond_timedwait(&presentation.freed, &presentation.lock, &until) == ETIMEDOUT &&
			presentation.queued)
		{
			/* the display takes no frames (the window minimized, covered,
			or between the desktop and fullscreen): the oldest queued is
			dropped, so that the game goes on */
			found = presentation.queue[0];
			memmove(presentation.queue, presentation.queue + 1, sizeof(int) * (size_t)(presentation.queued - 1));
			presentation.queued--;
			presentation.dropped++;
			break;
		}
	}
	atomic_store(&presentation.image_waiting, 0);
	replaced = presentation.images[found];
	presentation.images[found] = picture;
	presentation.image_states[found] = _image_queued;
	pthread_mutex_unlock(&presentation.lock);
	return replaced && replaced.width == picture.width && replaced.height == picture.height &&
		replaced.pixelFormat == picture.pixelFormat && replaced.usage == picture.usage &&
		replaced.storageMode == picture.storageMode ? replaced : nil;
}

void host_metal_present_queue(id<MTLTexture> image)
{
	int index;

	pthread_mutex_lock(&presentation.lock);
	for (index = 0; index < PRESENT_IMAGES; index++)
	{
		if (presentation.images[index] == image && presentation.queued < PRESENT_IMAGES)
		{
			presentation.queue[presentation.queued++] = index;
			presentation.frames_seen = YES;
			break;
		}
	}
	pthread_mutex_unlock(&presentation.lock);
}

static void image_release(int index)
{
	pthread_mutex_lock(&presentation.lock);
	presentation.image_states[index] = _image_free;
	pthread_cond_broadcast(&presentation.freed);
	pthread_mutex_unlock(&presentation.lock);
}

/* ---------- pacing */

static double seconds_now(void)
{
	return CACurrentMediaTime();
}

static void presented(double when, double target)
{
	double previous = presentation.last_presented;

	if (when <= 0.0)
		return;
	/* (a refresh's tolerance: the times are the display's) */
	if (target > 0.0 && when > target + 0.001)
		atomic_fetch_add(&presentation.missed, 1);
	presentation.last_presented = when;
	atomic_fetch_add(&presentation.shown, 1);
	if (previous > 0.0)
	{
		uint64_t interval = (uint64_t)((when - previous) * 1e6);
		uint64_t minimum = atomic_load(&presentation.interval_minimum_us);

		atomic_fetch_add(&presentation.interval_sum_us, interval);
		atomic_fetch_add(&presentation.intervals, 1);
		if (!minimum || interval < minimum)
			atomic_store(&presentation.interval_minimum_us, interval);
		if (interval > atomic_load(&presentation.interval_maximum_us))
			atomic_store(&presentation.interval_maximum_us, interval);
		if (presentation.refresh_rate > 0.0f && (double)interval > 1.5e6 / presentation.refresh_rate)
			atomic_fetch_add(&presentation.late, 1);
	}
}

static void pacing_log(double now)
{
	double seconds = now - presentation.stats_since;
	uint32_t shown = atomic_exchange(&presentation.shown, 0);
	uint64_t sum = atomic_exchange(&presentation.interval_sum_us, 0);
	uint32_t intervals = atomic_exchange(&presentation.intervals, 0);
	struct xgpu_metal_statistics statistics;
	struct xgpu_metal_texture_statistics textures;
	const struct xgpu_stream_ring *ring = host_render_stream_ring();
	static uint32_t waits_before, wait_nanoseconds_before;
	uint32_t waits = ring ? ring->waits : 0, wait_nanoseconds = ring ? ring->wait_nanoseconds : 0;
	uint32_t buffers = atomic_exchange(&presentation.present_buffers, 0);

	xgpu_metal_statistics(&statistics);
	xgpu_metal_textures_statistics(&textures);
	host_logf(HOST_LOG_INFO,
		"metal: %.1f s: %u refreshes (%.1f Hz), %u presented (%u frames, %u cleared), %u on screen (%.1f a second), "
		"intervals %.2f ms mean, %.2f min, %.2f max, %u late; %u refreshes without a new frame (the GPU %u, an image %u, "
		"playing %u, the game %u), %u callbacks late (drawables all pending %u, some free %u; after another %u, after a slow one %u, target skipped %u, delivered late %u; the slowest %.2f ms), %u waits for an image, "
		"%u frames dropped, %u shown after their refresh; "
		"%u ended on the GPU within 1 ms of their target (the least slack %.2f ms); "
		"drawable %ux%u, presentation's GPU %.3f ms a refresh, queued %.3f ms (the most %.3f); frames %u, draws %u, passes %u",
		seconds, presentation.ticks, presentation.ticks / seconds, presentation.frames_presented + presentation.idle_presented,
		presentation.frames_presented, presentation.idle_presented, shown, shown / seconds,
		intervals ? (double)sum / 1000.0 / intervals : 0.0,
		atomic_exchange(&presentation.interval_minimum_us, 0) / 1000.0,
		atomic_exchange(&presentation.interval_maximum_us, 0) / 1000.0, atomic_exchange(&presentation.late, 0),
		presentation.skipped, presentation.skipped_gpu, presentation.skipped_image, presentation.skipped_playing,
		presentation.skipped_game, presentation.gaps_starved + presentation.gaps_free, presentation.gaps_starved,
		presentation.gaps_free, presentation.gaps_following, presentation.gaps_after_slow, presentation.gaps_target_skipped,
		presentation.gaps_delivered_late,
		presentation.callback_most_seconds * 1000.0, presentation.waits, presentation.dropped, atomic_exchange(&presentation.missed, 0),
		atomic_exchange(&presentation.present_gpu_late, 0), atomic_exchange(&presentation.present_slack_least_us, 1000000) / 1000.0,
		presentation.drawable_width, presentation.drawable_height,
		buffers ? atomic_exchange(&presentation.present_gpu_ns, 0) / 1e6 / buffers : 0.0,
		buffers ? atomic_exchange(&presentation.present_queued_ns, 0) / 1e6 / buffers : 0.0,
		atomic_exchange(&presentation.present_queued_most_ns, 0) / 1e6,
		statistics.frames, statistics.draws, statistics.passes);
	if (statistics.frames)
		host_logf(HOST_LOG_INFO,
			"metal: a frame: GPU busy %.2f ms (%.0f%% of the time); its command buffers' GPU time %.2f ms (%u), render thread CPU %.2f ms, %.1f draws, %.1f passes, "
			"%.1f ordered uploads, %.1f copies of a target drawn into, %.1f pictures restored; the last played in %.2f ms, its stream %u KB "
			"(the most %u KB); the game waited %u times (%.1f ms); textures made %u, made again %u, images %u, "
			"%.1f MB read; pipelines made %u (%u from the archive, %.1f ms)",
			statistics.gpu_busy_seconds * 1000.0 / statistics.frames, statistics.gpu_busy_seconds * 100.0 / seconds,
			statistics.gpu_seconds * 1000.0 / statistics.frames, statistics.command_buffers,
			statistics.render_cpu_seconds * 1000.0 / statistics.frames, (double)statistics.draws / statistics.frames,
			(double)statistics.passes / statistics.frames, (double)statistics.uploads_ordered / statistics.frames,
			(double)statistics.feedback_copies / statistics.frames,
			(double)statistics.pictures_restored / statistics.frames, ring ? ring->decode_nanoseconds / 1e6 : 0.0,
			ring ? ring->frame_bytes / 1024 : 0, ring ? ring->frame_bytes_most / 1024 : 0, waits - waits_before,
			(wait_nanoseconds - wait_nanoseconds_before) / 1e6, textures.made, textures.remade, textures.images,
			textures.bytes / 1048576.0, statistics.pipelines_made, statistics.pipelines_from_archive,
			statistics.pipeline_seconds * 1000.0);
	waits_before = waits;
	wait_nanoseconds_before = wait_nanoseconds;
	presentation.stats_since = now;
	presentation.ticks = presentation.frames_presented = presentation.idle_presented = 0;
	presentation.skipped = presentation.waits = presentation.dropped = 0;
	presentation.gaps_starved = presentation.gaps_free = presentation.gaps_following = presentation.gaps_after_slow = 0;
	presentation.gaps_target_skipped = presentation.gaps_delivered_late = 0;
	presentation.callback_most_seconds = 0.0;
	presentation.skipped_gpu = presentation.skipped_image = presentation.skipped_playing = presentation.skipped_game = 0;
}

/* ---------- each refresh */

@interface HaloDisplayLinkTarget : NSObject <CAMetalDisplayLinkDelegate>
@end

@implementation HaloDisplayLinkTarget

- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update
{
	double start = seconds_now();

	[self refresh:update];
	presentation.last_callback_seconds = seconds_now() - start;
	if (presentation.last_callback_seconds > presentation.callback_most_seconds)
		presentation.callback_most_seconds = presentation.last_callback_seconds;
	(void)link;
}

- (void)refresh:(CAMetalDisplayLinkUpdate *)update
{
	CAMetalDisplayLink *link = presentation.link;

	@autoreleasepool
	{
		id<CAMetalDrawable> drawable = update.drawable;
		uint32_t width = atomic_load(&presentation.pixel_width), height = atomic_load(&presentation.pixel_height);
		MTLRenderPassDescriptor *pass;
		id<MTLCommandBuffer> buffer;
		id<MTLRenderCommandEncoder> encoder;
		id<MTLTexture> image = nil;
		double now = seconds_now(), target = update.targetPresentationTimestamp, committed;
		int index = -1;
		BOOL idle;

		(void)link;
		presentation.ticks++;
		if (presentation.last_tick > 0.0 && presentation.refresh_rate > 0.0f &&
			now - presentation.last_tick > 1.5 / presentation.refresh_rate)
		{
			if (atomic_load(&presentation.drawables_pending) >= (int)presentation.layer.maximumDrawableCount - 1)
				presentation.gaps_starved++;
			else
				presentation.gaps_free++;
			if (presentation.last_gap)
				presentation.gaps_following++;
			if (presentation.last_callback_seconds > 1.0 / presentation.refresh_rate)
				presentation.gaps_after_slow++;
			if (update.targetPresentationTimestamp - presentation.last_target > 1.5 / presentation.refresh_rate)
				presentation.gaps_target_skipped++;
			else
				presentation.gaps_delivered_late++;
			presentation.last_gap = YES;
		}
		else
		{
			presentation.last_gap = NO;
		}
		presentation.last_tick = now;
		presentation.last_target = update.targetPresentationTimestamp;
		if (presentation.stats_interval > 0.0 && now - presentation.stats_since >= presentation.stats_interval)
			pacing_log(now);
		/* (a new size is the next drawable's) */
		if (width && height && (width != presentation.drawable_width || height != presentation.drawable_height))
		{
			presentation.layer.drawableSize = CGSizeMake(width, height);
			presentation.drawable_width = width;
			presentation.drawable_height = height;
		}
		pthread_mutex_lock(&presentation.lock);
		if (presentation.queued)
		{
			index = presentation.queue[0];
			memmove(presentation.queue, presentation.queue + 1, sizeof(int) * (size_t)(presentation.queued - 1));
			presentation.queued--;
			presentation.image_states[index] = _image_showing;
			image = presentation.images[index];
		}
		idle = !presentation.frames_seen;
		pthread_mutex_unlock(&presentation.lock);
		if (!image && !idle)
		{
			/* (the frame shown stays: nothing new to present) */
			presentation.skipped++;
			if (xgpu_metal_frame_waiting())
				presentation.skipped_gpu++;
			else if (atomic_load(&presentation.image_waiting))
				presentation.skipped_image++;
			else if (host_render_stream_playing())
				presentation.skipped_playing++;
			else
				presentation.skipped_game++;
			return;
		}

		buffer = [xgpu_metal_queue() commandBuffer];
		pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = drawable.texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		/* (black until the game's first frame: the window opens before the
		game draws) */
		pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
		encoder = [buffer renderCommandEncoderWithDescriptor:pass];
		if (image)
		{
			/* letterboxed to the frame's shape */
			double drawable_width = drawable.texture.width, drawable_height = drawable.texture.height;
			double fit_width = drawable_width, fit_height = drawable_width * image.height / image.width;

			if (fit_height > drawable_height)
			{
				fit_height = drawable_height;
				fit_width = drawable_height * image.width / image.height;
			}
			[encoder setRenderPipelineState:presentation.present_pipeline];
			[encoder setViewport:(MTLViewport){ floor((drawable_width - fit_width) / 2.0),
				floor((drawable_height - fit_height) / 2.0), floor(fit_width), floor(fit_height), 0.0, 1.0 }];
			[encoder setFragmentTexture:image atIndex:0];
			[encoder setFragmentSamplerState:presentation.present_sampler atIndex:0];
			[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		}
		[encoder endEncoding];
		atomic_fetch_add(&presentation.drawables_pending, 1);
		[drawable addPresentedHandler:^(id<MTLDrawable> shown) {
			atomic_fetch_sub(&presentation.drawables_pending, 1);
			presented(shown.presentedTime, target);
		}];
		[buffer presentDrawable:drawable];
		committed = seconds_now();
		[buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
			if (done.GPUEndTime > done.GPUStartTime)
			{
				uint64_t queued = done.GPUStartTime > committed ? (uint64_t)((done.GPUStartTime - committed) * 1e9) : 0;

				xgpu_metal_gpu_interval(done.GPUStartTime, done.GPUEndTime);
				atomic_fetch_add(&presentation.present_gpu_ns, (uint64_t)((done.GPUEndTime - done.GPUStartTime) * 1e9));
				atomic_fetch_add(&presentation.present_queued_ns, queued);
				if (queued > atomic_load(&presentation.present_queued_most_ns))
					atomic_store(&presentation.present_queued_most_ns, queued);
				atomic_fetch_add(&presentation.present_buffers, 1);
				{
					int64_t slack = (int64_t)((target - done.GPUEndTime) * 1e6);

					if (slack < 1000)
						atomic_fetch_add(&presentation.present_gpu_late, 1);
					if (slack < atomic_load(&presentation.present_slack_least_us))
						atomic_store(&presentation.present_slack_least_us, slack);
				}
			}
		}];
		if (index >= 0)
		{
			[buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
				(void)done;
				image_release(index);
			}];
			presentation.frames_presented++;
		}
		else
		{
			presentation.idle_presented++;
		}
		[buffer commit];
	}
}

@end

static NSString *const present_source =
	@"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct present_vertex { float4 position [[position]]; float2 coordinates; };\n"
	"vertex present_vertex halo_present_vertex(uint id [[vertex_id]])\n"
	"{\n"
	"\tpresent_vertex out;\n"
	"\tfloat2 corner = float2(id & 1, id >> 1);\n"
	"\tout.position = float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);\n"
	"\tout.coordinates = corner;\n"
	"\treturn out;\n"
	"}\n"
	"fragment float4 halo_present_fragment(present_vertex in [[stage_in]], texture2d<float> picture [[texture(0)]],\n"
	"\tsampler linear [[sampler(0)]])\n"
	"{\n"
	"\treturn float4(picture.sample(linear, in.coordinates).rgb, 1.0);\n"
	"}\n";

static void *presentation_thread(void *context)
{
	HaloDisplayLinkTarget *target = (__bridge_transfer HaloDisplayLinkTarget *)context;

	pthread_setname_np("metal presentation");
	@autoreleasepool
	{
		CAMetalDisplayLink *link = [[CAMetalDisplayLink alloc] initWithMetalLayer:presentation.layer];
		float rate = presentation.refresh_rate > 0.0f ? presentation.refresh_rate : 60.0f;

		link.delegate = target;
		/* (a ProMotion display's rate follows the frames, up to its
		highest) */
		link.preferredFrameRateRange = CAFrameRateRangeMake(rate < 60.0f ? rate : 30.0f, rate, rate);
		link.preferredFrameLatency = 1.0f;
		presentation.link = link;
		[link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
	}
	for (;;)
	{
		@autoreleasepool
		{
			[[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate distantFuture]];
		}
	}
	return NULL;
}

/* (the main thread, as SDL reports the window's events) */
static bool SDLCALL window_event(void *userdata, SDL_Event *event)
{
	(void)userdata;
	if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
		SDL_GetWindowFromID(event->window.windowID) == presentation.window)
	{
		atomic_store(&presentation.pixel_width, (uint32_t)event->window.data1);
		atomic_store(&presentation.pixel_height, (uint32_t)event->window.data2);
	}
	return true;
}

/* the backend, set up for this process (once; host_main.c, before the
guest starts, so that the command stream finds it) */
int host_metal_initialize(void)
{
	static int result = -1;
	struct xgpu_metal_options options;
	char path[1024];
	const char *archive = getenv("HALO_METAL_ARCHIVE");
	const char *home = getenv("HOME");

	if (result >= 0)
		return result;
	memset(&options, 0, sizeof(options));
	if (!archive && home)
	{
		snprintf(path, sizeof(path), "%s/Library/Caches/OpenCE/metal_pipelines.bin", home);
		archive = path;
	}
	options.archive_path = archive && *archive ? archive : NULL;
	options.page_size = (uint32_t)getpagesize();
	options.guest_extent = host_memory_readable_extent;
	result = xgpu_metal_initialize(&options);
	if (result)
	{
		host_memory_remapped = xgpu_metal_guest_remapped;
		host_render_stream_set_backend(xgpu_metal_stream_backend());
		xgpu_metal_stream_set_shaders(xgpu_metal_shaders());
		host_render_record_set_shooter(xgpu_metal_stream_shot);
	}
	return result;
}

/* (the replay's: port/macos/tools/halo_replay.m) */
void host_metal_set_unpaced(int unpaced)
{
	presentation.unpaced = unpaced ? YES : NO;
}

/* (a host import: the guest's sdl_platform.c, on the main thread) */
int host_metal_window_open(uint32_t window)
{
	SDL_Window *object = host_sdl_handle_get(window, _handle_window);
	id<MTLLibrary> library;
	MTLRenderPipelineDescriptor *descriptor;
	MTLSamplerDescriptor *sampler;
	NSError *error = nil;
	pthread_t thread;
	pthread_attr_t attributes;
	int width = 0, height = 0, created;
	const char *stats = getenv("HALO_METAL_STATS");

	if (!object || presentation.window || !host_metal_initialize())
		return 0;
	presentation.view = SDL_Metal_CreateView(object);
	if (!presentation.view)
	{
		host_logf(HOST_LOG_ERROR, "metal: cannot make the window's view: %s", SDL_GetError());
		return 0;
	}
	presentation.window = object;
	/* (in front: a window started behind others, as one from a terminal
	can be, shows nothing until it is brought forward, and the drawables
	presented meanwhile never reach the screen) */
	SDL_RaiseWindow(object);
	presentation.layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(presentation.view);
	presentation.layer.device = xgpu_metal_device();
	presentation.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	presentation.layer.framebufferOnly = YES;
	/* (the presentation writes every pixel, alpha 1: the window server
	need not draw what is behind the window, nor blend the window over it) */
	presentation.layer.opaque = YES;
	presentation.layer.maximumDrawableCount = 3;
	SDL_GetWindowSizeInPixels(object, &width, &height);
	atomic_store(&presentation.pixel_width, (uint32_t)width);
	atomic_store(&presentation.pixel_height, (uint32_t)height);
	presentation.layer.drawableSize = CGSizeMake(width, height);
	presentation.drawable_width = (uint32_t)width;
	presentation.drawable_height = (uint32_t)height;
	{
		NSWindow *native = (__bridge NSWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(object),
			SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
		NSScreen *screen = native.screen ? native.screen : NSScreen.mainScreen;

		presentation.refresh_rate = (float)screen.maximumFramesPerSecond;
		host_logf(HOST_LOG_INFO, "metal: window %dx%d pixels (scale %.1f), display %s at up to %ld Hz%s", width, height,
			screen.backingScaleFactor, screen.localizedName.UTF8String, (long)screen.maximumFramesPerSecond,
			screen.minimumRefreshInterval < screen.maximumRefreshInterval ? " (variable)" : "");
	}

	library = [xgpu_metal_device() newLibraryWithSource:present_source options:nil error:&error];
	descriptor = [MTLRenderPipelineDescriptor new];
	descriptor.vertexFunction = [library newFunctionWithName:@"halo_present_vertex"];
	descriptor.fragmentFunction = [library newFunctionWithName:@"halo_present_fragment"];
	descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	presentation.present_pipeline = library ? [xgpu_metal_device() newRenderPipelineStateWithDescriptor:descriptor
		error:&error] : nil;
	if (!presentation.present_pipeline)
	{
		host_logf(HOST_LOG_ERROR, "metal: the presentation's pipeline: %s", error.localizedDescription.UTF8String);
		return 0;
	}
	sampler = [MTLSamplerDescriptor new];
	sampler.minFilter = MTLSamplerMinMagFilterLinear;
	sampler.magFilter = MTLSamplerMinMagFilterLinear;
	presentation.present_sampler = [xgpu_metal_device() newSamplerStateWithDescriptor:sampler];

	SDL_AddEventWatch(window_event, NULL);
	presentation.stats_interval = stats ? atof(stats) : 0.0;
	presentation.unpaced = getenv("HALO_METAL_UNPACED") && atoi(getenv("HALO_METAL_UNPACED"));
	presentation.stats_since = seconds_now();
	atomic_store(&presentation.present_slack_least_us, 1000000);
	atomic_store(&presentation.active, 1);
	/* (the display's deadlines are each refresh's: the scheduler runs it
	first, as it does the main threads of apps in front) */
	pthread_attr_init(&attributes);
	pthread_attr_set_qos_class_np(&attributes, QOS_CLASS_USER_INTERACTIVE, 0);
	created = pthread_create(&thread, &attributes, presentation_thread, (__bridge_retained void *)[HaloDisplayLinkTarget new]);
	pthread_attr_destroy(&attributes);
	if (created != 0)
	{
		atomic_store(&presentation.active, 0);
		host_logf(HOST_LOG_ERROR, "metal: cannot start the presentation thread");
		return 0;
	}
	pthread_detach(thread);
	return 1;
}
