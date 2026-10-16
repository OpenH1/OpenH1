/*
XGPU_METAL.M

The Metal backend: what xgpu_metal.h declares. Refer to it for what the
backend does; this file says how.

Frames. Each frame is a command buffer (more where a pixel read waits for
the GPU mid-frame), committed by xgpu_metal_frame_end, and its fence is the
serial of its last command buffer, counted by the buffers' completion
handlers (a queue's buffers complete in order). xgpu_metal_frame_begin
waits for the frame two before it, and takes that frame's place in the
three sets of per-frame resources: the upload ring's chunks and the
visibility result buffer.

Passes. set_targets ends the render command encoder (not for the same
targets: the pass, or the clears waiting for its start, go on); the next
draw or clear begins one, with load actions "load", or "clear" for an
attachment that a clear of the whole of it came first for. Any other clear
is a quad drawn through a clear pipeline. Uploads into a texture the GPU may still read are
blits in the command buffer's order (they end the encoder; the pass goes on
with "load"); into one it cannot, they write the texture at once.

Presentation. A frame's picture (the back buffer's texture) is given to the
presentation (host_metal.m) and the back buffer takes another texture, the
picture's kept as its previous: what draws or reads the back buffer before
a clear of the whole of it copies that in first (texture_contents).

Guest data. A guest range read in place is wrapped in a buffer over the
readable host pages around it, up to a granule of 4 MB, and the buffer is
kept until the guest maps those pages again (xgpu_metal_guest_remapped: a
buffer keeps the pages it was made over, and would not see the new ones).
Ranges that cannot be wrapped are copied into the upload ring.
*/

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

/* (host.h's host_page_size is Mach's too: not used here) */
#define host_page_size host_page_size_of_guest_memory
#include "host.h"
#undef host_page_size
#include "xgpu_metal_internal.h"
#include "xgpu_metal_pipeline.h"
#include "xgpu_metal_combiner.h"

#include <errno.h>
#include <os/lock.h>
#include <float.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FRAMES_IN_FLIGHT 2
#define FRAME_SLOTS (FRAMES_IN_FLIGHT + 1)
#define TEXTURE_MAXIMUM 16384
#define SHADER_MAXIMUM 8192
/* the upload ring: chunks of 8 MB (more where a frame needs them), the
offsets of chunk n starting at n << RING_CHUNK_SHIFT */
#define RING_CHUNK_SIZE (8u << 20)
#define RING_CHUNK_SHIFT 26
#define RING_CHUNK_MAXIMUM 32
#define RING_ALIGNMENT 256
/* the guest's buffers: at most this many, each inside a granule */
#define GUEST_REGION_MAXIMUM 512
#define GUEST_GRANULE (4u << 20)
/* the visibility result buffers' entries a frame (each a test in one
pass), and their passes: each pass counts into a buffer of its own (Apple's
GPUs zero the first results of a buffer that an earlier pass counted into
when a later pass counts into it too), of VISIBILITY_PASS_ENTRIES entries */
#define VISIBILITY_ENTRIES 16384
#define VISIBILITY_PASSES 512
#define VISIBILITY_PASS_ENTRIES 512

static void metal_log(int priority, NSString *format, ...) NS_FORMAT_FUNCTION(2, 3);

static void metal_log(int priority, NSString *format, ...)
{
	va_list arguments;
	NSString *text;

	va_start(arguments, format);
	text = [[NSString alloc] initWithFormat:format arguments:arguments];
	va_end(arguments);
	host_logf(priority, "metal: %s", text.UTF8String);
}

/* ---------- formats and the interface's values (xgpu_metal_pipeline.m) */

static const struct xgpu_metal_format_information *format_get(uint32_t format)
{
	return xgpu_metal_format_information(format);
}

/* ---------- the backend's state */

struct texture_entry
{
	id<MTLTexture> texture;
	/* a multisampled target's attachment, resolved into texture */
	id<MTLTexture> multisampled;
	struct xgpu_metal_texture_description description;
	/* the last command buffer that read or wrote it */
	uint64_t used;
	/* a back buffer given to the presentation (xgpu_metal_frame_end): its
	pictures, which texture (another) has yet to be given, unless a clear of
	the whole of it comes first */
	id<MTLTexture> previous;
	uint8_t present;
};

/* a shader: its function, once there is one (made on the making queue:
nil until then); while it is being made, its group */
struct shader_entry
{
	id<MTLFunction> function;
	dispatch_group_t making;
};

struct depth_stencil_key
{
	uint8_t depth_test, depth_function, depth_write;
	uint8_t stencil_test, stencil_function, stencil_fail, stencil_depth_fail, stencil_pass;
	uint8_t stencil_read_mask, stencil_write_mask;
};

struct ring_chunk
{
	id<MTLBuffer> buffer;
	uint32_t size;
};

struct frame_slot
{
	struct ring_chunk chunks[RING_CHUNK_MAXIMUM];
	uint32_t chunk_count;
	uint32_t chunk, position;
	/* the passes' visibility result buffers (made as passes need them),
	and each one's entries */
	id<MTLBuffer> visibility_buffers[VISIBILITY_PASSES];
	uint16_t visibility_pass_entries[VISIBILITY_PASSES];
	uint32_t visibility_passes;
	/* each entry: the visibility test it counts for, its pass's buffer and
	its place in it */
	uint16_t visibility_slots[VISIBILITY_ENTRIES];
	uint16_t visibility_entry_passes[VISIBILITY_ENTRIES];
	uint16_t visibility_entry_offsets[VISIBILITY_ENTRIES];
	uint32_t visibility_entries;
	uint64_t fence;
	uint64_t number;
};

struct guest_region
{
	uint32_t start, end;
	id<MTLBuffer> buffer;
	uint64_t used;
};

/* what the next draw must set on the encoder */
enum
{
	_dirty_pipeline = 1 << 0,
	_dirty_depth_stencil = 1 << 1,
	_dirty_raster = 1 << 2,
	_dirty_viewport = 1 << 3,
	_dirty_scissor = 1 << 4,
	_dirty_stencil_reference = 1 << 5,
	_dirty_blend_color = 1 << 6,
	_dirty_vertex_constants = 1 << 7,
	_dirty_draw_uniforms = 1 << 8,
	_dirty_textures = 1 << 9,
	_dirty_streams = 1 << 10,
	_dirty_constant_attributes = 1 << 11,
	_dirty_all = (1 << 12) - 1,
};

static struct
{
	BOOL ready;
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
	struct xgpu_metal_options options;
	uint32_t page_size;

	/* the binary archive read at the start, which pipelines are made from;
	the descriptions of the pipelines made or found this run, which the
	archive is written of; whether any are new; the queue writing it */
	id<MTLBinaryArchive> archive;
	NSString *archive_path;
	/* the app's shader library (nil: none), and the one the clears' and the
	presentation's functions are in (it, or theirs compiled) */
	id<MTLLibrary> library, fixed_library;
	NSMutableArray<MTLRenderPipelineDescriptor *> *archived;
	BOOL archive_changed;
	dispatch_queue_t archive_queue;
	/* when a pipeline was last added to it */
	double archive_changed_at;

	struct texture_entry textures[TEXTURE_MAXIMUM];
	uint32_t texture_search;
	struct shader_entry shaders[SHADER_MAXIMUM];
	uint32_t shader_count;
	NSMutableDictionary<NSString *, NSNumber *> *shader_handles;
	/* the pipelines made, by their key; those being made (all under lock) */
	NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *pipelines;
	NSMutableSet<NSData *> *pipelines_making;
	os_unfair_lock lock;
	/* the queue shaders and pipelines are made on (user-initiated: a map's,
	as it loads, or a draw's); the work the loading screen waits for
	(xgpu_metal_prepared) */
	dispatch_queue_t making_queue;
	dispatch_group_t preparing;
	BOOL preparing_draws;
	/* the library the combiner's function is in (the app's, or its source
	compiled where the app has none: the tests) */
	id<MTLLibrary> combiner_library;
	NSMutableDictionary<NSData *, id<MTLDepthStencilState>> *depth_stencil_states;
	NSMutableDictionary<NSData *, id<MTLSamplerState>> *samplers;
	NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *clear_pipelines;
	id<MTLFunction> clear_vertex, clear_fragment;
	id<MTLDepthStencilState> clear_depth_stencil[4];

	/* command buffers: serials, the last completed */
	id<MTLCommandBuffer> command_buffer;
	uint64_t command_serial;
	uint64_t completed;
	pthread_mutex_t completed_lock;
	pthread_cond_t completed_condition;

	/* frames */
	struct frame_slot slots[FRAME_SLOTS];
	struct frame_slot *slot;
	uint64_t frame_number;
	BOOL in_frame;

	/* the pass */
	uint32_t color, color_level, color_layer, depth;
	/* into the targets' multisampled attachments */
	BOOL multisampled;
	id<MTLRenderCommandEncoder> encoder;
	uint32_t target_width, target_height;
	MTLLoadAction color_load, depth_load, stencil_load;
	MTLClearColor clear_color;
	float clear_depth;
	uint32_t clear_stencil;

	/* the draw state, as set, and what of it the encoder lacks */
	uint32_t dirty;
	struct xgpu_metal_pipeline pipeline;
	BOOL have_pipeline;
	id<MTLRenderPipelineState> pipeline_state;
	struct xgpu_metal_pipeline_state_key pipeline_state_key;
	struct xgpu_metal_dynamic_state dynamic;
	uint32_t stage_textures[XGPU_METAL_TEXTURE_STAGE_COUNT];
	struct xgpu_metal_sampler stage_samplers[XGPU_METAL_TEXTURE_STAGE_COUNT];
	id<MTLSamplerState> stage_sampler_states[XGPU_METAL_TEXTURE_STAGE_COUNT];
	float vertex_constants[XGPU_METAL_VERTEX_CONSTANT_COUNT][4];
	unsigned char draw_uniforms[4096];
	uint32_t draw_uniforms_size;
	struct xgpu_metal_source streams[XGPU_METAL_STREAM_COUNT];
	float constant_attributes[16][4];

	/* an image for the presentation that a picture is copied into (a
	target of levels or layers) */
	id<MTLTexture> present_copy;

	/* the command stream's ring, wrapped */
	id<MTLBuffer> stream_ring;
	/* the backend's buffers (handle - 1) */
	NSMutableArray<id<MTLBuffer>> *buffers;
	NSMutableArray<NSNumber *> *buffers_used;

	/* the visibility test going on: its entry in this pass, and its first
	(its entries get the slot at its end) */
	BOOL visibility_active;
	int32_t visibility_entry;
	/* this pass's visibility result buffer (-1: none) */
	int32_t visibility_pass;
	/* the pass has no entry for it yet: the next draw begins one */
	BOOL visibility_pending;
	uint32_t visibility_first;
	volatile uint32_t *visibility_output;

	/* the guest's buffers */
	struct guest_region regions[GUEST_REGION_MAXIMUM];
	uint32_t region_count;
	pthread_mutex_t region_lock;
	uint64_t region_clock;
	/* counts the guest's remappings: a buffer made while one happened may
	be over the old pages */
	uint64_t remap_generation;

	struct xgpu_metal_statistics statistics;
	struct xgpu_metal_compile_totals compile_totals;
	pthread_mutex_t statistics_lock;
} metal = {
	.completed_lock = PTHREAD_MUTEX_INITIALIZER,
	.completed_condition = PTHREAD_COND_INITIALIZER,
	.region_lock = PTHREAD_MUTEX_INITIALIZER,
	.statistics_lock = PTHREAD_MUTEX_INITIALIZER,
};

/* the visibility tests' latest results */
static struct
{
	pthread_mutex_t lock;
	uint32_t samples[XGPU_METAL_VISIBILITY_SLOTS];
	uint64_t frames[XGPU_METAL_VISIBILITY_SLOTS];
	/* the samples a pixel of the game's is, as the last test of the slot
	said */
	float areas[XGPU_METAL_VISIBILITY_SLOTS];
} visibility = { .lock = PTHREAD_MUTEX_INITIALIZER };

static double seconds_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec * 1e-9;
}

id<MTLDevice> xgpu_metal_device(void)
{
	return metal.device;
}

id<MTLLibrary> xgpu_metal_fixed_library(void)
{
	return metal.fixed_library;
}

id<MTLCommandQueue> xgpu_metal_queue(void)
{
	return metal.queue;
}

/* ---------- command buffers and fences */

static void completed_update(uint64_t serial)
{
	pthread_mutex_lock(&metal.completed_lock);
	if (serial > metal.completed)
		metal.completed = serial;
	pthread_cond_broadcast(&metal.completed_condition);
	pthread_mutex_unlock(&metal.completed_lock);
}

uint64_t xgpu_metal_frame_completed(void)
{
	uint64_t completed;

	pthread_mutex_lock(&metal.completed_lock);
	completed = metal.completed;
	pthread_mutex_unlock(&metal.completed_lock);
	return completed;
}

static _Atomic int frame_waiting;

void xgpu_metal_frame_wait(uint64_t fence)
{
	pthread_mutex_lock(&metal.completed_lock);
	atomic_store(&frame_waiting, 1);
	while (metal.completed < fence)
		pthread_cond_wait(&metal.completed_condition, &metal.completed_lock);
	atomic_store(&frame_waiting, 0);
	pthread_mutex_unlock(&metal.completed_lock);
}

int xgpu_metal_frame_waiting(void)
{
	return atomic_load(&frame_waiting);
}

/* the command buffer being recorded, begun if there is none */
static id<MTLCommandBuffer> command_buffer(void)
{
	if (!metal.command_buffer)
	{
		metal.command_buffer = [metal.queue commandBuffer];
		metal.command_serial++;
	}
	return metal.command_buffer;
}

/* the serial the command buffer being recorded has or will have */
static uint64_t recording_serial(void)
{
	command_buffer();
	return metal.command_serial;
}

/* ---------- the passes' GPU times and frame captures

HALO_METAL_PASSES=<seconds>: a frame that often is timed pass by pass, with
the GPU's timestamps at each encoder's stages, and logged with each pass's
targets, load and store actions and draws. HALO_METAL_CAPTURE=<frame>:<path>
(with MTL_CAPTURE_ENABLED=1) writes that frame and the next, the
presentation of the first included, as a GPU trace document. */

