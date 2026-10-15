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

Passes. set_targets ends the render command encoder; the next draw or clear
begins one, with load actions "load", or "clear" for an attachment that a
clear of the whole of it came first for. Any other clear is a quad drawn
through a clear pipeline. Uploads into a texture the GPU may still read are
blits in the command buffer's order (they end the encoder; the pass goes on
with "load"); into one it cannot, they write the texture at once.

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

#include <errno.h>
#include <float.h>
#include <pthread.h>
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

/* ---------- formats */

struct format_information
{
	MTLPixelFormat pixel_format;
	/* bytes a pixel, or a block's (compressed: 4x4 pixels) */
	uint8_t bytes;
	uint8_t compressed;
	uint8_t depth, stencil;
	MTLTextureSwizzleChannels swizzle;
	uint8_t swizzled;
};

#define SWIZZLE(r, g, b, a) { MTLTextureSwizzle##r, MTLTextureSwizzle##g, MTLTextureSwizzle##b, MTLTextureSwizzle##a }

static const struct format_information formats[XGPU_METAL_FORMAT_COUNT] = {
	[XGPU_METAL_FORMAT_NONE] = { MTLPixelFormatInvalid, 0, 0, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_BGRA8] = { MTLPixelFormatBGRA8Unorm, 4, 0, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_BGRX8] = { MTLPixelFormatBGRA8Unorm, 4, 0, 0, 0, SWIZZLE(Red, Green, Blue, One), 1 },
	/* (Metal names packed formats from the low bits up: B5G6R5 has red in
	the top 5 bits, as D3D's R5G6B5) */
	[XGPU_METAL_FORMAT_R5G6B5] = { MTLPixelFormatB5G6R5Unorm, 2, 0, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_A1R5G5B5] = { MTLPixelFormatBGR5A1Unorm, 2, 0, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_X1R5G5B5] = { MTLPixelFormatBGR5A1Unorm, 2, 0, 0, 0, SWIZZLE(Red, Green, Blue, One), 1 },
	/* A4R4G4B4 (alpha in the top 4 bits) is read through ABGR4, whose red
	is the top 4 bits: D3D's alpha, red, green, blue are its red, green,
	blue, alpha */
	[XGPU_METAL_FORMAT_A4R4G4B4] = { MTLPixelFormatABGR4Unorm, 2, 0, 0, 0, SWIZZLE(Green, Blue, Alpha, Red), 1 },
	[XGPU_METAL_FORMAT_L8] = { MTLPixelFormatR8Unorm, 1, 0, 0, 0, SWIZZLE(Red, Red, Red, One), 1 },
	[XGPU_METAL_FORMAT_A8] = { MTLPixelFormatA8Unorm, 1, 0, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	/* A8L8: luminance in the low byte (red), alpha in the high (green) */
	[XGPU_METAL_FORMAT_A8L8] = { MTLPixelFormatRG8Unorm, 2, 0, 0, 0, SWIZZLE(Red, Red, Red, Green), 1 },
	[XGPU_METAL_FORMAT_DXT1] = { MTLPixelFormatBC1_RGBA, 8, 1, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_DXT3] = { MTLPixelFormatBC2_RGBA, 16, 1, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_DXT5] = { MTLPixelFormatBC3_RGBA, 16, 1, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_D24S8] = { MTLPixelFormatDepth32Float_Stencil8, 8, 0, 1, 1, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_D16] = { MTLPixelFormatDepth16Unorm, 2, 0, 1, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_RGBA8] = { MTLPixelFormatRGBA8Unorm, 4, 0, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_COVERAGE] = { MTLPixelFormatR8Unorm, 1, 0, 0, 0, SWIZZLE(One, One, One, Red), 1 },
};

static const struct format_information *format_get(uint32_t format)
{
	return format < XGPU_METAL_FORMAT_COUNT ? &formats[format] : &formats[XGPU_METAL_FORMAT_NONE];
}

/* ---------- the interface's values in Metal's */

static const MTLCompareFunction compare_functions[8] = {
	MTLCompareFunctionNever, MTLCompareFunctionLess, MTLCompareFunctionEqual, MTLCompareFunctionLessEqual,
	MTLCompareFunctionGreater, MTLCompareFunctionNotEqual, MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways,
};

static const MTLBlendFactor blend_factors[15] = {
	MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendFactorSourceColor, MTLBlendFactorOneMinusSourceColor,
	MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha, MTLBlendFactorDestinationAlpha,
	MTLBlendFactorOneMinusDestinationAlpha, MTLBlendFactorDestinationColor, MTLBlendFactorOneMinusDestinationColor,
	MTLBlendFactorSourceAlphaSaturated, MTLBlendFactorBlendColor, MTLBlendFactorOneMinusBlendColor,
	MTLBlendFactorBlendAlpha, MTLBlendFactorOneMinusBlendAlpha,
};

static const MTLBlendOperation blend_operations[5] = {
	MTLBlendOperationAdd, MTLBlendOperationSubtract, MTLBlendOperationReverseSubtract, MTLBlendOperationMin,
	MTLBlendOperationMax,
};

static const MTLStencilOperation stencil_operations[8] = {
	MTLStencilOperationKeep, MTLStencilOperationZero, MTLStencilOperationReplace,
	MTLStencilOperationIncrementClamp, MTLStencilOperationDecrementClamp, MTLStencilOperationInvert,
	MTLStencilOperationIncrementWrap, MTLStencilOperationDecrementWrap,
};

static MTLCompareFunction compare_function(uint32_t value)
{
	return value < 8 ? compare_functions[value] : MTLCompareFunctionAlways;
}

static MTLBlendFactor blend_factor(uint32_t value)
{
	return value < 15 ? blend_factors[value] : MTLBlendFactorOne;
}

static MTLStencilOperation stencil_operation(uint32_t value)
{
	return value < 8 ? stencil_operations[value] : MTLStencilOperationKeep;
}

static MTLVertexFormat vertex_format(uint32_t format)
{
	static const MTLVertexFormat vertex_formats[] = {
		[XGPU_METAL_ATTRIBUTE_FLOAT1] = MTLVertexFormatFloat,
		[XGPU_METAL_ATTRIBUTE_FLOAT2] = MTLVertexFormatFloat2,
		[XGPU_METAL_ATTRIBUTE_FLOAT3] = MTLVertexFormatFloat3,
		[XGPU_METAL_ATTRIBUTE_FLOAT4] = MTLVertexFormatFloat4,
		[XGPU_METAL_ATTRIBUTE_D3DCOLOR] = MTLVertexFormatUChar4Normalized_BGRA,
		[XGPU_METAL_ATTRIBUTE_SHORT1] = MTLVertexFormatShort,
		[XGPU_METAL_ATTRIBUTE_SHORT2] = MTLVertexFormatShort2,
		[XGPU_METAL_ATTRIBUTE_SHORT3] = MTLVertexFormatShort3,
		[XGPU_METAL_ATTRIBUTE_SHORT4] = MTLVertexFormatShort4,
		[XGPU_METAL_ATTRIBUTE_NORMSHORT1] = MTLVertexFormatShortNormalized,
		[XGPU_METAL_ATTRIBUTE_NORMSHORT2] = MTLVertexFormatShort2Normalized,
		[XGPU_METAL_ATTRIBUTE_NORMSHORT3] = MTLVertexFormatShort3Normalized,
		[XGPU_METAL_ATTRIBUTE_NORMSHORT4] = MTLVertexFormatShort4Normalized,
		[XGPU_METAL_ATTRIBUTE_PBYTE1] = MTLVertexFormatUCharNormalized,
		[XGPU_METAL_ATTRIBUTE_PBYTE2] = MTLVertexFormatUChar2Normalized,
		[XGPU_METAL_ATTRIBUTE_PBYTE3] = MTLVertexFormatUChar3Normalized,
		[XGPU_METAL_ATTRIBUTE_PBYTE4] = MTLVertexFormatUChar4Normalized,
		[XGPU_METAL_ATTRIBUTE_FLOAT2H] = MTLVertexFormatFloat3,
		[XGPU_METAL_ATTRIBUTE_PACKED] = MTLVertexFormatUInt,
	};

	return format < sizeof(vertex_formats) / sizeof(vertex_formats[0]) ? vertex_formats[format] : MTLVertexFormatInvalid;
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
	uint8_t present;
};

struct shader_entry
{
	id<MTLFunction> function;
};

/* what a render pipeline state is made from: the pipeline's shaders,
layout, blend and targets (the rest is depth-stencil or encoder state) */
struct pipeline_state_key
{
	uint32_t vertex_shader, pixel_shader;
	struct xgpu_metal_vertex_attribute attributes[16];
	uint16_t strides[XGPU_METAL_STREAM_COUNT];
	uint8_t blend, blend_source, blend_destination, blend_operation, color_mask;
	uint8_t color_format, depth_format, samples;
};

struct depth_stencil_key
{
	uint8_t depth_test, depth_function, depth_write;
	uint8_t stencil_test, stencil_function, stencil_fail, stencil_depth_fail, stencil_pass;
	uint8_t stencil_read_mask, stencil_write_mask;
};

struct clear_key
{
	uint8_t color_format, depth_format, samples, parts;
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
	NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *pipelines;
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
	struct pipeline_state_key pipeline_state_key;
	struct xgpu_metal_dynamic_state dynamic;
	uint32_t stage_textures[XGPU_METAL_TEXTURE_STAGE_COUNT];
	struct xgpu_metal_sampler stage_samplers[XGPU_METAL_TEXTURE_STAGE_COUNT];
	id<MTLSamplerState> stage_sampler_states[XGPU_METAL_TEXTURE_STAGE_COUNT];
	float vertex_constants[XGPU_METAL_VERTEX_CONSTANT_COUNT][4];
	unsigned char draw_uniforms[4096];
	uint32_t draw_uniforms_size;
	struct xgpu_metal_source streams[XGPU_METAL_STREAM_COUNT];
	float constant_attributes[16][4];

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

void xgpu_metal_frame_wait(uint64_t fence)
{
	pthread_mutex_lock(&metal.completed_lock);
	while (metal.completed < fence)
		pthread_cond_wait(&metal.completed_condition, &metal.completed_lock);
	pthread_mutex_unlock(&metal.completed_lock);
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

static void encoder_end(void)
{
	if (metal.encoder)
	{
		if (metal.visibility_active && metal.visibility_entry >= 0)
			[metal.encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
		[metal.encoder endEncoding];
		metal.encoder = nil;
		metal.visibility_entry = -1;
	}
}

static void pass_end(void);

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
			pthread_mutex_lock(&metal.statistics_lock);
			metal.statistics.gpu_seconds += done.GPUEndTime - done.GPUStartTime;
			metal.statistics.command_buffers++;
			pthread_mutex_unlock(&metal.statistics_lock);
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

static NSString *const clear_source =
	@"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct clear_parameters { float4 rectangle; float4 color; float depth; };\n"
	"struct clear_vertex { float4 position [[position]]; };\n"
	"vertex clear_vertex xgpu_clear_vertex(uint id [[vertex_id]], constant clear_parameters &p [[buffer(0)]])\n"
	"{\n"
	"\tclear_vertex out;\n"
	"\tfloat2 corner = float2((id & 1) ? p.rectangle.z : p.rectangle.x, (id & 2) ? p.rectangle.w : p.rectangle.y);\n"
	"\tout.position = float4(corner, p.depth, 1.0);\n"
	"\treturn out;\n"
	"}\n"
	"fragment float4 xgpu_clear_fragment(constant clear_parameters &p [[buffer(0)]])\n"
	"{\n"
	"\treturn p.color;\n"
	"}\n";

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
	metal.archive_changed = NO;
	pipelines = [metal.archived copy];
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
	metal.shader_handles = [NSMutableDictionary new];
	metal.pipelines = [NSMutableDictionary new];
	metal.depth_stencil_states = [NSMutableDictionary new];
	metal.samplers = [NSMutableDictionary new];
	metal.clear_pipelines = [NSMutableDictionary new];
	metal.texture_search = 1;
	metal.buffers = [NSMutableArray new];
	metal.buffers_used = [NSMutableArray new];
	metal.visibility_entry = -1;
	archive_open();
	metal.archived = [NSMutableArray new];
	metal.archive_queue = dispatch_queue_create("OpenCE pipeline archive", DISPATCH_QUEUE_SERIAL);

	library = [metal.device newLibraryWithSource:clear_source options:nil error:&error];
	if (!library)
	{
		metal_log(HOST_LOG_ERROR, @"the clear shaders: %@", error.localizedDescription);
		return 0;
	}
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

void xgpu_metal_statistics(struct xgpu_metal_statistics *statistics)
{
	pthread_mutex_lock(&metal.statistics_lock);
	*statistics = metal.statistics;
	memset(&metal.statistics, 0, sizeof(metal.statistics));
	pthread_mutex_unlock(&metal.statistics_lock);
}

/* ---------- shaders */

uint32_t xgpu_metal_shader_create(const char *source, size_t size, const char *entry)
{
	NSString *text = [[NSString alloc] initWithBytes:source length:size encoding:NSUTF8StringEncoding];
	NSString *key, *name = @(entry);
	NSNumber *existing;
	MTLCompileOptions *options;
	id<MTLLibrary> library;
	id<MTLFunction> function;
	NSError *error = nil;

	if (!text || metal.shader_count + 1 >= SHADER_MAXIMUM)
		return 0;
	key = [NSString stringWithFormat:@"%@\n%@", name, text];
	existing = metal.shader_handles[key];
	if (existing)
		return existing.unsignedIntValue;
	options = [MTLCompileOptions new];
	/* (no fast maths: the GLSL emitted for the other backends has none) */
	if (@available(macOS 15.0, *))
		options.mathMode = MTLMathModeSafe;
	else
		options.fastMathEnabled = NO;
	library = [metal.device newLibraryWithSource:text options:options error:&error];
	function = library ? [library newFunctionWithName:name] : nil;
	if (!function)
	{
		metal_log(HOST_LOG_ERROR, @"cannot compile %@: %@", name, error ? error.localizedDescription : @"no such function");
		return 0;
	}
	metal.shader_count++;
	metal.shaders[metal.shader_count].function = function;
	metal.shader_handles[key] = @(metal.shader_count);
	return metal.shader_count;
}

static id<MTLFunction> shader_function(uint32_t shader)
{
	return shader && shader <= metal.shader_count ? metal.shaders[shader].function : nil;
}

/* ---------- textures */

static struct texture_entry *texture_get(uint32_t texture)
{
	return texture && texture < TEXTURE_MAXIMUM && metal.textures[texture].present ? &metal.textures[texture] : NULL;
}

id<MTLTexture> xgpu_metal_texture(uint32_t texture)
{
	struct texture_entry *entry = texture_get(texture);

	return entry ? entry->texture : nil;
}

uint32_t xgpu_metal_texture_create(const struct xgpu_metal_texture_description *description)
{
	const struct format_information *format = format_get(description->format);
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
	entry->present = 1;
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
	entry->present = 0;
	for (stage = 0; stage < XGPU_METAL_TEXTURE_STAGE_COUNT; stage++)
	{
		if (metal.stage_textures[stage] == texture)
			metal.stage_textures[stage] = 0;
	}
}

static void statistics_add(uint32_t *counter, uint32_t amount);

void xgpu_metal_texture_make_levels(uint32_t texture)
{
	struct texture_entry *entry = texture_get(texture);
	id<MTLBlitCommandEncoder> blit;

	if (!entry || entry->texture.mipmapLevelCount < 2)
		return;
	if (!metal.in_frame)
		xgpu_metal_frame_begin();
	pass_end();
	blit = [command_buffer() blitCommandEncoder];
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
	width = from->texture.width < to->texture.width ? from->texture.width : to->texture.width;
	height = from->texture.height < to->texture.height ? from->texture.height : to->texture.height;
	if (!metal.in_frame)
		xgpu_metal_frame_begin();
	pass_end();
	blit = [command_buffer() blitCommandEncoder];
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

/* (the ring: below) */
static id<MTLBuffer> ring_buffer(struct xgpu_metal_source source, NSUInteger *offset);

void xgpu_metal_texture_upload(uint32_t texture, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t z,
	uint32_t width, uint32_t height, uint32_t depth, const void *data, uint32_t bytes_per_row,
	uint32_t bytes_per_image)
{
	struct texture_entry *entry = texture_get(texture);
	const struct format_information *format;
	MTLRegion region;
	uint32_t rows, size;

	if (!entry || !width || !height)
		return;
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
		blit = [command_buffer() blitCommandEncoder];
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
	blit = [command_buffer() blitCommandEncoder];
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

	if (!metal.in_frame)
		xgpu_metal_frame_begin();
	pass_end();
	slot = metal.slot;
	entries = slot->visibility_entries;
	number = slot->number;
	if (picture && host_metal_window_active())
	{
		/* the picture, copied into one of the presentation's images (the
		next frame may draw into the texture before the display shows
		this one) */
		id<MTLBlitCommandEncoder> blit;

		image = host_metal_present_acquire(picture->description.width, picture->description.height);
		blit = [command_buffer() blitCommandEncoder];
		[blit copyFromTexture:picture->texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
			sourceSize:MTLSizeMake(picture->description.width, picture->description.height, 1) toTexture:image
			destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];
		picture->used = recording_serial();
	}
	slot->fence = command_buffer_commit(entries ? ^{ visibility_publish(slot, entries, number); } : nil);
	if (image)
		host_metal_present_queue(image);
	metal.in_frame = NO;
	/* (the game ends the process without warning: the archive is saved
	once no pipeline has been added for a few seconds) */
	if (metal.archive_changed && seconds_now() - metal.archive_changed_at > 3.0)
		archive_save(NO);
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
	/* (the same targets: the pass goes on) */
	if (metal.encoder && metal.color == (color_entry ? color : 0) && metal.color_level == color_level &&
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
		const struct format_information *format = format_get(depth->description.format);
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
	struct clear_key key = { 0 };
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
	descriptor = [MTLRenderPipelineDescriptor new];
	descriptor.vertexFunction = metal.clear_vertex;
	descriptor.fragmentFunction = metal.clear_fragment;
	descriptor.rasterSampleCount = key.samples;
	if (color)
	{
		descriptor.colorAttachments[0].pixelFormat = format_get(key.color_format)->pixel_format;
		descriptor.colorAttachments[0].writeMask = ((key.parts & XGPU_METAL_CLEAR_RED) ? MTLColorWriteMaskRed : 0) |
			((key.parts & XGPU_METAL_CLEAR_GREEN) ? MTLColorWriteMaskGreen : 0) |
			((key.parts & XGPU_METAL_CLEAR_BLUE) ? MTLColorWriteMaskBlue : 0) |
			((key.parts & XGPU_METAL_CLEAR_ALPHA) ? MTLColorWriteMaskAlpha : 0);
	}
	if (depth)
	{
		descriptor.depthAttachmentPixelFormat = format_get(key.depth_format)->pixel_format;
		if (format_get(key.depth_format)->stencil)
			descriptor.stencilAttachmentPixelFormat = descriptor.depthAttachmentPixelFormat;
	}
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

static id<MTLRenderPipelineState> pipeline_state_get(const struct xgpu_metal_pipeline *pipeline)
{
	struct pipeline_state_key key;
	NSData *data;
	id<MTLRenderPipelineState> state;
	MTLRenderPipelineDescriptor *descriptor;
	MTLVertexDescriptor *vertex;
	NSError *error = nil;
	double start;
	int index;

	memset(&key, 0, sizeof(key));
	key.vertex_shader = pipeline->vertex_shader;
	key.pixel_shader = pipeline->pixel_shader;
	memcpy(key.attributes, pipeline->attributes, sizeof(key.attributes));
	for (index = 0; index < 16; index++)
	{
		if (key.attributes[index].format != XGPU_METAL_ATTRIBUTE_NONE &&
			key.attributes[index].stream < XGPU_METAL_STREAM_COUNT)
			key.strides[key.attributes[index].stream] = pipeline->strides[key.attributes[index].stream];
	}
	key.blend = pipeline->blend;
	if (key.blend)
	{
		key.blend_source = pipeline->blend_source;
		key.blend_destination = pipeline->blend_destination;
		key.blend_operation = pipeline->blend_operation;
	}
	key.color_mask = pipeline->color_mask;
	key.color_format = pipeline->color_format;
	key.depth_format = pipeline->depth_format;
	key.samples = pipeline->samples > 1 ? pipeline->samples : 1;
	if (state = metal.pipeline_state, state && !memcmp(&key, &metal.pipeline_state_key, sizeof(key)))
		return state;
	data = [NSData dataWithBytes:&key length:sizeof(key)];
	state = metal.pipelines[data];
	if (state)
	{
		metal.pipeline_state_key = key;
		return state;
	}

	start = seconds_now();
	descriptor = [MTLRenderPipelineDescriptor new];
	descriptor.vertexFunction = shader_function(key.vertex_shader);
	descriptor.fragmentFunction = shader_function(key.pixel_shader);
	if (!descriptor.vertexFunction)
		return nil;
	vertex = [MTLVertexDescriptor vertexDescriptor];
	for (index = 0; index < 16; index++)
	{
		const struct xgpu_metal_vertex_attribute *attribute = &key.attributes[index];

		if (attribute->format == XGPU_METAL_ATTRIBUTE_NONE)
			continue;
		if (attribute->stream == XGPU_METAL_STREAM_CONSTANT)
		{
			/* (its register's value, the same for every vertex) */
			vertex.attributes[index].format = MTLVertexFormatFloat4;
			vertex.attributes[index].offset = (NSUInteger)index * 16;
			vertex.attributes[index].bufferIndex = XGPU_METAL_BUFFER_CONSTANT_ATTRIBUTES;
			vertex.layouts[XGPU_METAL_BUFFER_CONSTANT_ATTRIBUTES].stride = 16 * 16;
			vertex.layouts[XGPU_METAL_BUFFER_CONSTANT_ATTRIBUTES].stepFunction = MTLVertexStepFunctionConstant;
			vertex.layouts[XGPU_METAL_BUFFER_CONSTANT_ATTRIBUTES].stepRate = 0;
			continue;
		}
		if (attribute->stream >= XGPU_METAL_STREAM_COUNT)
			continue;
		vertex.attributes[index].format = vertex_format(attribute->format);
		vertex.attributes[index].offset = attribute->offset;
		vertex.attributes[index].bufferIndex = XGPU_METAL_BUFFER_STREAM(attribute->stream);
		vertex.layouts[XGPU_METAL_BUFFER_STREAM(attribute->stream)].stride =
			key.strides[attribute->stream] ? key.strides[attribute->stream] : 4;
	}
	descriptor.vertexDescriptor = vertex;
	descriptor.rasterSampleCount = key.samples;
	if (key.color_format)
	{
		MTLRenderPipelineColorAttachmentDescriptor *attachment = descriptor.colorAttachments[0];

		attachment.pixelFormat = format_get(key.color_format)->pixel_format;
		attachment.writeMask = ((key.color_mask & XGPU_WRITE_RED) ? MTLColorWriteMaskRed : 0) |
			((key.color_mask & XGPU_WRITE_GREEN) ? MTLColorWriteMaskGreen : 0) |
			((key.color_mask & XGPU_WRITE_BLUE) ? MTLColorWriteMaskBlue : 0) |
			((key.color_mask & XGPU_WRITE_ALPHA) ? MTLColorWriteMaskAlpha : 0);
		attachment.blendingEnabled = key.blend != 0;
		attachment.sourceRGBBlendFactor = attachment.sourceAlphaBlendFactor = blend_factor(key.blend_source);
		attachment.destinationRGBBlendFactor = attachment.destinationAlphaBlendFactor =
			blend_factor(key.blend_destination);
		attachment.rgbBlendOperation = attachment.alphaBlendOperation =
			key.blend_operation < 5 ? blend_operations[key.blend_operation] : MTLBlendOperationAdd;
	}
	if (key.depth_format)
	{
		descriptor.depthAttachmentPixelFormat = format_get(key.depth_format)->pixel_format;
		if (format_get(key.depth_format)->stencil)
			descriptor.stencilAttachmentPixelFormat = descriptor.depthAttachmentPixelFormat;
	}
	/* from the archive where it has the pipeline's binaries; else made,
	and the archive written again with it */
	if (metal.archive)
	{
		descriptor.binaryArchives = @[metal.archive];
		state = [metal.device newRenderPipelineStateWithDescriptor:descriptor
			options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&error];
		if (state)
			statistics_add(&metal.statistics.pipelines_from_archive, 1);
		error = nil;
	}
	if (!state)
	{
		state = [metal.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
		if (state)
		{
			metal.archive_changed = YES;
			metal.archive_changed_at = seconds_now();
		}
	}
	if (!state)
	{
		metal_log(HOST_LOG_ERROR, @"cannot make a pipeline: %@", error.localizedDescription);
		return nil;
	}
	/* (not one that writes no colour, which Metal makes without its
	fragment stage: the archiver cannot write it) */
	if (key.color_format && key.color_mask)
		[metal.archived addObject:[descriptor copy]];
	metal.pipelines[data] = state;
	metal.pipeline_state_key = key;
	pthread_mutex_lock(&metal.statistics_lock);
	metal.statistics.pipelines_made++;
	metal.statistics.pipeline_seconds += seconds_now() - start;
	pthread_mutex_unlock(&metal.statistics_lock);
	return state;
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
	descriptor.depthCompareFunction = key.depth_test ? compare_function(key.depth_function) : MTLCompareFunctionAlways;
	descriptor.depthWriteEnabled = key.depth_write != 0;
	if (key.stencil_test)
	{
		MTLStencilDescriptor *stencil = [MTLStencilDescriptor new];

		stencil.stencilCompareFunction = compare_function(key.stencil_function);
		stencil.stencilFailureOperation = stencil_operation(key.stencil_fail);
		stencil.depthFailureOperation = stencil_operation(key.stencil_depth_fail);
		stencil.depthStencilPassOperation = stencil_operation(key.stencil_pass);
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
		descriptor.compareFunction = compare_function(sampler->compare_function);
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
	const struct format_information *format;
	id<MTLBuffer> buffer;
	id<MTLBlitCommandEncoder> blit;
	uint32_t row_size, row;
	uint64_t serial;
	BOOL depth;

	if (!entry || !width || !height)
		return 0;
	format = format_get(entry->description.format);
	if (format->compressed)
		return 0;
	depth = format->depth;
	/* (depth as 4-byte floats: the depth plane of D24S8; D16 as it is) */
	row_size = width * (depth && format->stencil ? 4 : format->bytes);
	buffer = [metal.device newBufferWithLength:(NSUInteger)row_size * height options:MTLResourceStorageModeShared];
	pass_end();
	blit = [command_buffer() blitCommandEncoder];
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