#define PROFILE_PASSES 256

struct profile_pass
{
	const char *what;
	uint32_t color, depth;
	uint32_t color_format, depth_format, width, height, samples;
	/* colour, depth, stencil (Metal's actions; -1: none) */
	int8_t loads[3], stores[3];
	uint32_t draws, clears;
	/* (HALO_METAL_PASSES_SPLIT: the draw's pipeline and textures) */
	char detail[256];
};

static struct
{
	double interval, last;
	/* 0: none; 1: this frame is timed; 2: its command buffers are being
	done */
	_Atomic int state;
	id<MTLCounterSampleBuffer> samples;
	uint32_t count;
	int32_t current;
	struct profile_pass passes[PROFILE_PASSES];
	MTLTimestamp cpu_start, gpu_start;
	uint64_t first_serial;
	double buffer_seconds;
	uint32_t buffers;
	uint64_t frame;

	/* each draw of a timed frame a pass of its own */
	BOOL split;

	uint64_t capture_frame;
	NSString *capture_path;
	BOOL capturing;
} profile = { .current = -1 };

static void profile_initialize(void)
{
	const char *passes = getenv("HALO_METAL_PASSES"), *capture = getenv("HALO_METAL_CAPTURE");

	profile.interval = passes ? atof(passes) : 0.0;
	profile.split = getenv("HALO_METAL_PASSES_SPLIT") && atoi(getenv("HALO_METAL_PASSES_SPLIT"));
	if (profile.interval > 0.0)
	{
		MTLCounterSampleBufferDescriptor *descriptor = [MTLCounterSampleBufferDescriptor new];
		NSError *error = nil;

		for (id<MTLCounterSet> set in metal.device.counterSets)
		{
			if ([set.name isEqualToString:MTLCommonCounterSetTimestamp])
				descriptor.counterSet = set;
		}
		descriptor.sampleCount = PROFILE_PASSES * 4;
		descriptor.storageMode = MTLStorageModeShared;
		profile.samples = descriptor.counterSet && [metal.device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary] ?
			[metal.device newCounterSampleBufferWithDescriptor:descriptor error:&error] : nil;
		if (!profile.samples)
		{
			host_logf(HOST_LOG_ERROR, "metal: no timestamps at the GPU's stages: %s",
				error ? error.localizedDescription.UTF8String : "not supported");
			profile.interval = 0.0;
		}
	}
	if (capture && strchr(capture, ':'))
	{
		profile.capture_frame = strtoull(capture, NULL, 10);
		profile.capture_path = @(strchr(capture, ':') + 1);
	}
}

static void profile_frame_begin(uint64_t frame)
{
	double now = seconds_now();

	if (profile.capture_path && frame == profile.capture_frame)
	{
		MTLCaptureDescriptor *descriptor = [MTLCaptureDescriptor new];
		NSError *error = nil;

		descriptor.captureObject = metal.device;
		descriptor.destination = MTLCaptureDestinationGPUTraceDocument;
		descriptor.outputURL = [NSURL fileURLWithPath:profile.capture_path];
		profile.capturing = [[MTLCaptureManager sharedCaptureManager] startCaptureWithDescriptor:descriptor error:&error];
		host_logf(HOST_LOG_INFO, "metal: capturing frames %llu and %llu into %s%s%s", (unsigned long long)frame,
			(unsigned long long)frame + 1, profile.capture_path.UTF8String, profile.capturing ? "" : ": ",
			profile.capturing ? "" : error.localizedDescription.UTF8String);
	}
	if (profile.interval <= 0.0 || atomic_load(&profile.state) != 0 || now - profile.last < profile.interval)
		return;
	profile.count = 0;
	profile.current = -1;
	profile.frame = frame;
	profile.buffer_seconds = 0.0;
	profile.buffers = 0;
	/* (the command buffer being recorded, if any, is this frame's) */
	profile.first_serial = metal.command_buffer ? metal.command_serial : metal.command_serial + 1;
	[metal.device sampleTimestamps:&profile.cpu_start gpuTimestamp:&profile.gpu_start];
	atomic_store(&profile.state, 1);
}

/* a pass of the frame timed: its index (-1: none) */
static int32_t profile_pass_add(const char *what)
{
	struct profile_pass *pass;

	if (atomic_load(&profile.state) != 1 || profile.count >= PROFILE_PASSES)
		return -1;
	pass = &profile.passes[profile.count];
	memset(pass, 0, sizeof(*pass));
	memset(pass->loads, -1, sizeof(pass->loads));
	memset(pass->stores, -1, sizeof(pass->stores));
	pass->what = what;
	return (int32_t)profile.count++;
}

/* a blit encoder, timed when the frame is */
static id<MTLBlitCommandEncoder> blit_encoder(const char *what)
{
	int32_t index = profile_pass_add(what);
	MTLBlitPassDescriptor *descriptor;

	if (index < 0)
		return [command_buffer() blitCommandEncoder];
	descriptor = [MTLBlitPassDescriptor blitPassDescriptor];
	descriptor.sampleBufferAttachments[0].sampleBuffer = profile.samples;
	descriptor.sampleBufferAttachments[0].startOfEncoderSampleIndex = (NSUInteger)index * 4;
	descriptor.sampleBufferAttachments[0].endOfEncoderSampleIndex = (NSUInteger)index * 4 + 1;
	return [command_buffer() blitCommandEncoderWithDescriptor:descriptor];
}

static char action_letter(int action, BOOL store)
{
	if (action < 0)
		return '-';
	if (store)
		return action == MTLStoreActionStore ? 'S' : action == MTLStoreActionDontCare ? 'D' :
			action == MTLStoreActionMultisampleResolve ? 'R' : action == MTLStoreActionStoreAndMultisampleResolve ? 'B' : '?';
	return action == MTLLoadActionLoad ? 'L' : action == MTLLoadActionClear ? 'C' : action == MTLLoadActionDontCare ? 'D' : '?';
}

/* the timed frame's passes, logged (its last command buffer is done) */
static void profile_log(void)
{
	NSData *data = [profile.samples resolveCounterRange:NSMakeRange(0, profile.count * 4)];
	const MTLCounterResultTimestamp *samples = data.bytes;
	MTLTimestamp cpu, gpu;
	double scale, first = 0.0, last = 0.0, previous_end = 0.0;
	double vertex_sum = 0.0, fragment_sum = 0.0;
	uint32_t index;

	[metal.device sampleTimestamps:&cpu gpuTimestamp:&gpu];
	/* (nanoseconds a GPU tick) */
	scale = gpu > profile.gpu_start ? (double)(cpu - profile.cpu_start) / (double)(gpu - profile.gpu_start) : 1.0;
	for (index = 0; data && index < profile.count; index++)
	{
		const struct profile_pass *pass = &profile.passes[index];
		uint64_t values[4];
		double start = 0.0, end = 0.0, vertex = 0.0, fragment = 0.0;
		int part;

		for (part = 0; part < 4; part++)
			values[part] = samples[index * 4 + part].timestamp;
		for (part = 0; part < 4; part++)
		{
			double value;

			if (values[part] == MTLCounterErrorValue || !values[part] || (!pass->width && part >= 2))
				continue;
			value = (double)(values[part] - profile.gpu_start) * scale / 1e6;
			if (part % 2 == 0 && (start == 0.0 || value < start))
				start = value;
			if (part % 2 == 1 && value > end)
				end = value;
		}
		if (pass->width)
		{
			if (values[1] > values[0] && values[0] != MTLCounterErrorValue && values[1] != MTLCounterErrorValue)
				vertex = (double)(values[1] - values[0]) * scale / 1e6;
			if (values[3] > values[2] && values[2] != MTLCounterErrorValue && values[3] != MTLCounterErrorValue)
				fragment = (double)(values[3] - values[2]) * scale / 1e6;
		}
		else
		{
			fragment = end > start ? end - start : 0.0;
		}
		vertex_sum += vertex;
		fragment_sum += fragment;
		if (first == 0.0 || (start > 0.0 && start < first))
			first = start;
		if (end > last)
			last = end;
		host_logf(HOST_LOG_INFO,
			"metal: pass %2u %-7s %4ux%-4u t%u/%u fmt %2u/%2u x%u load %c%c%c store %c%c%c draws %3u clears %2u: "
			"at %7.3f ms, %6.3f long (vertex %6.3f, fragment %6.3f), %6.3f after the last",
			index, pass->what, pass->width, pass->height, pass->color, pass->depth, pass->color_format, pass->depth_format, pass->samples,
			action_letter(pass->loads[0], NO), action_letter(pass->loads[1], NO), action_letter(pass->loads[2], NO),
			action_letter(pass->stores[0], YES), action_letter(pass->stores[1], YES), action_letter(pass->stores[2], YES),
			pass->draws, pass->clears, start, end - start, vertex, fragment, previous_end > 0.0 ? start - previous_end : 0.0);
		if (pass->detail[0])
			host_logf(HOST_LOG_INFO, "metal:    %s", pass->detail);
		if (end > 0.0)
			previous_end = end;
	}
	host_logf(HOST_LOG_INFO,
		"metal: frame %llu timed: %u passes from %.3f to %.3f ms (%.3f ms); stages' sums: vertex %.3f, fragment %.3f; "
		"its %u command buffers %.3f ms",
		(unsigned long long)profile.frame, profile.count, first, last, last - first, vertex_sum, fragment_sum,
		profile.buffers, profile.buffer_seconds * 1000.0);
	profile.last = seconds_now();
	atomic_store(&profile.state, 0);
}

static void encoder_end(void)
{
	if (metal.encoder)
	{
		if (metal.visibility_active && metal.visibility_entry >= 0)
			[metal.encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
		[metal.encoder endEncoding];
		metal.encoder = nil;
		metal.visibility_entry = -1;
		profile.current = -1;
	}
}

static void pass_end(void);
static MTLCompileOptions *compile_options(void);

/* commits the command buffer being recorded; returns its serial */
static uint64_t command_buffer_commit(void (^completed)(void))
{
	id<MTLCommandBuffer> buffer = command_buffer();
	uint64_t serial = metal.command_serial;

	pass_end();
	[buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
		if (done.status == MTLCommandBufferStatusError)
			host_logf(HOST_LOG_ERROR, "metal: a command buffer failed: %s",
				done.error.localizedDescription.UTF8String);
		if (done.GPUEndTime > done.GPUStartTime)
		{
			xgpu_metal_gpu_interval(done.GPUStartTime, done.GPUEndTime);
			pthread_mutex_lock(&metal.statistics_lock);
			metal.statistics.gpu_seconds += done.GPUEndTime - done.GPUStartTime;
			metal.statistics.command_buffers++;
			pthread_mutex_unlock(&metal.statistics_lock);
			if (atomic_load(&profile.state) && serial >= profile.first_serial)
			{
				profile.buffer_seconds += done.GPUEndTime - done.GPUStartTime;
				profile.buffers++;
			}
		}
		if (completed)
			completed();
		completed_update(serial);
	}];
	[buffer commit];
	metal.command_buffer = nil;
	return serial;
}

/* ---------- start and end */

/* the app's library of the shaders built ahead of time (the renderer's
manifest's: tools/macos_build.py), which a shader is looked up in by its
name before it is compiled */
static void library_open(const char *path)
{
	NSError *error = nil;

	if (!path || !*path)
		return;
	if (![[NSFileManager defaultManager] fileExistsAtPath:@(path)])
	{
		host_logf(HOST_LOG_WARN, "metal: no shader library at %s: each shader is compiled as it is first drawn", path);
		return;
	}
	metal.library = [metal.device newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&error];
	if (!metal.library)
		metal_log(HOST_LOG_WARN, @"cannot read the shader library %s (%@): each shader is compiled as it is first drawn",
			path, error.localizedDescription);
}

static void archive_open(void)
{
	MTLBinaryArchiveDescriptor *descriptor = [MTLBinaryArchiveDescriptor new];
	NSError *error = nil;

	if (metal.archive_path && [[NSFileManager defaultManager] fileExistsAtPath:metal.archive_path])
	{
		descriptor.url = [NSURL fileURLWithPath:metal.archive_path];
		metal.archive = [metal.device newBinaryArchiveWithDescriptor:descriptor error:&error];
		if (!metal.archive)
			metal_log(HOST_LOG_WARN, @"cannot read the pipeline archive %@ (%@); starting a new one", metal.archive_path,
				error.localizedDescription);
	}
	if (!metal.archive)
	{
		descriptor.url = nil;
		metal.archive = [metal.device newBinaryArchiveWithDescriptor:descriptor error:&error];
		if (!metal.archive)
			metal_log(HOST_LOG_WARN, @"no pipeline archive: %@", error.localizedDescription);
	}
}

/* writes the archive of the pipelines made or found this run, on the
archive's queue (wait: and waits for it). It is a new archive each time:
one that has been written, or read from a file, and has had pipelines
added since is written without some of their stages (Metal's archiver
says so, and the archive is not written). The pipelines of the archive
read at the start that this run has not made are not kept. */
static void archive_save(BOOL wait)
{
	NSArray<MTLRenderPipelineDescriptor *> *pipelines;
	NSString *path = metal.archive_path;
	id<MTLDevice> device = metal.device;
	void (^save)(void);

	if (!metal.archive_queue || !path || !metal.archive_changed)
		return;
	os_unfair_lock_lock(&metal.lock);
	metal.archive_changed = NO;
	pipelines = [metal.archived copy];
	os_unfair_lock_unlock(&metal.lock);
	save = ^{
		MTLBinaryArchiveDescriptor *descriptor = [MTLBinaryArchiveDescriptor new];
		NSString *temporary = [path stringByAppendingString:@".new"];
		id<MTLBinaryArchive> archive;
		NSError *error = nil;

		archive = [device newBinaryArchiveWithDescriptor:descriptor error:&error];
		for (MTLRenderPipelineDescriptor *pipeline in pipelines)
		{
			if (archive && ![archive addRenderPipelineFunctionsWithDescriptor:pipeline error:&error])
				break;
		}
		[[NSFileManager defaultManager] createDirectoryAtPath:[path stringByDeletingLastPathComponent]
			withIntermediateDirectories:YES attributes:nil error:nil];
		/* (written beside it, then put in its place: the archive read at the
		start may be mapped from the file) */
		if (!archive || error || ![archive serializeToURL:[NSURL fileURLWithPath:temporary] error:&error] ||
			rename(temporary.fileSystemRepresentation, path.fileSystemRepresentation) != 0)
			metal_log(HOST_LOG_WARN, @"cannot write the pipeline archive %@: %@", path,
				error ? error.localizedDescription : @(strerror(errno)));
	};
	if (wait)
		dispatch_sync(metal.archive_queue, save);
	else
		dispatch_async(metal.archive_queue, save);
}

int xgpu_metal_initialize(const struct xgpu_metal_options *options)
{
	NSError *error = nil;
	id<MTLLibrary> library;
	int index;

	if (metal.ready)
		return 1;
	metal.device = MTLCreateSystemDefaultDevice();
	if (!metal.device)
	{
		host_logf(HOST_LOG_ERROR, "metal: no Metal device");
		return 0;
	}
	metal.queue = [metal.device newCommandQueueWithMaxCommandBufferCount:16];
	metal.options = *options;
	metal.page_size = options->page_size ? options->page_size : (uint32_t)getpagesize();
	metal.archive_path = options->archive_path ? @(options->archive_path) : nil;
	metal.options.archive_path = NULL;
	metal.options.library_path = NULL;
	metal.shader_handles = [NSMutableDictionary new];
	metal.pipelines_making = [NSMutableSet new];
	metal.lock = OS_UNFAIR_LOCK_INIT;
	metal.making_queue = dispatch_queue_create("OpenCE shaders and pipelines", dispatch_queue_attr_make_with_qos_class(
		DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_USER_INITIATED, 0));
	metal.preparing = dispatch_group_create();
	metal.pipelines = [NSMutableDictionary new];
	metal.depth_stencil_states = [NSMutableDictionary new];
	metal.samplers = [NSMutableDictionary new];
	metal.clear_pipelines = [NSMutableDictionary new];
	metal.texture_search = 1;
	metal.buffers = [NSMutableArray new];
	metal.buffers_used = [NSMutableArray new];
	metal.visibility_entry = -1;
	archive_open();
	profile_initialize();
	metal.archived = [NSMutableArray new];
	metal.archive_queue = dispatch_queue_create("OpenCE pipeline archive", dispatch_queue_attr_make_with_qos_class(
		DISPATCH_QUEUE_SERIAL, QOS_CLASS_BACKGROUND, 0));

	library_open(options->library_path);
	library = metal.library;
	if (!library || ![library newFunctionWithName:@"xgpu_clear_vertex"])
		library = [metal.device newLibraryWithSource:xgpu_metal_fixed_source options:nil error:&error];
	if (!library)
	{
		metal_log(HOST_LOG_ERROR, @"the clear shaders: %@", error.localizedDescription);
		return 0;
	}
	metal.fixed_library = library;
	metal.combiner_library = metal.library && [metal.library newFunctionWithName:@"xgpu_combiner_fragment"] ?
		metal.library : [metal.device newLibraryWithSource:@(xgpu_metal_combiner_source) options:compile_options()
		error:&error];
	if (!metal.combiner_library)
		metal_log(HOST_LOG_WARN, @"no combiner's function (%@): a program the library lacks is compiled before it is drawn",
			error.localizedDescription);
	metal.clear_vertex = [library newFunctionWithName:@"xgpu_clear_vertex"];
	metal.clear_fragment = [library newFunctionWithName:@"xgpu_clear_fragment"];
	/* depth written or not, stencil replaced or not */
	for (index = 0; index < 4; index++)
	{
		MTLDepthStencilDescriptor *descriptor = [MTLDepthStencilDescriptor new];

		descriptor.depthCompareFunction = MTLCompareFunctionAlways;
		descriptor.depthWriteEnabled = (index & 1) != 0;
		if (index & 2)
		{
			MTLStencilDescriptor *stencil = [MTLStencilDescriptor new];

			stencil.stencilCompareFunction = MTLCompareFunctionAlways;
			stencil.depthStencilPassOperation = MTLStencilOperationReplace;
			stencil.readMask = 0xff;
			stencil.writeMask = 0xff;
			descriptor.frontFaceStencil = stencil;
			descriptor.backFaceStencil = stencil;
		}
		metal.clear_depth_stencil[index] = [metal.device newDepthStencilStateWithDescriptor:descriptor];
	}
	for (index = 0; index < FRAME_SLOTS; index++)
	{
		metal.slots[index].chunks[0].buffer = [metal.device newBufferWithLength:RING_CHUNK_SIZE
			options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
		metal.slots[index].chunks[0].size = RING_CHUNK_SIZE;
		metal.slots[index].chunk_count = 1;
	}
	metal.slot = &metal.slots[0];
	metal.ready = YES;
	host_logf(HOST_LOG_INFO, "metal: %s, pipeline archive %s", metal.device.name.UTF8String,
		metal.archive_path ? metal.archive_path.UTF8String : "none");
	return 1;
}

void xgpu_metal_shutdown(void)
{
	if (!metal.ready)
		return;
	if (metal.command_buffer)
		xgpu_metal_frame_wait(command_buffer_commit(nil));
	xgpu_metal_frame_wait(metal.command_serial);
	archive_save(YES);
}

void xgpu_metal_exiting(void)
{
	if (metal.ready)
		archive_save(YES);
}

void xgpu_metal_capabilities(struct xgpu_metal_capabilities *capabilities)
{
	uint32_t samples = 1;

	memset(capabilities, 0, sizeof(*capabilities));
	capabilities->largest_target = 16384;
	while (samples < 8 && [metal.device supportsTextureSampleCount:samples * 2])
		samples *= 2;
	capabilities->samples = samples;
	capabilities->anisotropy = 16;
	capabilities->border_colors = 1;
	capabilities->base_vertex = 1;
	capabilities->fill_modes = 1;
}

/* the GPU's time on a command buffer, of the game's or the presentation's:
the time the GPU was busy with any is their union (they overlap: the GPU
begins one before the last is done) */
void xgpu_metal_gpu_interval(double start, double end)
{
	static double busy_until;

	pthread_mutex_lock(&metal.statistics_lock);
	if (start < busy_until)
		start = busy_until;
	if (end > start)
		metal.statistics.gpu_busy_seconds += end - start;
	if (end > busy_until)
		busy_until = end;
	pthread_mutex_unlock(&metal.statistics_lock);
}

void xgpu_metal_statistics(struct xgpu_metal_statistics *statistics)
{
	pthread_mutex_lock(&metal.statistics_lock);
	*statistics = metal.statistics;
	memset(&metal.statistics, 0, sizeof(metal.statistics));
	pthread_mutex_unlock(&metal.statistics_lock);
}

void xgpu_metal_compile_totals(struct xgpu_metal_compile_totals *totals)
{
	pthread_mutex_lock(&metal.statistics_lock);
	*totals = metal.compile_totals;
	pthread_mutex_unlock(&metal.statistics_lock);
}

static void statistics_add(uint32_t *counter, uint32_t amount);

/* ---------- shaders */

/* the options a shader is compiled with here, as the app's library's are
(tools/macos_shaders.py, METAL_FLAGS) */
static MTLCompileOptions *compile_options(void)
{
	MTLCompileOptions *options = [MTLCompileOptions new];

	/* (safe maths, as the GLSL emitted for the other backends: but the
	shaders' divides whose last bit does not matter are fast::divide, or a
	multiply by a reciprocal, which safe maths would make correctly rounded
	IEEE divides of about 25 instructions each: xgpu_msl.c,
	xgpu_metal_combiner.c) */
	if (@available(macOS 15.0, *))
		options.mathMode = MTLMathModeSafe;
	else
		options.fastMathEnabled = NO;
	return options;
}

/* compiles a source's function (named entry); nil (logged) if it does not
compile. Any thread. */
static id<MTLFunction> function_compile(NSString *text, NSString *name)
{
	id<MTLLibrary> library;
	id<MTLFunction> function;
	NSError *error = nil;
	double start = seconds_now(), seconds;

	library = [metal.device newLibraryWithSource:text options:compile_options() error:&error];
	function = library ? [library newFunctionWithName:name] : nil;
	seconds = seconds_now() - start;
	pthread_mutex_lock(&metal.statistics_lock);
	metal.statistics.shaders_made++;
	metal.statistics.shader_seconds += seconds;
	metal.compile_totals.shaders++;
	metal.compile_totals.shader_seconds += seconds;
	pthread_mutex_unlock(&metal.statistics_lock);
	if (!function)
		metal_log(HOST_LOG_ERROR, @"cannot compile %@: %@", name, error ? error.localizedDescription : @"no such function");
	return function;
}

/* the combiner's function specialised for a program's configuration (any
thread) */
static id<MTLFunction> combiner_specialise(const uint32_t constants[XGPU_METAL_COMBINER_CONSTANTS])
{
	MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
	id<MTLFunction> function;
	NSError *error = nil;
	double start = seconds_now();
	int index;

	for (index = 0; index < XGPU_METAL_COMBINER_CONSTANTS; index++)
		[values setConstantValue:&constants[index] type:MTLDataTypeUInt atIndex:(NSUInteger)index];
	function = [metal.combiner_library newFunctionWithName:@"xgpu_combiner_fragment" constantValues:values error:&error];
	pthread_mutex_lock(&metal.statistics_lock);
	metal.statistics.shaders_specialised++;
	metal.statistics.shader_seconds += seconds_now() - start;
	pthread_mutex_unlock(&metal.statistics_lock);
	if (!function)
		metal_log(HOST_LOG_ERROR, @"cannot specialise the combiner's function: %@", error.localizedDescription);
	return function;
}

uint32_t xgpu_metal_shader_create(const char *source, size_t size, const char *entry)
{
	return xgpu_metal_pixel_shader_create(source, size, entry, NULL);
}

uint32_t xgpu_metal_pixel_shader_create(const char *source, size_t size, const char *entry,
	const uint32_t *constants)
{
	NSString *name = @(entry), *text;
	NSNumber *existing;
	struct shader_entry *shader;
	id<MTLFunction> function;
	uint32_t handle;
	/* (a block takes a copy of a structure, not of an array) */
	struct { uint32_t values[XGPU_METAL_COMBINER_CONSTANTS]; } copied;

	if (metal.shader_count + 1 >= SHADER_MAXIMUM)
		return 0;
	/* (an entry's name is its source's: xgpu_metal_shaders.c) */
	existing = metal.shader_handles[name];
	if (existing)
		return existing.unsignedIntValue;
	handle = ++metal.shader_count;
	shader = &metal.shaders[handle];
	metal.shader_handles[name] = @(handle);
	function = metal.library ? [metal.library newFunctionWithName:name] : nil;
	if (function)
	{
		shader->function = function;
		statistics_add(&metal.statistics.shaders_from_library, 1);
		return handle;
	}
	/* not in the app's library: made on the making queue, never here. A
	pixel program is the combiner's function specialised for it (the same
	machine code as its own source compiled: port/macos/README.md); one it
	cannot draw (each pixel's lighting, a debugging setting's) is compiled */
	if (constants && metal.combiner_library)
	{
		memcpy(copied.values, constants, sizeof(copied.values));
		if (metal.options.synchronous)
		{
			shader->function = combiner_specialise(copied.values);
			return handle;
		}
		shader->making = dispatch_group_create();
		dispatch_group_async(shader->making, metal.making_queue, ^{
			id<MTLFunction> specialised = combiner_specialise(copied.values);

			os_unfair_lock_lock(&metal.lock);
			metal.shaders[handle].function = specialised;
			os_unfair_lock_unlock(&metal.lock);
		});
	}
	else
	{
		text = [[NSString alloc] initWithBytes:source length:size encoding:NSUTF8StringEncoding];
		if (!text)
			return 0;
		host_logf(HOST_LOG_INFO, "metal: %s is not in the app's library: compiled", entry);
		if (metal.options.synchronous)
		{
			shader->function = function_compile(text, name);
			return handle;
		}
		shader->making = dispatch_group_create();
		dispatch_group_async(shader->making, metal.making_queue, ^{
			id<MTLFunction> compiled = function_compile(text, name);

			os_unfair_lock_lock(&metal.lock);
			metal.shaders[handle].function = compiled;
			os_unfair_lock_unlock(&metal.lock);
		});
	}
	return handle;
}

/* a shader's function (nil while it is made; any thread) */
static id<MTLFunction> shader_function(uint32_t shader)
{
	id<MTLFunction> function = nil;

	if (!shader || shader > metal.shader_count)
		return nil;
	os_unfair_lock_lock(&metal.lock);
	function = metal.shaders[shader].function;
	os_unfair_lock_unlock(&metal.lock);
	return function;
}

/* ---------- textures */

static struct texture_entry *texture_get(uint32_t texture)
{
	return texture && texture < TEXTURE_MAXIMUM && metal.textures[texture].present ? &metal.textures[texture] : NULL;
}

static void texture_contents(struct texture_entry *entry);

id<MTLTexture> xgpu_metal_texture(uint32_t texture)
{
	struct texture_entry *entry = texture_get(texture);

	texture_contents(entry);
	return entry ? entry->texture : nil;
}

/* a new target's first contents, black (depth 1, stencil 0): Apple's GPUs
give a new private texture none of its own (an M1 Max's read as magenta),
and the game may present or sample a target before it draws into it (its
first frame presents the back buffer as the device made it) */
static void texture_clear_new(id<MTLTexture> texture)
{
	MTLPixelFormat format = texture.pixelFormat;
	BOOL depth = format == MTLPixelFormatDepth32Float_Stencil8 || format == MTLPixelFormatDepth32Float ||
		format == MTLPixelFormatDepth16Unorm || format == MTLPixelFormatX32_Stencil8;
	BOOL stencil = format == MTLPixelFormatDepth32Float_Stencil8;
	NSUInteger slices = texture.textureType == MTLTextureTypeCube ? 6 :
		texture.textureType == MTLTextureType2DArray ? texture.arrayLength : 1, slice;

	if (texture.textureType == MTLTextureType3D)
		return;
	if (!metal.in_frame)
		xgpu_metal_frame_begin();
	/* (not pass_end: the clears waiting for the pass's start still wait,
	for the targets they are of) */
	encoder_end();
	for (slice = 0; slice < slices; slice++)
	{
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];

		if (depth)
		{
			pass.depthAttachment.texture = texture;
			pass.depthAttachment.slice = slice;
			pass.depthAttachment.loadAction = MTLLoadActionClear;
			pass.depthAttachment.storeAction = MTLStoreActionStore;
			pass.depthAttachment.clearDepth = 1.0;
			if (stencil)
			{
				pass.stencilAttachment.texture = texture;
				pass.stencilAttachment.slice = slice;
				pass.stencilAttachment.loadAction = MTLLoadActionClear;
				pass.stencilAttachment.storeAction = MTLStoreActionStore;
				pass.stencilAttachment.clearStencil = 0;
			}
		}
		else
		{
			pass.colorAttachments[0].texture = texture;
			pass.colorAttachments[0].slice = slice;
			pass.colorAttachments[0].loadAction = MTLLoadActionClear;
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
		}
		[[command_buffer() renderCommandEncoderWithDescriptor:pass] endEncoding];
	}
}

uint32_t xgpu_metal_texture_create(const struct xgpu_metal_texture_description *description)
{
	const struct xgpu_metal_format_information *format = format_get(description->format);
	MTLTextureDescriptor *descriptor;
	struct texture_entry *entry;
	uint32_t handle, tries;

	if (format->pixel_format == MTLPixelFormatInvalid || !description->width || !description->height)
	{
		host_logf(HOST_LOG_ERROR, "metal: cannot make a texture of format %u, %ux%u", description->format,
			description->width, description->height);
		return 0;
	}
	for (handle = 0, tries = 1; tries < TEXTURE_MAXIMUM; tries++)
	{
		uint32_t candidate = metal.texture_search;

		metal.texture_search = metal.texture_search + 1 < TEXTURE_MAXIMUM ? metal.texture_search + 1 : 1;
		if (!metal.textures[candidate].present)
		{
			handle = candidate;
			break;
		}
	}
	if (!handle)
	{
		host_logf(HOST_LOG_ERROR, "metal: out of texture handles");
		return 0;
	}
	descriptor = [MTLTextureDescriptor new];
	descriptor.pixelFormat = format->pixel_format;
	descriptor.width = description->width;
	descriptor.height = description->height;
	descriptor.mipmapLevelCount = description->levels ? description->levels : 1;
	switch (description->type)
	{
	case XGPU_METAL_TEXTURE_3D:
		descriptor.textureType = MTLTextureType3D;
		descriptor.depth = description->depth_or_layers ? description->depth_or_layers : 1;
		break;
	case XGPU_METAL_TEXTURE_CUBE:
		descriptor.textureType = MTLTextureTypeCube;
		break;
	case XGPU_METAL_TEXTURE_2D_ARRAY:
		descriptor.textureType = MTLTextureType2DArray;
		descriptor.arrayLength = description->depth_or_layers ? description->depth_or_layers : 1;
		break;
	default:
		descriptor.textureType = MTLTextureType2D;
		break;
	}
	descriptor.usage = MTLTextureUsageShaderRead;
	if (description->usage & XGPU_METAL_USAGE_TARGET)
	{
		/* (drawn into, and copied out of by blits: private) */
		descriptor.usage |= MTLTextureUsageRenderTarget;
		descriptor.storageMode = MTLStorageModePrivate;
	}
	else
	{
		/* (written by the CPU where the GPU is not reading it) */
		descriptor.storageMode = MTLStorageModeShared;
	}
	if (description->channels & 0x100)
	{
		/* (each channel taken from another of the format's) */
		const MTLTextureSwizzle from[4] = { format->swizzle.red, format->swizzle.green, format->swizzle.blue,
			format->swizzle.alpha };

		descriptor.swizzle = (MTLTextureSwizzleChannels){ from[description->channels & 3],
			from[(description->channels >> 2) & 3], from[(description->channels >> 4) & 3],
			from[(description->channels >> 6) & 3] };
	}
	else if (format->swizzled)
		descriptor.swizzle = format->swizzle;
	entry = &metal.textures[handle];
	entry->texture = [metal.device newTextureWithDescriptor:descriptor];
	entry->multisampled = nil;
	if (entry->texture && description->samples > 1 && (description->usage & XGPU_METAL_USAGE_TARGET))
	{
		MTLTextureDescriptor *multisampled = [MTLTextureDescriptor new];

		multisampled.textureType = MTLTextureType2DMultisample;
		multisampled.pixelFormat = format->pixel_format;
		multisampled.width = description->width;
		multisampled.height = description->height;
		multisampled.sampleCount = description->samples;
		multisampled.usage = MTLTextureUsageRenderTarget;
		multisampled.storageMode = MTLStorageModePrivate;
		entry->multisampled = [metal.device newTextureWithDescriptor:multisampled];
		if (!entry->multisampled)
			entry->texture = nil;
	}
	if (!entry->texture)
	{
		host_logf(HOST_LOG_ERROR, "metal: cannot make a %ux%u texture of format %u", description->width,
			description->height, description->format);
		return 0;
	}
	entry->description = *description;
	entry->used = 0;
	entry->previous = nil;
	entry->present = 1;
	if (description->usage & XGPU_METAL_USAGE_TARGET)
	{
		texture_clear_new(entry->texture);
		if (entry->multisampled)
			texture_clear_new(entry->multisampled);
		entry->used = recording_serial();
	}
	return handle;
}

void xgpu_metal_texture_destroy(uint32_t texture)
{
	struct texture_entry *entry = texture_get(texture);
	int stage;

	if (!entry)
		return;
	/* (the command buffers that use it keep it until they are done) */
	entry->texture = nil;
	entry->multisampled = nil;
	entry->previous = nil;
	entry->present = 0;
	for (stage = 0; stage < XGPU_METAL_TEXTURE_STAGE_COUNT; stage++)
	{
		if (metal.stage_textures[stage] == texture)
			metal.stage_textures[stage] = 0;
	}
}


void xgpu_metal_texture_make_levels(uint32_t texture)
{
	struct texture_entry *entry = texture_get(texture);
	id<MTLBlitCommandEncoder> blit;

	if (!entry || entry->texture.mipmapLevelCount < 2)
		return;
	if (!metal.in_frame)
		xgpu_metal_frame_begin();
	texture_contents(entry);
	pass_end();
	blit = blit_encoder("levels");
	[blit generateMipmapsForTexture:entry->texture];
	[blit endEncoding];
	entry->used = recording_serial();
}

void xgpu_metal_texture_copy(uint32_t source, uint32_t destination)
{
	struct texture_entry *from = texture_get(source), *to = texture_get(destination);
	id<MTLBlitCommandEncoder> blit;
	NSUInteger width, height;

	if (!from || !to || from->texture.pixelFormat != to->texture.pixelFormat)
		return;
	texture_contents(from);
	texture_contents(to);
	width = from->texture.width < to->texture.width ? from->texture.width : to->texture.width;
	height = from->texture.height < to->texture.height ? from->texture.height : to->texture.height;
	if (!metal.in_frame)
		xgpu_metal_frame_begin();
	pass_end();
	blit = blit_encoder("copy");
	[blit copyFromTexture:from->texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
		sourceSize:MTLSizeMake(width, height, 1) toTexture:to->texture destinationSlice:0 destinationLevel:0
		destinationOrigin:MTLOriginMake(0, 0, 0)];
	[blit endEncoding];
	from->used = to->used = recording_serial();
	statistics_add(&metal.statistics.feedback_copies, 1);
}

static void statistics_add(uint32_t *counter, uint32_t amount)
{
	pthread_mutex_lock(&metal.statistics_lock);
	*counter += amount;
	pthread_mutex_unlock(&metal.statistics_lock);
}

/* a back buffer given to the presentation at the last frame's end: the
pictures it had, copied into the texture in its place before anything but
a clear of the whole of it uses it */
static void texture_contents(struct texture_entry *entry)
{
	id<MTLBlitCommandEncoder> blit;

	if (!entry || !entry->previous)
		return;
	if (!metal.in_frame)
		xgpu_metal_frame_begin();
	/* (not pass_end: the clears waiting for the pass's start still wait,
	for the targets they are of) */
	encoder_end();
	blit = blit_encoder("restore");
	[blit copyFromTexture:entry->previous toTexture:entry->texture];
	[blit endEncoding];
	entry->previous = nil;
	entry->used = recording_serial();
	statistics_add(&metal.statistics.pictures_restored, 1);
}

/* (the ring: below) */
static id<MTLBuffer> ring_buffer(struct xgpu_metal_source source, NSUInteger *offset);

void xgpu_metal_texture_upload(uint32_t texture, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t z,
	uint32_t width, uint32_t height, uint32_t depth, const void *data, uint32_t bytes_per_row,
	uint32_t bytes_per_image)
{
	struct texture_entry *entry = texture_get(texture);
	const struct xgpu_metal_format_information *format;
	MTLRegion region;
	uint32_t rows, size;

	if (!entry || !width || !height)
		return;
	texture_contents(entry);
	format = format_get(entry->description.format);
	if (!depth)
		depth = 1;
	rows = format->compressed ? (height + 3) / 4 : height;
	if (!bytes_per_image)
		bytes_per_image = bytes_per_row * rows;
	size = bytes_per_image * (depth - 1) + bytes_per_row * (rows - 1) +
		(format->compressed ? (width + 3) / 4 * format->bytes : width * format->bytes);
	region = MTLRegionMake3D(x, y, z, width, height, depth);
	if (entry->texture.storageMode == MTLStorageModeShared && entry->used <= xgpu_metal_frame_completed())
	{
		/* nothing the GPU has to do reads it: written now */
		[entry->texture replaceRegion:region mipmapLevel:level slice:layer withBytes:data bytesPerRow:bytes_per_row
			bytesPerImage:entry->description.type == XGPU_METAL_TEXTURE_3D ? bytes_per_image : 0];
		statistics_add(&metal.statistics.uploads_direct, 1);
	}
	else
	{
		/* in the command buffer's order: a blit from the ring */
		struct xgpu_metal_source source = xgpu_metal_stream(data, size);
		NSUInteger offset;
		id<MTLBuffer> buffer = ring_buffer(source, &offset);
		id<MTLBlitCommandEncoder> blit;

		if (!buffer)
			return;
		pass_end();
		blit = blit_encoder("upload");
		[blit copyFromBuffer:buffer sourceOffset:offset sourceBytesPerRow:bytes_per_row
			sourceBytesPerImage:entry->description.type == XGPU_METAL_TEXTURE_3D ? bytes_per_image : 0
			sourceSize:MTLSizeMake(width, height, depth) toTexture:entry->texture destinationSlice:layer
			destinationLevel:level destinationOrigin:MTLOriginMake(x, y, z)];
		[blit endEncoding];
		entry->used = recording_serial();
		statistics_add(&metal.statistics.uploads_ordered, 1);
	}
}

/* ---------- the upload ring */

struct xgpu_metal_source xgpu_metal_stream(const void *data, uint32_t size)
{
	struct frame_slot *slot;
	struct xgpu_metal_source source = { XGPU_METAL_SOURCE_NONE, 0, 0, 0 };
	uint32_t position;

	if (!size || size > (1u << RING_CHUNK_SHIFT))
		return source;
	/* (the data is the frame's: one begins, if none has, before it takes
	ring space whose fence it sets) */
	if (!metal.in_frame)
		xgpu_metal_frame_begin();
	slot = metal.slot;
	position = (slot->position + RING_ALIGNMENT - 1) & ~(uint32_t)(RING_ALIGNMENT - 1);
	if (position + size > slot->chunks[slot->chunk].size)
	{
		/* the next chunk, made where there is none or it is too small */
		uint32_t next = slot->chunk + 1;

		if (next >= RING_CHUNK_MAXIMUM)
		{
			host_logf(HOST_LOG_ERROR, "metal: the frame's upload ring is full");
			return source;
		}
		if (next >= slot->chunk_count || slot->chunks[next].size < size)
		{
			uint32_t chunk_size = size > RING_CHUNK_SIZE ? size : RING_CHUNK_SIZE;

			slot->chunks[next].buffer = [metal.device newBufferWithLength:chunk_size
				options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
			slot->chunks[next].size = chunk_size;
			if (next >= slot->chunk_count)
				slot->chunk_count = next + 1;
		}
		slot->chunk = next;
		position = 0;
	}
	memcpy((unsigned char *)slot->chunks[slot->chunk].buffer.contents + position, data, size);
	slot->position = position + size;
	source.kind = XGPU_METAL_SOURCE_RING;
	source.offset = (slot->chunk << RING_CHUNK_SHIFT) + position;
	source.size = size;
	pthread_mutex_lock(&metal.statistics_lock);
	metal.statistics.ring_bytes += size;
	pthread_mutex_unlock(&metal.statistics_lock);
	return source;
}

static id<MTLBuffer> ring_buffer(struct xgpu_metal_source source, NSUInteger *offset)
{
	uint32_t chunk = source.offset >> RING_CHUNK_SHIFT;

	if (source.kind != XGPU_METAL_SOURCE_RING || chunk >= metal.slot->chunk_count)
		return nil;
	*offset = source.offset & ((1u << RING_CHUNK_SHIFT) - 1);
	return metal.slot->chunks[chunk].buffer;
}

/* ---------- the guest's memory, read in place */

struct xgpu_metal_source xgpu_metal_guest(uint32_t address, uint32_t size)
{
	struct xgpu_metal_source source = { XGPU_METAL_SOURCE_GUEST, address, size };

	return source;
}

void xgpu_metal_guest_remapped(uint32_t address, uint32_t size)
{
	uint64_t end = (uint64_t)address + size;
	uint32_t index;

	pthread_mutex_lock(&metal.region_lock);
	metal.remap_generation++;
	for (index = 0; index < metal.region_count;)
	{
		struct guest_region *region = &metal.regions[index];

		if (region->start < end && address < region->end)
		{
			/* (a command buffer that uses it keeps it, and the pages it
			had, until it is done) */
			metal.region_count--;
			metal.regions[index] = metal.regions[metal.region_count];
			metal.regions[metal.region_count].buffer = nil;
		}
		else
		{
			index++;
		}
	}
	pthread_mutex_unlock(&metal.region_lock);
}

/* a buffer over the guest range and the range's offset in it, or nil */
static id<MTLBuffer> guest_buffer(uint32_t address, uint32_t size, NSUInteger *offset)
{
	uint64_t end = (uint64_t)address + size;
	uint32_t index, start, extent_end, limit_start;
	uint64_t limit_end;
	struct guest_region *region;
	id<MTLBuffer> buffer = nil;
	uint64_t generation;

	pthread_mutex_lock(&metal.region_lock);
	metal.region_clock++;
	generation = metal.remap_generation;
	for (index = 0; index < metal.region_count; index++)
	{
		region = &metal.regions[index];
		if (region->start <= address && end <= region->end)
		{
			region->used = metal.region_clock;
			*offset = address - region->start;
			buffer = region->buffer;
			break;
		}
	}
	pthread_mutex_unlock(&metal.region_lock);
	if (buffer || !metal.options.guest_extent || !size)
		return buffer;

	/* a new one: the readable pages about the range, inside its granules */
	limit_start = address & ~(GUEST_GRANULE - 1);
	limit_end = (end + GUEST_GRANULE - 1) & ~(uint64_t)(GUEST_GRANULE - 1);
	if (limit_end > 0xffffffffull)
		limit_end = 0x100000000ull - metal.page_size;
	if (!metal.options.guest_extent(address, size, limit_start, (uint32_t)limit_end, &start, &extent_end) ||
		start > address || extent_end < end || ((start | extent_end) & (metal.page_size - 1)))
		return nil;
	buffer = [metal.device newBufferWithBytesNoCopy:(void *)host_address(start) length:extent_end - start
		options:MTLResourceStorageModeShared deallocator:nil];
	if (!buffer)
		return nil;
	statistics_add(&metal.statistics.guest_buffers_made, 1);
	pthread_mutex_lock(&metal.region_lock);
	if (generation != metal.remap_generation)
	{
		pthread_mutex_unlock(&metal.region_lock);
		return nil;
	}
	if (metal.region_count == GUEST_REGION_MAXIMUM)
	{
		/* the least recently used goes */
		uint32_t oldest = 0;

		for (index = 1; index < metal.region_count; index++)
		{
			if (metal.regions[index].used < metal.regions[oldest].used)
				oldest = index;
		}
		metal.region_count--;
		metal.regions[oldest] = metal.regions[metal.region_count];
		metal.regions[metal.region_count].buffer = nil;
	}
	region = &metal.regions[metal.region_count++];
	region->start = start;
	region->end = extent_end;
	region->buffer = buffer;
	region->used = metal.region_clock;
	pthread_mutex_unlock(&metal.region_lock);
	*offset = address - start;
	return buffer;
}

/* the buffer and offset a source is read from, or nil */
static id<MTLBuffer> source_buffer(struct xgpu_metal_source source, NSUInteger *offset)
{
	id<MTLBuffer> buffer;

	switch (source.kind)
	{
	case XGPU_METAL_SOURCE_RING:
		return ring_buffer(source, offset);
	case XGPU_METAL_SOURCE_STREAM_RING:
		if (!metal.stream_ring || (uint64_t)source.offset + source.size > metal.stream_ring.length)
			return nil;
		*offset = source.offset;
		return metal.stream_ring;
	case XGPU_METAL_SOURCE_BUFFER:
		if (!source.handle || source.handle > metal.buffers.count ||
			(uint64_t)source.offset + source.size > metal.buffers[source.handle - 1].length)
			return nil;
		metal.buffers_used[source.handle - 1] = @(recording_serial());
		*offset = source.offset;
		return metal.buffers[source.handle - 1];
	case XGPU_METAL_SOURCE_GUEST:
		buffer = guest_buffer(source.offset, source.size, offset);
		if (buffer)
			return buffer;
		/* (not mapped as a whole, or not wrapped: copied) */
		statistics_add(&metal.statistics.guest_bytes_copied, source.size);
		return ring_buffer(xgpu_metal_stream((const void *)host_address(source.offset), source.size), offset);
	default:
		return nil;
	}
}

int xgpu_metal_set_stream_ring(void *ring, uint32_t size)
{
	metal.stream_ring = [metal.device newBufferWithBytesNoCopy:ring length:size options:MTLResourceStorageModeShared
		deallocator:nil];
	if (!metal.stream_ring)
		host_logf(HOST_LOG_ERROR, "metal: cannot wrap the command stream's ring (%u bytes at %p)", size, ring);
	return metal.stream_ring != nil;
}

uint32_t xgpu_metal_buffer_create(uint32_t size)
{
	id<MTLBuffer> buffer = [metal.device newBufferWithLength:size options:MTLResourceStorageModeShared];

	if (!buffer)
		return 0;
	[metal.buffers addObject:buffer];
	[metal.buffers_used addObject:@0];
	return (uint32_t)metal.buffers.count;
}

const void *xgpu_metal_buffer_contents(uint32_t buffer)
{
	return buffer && buffer <= metal.buffers.count ? metal.buffers[buffer - 1].contents : NULL;
}

void xgpu_metal_buffer_write(uint32_t buffer, uint32_t offset, const void *data, uint32_t size, int unused)
{
	id<MTLBuffer> object;
	struct xgpu_metal_source source;
	NSUInteger source_offset;
	id<MTLBuffer> source_buffer_object;
	id<MTLBlitCommandEncoder> blit;

	if (!buffer || buffer > metal.buffers.count || !size)
		return;
	object = metal.buffers[buffer - 1];
	if ((uint64_t)offset + size > object.length)
		return;
	if (unused || metal.buffers_used[buffer - 1].unsignedLongLongValue <= xgpu_metal_frame_completed())
	{
		/* nothing the GPU has to do reads those bytes */
		memcpy((unsigned char *)object.contents + offset, data, size);
		statistics_add(&metal.statistics.uploads_direct, 1);
		return;
	}
	/* in the command buffer's order: a blit from the ring */
	source = xgpu_metal_stream(data, size);
	source_buffer_object = ring_buffer(source, &source_offset);
	if (!source_buffer_object)
		return;
	pass_end();
	blit = blit_encoder("buffer");
	[blit copyFromBuffer:source_buffer_object sourceOffset:source_offset toBuffer:object destinationOffset:offset size:size];
	[blit endEncoding];
	metal.buffers_used[buffer - 1] = @(recording_serial());
	statistics_add(&metal.statistics.uploads_ordered, 1);
}

uint64_t xgpu_metal_fence(void)
{
	if (metal.command_buffer || metal.encoder || metal.color_load == MTLLoadActionClear ||
		metal.depth_load == MTLLoadActionClear || metal.stencil_load == MTLLoadActionClear)
	{
		if (!metal.in_frame)
			xgpu_metal_frame_begin();
		metal.slot->fence = command_buffer_commit(nil);
		/* (the next pass loads what this one stored) */
		metal.dirty = _dirty_all;
	}
	return metal.command_serial;
}

/* ---------- frames */


void xgpu_metal_frame_begin(void)
{
	struct frame_slot *slot;
	uint32_t index;

	if (metal.in_frame)
		return;
	metal.frame_number++;
	slot = &metal.slots[metal.frame_number % FRAME_SLOTS];
	/* (the frame that last had these resources, FRAMES_IN_FLIGHT + 1
	frames ago, is done after the one FRAMES_IN_FLIGHT ago) */
	xgpu_metal_frame_wait(slot->fence);
	slot->chunk = 0;
	slot->position = 0;
	for (index = 0; index < slot->visibility_passes; index++)
	{
		if (slot->visibility_pass_entries[index])
			memset(slot->visibility_buffers[index].contents, 0, slot->visibility_pass_entries[index] * sizeof(uint64_t));
		slot->visibility_pass_entries[index] = 0;
	}
	slot->visibility_passes = 0;
	slot->visibility_entries = 0;
	slot->number = metal.frame_number;
	metal.slot = slot;
	metal.in_frame = YES;
	metal.dirty = _dirty_all;
	profile_frame_begin(metal.frame_number);
	if (metal.frame_number > FRAMES_IN_FLIGHT)
		xgpu_metal_frame_wait(metal.slots[(metal.frame_number - FRAMES_IN_FLIGHT) % FRAME_SLOTS].fence);
}

/* the frame's visibility tests' counts, as its last command buffer
completes */
static void visibility_publish(struct frame_slot *slot, uint32_t entries, uint64_t number)
{
	uint32_t entry;

	pthread_mutex_lock(&visibility.lock);
	for (entry = 0; entry < entries; entry++)
	{
		uint32_t test = slot->visibility_slots[entry];

		if (visibility.frames[test] != number)
		{
			visibility.frames[test] = number;
			visibility.samples[test] = 0;
		}
		visibility.samples[test] += (uint32_t)((const uint64_t *)
			slot->visibility_buffers[slot->visibility_entry_passes[entry]].contents)[slot->visibility_entry_offsets[entry]];
		if (metal.visibility_output)
			metal.visibility_output[test] = visibility.areas[test] > 1.0f ?
				(uint32_t)((float)visibility.samples[test] / visibility.areas[test] + 0.5f) : visibility.samples[test];
	}
	pthread_mutex_unlock(&visibility.lock);
}

uint64_t xgpu_metal_frame_end(uint32_t present)
{
	struct frame_slot *slot;
	uint32_t entries;
	uint64_t number;
	struct texture_entry *picture = texture_get(present);
	id<MTLTexture> image = nil;
	BOOL timed;

	if (!metal.in_frame)
		xgpu_metal_frame_begin();
	pass_end();
	slot = metal.slot;
	entries = slot->visibility_entries;
	number = slot->number;
	if (picture && host_metal_window_active() && (picture->texture.textureType != MTLTextureType2D ||
		picture->texture.mipmapLevelCount != 1))
	{
		/* (a target of levels or layers: its first, copied into an image of
		the presentation's) */
		id<MTLBlitCommandEncoder> blit;

		texture_contents(picture);
		if (!metal.present_copy || metal.present_copy.width != picture->description.width ||
			metal.present_copy.height != picture->description.height)
		{
			MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
				width:picture->description.width height:picture->description.height mipmapped:NO];

			descriptor.usage = MTLTextureUsageShaderRead;
			descriptor.storageMode = MTLStorageModePrivate;
			metal.present_copy = [metal.device newTextureWithDescriptor:descriptor];
		}
		image = metal.present_copy;
		blit = blit_encoder("present");
		[blit copyFromTexture:picture->texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
			sourceSize:MTLSizeMake(picture->description.width, picture->description.height, 1) toTexture:image
			destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];
		picture->used = recording_serial();
		metal.present_copy = host_metal_present_exchange(image);
	}
	else if (picture && host_metal_window_active())
	{
		/* the picture is the back buffer's texture, given to the
		presentation: the next frame draws into another (the one the
		presentation gives back, no longer shown). A copy of the picture
		into one of the presentation's would be a pass over the whole of
		both. That frame finds the pictures of this one where it does not
		clear the whole of it first (texture_contents) */
		id<MTLTexture> next;

		texture_contents(picture);
		image = picture->texture;
		next = host_metal_present_exchange(image);
		if (!next)
		{
			MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:image.pixelFormat
				width:image.width height:image.height mipmapped:NO];

			descriptor.usage = image.usage;
			descriptor.storageMode = image.storageMode;
			descriptor.swizzle = image.swizzle;
			next = [metal.device newTextureWithDescriptor:descriptor];
		}
		picture->previous = image;
		picture->texture = next;
		picture->used = recording_serial();
	}
	timed = atomic_load(&profile.state) == 1;
	if (timed)
		atomic_store(&profile.state, 2);
	slot->fence = command_buffer_commit(entries || timed ? ^{
		if (entries)
			visibility_publish(slot, entries, number);
		if (timed)
			profile_log();
	} : nil);
	if (profile.capturing && number == profile.capture_frame + 1)
	{
		[[MTLCaptureManager sharedCaptureManager] stopCapture];
		profile.capturing = NO;
		host_logf(HOST_LOG_INFO, "metal: captured into %s", profile.capture_path.UTF8String);
	}
	/* (nil: a frame with nothing to show, taken at once) */
	host_metal_present_queue(image);
	metal.in_frame = NO;
	statistics_add(&metal.statistics.frames, 1);
	{
		/* (the render thread's CPU time since the last frame's end) */
		static double last;
		struct timespec now;
		double seconds;

		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
		seconds = (double)now.tv_sec + (double)now.tv_nsec * 1e-9;
		pthread_mutex_lock(&metal.statistics_lock);
		if (last > 0.0)
			metal.statistics.render_cpu_seconds += seconds - last;
		pthread_mutex_unlock(&metal.statistics_lock);
		last = seconds;
	}
	return slot->fence;
}

uint64_t xgpu_metal_visibility_result(uint32_t slot, uint32_t *samples)
{
	uint64_t frame;

	if (slot >= XGPU_METAL_VISIBILITY_SLOTS)
	{
		*samples = 0;
		return 0;
	}
	pthread_mutex_lock(&visibility.lock);
	*samples = visibility.samples[slot];
	frame = visibility.frames[slot];
	pthread_mutex_unlock(&visibility.lock);
	return frame;
}

/* ---------- passes */

void xgpu_metal_set_targets(uint32_t color, uint32_t color_level, uint32_t color_layer, uint32_t depth,
	int multisampled)
{
	struct texture_entry *color_entry = texture_get(color), *depth_entry = texture_get(depth);

	if (!metal.in_frame)
		xgpu_metal_frame_begin();
	/* (the same targets: the pass goes on, or the clears of whole
	attachments waiting for it to begin still wait: a pass of its own for
	them would store the targets, and the next would load them again) */
	if ((metal.encoder || metal.color_load == MTLLoadActionClear || metal.depth_load == MTLLoadActionClear ||
		metal.stencil_load == MTLLoadActionClear) && metal.color == (color_entry ? color : 0) && metal.color_level == color_level &&
		metal.color_layer == color_layer && metal.depth == (depth_entry ? depth : 0) &&
		metal.multisampled == (multisampled != 0))
		return;
	pass_end();
	metal.color = color_entry ? color : 0;
	metal.color_level = color_level;
	metal.color_layer = color_layer;
	metal.depth = depth_entry ? depth : 0;
	metal.multisampled = multisampled != 0;
	metal.color_load = MTLLoadActionLoad;
	metal.depth_load = MTLLoadActionLoad;
	metal.stencil_load = MTLLoadActionLoad;
	if (color_entry)
	{
		metal.target_width = color_entry->description.width >> color_level;
		metal.target_height = color_entry->description.height >> color_level;
	}
	else if (depth_entry)
	{
		metal.target_width = depth_entry->description.width;
		metal.target_height = depth_entry->description.height;
	}
	else
	{
		metal.target_width = metal.target_height = 0;
	}
	if (!metal.target_width)
		metal.target_width = 1;
	if (!metal.target_height)
		metal.target_height = 1;
}

static void visibility_entry_begin(void)
{
	struct frame_slot *slot = metal.slot;

	int32_t entry;

	if (slot->visibility_entries >= VISIBILITY_ENTRIES || metal.visibility_pass < 0 ||
		slot->visibility_pass_entries[metal.visibility_pass] >= VISIBILITY_PASS_ENTRIES)
	{
		metal.visibility_entry = -1;
		return;
	}
	entry = (int32_t)slot->visibility_entries++;
	slot->visibility_slots[entry] = 0;
	slot->visibility_entry_passes[entry] = (uint16_t)metal.visibility_pass;
	slot->visibility_entry_offsets[entry] = slot->visibility_pass_entries[metal.visibility_pass]++;
	metal.visibility_entry = entry;
	[metal.encoder setVisibilityResultMode:MTLVisibilityResultModeCounting
		offset:(NSUInteger)slot->visibility_entry_offsets[entry] * sizeof(uint64_t)];
}

/* the render command encoder for the targets, begun if there is none */
static id<MTLRenderCommandEncoder> encoder(void)
{
	MTLRenderPassDescriptor *pass;
	struct texture_entry *color = texture_get(metal.color), *depth = texture_get(metal.depth);
	uint64_t serial;

	if (metal.encoder)
		return metal.encoder;
	if (!color && !depth)
		return nil;
	if (!metal.in_frame)
		xgpu_metal_frame_begin();
	/* (a back buffer given to the presentation: its pictures, unless this
	pass clears the whole of it, or resolves into the whole of it) */
	if (color && color->previous)
	{
		if (metal.color_load == MTLLoadActionClear || (color->multisampled && metal.multisampled))
			color->previous = nil;
		else
			texture_contents(color);
	}
	serial = recording_serial();
	pass = [MTLRenderPassDescriptor renderPassDescriptor];
	if (color)
	{
		MTLRenderPassColorAttachmentDescriptor *attachment = pass.colorAttachments[0];

		if (color->multisampled && metal.multisampled)
		{
			attachment.texture = color->multisampled;
			attachment.resolveTexture = color->texture;
			attachment.resolveLevel = metal.color_level;
			attachment.resolveSlice = metal.color_layer;
			attachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
		}
		else
		{
			attachment.texture = color->texture;
			attachment.level = metal.color_level;
			attachment.slice = metal.color_layer;
			attachment.storeAction = MTLStoreActionStore;
		}
		attachment.loadAction = metal.color_load;
		attachment.clearColor = metal.clear_color;
		color->used = serial;
	}
	if (depth)
	{
		const struct xgpu_metal_format_information *format = format_get(depth->description.format);
		id<MTLTexture> texture = depth->multisampled && metal.multisampled ? depth->multisampled : depth->texture;

		pass.depthAttachment.texture = texture;
		pass.depthAttachment.loadAction = metal.depth_load;
		pass.depthAttachment.clearDepth = metal.clear_depth;
		pass.depthAttachment.storeAction = MTLStoreActionStore;
		if (format->stencil)
		{
			pass.stencilAttachment.texture = texture;
			pass.stencilAttachment.loadAction = metal.stencil_load;
			pass.stencilAttachment.clearStencil = metal.clear_stencil;
			pass.stencilAttachment.storeAction = MTLStoreActionStore;
		}
		depth->used = serial;
	}
	/* (a buffer for each pass: those of earlier passes stay as they are) */
	metal.visibility_pass = -1;
	if (metal.slot->visibility_passes < VISIBILITY_PASSES)
	{
		struct frame_slot *slot = metal.slot;
		uint32_t index = slot->visibility_passes++;

		if (!slot->visibility_buffers[index])
			slot->visibility_buffers[index] = [metal.device newBufferWithLength:VISIBILITY_PASS_ENTRIES * sizeof(uint64_t)
				options:MTLResourceStorageModeShared];
		pass.visibilityResultBuffer = slot->visibility_buffers[index];
		metal.visibility_pass = (int32_t)index;
	}
	profile.current = profile_pass_add("render");
	if (profile.current >= 0)
	{
		struct profile_pass *timed = &profile.passes[profile.current];
		MTLRenderPassSampleBufferAttachmentDescriptor *samples = pass.sampleBufferAttachments[0];

		samples.sampleBuffer = profile.samples;
		samples.startOfVertexSampleIndex = (NSUInteger)profile.current * 4;
		samples.endOfVertexSampleIndex = (NSUInteger)profile.current * 4 + 1;
		samples.startOfFragmentSampleIndex = (NSUInteger)profile.current * 4 + 2;
		samples.endOfFragmentSampleIndex = (NSUInteger)profile.current * 4 + 3;
		timed->color = color ? metal.color : 0;
		timed->depth = depth ? metal.depth : 0;
		timed->width = metal.target_width;
		timed->height = metal.target_height;
		timed->color_format = color ? color->description.format : 0;
		timed->depth_format = depth ? depth->description.format : 0;
		timed->samples = metal.multisampled && ((color && color->multisampled) || (depth && depth->multisampled)) ?
			(color && color->multisampled ? color->description.samples : depth->description.samples) : 1;
		if (color)
		{
			timed->loads[0] = (int8_t)pass.colorAttachments[0].loadAction;
			timed->stores[0] = (int8_t)pass.colorAttachments[0].storeAction;
		}
		if (depth)
		{
			timed->loads[1] = (int8_t)pass.depthAttachment.loadAction;
			timed->stores[1] = (int8_t)pass.depthAttachment.storeAction;
			if (pass.stencilAttachment.texture)
			{
				timed->loads[2] = (int8_t)pass.stencilAttachment.loadAction;
				timed->stores[2] = (int8_t)pass.stencilAttachment.storeAction;
			}
		}
	}
	metal.encoder = [command_buffer() renderCommandEncoderWithDescriptor:pass];
	/* (the clears are done: the pass goes on from what it stores) */
	metal.color_load = metal.depth_load = metal.stencil_load = MTLLoadActionLoad;
	metal.dirty = _dirty_all;
	metal.visibility_pending = metal.visibility_active;
	statistics_add(&metal.statistics.passes, 1);
	return metal.encoder;
}

/* ends the pass; one that has not begun is begun first if it clears
something (a clear of whole attachments waits for the pass's start) */
static void pass_end(void)
{
	if (!metal.encoder && (metal.color_load == MTLLoadActionClear || metal.depth_load == MTLLoadActionClear ||
		metal.stencil_load == MTLLoadActionClear))
		encoder();
	encoder_end();
}

/* the clear pipeline for the targets and parts */
static id<MTLRenderPipelineState> clear_pipeline(uint32_t parts)
{
	struct texture_entry *color = texture_get(metal.color), *depth = texture_get(metal.depth);
	struct xgpu_metal_clear_key key = { 0 };
	NSData *data;
	id<MTLRenderPipelineState> state;
	MTLRenderPipelineDescriptor *descriptor;
	NSError *error = nil;

	key.color_format = color ? color->description.format : 0;
	key.depth_format = depth ? depth->description.format : 0;
	key.samples = !metal.multisampled ? 1 : color && color->multisampled ? color->description.samples :
		depth && depth->multisampled ? depth->description.samples : 1;
	/* (the colour's channels written) */
	key.parts = (uint8_t)(parts & XGPU_METAL_CLEAR_COLOR ? parts & XGPU_METAL_CLEAR_CHANNELS : 0);
	data = [NSData dataWithBytes:&key length:sizeof(key)];
	state = metal.clear_pipelines[data];
	if (state)
		return state;
	descriptor = xgpu_metal_clear_descriptor(&key);
	descriptor.vertexFunction = metal.clear_vertex;
	descriptor.fragmentFunction = metal.clear_fragment;
	state = [metal.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
	if (!state)
		metal_log(HOST_LOG_ERROR, @"the clear pipeline: %@", error.localizedDescription);
	else
		metal.clear_pipelines[data] = state;
	return state;
}

void xgpu_metal_clear(const int32_t (*rectangles)[4], uint32_t count, uint32_t parts, const float color[4],
	float depth, uint32_t stencil)
{
	struct texture_entry *color_entry = texture_get(metal.color), *depth_entry = texture_get(metal.depth);
	int32_t whole[1][4];
	BOOL has_stencil = depth_entry && format_get(depth_entry->description.format)->stencil;
	id<MTLRenderCommandEncoder> render;
	id<MTLRenderPipelineState> state;
	uint32_t index;

	if (!color_entry || !(parts & XGPU_METAL_CLEAR_CHANNELS))
		parts &= ~(uint32_t)XGPU_METAL_CLEAR_COLOR;
	if (!depth_entry)
		parts &= ~(uint32_t)XGPU_METAL_CLEAR_DEPTH;
	if (!has_stencil)
		parts &= ~(uint32_t)XGPU_METAL_CLEAR_STENCIL;
	if (!(parts & (XGPU_METAL_CLEAR_COLOR | XGPU_METAL_CLEAR_DEPTH | XGPU_METAL_CLEAR_STENCIL)))
		return;
	if (!count)
	{
		whole[0][0] = whole[0][1] = 0;
		whole[0][2] = (int32_t)metal.target_width;
		whole[0][3] = (int32_t)metal.target_height;
		rectangles = (const int32_t (*)[4])whole;
		count = 1;
	}
	/* before the pass begins, a clear of whole attachments is their load
	action (not of some of a colour's channels: drawn) */
	if (!metal.encoder && count == 1 && rectangles[0][0] <= 0 && rectangles[0][1] <= 0 &&
		(!(parts & XGPU_METAL_CLEAR_COLOR) || (parts & XGPU_METAL_CLEAR_CHANNELS) == XGPU_METAL_CLEAR_CHANNELS) &&
		rectangles[0][0] + rectangles[0][2] >= (int32_t)metal.target_width &&
		rectangles[0][1] + rectangles[0][3] >= (int32_t)metal.target_height)
	{
		if (parts & XGPU_METAL_CLEAR_COLOR)
		{
			metal.color_load = MTLLoadActionClear;
			metal.clear_color = MTLClearColorMake(color[0], color[1], color[2], color[3]);
		}
		if (parts & XGPU_METAL_CLEAR_DEPTH)
		{
			metal.depth_load = MTLLoadActionClear;
			metal.clear_depth = depth;
		}
		if (parts & XGPU_METAL_CLEAR_STENCIL)
		{
			metal.stencil_load = MTLLoadActionClear;
			metal.clear_stencil = stencil;
		}
		statistics_add(&metal.statistics.clears_as_loads, 1);
		return;
	}

	/* else a quad for each rectangle */
	render = encoder();
	state = render ? clear_pipeline(parts) : nil;
	if (!state)
		return;
	[render setRenderPipelineState:state];
	[render setDepthStencilState:metal.clear_depth_stencil[((parts & XGPU_METAL_CLEAR_DEPTH) ? 1 : 0) |
		((parts & XGPU_METAL_CLEAR_STENCIL) ? 2 : 0)]];
	[render setStencilReferenceValue:stencil & 0xff];
	[render setCullMode:MTLCullModeNone];
	[render setTriangleFillMode:MTLTriangleFillModeFill];
	[render setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
	[render setDepthClipMode:MTLDepthClipModeClip];
	[render setViewport:(MTLViewport){ 0.0, 0.0, metal.target_width, metal.target_height, 0.0, 1.0 }];
	[render setScissorRect:(MTLScissorRect){ 0, 0, metal.target_width, metal.target_height }];
	if (metal.visibility_active && metal.visibility_entry >= 0)
		[render setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
	for (index = 0; index < count; index++)
	{
		int32_t left = rectangles[index][0] > 0 ? rectangles[index][0] : 0;
		int32_t top = rectangles[index][1] > 0 ? rectangles[index][1] : 0;
		int32_t right = rectangles[index][0] + rectangles[index][2];
		int32_t bottom = rectangles[index][1] + rectangles[index][3];
		struct
		{
			float rectangle[4];
			float color[4];
			float depth;
			float pad[3];
		} parameters;

		if (right > (int32_t)metal.target_width)
			right = (int32_t)metal.target_width;
		if (bottom > (int32_t)metal.target_height)
			bottom = (int32_t)metal.target_height;
		if (right <= left || bottom <= top)
			continue;
		/* (clip space: y up, the targets' rows from the top) */
		parameters.rectangle[0] = 2.0f * (float)left / (float)metal.target_width - 1.0f;
		parameters.rectangle[1] = 1.0f - 2.0f * (float)top / (float)metal.target_height;
		parameters.rectangle[2] = 2.0f * (float)right / (float)metal.target_width - 1.0f;
		parameters.rectangle[3] = 1.0f - 2.0f * (float)bottom / (float)metal.target_height;
		memcpy(parameters.color, color, sizeof(parameters.color));
		parameters.depth = depth;
		[render setVertexBytes:&parameters length:sizeof(parameters) atIndex:0];
		[render setFragmentBytes:&parameters length:sizeof(parameters) atIndex:0];
		[render drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		statistics_add(&metal.statistics.clears_as_quads, 1);
		if (profile.current >= 0)
			profile.passes[profile.current].clears++;
	}
	if (metal.visibility_active && metal.visibility_entry >= 0)
		[render setVisibilityResultMode:MTLVisibilityResultModeCounting
			offset:(NSUInteger)metal.slot->visibility_entry_offsets[metal.visibility_entry] * sizeof(uint64_t)];
	/* (the draws' state is set again) */
	metal.dirty = _dirty_all;
}

/* ---------- pipelines */

void xgpu_metal_set_pipeline(const struct xgpu_metal_pipeline *pipeline)
{
	if (metal.have_pipeline && !memcmp(&metal.pipeline, pipeline, sizeof(*pipeline)))
		return;
	metal.pipeline = *pipeline;
	metal.have_pipeline = YES;
	metal.dirty |= _dirty_pipeline | _dirty_depth_stencil | _dirty_raster | _dirty_streams;
}

/* work on the making queue once both groups (either NULL) are done */
static void pipeline_after(dispatch_group_t first, dispatch_group_t second, dispatch_block_t work)
{
	if (first)
		dispatch_group_notify(first, metal.making_queue, ^{
			pipeline_after(nil, second, work);
		});
	else if (second)
		dispatch_group_notify(second, metal.making_queue, work);
	else
		dispatch_async(metal.making_queue, work);
}

/* makes a pipeline on the making queue (once: none is made twice at once),
when its shaders are; the draws use it from when it is made (into group,
if not NULL: the loading screen's, or a draw's waiting for it) */
static void pipeline_make_later(const struct xgpu_metal_pipeline_state_key *key, dispatch_group_t group)
{
	struct xgpu_metal_pipeline_state_key copy = *key;
	dispatch_group_t vertex_making, pixel_making;
	NSData *data = [NSData dataWithBytes:key length:sizeof(*key)];

	os_unfair_lock_lock(&metal.lock);
	if (metal.pipelines[data] || ([metal.pipelines_making containsObject:data] && !group))
	{
		os_unfair_lock_unlock(&metal.lock);
		return;
	}
	if ([metal.pipelines_making containsObject:data])
	{
		/* (being made: a group waits for it by making it again, which
		finds it made) */
		os_unfair_lock_unlock(&metal.lock);
		dispatch_group_enter(group);
		dispatch_async(metal.making_queue, ^{
			pipeline_make_later(&copy, nil);
			dispatch_group_leave(group);
		});
		return;
	}
	[metal.pipelines_making addObject:data];
	vertex_making = key->vertex_shader <= metal.shader_count ? metal.shaders[key->vertex_shader].making : nil;
	pixel_making = key->pixel_shader <= metal.shader_count ? metal.shaders[key->pixel_shader].making : nil;
	os_unfair_lock_unlock(&metal.lock);
	if (group)
		dispatch_group_enter(group);
	/* (made once its shaders are: waited for without a thread held) */
	pipeline_after(vertex_making, pixel_making, ^{
		MTLRenderPipelineDescriptor *descriptor;
		id<MTLRenderPipelineState> state = nil;
		NSError *error = nil;
		double start;

		start = seconds_now();
		descriptor = xgpu_metal_pipeline_descriptor(&copy);
		descriptor.vertexFunction = shader_function(copy.vertex_shader);
		descriptor.fragmentFunction = shader_function(copy.pixel_shader);
		if (descriptor.vertexFunction)
		{
			if (metal.archive)
			{
				descriptor.binaryArchives = @[metal.archive];
				state = [metal.device newRenderPipelineStateWithDescriptor:descriptor
					options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&error];
				if (state)
					statistics_add(&metal.statistics.pipelines_from_archive, 1);
				descriptor.binaryArchives = nil;
				error = nil;
			}
			if (!state)
				state = [metal.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
			if (!state)
				metal_log(HOST_LOG_ERROR, @"cannot make a pipeline: %@", error.localizedDescription);
		}
		os_unfair_lock_lock(&metal.lock);
		[metal.pipelines_making removeObject:data];
		if (state)
		{
			metal.pipelines[data] = state;
			if (xgpu_metal_pipeline_archivable(&copy))
				[metal.archived addObject:[descriptor copy]];
			metal.archive_changed = YES;
		}
		os_unfair_lock_unlock(&metal.lock);
		pthread_mutex_lock(&metal.statistics_lock);
		metal.statistics.pipelines_made++;
		metal.statistics.pipeline_seconds += seconds_now() - start;
		metal.compile_totals.pipelines++;
		metal.compile_totals.pipeline_seconds += seconds_now() - start;
		pthread_mutex_unlock(&metal.statistics_lock);
		if (group)
			dispatch_group_leave(group);
	});
}

/* a draw's pipeline state: made (by the loading screen's preparation, or
an earlier draw); else made on the making queue and waited for (logged and
counted: one the loading screen did not prepare), as the picture must be
whole: a draw is never skipped. During a preparation (draws unseen), only
asked for. Never compiled on this thread. */
static id<MTLRenderPipelineState> pipeline_state_get(const struct xgpu_metal_pipeline *pipeline)
{
	struct xgpu_metal_pipeline_state_key key;
	NSData *data;
	id<MTLRenderPipelineState> state;
	dispatch_group_t made;

	xgpu_metal_pipeline_state_key_make(pipeline, &key);
	if (state = metal.pipeline_state, state && !memcmp(&key, &metal.pipeline_state_key, sizeof(key)))
		return state;
	data = [NSData dataWithBytes:&key length:sizeof(key)];
	os_unfair_lock_lock(&metal.lock);
	state = metal.pipelines[data];
	os_unfair_lock_unlock(&metal.lock);
	if (!state && metal.preparing_draws)
	{
		pipeline_make_later(&key, metal.preparing);
		return nil;
	}
	if (!state)
	{
		if (!metal.options.synchronous)
		{
			statistics_add(&metal.statistics.draws_waiting, 1);
			host_logf(HOST_LOG_WARN, "metal: a pipeline made while drawing (not prepared as the map loaded)");
		}
		made = dispatch_group_create();
		pipeline_make_later(&key, made);
		dispatch_group_wait(made, DISPATCH_TIME_FOREVER);
		os_unfair_lock_lock(&metal.lock);
		state = metal.pipelines[data];
		os_unfair_lock_unlock(&metal.lock);
	}
	if (state)
		metal.pipeline_state_key = key;
	return state;
}

void xgpu_metal_prepare(const struct xgpu_metal_pipeline *pipeline)
{
	struct xgpu_metal_pipeline_state_key key;

	xgpu_metal_pipeline_state_key_make(pipeline, &key);
	pipeline_make_later(&key, metal.preparing);
}

void xgpu_metal_prepare_begin(void)
{
	metal.preparing_draws = YES;
}

double xgpu_metal_prepared(void)
{
	double start = seconds_now();

	metal.preparing_draws = NO;
	dispatch_group_wait(metal.preparing, DISPATCH_TIME_FOREVER);
	/* (a safe point: the user's archive written with the map's pipelines,
	in the background) */
	archive_save(NO);
	return seconds_now() - start;
}

static id<MTLDepthStencilState> depth_stencil_state_get(const struct xgpu_metal_pipeline *pipeline)
{
	struct depth_stencil_key key;
	NSData *data;
	id<MTLDepthStencilState> state;
	MTLDepthStencilDescriptor *descriptor;

	memset(&key, 0, sizeof(key));
	if (pipeline->depth_format)
	{
		key.depth_test = pipeline->depth_test;
		key.depth_function = pipeline->depth_test ? pipeline->depth_function : XGPU_COMPARE_ALWAYS;
		key.depth_write = pipeline->depth_write;
		if (format_get(pipeline->depth_format)->stencil && pipeline->stencil_test)
		{
			key.stencil_test = 1;
			key.stencil_function = pipeline->stencil_function;
			key.stencil_fail = pipeline->stencil_fail;
			key.stencil_depth_fail = pipeline->stencil_depth_fail;
			key.stencil_pass = pipeline->stencil_pass;
			key.stencil_read_mask = pipeline->stencil_read_mask;
			key.stencil_write_mask = pipeline->stencil_write_mask;
		}
	}
	data = [NSData dataWithBytes:&key length:sizeof(key)];
	state = metal.depth_stencil_states[data];
	if (state)
		return state;
	descriptor = [MTLDepthStencilDescriptor new];
	descriptor.depthCompareFunction = key.depth_test ? xgpu_metal_compare_function(key.depth_function) : MTLCompareFunctionAlways;
	descriptor.depthWriteEnabled = key.depth_write != 0;
	if (key.stencil_test)
	{
		MTLStencilDescriptor *stencil = [MTLStencilDescriptor new];

		stencil.stencilCompareFunction = xgpu_metal_compare_function(key.stencil_function);
		stencil.stencilFailureOperation = xgpu_metal_stencil_operation(key.stencil_fail);
		stencil.depthFailureOperation = xgpu_metal_stencil_operation(key.stencil_depth_fail);
		stencil.depthStencilPassOperation = xgpu_metal_stencil_operation(key.stencil_pass);
		stencil.readMask = key.stencil_read_mask;
		stencil.writeMask = key.stencil_write_mask;
		descriptor.frontFaceStencil = stencil;
		descriptor.backFaceStencil = stencil;
	}
	state = [metal.device newDepthStencilStateWithDescriptor:descriptor];
	metal.depth_stencil_states[data] = state;
	statistics_add(&metal.statistics.depth_stencil_states_made, 1);
	return state;
}

void xgpu_metal_set_dynamic_state(const struct xgpu_metal_dynamic_state *state)
{
	if (memcmp(metal.dynamic.viewport, state->viewport, sizeof(state->viewport)) ||
		memcmp(metal.dynamic.depth_range, state->depth_range, sizeof(state->depth_range)))
		metal.dirty |= _dirty_viewport;
	if (memcmp(metal.dynamic.scissor, state->scissor, sizeof(state->scissor)))
		metal.dirty |= _dirty_scissor;
	if (metal.dynamic.stencil_reference != state->stencil_reference)
		metal.dirty |= _dirty_stencil_reference;
	if (memcmp(metal.dynamic.blend_color, state->blend_color, sizeof(state->blend_color)))
		metal.dirty |= _dirty_blend_color;
	if (memcmp(metal.dynamic.depth_bias, state->depth_bias, sizeof(state->depth_bias)))
		metal.dirty |= _dirty_raster;
	metal.dynamic = *state;
}

/* ---------- bindings */

static id<MTLSamplerState> sampler_get(const struct xgpu_metal_sampler *sampler)
{
	static const MTLSamplerAddressMode address_modes[5] = {
		MTLSamplerAddressModeRepeat, MTLSamplerAddressModeMirrorRepeat, MTLSamplerAddressModeClampToEdge,
		MTLSamplerAddressModeClampToBorderColor, MTLSamplerAddressModeMirrorClampToEdge,
	};
	static const MTLSamplerBorderColor border_colors[3] = {
		MTLSamplerBorderColorTransparentBlack, MTLSamplerBorderColorOpaqueBlack, MTLSamplerBorderColorOpaqueWhite,
	};
	NSData *data = [NSData dataWithBytes:sampler length:sizeof(*sampler)];
	id<MTLSamplerState> state = metal.samplers[data];
	MTLSamplerDescriptor *descriptor;

	if (state)
		return state;
	descriptor = [MTLSamplerDescriptor new];
	descriptor.minFilter = sampler->min_filter ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
	descriptor.magFilter = sampler->mag_filter ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
	descriptor.mipFilter = sampler->mip_filter == XGPU_METAL_MIP_LINEAR ? MTLSamplerMipFilterLinear :
		sampler->mip_filter == XGPU_METAL_MIP_POINT ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterNotMipmapped;
	descriptor.sAddressMode = address_modes[sampler->address_u < 5 ? sampler->address_u : 0];
	descriptor.tAddressMode = address_modes[sampler->address_v < 5 ? sampler->address_v : 0];
	descriptor.rAddressMode = address_modes[sampler->address_w < 5 ? sampler->address_w : 0];
	descriptor.borderColor = border_colors[sampler->border < 3 ? sampler->border : 0];
	descriptor.maxAnisotropy = sampler->max_anisotropy > 1 ? (sampler->max_anisotropy > 16 ? 16 : sampler->max_anisotropy) : 1;
	descriptor.lodMinClamp = sampler->min_lod;
	descriptor.lodMaxClamp = sampler->max_lod > 0.0f ? sampler->max_lod : FLT_MAX;
	if (sampler->compare)
		descriptor.compareFunction = xgpu_metal_compare_function(sampler->compare_function);
	descriptor.supportArgumentBuffers = NO;
	state = [metal.device newSamplerStateWithDescriptor:descriptor];
	metal.samplers[data] = state;
	statistics_add(&metal.statistics.samplers_made, 1);
	return state;
}

void xgpu_metal_set_texture(uint32_t stage, uint32_t texture, const struct xgpu_metal_sampler *sampler)
{
	if (stage >= XGPU_METAL_TEXTURE_STAGE_COUNT)
		return;
	if (metal.stage_textures[stage] != texture)
	{
		texture_contents(texture_get(texture));
		metal.stage_textures[stage] = texture;
		metal.dirty |= _dirty_textures;
	}
	if (sampler && (!metal.stage_sampler_states[stage] || memcmp(&metal.stage_samplers[stage], sampler, sizeof(*sampler))))
	{
		metal.stage_samplers[stage] = *sampler;
		metal.stage_sampler_states[stage] = sampler_get(sampler);
		metal.dirty |= _dirty_textures;
	}
}

void xgpu_metal_set_vertex_constants(uint32_t first, uint32_t count, const float *values)
{
	if (first >= XGPU_METAL_VERTEX_CONSTANT_COUNT)
		return;
	if (count > XGPU_METAL_VERTEX_CONSTANT_COUNT - first)
		count = XGPU_METAL_VERTEX_CONSTANT_COUNT - first;
	memcpy(metal.vertex_constants[first], values, count * 4 * sizeof(float));
	metal.dirty |= _dirty_vertex_constants;
}

void xgpu_metal_set_draw_uniforms(const void *data, uint32_t size)
{
	if (size > sizeof(metal.draw_uniforms))
		size = sizeof(metal.draw_uniforms);
	if (size == metal.draw_uniforms_size && !memcmp(metal.draw_uniforms, data, size))
		return;
	memcpy(metal.draw_uniforms, data, size);
	metal.draw_uniforms_size = size;
	metal.dirty |= _dirty_draw_uniforms;
}

void xgpu_metal_set_stream(uint32_t stream, struct xgpu_metal_source source)
{
	if (stream >= XGPU_METAL_STREAM_COUNT)
		return;
	metal.streams[stream] = source;
	metal.dirty |= _dirty_streams;
}

void xgpu_metal_set_constant_attributes(const float (*values)[4])
{
	if (!memcmp(metal.constant_attributes, values, sizeof(metal.constant_attributes)))
		return;
	memcpy(metal.constant_attributes, values, sizeof(metal.constant_attributes));
	metal.dirty |= _dirty_constant_attributes;
}

/* ---------- draws */

static const MTLPrimitiveType primitive_types[5] = {
	MTLPrimitiveTypePoint, MTLPrimitiveTypeLine, MTLPrimitiveTypeLineStrip, MTLPrimitiveTypeTriangle,
	MTLPrimitiveTypeTriangleStrip,
};

/* sets on the encoder what the draw needs of the state; NO if it cannot
be drawn */
static BOOL draw_state_apply(id<MTLRenderCommandEncoder> render)
{
	uint32_t dirty = metal.dirty;
	uint64_t serial = metal.command_serial;
	int index;

	if (dirty & _dirty_pipeline)
	{
		id<MTLRenderPipelineState> state = pipeline_state_get(&metal.pipeline);

		if (!state)
			return NO;
		metal.pipeline_state = state;
		[render setRenderPipelineState:state];
	}
	if (dirty & _dirty_depth_stencil)
		[render setDepthStencilState:depth_stencil_state_get(&metal.pipeline)];
	if (dirty & _dirty_raster)
	{
		const struct xgpu_metal_pipeline *pipeline = &metal.pipeline;

		[render setCullMode:!pipeline->cull ? MTLCullModeNone : pipeline->cull_front ? MTLCullModeFront : MTLCullModeBack];
		[render setFrontFacingWinding:pipeline->front_counter_clockwise ? MTLWindingCounterClockwise : MTLWindingClockwise];
		/* (Metal draws no points for a polygon: lines) */
		[render setTriangleFillMode:pipeline->fill == XGPU_FILL_SOLID ? MTLTriangleFillModeFill : MTLTriangleFillModeLines];
		if (pipeline->depth_bias)
			[render setDepthBias:metal.dynamic.depth_bias[1] slopeScale:metal.dynamic.depth_bias[0] clamp:0.0f];
		else
			[render setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
		[render setDepthClipMode:pipeline->depth_clamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
	}
	if (dirty & _dirty_viewport)
	{
		const int32_t *viewport = metal.dynamic.viewport;

		[render setViewport:(MTLViewport){ viewport[0], viewport[1], viewport[2], viewport[3],
			metal.dynamic.depth_range[0], metal.dynamic.depth_range[1] }];
	}
	if (dirty & _dirty_scissor)
	{
		/* (inside the targets, as Metal requires) */
		int32_t left = metal.dynamic.scissor[0] > 0 ? metal.dynamic.scissor[0] : 0;
		int32_t top = metal.dynamic.scissor[1] > 0 ? metal.dynamic.scissor[1] : 0;
		int32_t right = metal.dynamic.scissor[0] + metal.dynamic.scissor[2];
		int32_t bottom = metal.dynamic.scissor[1] + metal.dynamic.scissor[3];

		if (!metal.dynamic.scissor[2] && !metal.dynamic.scissor[3])
		{
			right = (int32_t)metal.target_width;
			bottom = (int32_t)metal.target_height;
		}
		if (right > (int32_t)metal.target_width)
			right = (int32_t)metal.target_width;
		if (bottom > (int32_t)metal.target_height)
			bottom = (int32_t)metal.target_height;
		if (left > right)
			left = right;
		if (top > bottom)
			top = bottom;
		[render setScissorRect:(MTLScissorRect){ (NSUInteger)left, (NSUInteger)top, (NSUInteger)(right - left),
			(NSUInteger)(bottom - top) }];
	}
	if (dirty & _dirty_stencil_reference)
		[render setStencilReferenceValue:metal.dynamic.stencil_reference & 0xff];
	if (dirty & _dirty_blend_color)
		[render setBlendColorRed:metal.dynamic.blend_color[0] green:metal.dynamic.blend_color[1]
			blue:metal.dynamic.blend_color[2] alpha:metal.dynamic.blend_color[3]];
	if (dirty & _dirty_vertex_constants)
		[render setVertexBytes:metal.vertex_constants length:sizeof(metal.vertex_constants)
			atIndex:XGPU_METAL_BUFFER_VERTEX_CONSTANTS];
	if (dirty & _dirty_constant_attributes)
		[render setVertexBytes:metal.constant_attributes length:sizeof(metal.constant_attributes)
			atIndex:XGPU_METAL_BUFFER_CONSTANT_ATTRIBUTES];
	if ((dirty & _dirty_draw_uniforms) && metal.draw_uniforms_size)
	{
		[render setVertexBytes:metal.draw_uniforms length:metal.draw_uniforms_size atIndex:XGPU_METAL_BUFFER_DRAW_UNIFORMS];
		[render setFragmentBytes:metal.draw_uniforms length:metal.draw_uniforms_size atIndex:XGPU_METAL_BUFFER_DRAW_UNIFORMS];
	}
	if (dirty & _dirty_textures)
	{
		for (index = 0; index < XGPU_METAL_TEXTURE_STAGE_COUNT; index++)
		{
			struct texture_entry *entry = texture_get(metal.stage_textures[index]);

			[render setFragmentTexture:entry ? entry->texture : nil atIndex:XGPU_METAL_TEXTURE_STAGE(index)];
			/* (and at its type's place, as the combiner's function reads it:
			xgpu_metal_combiner.h) */
			if (entry && entry->texture.textureType != MTLTextureType2D)
			{
				NSUInteger place = entry->texture.textureType == MTLTextureType3D ? XGPU_METAL_COMBINER_TEXTURE_3D :
					entry->texture.textureType == MTLTextureTypeCube ? XGPU_METAL_COMBINER_TEXTURE_CUBE :
					XGPU_METAL_COMBINER_TEXTURE_2D_ARRAY;

				[render setFragmentTexture:entry->texture atIndex:place + (NSUInteger)index];
			}
			if (metal.stage_sampler_states[index])
				[render setFragmentSamplerState:metal.stage_sampler_states[index] atIndex:XGPU_METAL_SAMPLER_STAGE(index)];
		}
	}
	/* (each draw: the textures it reads are in use until its command buffer
	is done) */
	for (index = 0; index < XGPU_METAL_TEXTURE_STAGE_COUNT; index++)
	{
		struct texture_entry *entry = texture_get(metal.stage_textures[index]);

		if (entry)
			entry->used = serial;
	}
	if (dirty & _dirty_streams)
	{
		uint32_t used = 0;

		for (index = 0; index < 16; index++)
		{
			if (metal.pipeline.attributes[index].format != XGPU_METAL_ATTRIBUTE_NONE &&
				metal.pipeline.attributes[index].stream < XGPU_METAL_STREAM_COUNT)
				used |= 1u << metal.pipeline.attributes[index].stream;
		}
		for (index = 0; index < XGPU_METAL_STREAM_COUNT; index++)
		{
			NSUInteger offset = 0;
			id<MTLBuffer> buffer;

			if (!(used & (1u << index)))
				continue;
			buffer = source_buffer(metal.streams[index], &offset);
			if (!buffer)
				return NO;
			[render setVertexBuffer:buffer offset:offset atIndex:XGPU_METAL_BUFFER_STREAM(index)];
		}
	}
	metal.dirty = 0;
	return YES;
}

/* (HALO_METAL_PASSES_SPLIT) */
static void profile_draw_describe(struct profile_pass *pass, uint32_t primitive, uint32_t count)
{
	size_t used;
	int index;
	id<MTLFunction> vertex = shader_function(metal.pipeline.vertex_shader);
	id<MTLFunction> pixel = shader_function(metal.pipeline.pixel_shader);

	used = (size_t)snprintf(pass->detail, sizeof(pass->detail),
		"%s %s prim %u x%u blend %u(%u,%u) mask %x depth %u/%u stencil %u viewport %dx%d scissor %dx%d",
		vertex ? vertex.name.UTF8String : "-", pixel ? pixel.name.UTF8String : "-", primitive, count,
		metal.pipeline.blend, metal.pipeline.blend_source, metal.pipeline.blend_destination, metal.pipeline.color_mask,
		metal.pipeline.depth_test, metal.pipeline.depth_write, metal.pipeline.stencil_test, metal.dynamic.viewport[2],
		metal.dynamic.viewport[3], metal.dynamic.scissor[2], metal.dynamic.scissor[3]);
	for (index = 0; index < XGPU_METAL_TEXTURE_STAGE_COUNT && used < sizeof(pass->detail); index++)
	{
		struct texture_entry *entry = texture_get(metal.stage_textures[index]);
		const struct xgpu_metal_sampler *sampler = &metal.stage_samplers[index];

		if (!entry)
			continue;
		used += (size_t)snprintf(pass->detail + used, sizeof(pass->detail) - used, " t%d %ux%u f%u l%u (%u%u%u a%u)",
			index, entry->description.width, entry->description.height, entry->description.format,
			(unsigned)entry->texture.mipmapLevelCount, sampler->min_filter, sampler->mag_filter, sampler->mip_filter,
			sampler->max_anisotropy);
	}
}

void xgpu_metal_draw(uint32_t primitive, uint32_t count, uint32_t first_vertex, int32_t base_vertex,
	struct xgpu_metal_source indices)
{
	static BOOL refused;
	id<MTLRenderCommandEncoder> render;

	if (!count || !metal.have_pipeline)
		return;
	if (primitive >= sizeof(primitive_types) / sizeof(primitive_types[0]))
	{
		if (!refused)
			host_logf(HOST_LOG_ERROR, "metal: primitive %u has no Metal equivalent (the front end converts fans and loops)",
				primitive);
		refused = YES;
		return;
	}
	render = encoder();
	if (!render || !draw_state_apply(render))
	{
		/* (the state not applied is applied at the next draw) */
		metal.dirty |= _dirty_all;
		return;
	}
	/* (a visibility test's entry in this pass, its counting begun once the
	draw's state is set) */
	if (metal.visibility_pending)
	{
		visibility_entry_begin();
		metal.visibility_pending = NO;
	}
	if (indices.kind != XGPU_METAL_SOURCE_NONE)
	{
		NSUInteger offset = 0;
		id<MTLBuffer> buffer = source_buffer(indices, &offset);

		if (!buffer)
			return;
		[render drawIndexedPrimitives:primitive_types[primitive] indexCount:count indexType:MTLIndexTypeUInt16
			indexBuffer:buffer indexBufferOffset:offset instanceCount:1 baseVertex:base_vertex baseInstance:0];
	}
	else
	{
		[render drawPrimitives:primitive_types[primitive] vertexStart:first_vertex vertexCount:count];
	}
	statistics_add(&metal.statistics.draws, 1);
	if (profile.current >= 0)
	{
		profile.passes[profile.current].draws++;
		if (profile.split)
		{
			profile_draw_describe(&profile.passes[profile.current], primitive, count);
			encoder_end();
			metal.dirty = _dirty_all;
		}
	}
}

/* ---------- visibility tests */

void xgpu_metal_visibility_begin(void)
{
	if (metal.visibility_active)
		xgpu_metal_visibility_end(0, 1.0f);
	metal.visibility_active = YES;
	metal.visibility_entry = -1;
	metal.visibility_first = metal.slot->visibility_entries;
	/* (each pass counts into an entry of its own, begun at the pass's
	first draw of the test) */
	metal.visibility_pending = YES;
}

void xgpu_metal_visibility_end(uint32_t slot, float area)
{
	uint32_t entry;

	if (!metal.visibility_active)
		return;
	if (slot < XGPU_METAL_VISIBILITY_SLOTS)
	{
		pthread_mutex_lock(&visibility.lock);
		visibility.areas[slot] = area;
		pthread_mutex_unlock(&visibility.lock);
	}
	if (metal.encoder && metal.visibility_entry >= 0)
		[metal.encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
	for (entry = metal.visibility_first; entry < metal.slot->visibility_entries; entry++)
		metal.slot->visibility_slots[entry] = (uint16_t)(slot < XGPU_METAL_VISIBILITY_SLOTS ? slot : 0);
	metal.visibility_active = NO;
	metal.visibility_entry = -1;
	metal.visibility_pending = NO;
}

void xgpu_metal_set_visibility_output(volatile uint32_t *counts)
{
	pthread_mutex_lock(&visibility.lock);
	metal.visibility_output = counts;
	pthread_mutex_unlock(&visibility.lock);
}

/* ---------- reading pixels */

int xgpu_metal_read_pixels(uint32_t texture, uint32_t level, uint32_t layer, uint32_t x, uint32_t y,
	uint32_t width, uint32_t height, void *pixels, uint32_t bytes_per_row)
{
	struct texture_entry *entry = texture_get(texture);
	const struct xgpu_metal_format_information *format;
	id<MTLBuffer> buffer;
	id<MTLBlitCommandEncoder> blit;
	uint32_t row_size, row;
	uint64_t serial;
	BOOL depth;

	if (!entry || !width || !height)
		return 0;
	texture_contents(entry);
	format = format_get(entry->description.format);
	if (format->compressed)
		return 0;
	depth = format->depth;
	/* (depth as 4-byte floats: the depth plane of D24S8; D16 as it is) */
	row_size = width * (depth && format->stencil ? 4 : format->bytes);
	buffer = [metal.device newBufferWithLength:(NSUInteger)row_size * height options:MTLResourceStorageModeShared];
	pass_end();
	blit = blit_encoder("read");
	[blit copyFromTexture:entry->texture sourceSlice:layer sourceLevel:level sourceOrigin:MTLOriginMake(x, y, 0)
		sourceSize:MTLSizeMake(width, height, 1) toBuffer:buffer destinationOffset:0
		destinationBytesPerRow:row_size destinationBytesPerImage:(NSUInteger)row_size * height
		options:depth && format->stencil ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
	[blit endEncoding];
	entry->used = recording_serial();
	/* (the frame goes on in a command buffer of its own) */
	serial = command_buffer_commit(nil);
	xgpu_metal_frame_wait(serial);
	for (row = 0; row < height; row++)
		memcpy((unsigned char *)pixels + (size_t)row * bytes_per_row, (const unsigned char *)buffer.contents + (size_t)row * row_size,
			row_size);
	metal.dirty = _dirty_all;
	return 1;
}
