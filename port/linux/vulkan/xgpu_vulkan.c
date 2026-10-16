/*
XGPU_VULKAN.C

The Vulkan backend's core (xgpu_vulkan.h): the device and its memory, the
resources the command stream's draws need, and the commands that draw them.

Recording: the work of a submission is two command buffers, submitted
together. The second (main) has the draws, the clears and what is ordered
between them; the first (early) has the uploads into images and buffers that
no work recorded so far in this submission has used, which can then be done
before all of it, without ending a pass for them. A barrier begins the early
work (after everything submitted before it) and ends it (before everything
after it).

Every image is made in the GENERAL layout and stays in it, sampled, drawn
into and copied; the barrier at the end of each pass and after each copy
makes what it wrote visible to everything after it.

Submissions are numbered by a timeline semaphore. What a submission uses
(its command buffers and descriptor pool, its bytes of the upload ring, the
images and buffers let go of while it was recorded) is reused or freed once
the GPU has passed its number. A frame's visibility tests are read from its
query pool once its last submission is done.
*/

#include "xgpu_vulkan.h"
#include "host.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- the functions (loaded at run time) */

#define GLOBAL_FUNCTIONS(F) \
	F(vkCreateInstance) F(vkEnumerateInstanceExtensionProperties) F(vkEnumerateInstanceLayerProperties) \
	F(vkEnumerateInstanceVersion)

#define INSTANCE_FUNCTIONS(F) \
	F(vkEnumeratePhysicalDevices) F(vkGetPhysicalDeviceProperties) \
	F(vkGetPhysicalDeviceFeatures2) F(vkGetPhysicalDeviceQueueFamilyProperties) \
	F(vkGetPhysicalDeviceProperties2) F(vkGetPhysicalDeviceMemoryProperties) F(vkGetPhysicalDeviceFormatProperties) F(vkCreateDevice) \
	F(vkGetDeviceProcAddr) F(vkEnumerateDeviceExtensionProperties)

#define SURFACE_FUNCTIONS(F) \
	F(vkGetPhysicalDeviceSurfaceSupportKHR) F(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
	F(vkGetPhysicalDeviceSurfaceFormatsKHR) F(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define DEBUG_FUNCTIONS(F) \
	F(vkCreateDebugUtilsMessengerEXT)

#define DEVICE_FUNCTIONS(F) \
	F(vkGetDeviceQueue) F(vkDeviceWaitIdle) F(vkQueueSubmit) \
	F(vkCreateCommandPool) F(vkResetCommandPool) F(vkAllocateCommandBuffers) \
	F(vkBeginCommandBuffer) F(vkEndCommandBuffer) F(vkAllocateMemory) F(vkFreeMemory) F(vkMapMemory) \
	F(vkCreateBuffer) F(vkDestroyBuffer) F(vkGetBufferMemoryRequirements) F(vkBindBufferMemory) \
	F(vkCreateImage) F(vkDestroyImage) F(vkGetImageMemoryRequirements) F(vkBindImageMemory) \
	F(vkCreateImageView) F(vkDestroyImageView) F(vkCreateSampler) \
	F(vkCreateShaderModule) F(vkCreatePipelineCache) \
	F(vkGetPipelineCacheData) F(vkCreateGraphicsPipelines) F(vkCreatePipelineLayout) \
	F(vkCreateDescriptorSetLayout) \
	F(vkCreateDescriptorPool) F(vkResetDescriptorPool) F(vkAllocateDescriptorSets) \
	F(vkUpdateDescriptorSets) F(vkCreateSemaphore) F(vkDestroySemaphore) F(vkWaitSemaphores) \
	F(vkGetSemaphoreCounterValue) F(vkCreateQueryPool) F(vkResetQueryPool) \
	F(vkGetQueryPoolResults) \
	F(vkCmdPipelineBarrier) F(vkCmdBeginRendering) F(vkCmdEndRendering) F(vkCmdBindPipeline) F(vkCmdSetViewport) \
	F(vkCmdSetScissor) F(vkCmdSetDepthBias) F(vkCmdSetBlendConstants) F(vkCmdSetStencilReference) \
	F(vkCmdSetPrimitiveTopology) F(vkCmdBindVertexBuffers) F(vkCmdBindIndexBuffer) F(vkCmdBindDescriptorSets) \
	F(vkCmdDraw) F(vkCmdDrawIndexed) F(vkCmdClearAttachments) F(vkCmdCopyBuffer) F(vkCmdCopyBufferToImage) \
	F(vkCmdCopyImageToBuffer) F(vkCmdCopyImage) F(vkCmdBlitImage) F(vkCmdResolveImage) F(vkCmdClearColorImage) \
	F(vkCmdClearDepthStencilImage) F(vkCmdBeginQuery) F(vkCmdEndQuery)

#define SWAPCHAIN_FUNCTIONS(F) \
	F(vkCreateSwapchainKHR) F(vkDestroySwapchainKHR) F(vkGetSwapchainImagesKHR) F(vkAcquireNextImageKHR) \
	F(vkQueuePresentKHR)

#define DECLARE(name) static PFN_##name name;
GLOBAL_FUNCTIONS(DECLARE)
INSTANCE_FUNCTIONS(DECLARE)
SURFACE_FUNCTIONS(DECLARE)
DEBUG_FUNCTIONS(DECLARE)
DEVICE_FUNCTIONS(DECLARE)
SWAPCHAIN_FUNCTIONS(DECLARE)
#undef DECLARE
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;

/* ---------- sizes */

/* submissions whose resources are kept until the GPU is done with them */
#define SLOTS 8
/* the upload ring */
#define RING_SIZE (128u * 1024 * 1024)
/* images' memory, in blocks of this (larger images have their own) */
#define BLOCK_SIZE (64u * 1024 * 1024)
#define SAMPLER_CACHE 1024
#define PIPELINE_BUCKETS 4096
#define SHADER_BUCKETS 1024
/* frames whose visibility tests may be in flight, and the queries a frame
has (a test can be several, one for each pass it spans) */
#define QUERY_FRAMES 4
#define QUERIES_A_FRAME 8192
/* texture descriptor sets a submission's pool has, before another pool */
#define SETS_A_POOL 1024

/* ---------- memory */

struct free_range
{
	VkDeviceSize offset, size;
};

struct block
{
	VkDeviceMemory memory;
	VkDeviceSize size;
	uint32_t type;
	struct free_range *free;
	uint32_t free_count, free_capacity;
	struct block *next;
};

struct allocation
{
	struct block *block;
	VkDeviceMemory memory;
	VkDeviceSize offset, size;
	/* a dedicated allocation (block NULL): its memory is its own */
};

/* ---------- resources */

struct xgpu_vulkan_image
{
	struct xgpu_vulkan_image_description description;
	VkImage image;
	VkFormat format;
	VkImageAspectFlags aspect;
	struct allocation memory;
	/* sampled (colour images; NULL for a depth image), and drawn into, a
	layer each (made at the first draw into the layer) */
	VkImageView sampled;
	VkImageView *attachments;
	uint32_t attachment_count;
	/* the submission whose recording last used it (draws, copies) */
	uint64_t used;
};

struct xgpu_vulkan_buffer
{
	VkBuffer buffer;
	struct allocation memory;
	void *mapped;
	uint32_t size;
	uint64_t used;
};

/* things let go of, freed once the GPU has passed their submission */
enum
{
	_garbage_image, _garbage_buffer, _garbage_view,
};

struct garbage
{
	int kind;
	uint64_t serial;
	VkImage image;
	VkImageView view;
	VkBuffer buffer;
	struct allocation memory;
};

/* a submission's own */
struct slot
{
	uint64_t serial;
	VkCommandPool pool;
	VkCommandBuffer main, early;
	VkDescriptorPool *descriptor_pools;
	uint32_t descriptor_pool_count, descriptor_pool_used;
	/* the upload ring's position its uploads end at */
	uint64_t ring_end;
};

/* a frame's visibility tests: its queries, and the slot and area of each */
struct query_frame
{
	VkQueryPool pool;
	uint32_t count;
	uint16_t slots[QUERIES_A_FRAME];
	float areas[QUERIES_A_FRAME];
	/* the test each query is of (a test of several passes: several) */
	uint32_t tests[QUERIES_A_FRAME];
	/* the slots of tests that drew in no pass (a count of 0) */
	uint16_t empty[QUERIES_A_FRAME];
	uint32_t empty_count;
	/* the submission that ends the frame (0: the frame is being recorded),
	and the frame's number */
	uint64_t serial;
	uint64_t frame;
	int in_use;
};

struct pipeline_entry
{
	struct pipeline_entry *next;
	uint32_t hash;
	struct xgpu_vulkan_pipeline_description description;
	VkPipeline pipeline;
};

struct shader_entry
{
	struct shader_entry *next;
	uint64_t hash;
	size_t count;
	uint32_t *words;
	VkShaderModule module;
};

static struct
{
	void *library;
	VkInstance instance;
	VkDebugUtilsMessengerEXT messenger;
	VkPhysicalDevice physical;
	VkPhysicalDeviceProperties properties;
	VkPhysicalDeviceMemoryProperties memory_properties;
	VkDevice device;
	VkQueue queue;
	uint32_t queue_family;
	int validation;
	uint32_t validation_messages;

	struct xgpu_vulkan_capabilities capabilities;
	VkFormat depth_format;
	/* NVIDIA's own driver (its anisotropic filtering: anisotropic_bias) */
	int nvidia;
	/* cube maps sampled face by face (VK_EXT_non_seamless_cube_map) */
	int non_seamless;

	/* memory blocks for images, by memory type */
	struct block *blocks;

	/* the timeline: the next submission's number, the last done */
	VkSemaphore timeline;
	uint64_t serial;
	uint64_t completed;
	struct slot slots[SLOTS];
	struct slot *slot;
	int recording, early_recording;

	/* the upload ring: its buffer, mapped, and the running count of bytes
	given out (its position: modulo its size) */
	VkBuffer ring;
	struct allocation ring_memory;
	unsigned char *ring_mapped;
	uint64_t ring_head, ring_free_from;

	struct garbage *garbage;
	uint32_t garbage_count, garbage_capacity;

	/* the readback buffer (read_pixels) */
	VkBuffer readback;
	struct allocation readback_memory;
	unsigned char *readback_mapped;
	VkDeviceSize readback_size;

	VkDescriptorSetLayout draw_layout, texture_layout;
	VkPipelineLayout pipeline_layout;
	VkDescriptorPool draw_pool;
	VkDescriptorSet draw_set;
	/* the draw set's other resources: the enhanced dynamic lights' block
	and cells, and the visibility counters (unused by the flat game) */
	struct xgpu_vulkan_buffer *lights_block, *samples;
	struct xgpu_vulkan_image *lights_cells;
	VkSampler stand_in_sampler;
	struct xgpu_vulkan_image *stand_ins[4];

	VkPipelineCache pipeline_cache;
	char cache_path[1024];
	struct pipeline_entry *pipelines[PIPELINE_BUCKETS];
	struct shader_entry *shaders[SHADER_BUCKETS];
	struct
	{
		struct xgpu_vulkan_sampler_description description;
		VkSampler sampler;
	} samplers[SAMPLER_CACHE];
	uint32_t sampler_count;

	/* the targets bound, and the pass drawing into them */
	struct xgpu_vulkan_image *color, *depth;
	uint32_t layer;
	int pass_active;
	uint32_t pass_width, pass_height;
	/* what the main command buffer has bound in this pass */
	VkPipeline bound_pipeline;
	VkDescriptorSet bound_textures;
	struct xgpu_vulkan_texture last_textures[4];
	VkDescriptorSet last_texture_set;
	int last_texture_valid;

	/* visibility tests; the frames ended */
	volatile uint32_t *visibility_output;
	uint64_t frame;
	struct query_frame query_frames[QUERY_FRAMES];
	struct query_frame *queries;
	int test_active;
	/* the current test's queries (indices in queries), and the one begun */
	uint32_t test_queries[64];
	uint32_t test_query_count;
	int test_query_open;
	uint32_t test_number;

	/* the window */
	VkSurfaceKHR surface;
	VkSwapchainKHR swapchain;
	VkFormat swapchain_format;
	VkExtent2D swapchain_extent;
	VkImage swapchain_images[8];
	uint32_t swapchain_image_count;
	VkSemaphore acquired[SLOTS], presentable[8];
	int swapchain_stale;
	VkPresentModeKHR present_mode;
	volatile uint32_t window_width, window_height;

	struct xgpu_vulkan_statistics statistics;
} vk;

/* ---------- logging */

static const char *result_name(VkResult result)
{
	switch (result)
	{
	case VK_SUCCESS: return "VK_SUCCESS";
	case VK_NOT_READY: return "VK_NOT_READY";
	case VK_TIMEOUT: return "VK_TIMEOUT";
	case VK_INCOMPLETE: return "VK_INCOMPLETE";
	case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
	case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
	case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
	case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
	case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
	case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
	case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
	case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
	case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
	case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
	case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
	default: return "VkResult";
	}
}

/* a result checked: 1 if it succeeded, else logged (what failed) */
static int check(VkResult result, const char *what)
{
	if (result >= 0)
		return 1;
	host_logf(HOST_LOG_ERROR, "vulkan: %s: %s (%d)", what, result_name(result), (int)result);
	if (result == VK_ERROR_DEVICE_LOST)
		host_fatal("vulkan: the device was lost");
	return 0;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL validation_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT *data, void *context)
{
	(void)types;
	(void)context;
	/* (any thread: counted roughly) */
	if (vk.validation_messages++ < 1000)
		host_logf(severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? HOST_LOG_ERROR : HOST_LOG_WARN,
			"vulkan validation: %s", data->pMessage ? data->pMessage : "(no message)");
	return VK_FALSE;
}

static double seconds_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec * 1e-9;
}

/* ---------- memory */

static int memory_type(uint32_t bits, VkMemoryPropertyFlags wanted, VkMemoryPropertyFlags avoided)
{
	uint32_t index;

	for (index = 0; index < vk.memory_properties.memoryTypeCount; index++)
	{
		VkMemoryPropertyFlags flags = vk.memory_properties.memoryTypes[index].propertyFlags;

		if ((bits & (1u << index)) && (flags & wanted) == wanted && !(flags & avoided))
			return (int)index;
	}
	for (index = 0; index < vk.memory_properties.memoryTypeCount; index++)
	{
		VkMemoryPropertyFlags flags = vk.memory_properties.memoryTypes[index].propertyFlags;

		if ((bits & (1u << index)) && (flags & wanted) == wanted)
			return (int)index;
	}
	return -1;
}

static int memory_dedicated(VkDeviceSize size, uint32_t type, struct allocation *allocation)
{
	VkMemoryAllocateInfo info;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	info.allocationSize = size;
	info.memoryTypeIndex = type;
	memset(allocation, 0, sizeof(*allocation));
	if (!check(vkAllocateMemory(vk.device, &info, NULL, &allocation->memory), "vkAllocateMemory"))
		return 0;
	allocation->size = size;
	return 1;
}

static void block_free_insert(struct block *block, VkDeviceSize offset, VkDeviceSize size)
{
	uint32_t at = 0, index;

	while (at < block->free_count && block->free[at].offset < offset)
		at++;
	/* (merged with its neighbours where they touch) */
	if (at > 0 && block->free[at - 1].offset + block->free[at - 1].size == offset)
	{
		block->free[at - 1].size += size;
		if (at < block->free_count && block->free[at - 1].offset + block->free[at - 1].size == block->free[at].offset)
		{
			block->free[at - 1].size += block->free[at].size;
			memmove(&block->free[at], &block->free[at + 1], (block->free_count - at - 1) * sizeof(block->free[0]));
			block->free_count--;
		}
		return;
	}
	if (at < block->free_count && offset + size == block->free[at].offset)
	{
		block->free[at].offset = offset;
		block->free[at].size += size;
		return;
	}
	if (block->free_count == block->free_capacity)
	{
		uint32_t capacity = block->free_capacity ? block->free_capacity * 2 : 64;
		struct free_range *grown = realloc(block->free, capacity * sizeof(*grown));

		if (!grown)
			return;
		block->free = grown;
		block->free_capacity = capacity;
	}
	for (index = block->free_count; index > at; index--)
		block->free[index] = block->free[index - 1];
	block->free[at].offset = offset;
	block->free[at].size = size;
	block->free_count++;
}

/* memory for an image's requirements, of device-local memory */
static int memory_for(const VkMemoryRequirements *requirements, struct allocation *allocation)
{
	int type = memory_type(requirements->memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
	struct block *block;

	if (type < 0)
		return 0;
	if (requirements->size > BLOCK_SIZE / 4)
		return memory_dedicated(requirements->size, (uint32_t)type, allocation);
	for (;;)
	{
		for (block = vk.blocks; block; block = block->next)
		{
			uint32_t index;

			if (block->type != (uint32_t)type)
				continue;
			for (index = 0; index < block->free_count; index++)
			{
				struct free_range *range = &block->free[index];
				VkDeviceSize start = (range->offset + requirements->alignment - 1) & ~(requirements->alignment - 1);

				if (start + requirements->size > range->offset + range->size)
					continue;
				allocation->block = block;
				allocation->memory = block->memory;
				allocation->offset = start;
				allocation->size = requirements->size;
				{
					VkDeviceSize before = start - range->offset;
					VkDeviceSize after = range->offset + range->size - (start + requirements->size);
					VkDeviceSize after_offset = start + requirements->size;

					/* (the range becomes what is left before and after it) */
					memmove(&block->free[index], &block->free[index + 1],
						(block->free_count - index - 1) * sizeof(block->free[0]));
					block->free_count--;
					if (before)
						block_free_insert(block, start - before, before);
					if (after)
						block_free_insert(block, after_offset, after);
				}
				return 1;
			}
		}
		block = calloc(1, sizeof(*block));
		if (!block)
			return 0;
		{
			struct allocation whole;

			if (!memory_dedicated(BLOCK_SIZE, (uint32_t)type, &whole))
			{
				free(block);
				return 0;
			}
			block->memory = whole.memory;
		}
		block->size = BLOCK_SIZE;
		block->type = (uint32_t)type;
		block_free_insert(block, 0, BLOCK_SIZE);
		block->next = vk.blocks;
		vk.blocks = block;
	}
}

static void memory_release(struct allocation *allocation)
{
	if (allocation->block)
		block_free_insert(allocation->block, allocation->offset, allocation->size);
	else if (allocation->memory)
		vkFreeMemory(vk.device, allocation->memory, NULL);
	memset(allocation, 0, sizeof(*allocation));
}

/* a buffer of its own memory, host-visible (mapped) or device-local */
static int buffer_make(VkDeviceSize size, VkBufferUsageFlags usage, int host, VkBuffer *buffer,
	struct allocation *memory, void **mapped)
{
	VkBufferCreateInfo info;
	VkMemoryRequirements requirements;
	int type;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = size;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (!check(vkCreateBuffer(vk.device, &info, NULL, buffer), "vkCreateBuffer"))
		return 0;
	vkGetBufferMemoryRequirements(vk.device, *buffer, &requirements);
	type = host ? memory_type(requirements.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0) :
		memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
	if (type < 0 || !memory_dedicated(requirements.size, (uint32_t)type, memory))
	{
		vkDestroyBuffer(vk.device, *buffer, NULL);
		*buffer = VK_NULL_HANDLE;
		return 0;
	}
	if (!check(vkBindBufferMemory(vk.device, *buffer, memory->memory, 0), "vkBindBufferMemory") ||
		(host && !check(vkMapMemory(vk.device, memory->memory, 0, VK_WHOLE_SIZE, 0, mapped), "vkMapMemory")))
	{
		vkDestroyBuffer(vk.device, *buffer, NULL);
		memory_release(memory);
		*buffer = VK_NULL_HANDLE;
		return 0;
	}
	return 1;
}

/* ---------- the timeline */

static uint64_t completed_serial(void)
{
	uint64_t value = vk.completed;

	if (vkGetSemaphoreCounterValue(vk.device, vk.timeline, &value) == VK_SUCCESS && value > vk.completed)
		vk.completed = value;
	return vk.completed;
}

static void garbage_collect(void);
static void ring_reclaim(void);

/* waits until the GPU has passed serial */
static void serial_wait(uint64_t serial)
{
	VkSemaphoreWaitInfo info;

	if (serial <= completed_serial())
		return;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
	info.semaphoreCount = 1;
	info.pSemaphores = &vk.timeline;
	info.pValues = &serial;
	check(vkWaitSemaphores(vk.device, &info, UINT64_MAX), "vkWaitSemaphores");
	completed_serial();
	garbage_collect();
	ring_reclaim();
}

/* ---------- garbage */

static void garbage_add(const struct garbage *item)
{
	if (vk.garbage_count == vk.garbage_capacity)
	{
		uint32_t capacity = vk.garbage_capacity ? vk.garbage_capacity * 2 : 256;
		struct garbage *grown = realloc(vk.garbage, capacity * sizeof(*grown));

		if (!grown)
			return;
		vk.garbage = grown;
		vk.garbage_capacity = capacity;
	}
	vk.garbage[vk.garbage_count++] = *item;
}

static void garbage_free(struct garbage *item)
{
	switch (item->kind)
	{
	case _garbage_image:
		vkDestroyImage(vk.device, item->image, NULL);
		memory_release(&item->memory);
		break;
	case _garbage_buffer:
		vkDestroyBuffer(vk.device, item->buffer, NULL);
		memory_release(&item->memory);
		break;
	default:
		vkDestroyImageView(vk.device, item->view, NULL);
		break;
	}
}

static void garbage_collect(void)
{
	uint64_t done = vk.completed;
	uint32_t index = 0;

	while (index < vk.garbage_count)
	{
		if (vk.garbage[index].serial <= done)
		{
			garbage_free(&vk.garbage[index]);
			vk.garbage[index] = vk.garbage[--vk.garbage_count];
		}
		else
		{
			index++;
		}
	}
}

/* ---------- recording */

static void barrier(VkCommandBuffer commands, VkPipelineStageFlags source_stages, VkAccessFlags source_access,
	VkPipelineStageFlags destination_stages, VkAccessFlags destination_access)
{
	VkMemoryBarrier memory;

	memset(&memory, 0, sizeof(memory));
	memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory.srcAccessMask = source_access;
	memory.dstAccessMask = destination_access;
	vkCmdPipelineBarrier(commands, source_stages, destination_stages, 0, 1, &memory, 0, NULL, 0, NULL);
}

/* everything a draw or a copy after it can do with what was written before */
#define AFTER_STAGES (VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | \
	VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | \
	VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT)
#define AFTER_ACCESS (VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | \
	VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | \
	VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | \
	VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT)

/* the current submission's slot, its command buffers begun: its earlier use
done first */
static void recording_begin(void)
{
	struct slot *slot;
	VkCommandBufferBeginInfo begin;

	if (vk.recording)
		return;
	slot = &vk.slots[vk.serial % SLOTS];
	if (slot->serial)
		serial_wait(slot->serial);
	vk.slot = slot;
	slot->serial = vk.serial;
	slot->ring_end = 0;
	vkResetCommandPool(vk.device, slot->pool, 0);
	if (slot->descriptor_pool_count)
	{
		uint32_t index;

		for (index = 0; index < slot->descriptor_pool_count; index++)
			vkResetDescriptorPool(vk.device, slot->descriptor_pools[index], 0);
	}
	slot->descriptor_pool_used = 0;
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	check(vkBeginCommandBuffer(slot->main, &begin), "vkBeginCommandBuffer");
	vk.recording = 1;
	vk.early_recording = 0;
	vk.bound_pipeline = VK_NULL_HANDLE;
	vk.last_texture_valid = 0;
}

/* the early command buffer, begun, after everything submitted before */
static VkCommandBuffer early_commands(void)
{
	recording_begin();
	if (!vk.early_recording)
	{
		VkCommandBufferBeginInfo begin;

		memset(&begin, 0, sizeof(begin));
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		check(vkBeginCommandBuffer(vk.slot->early, &begin), "vkBeginCommandBuffer");
		barrier(vk.slot->early, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
		vk.early_recording = 1;
	}
	return vk.slot->early;
}

static void pass_end(void);

/* the main command buffer, outside a pass */
static VkCommandBuffer main_commands(void)
{
	recording_begin();
	pass_end();
	return vk.slot->main;
}

/* ---------- the upload ring */

/* the start of size bytes at the ring's head, aligned, wrapped to the
ring's beginning rather than across its end */
static uint64_t ring_start(uint32_t size, uint32_t alignment)
{
	uint64_t start = (vk.ring_head + alignment - 1) & ~(uint64_t)(alignment - 1);

	if (start / RING_SIZE != (start + size - 1) / RING_SIZE)
		start = (start + size - 1) / RING_SIZE * RING_SIZE;
	return start;
}

void *xgpu_vulkan_upload(uint32_t size, uint32_t alignment, struct xgpu_vulkan_range *range)
{
	uint64_t start;

	if (!size || size > RING_SIZE / 4)
		return NULL;
	recording_begin();
	start = ring_start(size, alignment);
	/* the bytes the GPU may still read: wait for the submissions that read
	them, or (all of them are this recording's) submit it first */
	while (start + size - vk.ring_free_from > RING_SIZE)
	{
		uint32_t index;
		uint64_t oldest = 0;

		for (index = 0; index < SLOTS; index++)
		{
			struct slot *slot = &vk.slots[index];

			if (slot->serial && slot->serial < vk.serial && slot->serial > vk.completed &&
				(!oldest || slot->serial < oldest))
				oldest = slot->serial;
		}
		if (oldest)
		{
			serial_wait(oldest);
			continue;
		}
		xgpu_vulkan_wait(xgpu_vulkan_fence());
		recording_begin();
		start = ring_start(size, alignment);
		if (start + size - vk.ring_free_from > RING_SIZE)
			return NULL;
	}
	vk.ring_head = start + size;
	vk.statistics.ring_bytes += size;
	range->buffer = vk.ring;
	range->offset = start % RING_SIZE;
	return vk.ring_mapped + range->offset;
}

/* ---------- queries */

/* The visibility tests' counts reach the game as the OpenGL backend's do:
the counts of a frame's tests are copied at the next frame's end (by the GPU,
after that frame's work) and read at the end of the frame after it
(xgpu_gl.c, visibility_copy_batch, frame_end_buffers), so that the game,
drawing frame k + 1, has frame k - 2's. Here they are written into the
game's slots at the end of frame k (the frame's queries waited for, which
they have long been): with the guest waiting for each frame's GPU work (as
a test of the same frames does), the game has the same counts in the same
frames as with OpenGL. */
static void queries_publish(uint64_t last)
{
	/* the latest test of each slot, and its count so far */
	static uint64_t sums[XGPU_VULKAN_VISIBILITY_SLOTS];
	static float areas[XGPU_VULKAN_VISIBILITY_SLOTS];
	static uint32_t tests[XGPU_VULKAN_VISIBILITY_SLOTS];
	static uint8_t seen[XGPU_VULKAN_VISIBILITY_SLOTS];

	for (;;)
	{
		struct query_frame *frame = NULL;
		uint64_t *results = NULL;
		uint32_t query;
		int index;

		/* (the oldest frame's first: a slot's latest count wins) */
		for (index = 0; index < QUERY_FRAMES; index++)
		{
			struct query_frame *candidate = &vk.query_frames[index];

			if (candidate->in_use && candidate->serial && candidate->frame <= last &&
				(!frame || candidate->frame < frame->frame))
				frame = candidate;
		}
		if (!frame)
			return;
		serial_wait(frame->serial);
		memset(seen, 0, sizeof(seen));
		/* (in the order the tests were made: a slot tested twice in a frame
		has the later's count, a test of several queries their sum) */
		for (query = 0; query < frame->empty_count; query++)
		{
			uint16_t slot = frame->empty[query];

			if (slot < XGPU_VULKAN_VISIBILITY_SLOTS)
			{
				seen[slot] = 1;
				sums[slot] = 0;
				areas[slot] = 1.0f;
				tests[slot] = 0;
			}
		}
		if (frame->count)
		{
			results = malloc(frame->count * sizeof(*results));
			if (results && vkGetQueryPoolResults(vk.device, frame->pool, 0, frame->count, frame->count * sizeof(*results),
				results, sizeof(*results), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
			{
				for (query = 0; query < frame->count; query++)
				{
					uint16_t slot = frame->slots[query];

					if (slot >= XGPU_VULKAN_VISIBILITY_SLOTS)
						continue;
					if (!seen[slot] || tests[slot] != frame->tests[query])
					{
						sums[slot] = 0;
						tests[slot] = frame->tests[query];
						seen[slot] = 1;
					}
					sums[slot] += results[query];
					areas[slot] = frame->areas[query];
				}
			}
			free(results);
			vkResetQueryPool(vk.device, frame->pool, 0, frame->count);
		}
		if (vk.visibility_output)
		{
			for (query = 0; query < XGPU_VULKAN_VISIBILITY_SLOTS; query++)
			{
				if (!seen[query])
					continue;
				/* the count in the game's pixels (as the OpenGL backend's
				visibility_unscaled) */
				vk.visibility_output[query] = areas[query] > 1.0f ? (uint32_t)((float)sums[query] / areas[query] + 0.5f) :
					(uint32_t)sums[query];
			}
		}
		frame->count = 0;
		frame->empty_count = 0;
		frame->serial = 0;
		frame->in_use = 0;
	}
}

/* the frame being recorded's queries */
static struct query_frame *queries_current(void)
{
	int index;

	if (vk.queries)
		return vk.queries;
	for (;;)
	{
		for (index = 0; index < QUERY_FRAMES; index++)
		{
			if (!vk.query_frames[index].in_use)
			{
				vk.queries = &vk.query_frames[index];
				vk.queries->in_use = 1;
				vk.queries->serial = 0;
				vk.queries->frame = vk.frame;
				vk.queries->count = 0;
				vk.queries->empty_count = 0;
				return vk.queries;
			}
		}
		/* (every pool in use: the oldest frame's counts written now) */
		{
			uint64_t oldest = 0;
			int found = 0;

			for (index = 0; index < QUERY_FRAMES; index++)
			{
				if (vk.query_frames[index].serial && (!found || vk.query_frames[index].frame < oldest))
				{
					oldest = vk.query_frames[index].frame;
					found = 1;
				}
			}
			if (!found)
				return NULL;
			queries_publish(oldest);
		}
	}
}

/* a query of the current test begun (in a pass) */
static void test_query_begin(void)
{
	struct query_frame *frame = queries_current();

	if (!frame || frame->count >= QUERIES_A_FRAME || vk.test_query_count >= 64)
		return;
	frame->slots[frame->count] = 0xffff;
	frame->areas[frame->count] = 1.0f;
	frame->tests[frame->count] = vk.test_number;
	vk.test_queries[vk.test_query_count++] = frame->count;
	vkCmdBeginQuery(vk.slot->main, frame->pool, frame->count, VK_QUERY_CONTROL_PRECISE_BIT);
	frame->count++;
	vk.test_query_open = 1;
}

static void test_query_end(void)
{
	if (!vk.test_query_open)
		return;
	vkCmdEndQuery(vk.slot->main, vk.queries->pool, vk.test_queries[vk.test_query_count - 1]);
	vk.test_query_open = 0;
}

void xgpu_vulkan_visibility_begin(void)
{
	vk.test_active = 1;
	vk.test_number++;
	vk.test_query_count = 0;
	vk.test_query_open = 0;
	if (vk.pass_active)
		test_query_begin();
}

void xgpu_vulkan_visibility_end(uint32_t slot, float area)
{
	uint32_t index;

	if (!vk.test_active)
		return;
	test_query_end();
	/* (a test that drew in no pass: its slot's count is 0, as a query
	that counted nothing) */
	if (!vk.test_query_count)
	{
		struct query_frame *frame = queries_current();

		if (frame && frame->empty_count < QUERIES_A_FRAME)
			frame->empty[frame->empty_count++] = (uint16_t)slot;
		vk.test_active = 0;
		return;
	}
	for (index = 0; index < vk.test_query_count; index++)
	{
		vk.queries->slots[vk.test_queries[index]] = (uint16_t)slot;
		vk.queries->areas[vk.test_queries[index]] = area;
	}
	vk.test_active = 0;
	vk.test_query_count = 0;
}

void xgpu_vulkan_set_visibility_output(volatile uint32_t *counts)
{
	vk.visibility_output = counts;
}

/* ---------- images */

static VkFormat vulkan_format(uint8_t format)
{
	switch (format)
	{
	case XGPU_VULKAN_FORMAT_BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
	case XGPU_VULKAN_FORMAT_D24S8: return vk.depth_format;
	case XGPU_VULKAN_FORMAT_DXT1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
	case XGPU_VULKAN_FORMAT_DXT3: return VK_FORMAT_BC2_UNORM_BLOCK;
	case XGPU_VULKAN_FORMAT_DXT5: return VK_FORMAT_BC3_UNORM_BLOCK;
	case XGPU_VULKAN_FORMAT_RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
	case XGPU_VULKAN_FORMAT_COVERAGE: return VK_FORMAT_R8_UNORM;
	case XGPU_VULKAN_FORMAT_RGBA32_UINT: return VK_FORMAT_R32G32B32A32_UINT;
	default: return VK_FORMAT_UNDEFINED;
	}
}

static int format_blocks(uint8_t format)
{
	return format == XGPU_VULKAN_FORMAT_DXT1 || format == XGPU_VULKAN_FORMAT_DXT3 || format == XGPU_VULKAN_FORMAT_DXT5;
}

static VkComponentSwizzle swizzle_of(uint32_t source)
{
	static const VkComponentSwizzle components[4] =
	{
		VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A,
	};

	return components[source & 3];
}

static VkImageView view_make(const struct xgpu_vulkan_image *image, VkImageViewType type, uint32_t first_layer,
	uint32_t layers, int sampled)
{
	VkImageViewCreateInfo info;
	VkImageView view = VK_NULL_HANDLE;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	info.image = image->image;
	info.viewType = type;
	info.format = image->format;
	info.subresourceRange.aspectMask = image->aspect;
	info.subresourceRange.baseMipLevel = 0;
	info.subresourceRange.levelCount = sampled ? image->description.levels : 1;
	info.subresourceRange.baseArrayLayer = first_layer;
	info.subresourceRange.layerCount = layers;
	if (sampled && image->description.format == XGPU_VULKAN_FORMAT_COVERAGE)
	{
		info.components.r = info.components.g = info.components.b = VK_COMPONENT_SWIZZLE_ONE;
		info.components.a = VK_COMPONENT_SWIZZLE_R;
	}
	else if (sampled && (image->description.channels & 0x100))
	{
		uint32_t channels = image->description.channels;

		info.components.r = swizzle_of(channels);
		info.components.g = swizzle_of(channels >> 2);
		info.components.b = swizzle_of(channels >> 4);
		info.components.a = swizzle_of(channels >> 6);
	}
	check(vkCreateImageView(vk.device, &info, NULL, &view), "vkCreateImageView");
	return view;
}

struct xgpu_vulkan_image *xgpu_vulkan_image_create(const struct xgpu_vulkan_image_description *description)
{
	struct xgpu_vulkan_image *image = calloc(1, sizeof(*image));
	VkImageCreateInfo info;
	VkMemoryRequirements requirements;
	uint32_t layers = 1;

	if (!image)
		return NULL;
	image->description = *description;
	if (!image->description.levels)
		image->description.levels = 1;
	image->format = vulkan_format(description->format);
	image->aspect = description->format == XGPU_VULKAN_FORMAT_D24S8 ?
		VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType = description->type == XGPU_VULKAN_IMAGE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
	info.format = image->format;
	info.extent.width = description->width ? description->width : 1;
	info.extent.height = description->height ? description->height : 1;
	info.extent.depth = description->type == XGPU_VULKAN_IMAGE_3D ? description->depth_or_layers : 1;
	if (description->type == XGPU_VULKAN_IMAGE_CUBE)
	{
		layers = 6;
		info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	}
	else if (description->type == XGPU_VULKAN_IMAGE_2D_ARRAY)
	{
		layers = description->depth_or_layers ? description->depth_or_layers : 1;
	}
	info.mipLevels = image->description.levels;
	info.arrayLayers = layers;
	info.samples = description->samples > 1 ? (VkSampleCountFlagBits)description->samples : VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (description->samples <= 1)
		info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	if (description->usage & XGPU_VULKAN_USAGE_TARGET)
		info.usage |= image->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT :
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (image->format == VK_FORMAT_UNDEFINED || !check(vkCreateImage(vk.device, &info, NULL, &image->image), "vkCreateImage"))
	{
		host_logf(HOST_LOG_ERROR, "vulkan: cannot make a %ux%u image of format %u", description->width, description->height,
			(unsigned)description->format);
		free(image);
		return NULL;
	}
	vkGetImageMemoryRequirements(vk.device, image->image, &requirements);
	if (!memory_for(&requirements, &image->memory) ||
		!check(vkBindImageMemory(vk.device, image->image, image->memory.memory, image->memory.offset), "vkBindImageMemory"))
	{
		vkDestroyImage(vk.device, image->image, NULL);
		memory_release(&image->memory);
		free(image);
		return NULL;
	}
	if (description->samples <= 1 && !(image->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
	{
		VkImageViewType type = description->type == XGPU_VULKAN_IMAGE_3D ? VK_IMAGE_VIEW_TYPE_3D :
			description->type == XGPU_VULKAN_IMAGE_CUBE ? VK_IMAGE_VIEW_TYPE_CUBE :
			description->type == XGPU_VULKAN_IMAGE_2D_ARRAY ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;

		image->sampled = view_make(image, type, 0, layers, 1);
	}
	/* (into the GENERAL layout, which it keeps, its texels zero: before the
	work recorded so far, or with the early work) */
	{
		VkCommandBuffer commands = early_commands();
		VkImageMemoryBarrier transition;

		memset(&transition, 0, sizeof(transition));
		transition.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		transition.srcAccessMask = 0;
		transition.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		transition.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		transition.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		transition.srcQueueFamilyIndex = transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		transition.image = image->image;
		transition.subresourceRange.aspectMask = image->aspect;
		transition.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
		transition.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
		vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
			1, &transition);
		if (image->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
		{
			VkClearDepthStencilValue clear = { 1.0f, 0 };

			vkCmdClearDepthStencilImage(commands, image->image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
				&transition.subresourceRange);
		}
		else if (!format_blocks(description->format))
		{
			VkClearColorValue clear;

			memset(&clear, 0, sizeof(clear));
			vkCmdClearColorImage(commands, image->image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &transition.subresourceRange);
		}
		/* (before the uploads after it) */
		barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
	}
	return image;
}

void xgpu_vulkan_image_destroy(struct xgpu_vulkan_image *image)
{
	struct garbage item;
	uint32_t index;

	if (!image)
		return;
	if (vk.color == image || vk.depth == image)
	{
		pass_end();
		if (vk.color == image)
			vk.color = NULL;
		if (vk.depth == image)
			vk.depth = NULL;
	}
	if (vk.last_texture_valid)
	{
		for (index = 0; index < 4; index++)
		{
			if (vk.last_textures[index].image == image)
				vk.last_texture_valid = 0;
		}
	}
	memset(&item, 0, sizeof(item));
	/* (after the submission being recorded, which may use it) */
	item.serial = vk.serial;
	item.kind = _garbage_view;
	if (image->sampled)
	{
		item.view = image->sampled;
		garbage_add(&item);
	}
	for (index = 0; index < image->attachment_count; index++)
	{
		if (image->attachments[index])
		{
			item.view = image->attachments[index];
			garbage_add(&item);
		}
	}
	item.kind = _garbage_image;
	item.image = image->image;
	item.memory = image->memory;
	garbage_add(&item);
	free(image->attachments);
	free(image);
}

const struct xgpu_vulkan_image_description *xgpu_vulkan_image_description(const struct xgpu_vulkan_image *image)
{
	return &image->description;
}

/* the commands an upload into an image goes in: the early ones where no
work recorded so far in this submission uses it */
static VkCommandBuffer upload_commands(uint64_t used)
{
	if (vk.recording && used == vk.serial)
	{
		vk.statistics.uploads_ordered++;
		return main_commands();
	}
	vk.statistics.uploads_early++;
	return early_commands();
}

/* the barrier after a transfer in the main commands (the early ones end
with one) */
static void transfer_done(VkCommandBuffer commands)
{
	if (commands == vk.slot->main)
		barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
			AFTER_STAGES, AFTER_ACCESS);
}

void xgpu_vulkan_image_upload(struct xgpu_vulkan_image *image, uint32_t level, uint32_t layer, uint32_t x, uint32_t y,
	uint32_t z, uint32_t width, uint32_t height, uint32_t depth, const void *data, uint32_t bytes_per_row,
	uint32_t bytes_per_image)
{
	const struct xgpu_vulkan_image_description *description = &image->description;
	int blocks = format_blocks(description->format);
	uint32_t texel = description->format == XGPU_VULKAN_FORMAT_COVERAGE ? 1 :
		description->format == XGPU_VULKAN_FORMAT_RGBA32_UINT ? 16 : 4;
	uint32_t rows = blocks ? (height + 3) / 4 : height;
	uint32_t row_bytes = blocks ? (description->format == XGPU_VULKAN_FORMAT_DXT1 ? 8 : 16) * ((width + 3) / 4) :
		width * texel;
	uint32_t size, slice, row;
	struct xgpu_vulkan_range range;
	unsigned char *staging;
	VkBufferImageCopy copy;
	VkCommandBuffer commands;

	if (!depth)
		depth = 1;
	if (!bytes_per_row)
		bytes_per_row = row_bytes;
	if (!bytes_per_image)
		bytes_per_image = bytes_per_row * rows;
	size = row_bytes * rows * depth;
	staging = xgpu_vulkan_upload(size, 16, &range);
	if (!staging)
		return;
	/* (the commands chosen first: the upload may have submitted the
	recording) */
	commands = upload_commands(image->used);
	for (slice = 0; slice < depth; slice++)
	{
		for (row = 0; row < rows; row++)
		{
			memcpy(staging + (slice * rows + row) * row_bytes,
				(const unsigned char *)data + slice * bytes_per_image + row * bytes_per_row, row_bytes);
		}
	}
	memset(&copy, 0, sizeof(copy));
	copy.bufferOffset = range.offset;
	copy.imageSubresource.aspectMask = image->aspect;
	copy.imageSubresource.mipLevel = level;
	copy.imageSubresource.baseArrayLayer = description->type == XGPU_VULKAN_IMAGE_3D ? 0 : layer;
	copy.imageSubresource.layerCount = 1;
	copy.imageOffset.x = (int32_t)x;
	copy.imageOffset.y = (int32_t)y;
	copy.imageOffset.z = (int32_t)z;
	copy.imageExtent.width = width;
	copy.imageExtent.height = height;
	copy.imageExtent.depth = depth;
	vkCmdCopyBufferToImage(commands, range.buffer, image->image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
	transfer_done(commands);
}

void xgpu_vulkan_image_make_levels(struct xgpu_vulkan_image *image, uint32_t from)
{
	const struct xgpu_vulkan_image_description *description = &image->description;
	VkCommandBuffer commands = upload_commands(image->used);
	uint32_t level, layers = description->type == XGPU_VULKAN_IMAGE_CUBE ? 6 :
		description->type == XGPU_VULKAN_IMAGE_2D_ARRAY ? description->depth_or_layers : 1;

	if (!from)
		from = 1;
	for (level = from; level < description->levels; level++)
	{
		VkImageBlit blit;
		int32_t width = (int32_t)(description->width >> (level - 1)), height = (int32_t)(description->height >> (level - 1));
		int32_t depth = description->type == XGPU_VULKAN_IMAGE_3D ? (int32_t)(description->depth_or_layers >> (level - 1)) : 1;

		barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_READ_BIT);
		memset(&blit, 0, sizeof(blit));
		blit.srcSubresource.aspectMask = blit.dstSubresource.aspectMask = image->aspect;
		blit.srcSubresource.mipLevel = level - 1;
		blit.dstSubresource.mipLevel = level;
		blit.srcSubresource.layerCount = blit.dstSubresource.layerCount = layers;
		blit.srcOffsets[1].x = width > 0 ? width : 1;
		blit.srcOffsets[1].y = height > 0 ? height : 1;
		blit.srcOffsets[1].z = depth > 0 ? depth : 1;
		blit.dstOffsets[1].x = width > 1 ? width / 2 : 1;
		blit.dstOffsets[1].y = height > 1 ? height / 2 : 1;
		blit.dstOffsets[1].z = depth > 1 ? depth / 2 : 1;
		vkCmdBlitImage(commands, image->image, VK_IMAGE_LAYOUT_GENERAL, image->image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
			VK_FILTER_LINEAR);
	}
	transfer_done(commands);
}

void xgpu_vulkan_image_copy(struct xgpu_vulkan_image *source, uint32_t source_level, uint32_t source_layer,
	struct xgpu_vulkan_image *destination, uint32_t destination_level, uint32_t destination_layer)
{
	VkCommandBuffer commands = main_commands();
	VkImageCopy copy;
	uint32_t width = source->description.width >> source_level, height = source->description.height >> source_level;
	uint32_t destination_width = destination->description.width >> destination_level;
	uint32_t destination_height = destination->description.height >> destination_level;

	memset(&copy, 0, sizeof(copy));
	copy.srcSubresource.aspectMask = source->aspect;
	copy.srcSubresource.mipLevel = source_level;
	copy.srcSubresource.baseArrayLayer = source_layer;
	copy.srcSubresource.layerCount = 1;
	copy.dstSubresource.aspectMask = destination->aspect;
	copy.dstSubresource.mipLevel = destination_level;
	copy.dstSubresource.baseArrayLayer = destination_layer;
	copy.dstSubresource.layerCount = 1;
	copy.extent.width = (width < destination_width ? width : destination_width);
	copy.extent.height = (height < destination_height ? height : destination_height);
	copy.extent.depth = 1;
	if (!copy.extent.width || !copy.extent.height)
		return;
	source->used = destination->used = vk.serial;
	vkCmdCopyImage(commands, source->image, VK_IMAGE_LAYOUT_GENERAL, destination->image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
	transfer_done(commands);
}

void xgpu_vulkan_image_resolve(struct xgpu_vulkan_image *multisampled, struct xgpu_vulkan_image *image)
{
	VkCommandBuffer commands = main_commands();

	multisampled->used = image->used = vk.serial;
	if (image->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
	{
		/* (depth: the first sample, drawn by a pass that resolves it) */
		VkRenderingAttachmentInfo depth;
		VkRenderingInfo rendering;
		VkImageView source = VK_NULL_HANDLE, destination = VK_NULL_HANDLE;

		source = view_make(multisampled, VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0);
		destination = view_make(image, VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0);
		if (source && destination)
		{
			memset(&depth, 0, sizeof(depth));
			depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
			depth.imageView = source;
			depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
			depth.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
			depth.resolveImageView = destination;
			depth.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
			depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
			depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			memset(&rendering, 0, sizeof(rendering));
			rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
			rendering.renderArea.extent.width = image->description.width;
			rendering.renderArea.extent.height = image->description.height;
			rendering.layerCount = 1;
			rendering.pDepthAttachment = &depth;
			rendering.pStencilAttachment = &depth;
			vkCmdBeginRendering(commands, &rendering);
			vkCmdEndRendering(commands);
			barrier(commands, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, AFTER_STAGES, AFTER_ACCESS);
		}
		{
			struct garbage item;

			memset(&item, 0, sizeof(item));
			item.kind = _garbage_view;
			item.serial = vk.serial;
			if (source)
			{
				item.view = source;
				garbage_add(&item);
			}
			if (destination)
			{
				item.view = destination;
				garbage_add(&item);
			}
		}
		return;
	}
	{
		VkImageResolve resolve;

		memset(&resolve, 0, sizeof(resolve));
		resolve.srcSubresource.aspectMask = resolve.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		resolve.srcSubresource.layerCount = resolve.dstSubresource.layerCount = 1;
		resolve.extent.width = image->description.width;
		resolve.extent.height = image->description.height;
		resolve.extent.depth = 1;
		vkCmdResolveImage(commands, multisampled->image, VK_IMAGE_LAYOUT_GENERAL, image->image, VK_IMAGE_LAYOUT_GENERAL, 1,
			&resolve);
		transfer_done(commands);
	}
}

int xgpu_vulkan_read_pixels(struct xgpu_vulkan_image *image, uint32_t layer, void *pixels, uint32_t bytes_per_row)
{
	uint32_t width = image->description.width, height = image->description.height, row;
	VkDeviceSize size = (VkDeviceSize)width * height * 4;
	VkCommandBuffer commands;
	VkBufferImageCopy copy;

	if (image->description.samples > 1 || (image->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
		return 0;
	if (size > vk.readback_size)
	{
		if (vk.readback)
		{
			vkDestroyBuffer(vk.device, vk.readback, NULL);
			memory_release(&vk.readback_memory);
			vk.readback = VK_NULL_HANDLE;
		}
		if (!buffer_make(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &vk.readback, &vk.readback_memory,
			(void **)&vk.readback_mapped))
		{
			vk.readback_size = 0;
			return 0;
		}
		vk.readback_size = size;
	}
	commands = main_commands();
	memset(&copy, 0, sizeof(copy));
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.baseArrayLayer = layer;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent.width = width;
	copy.imageExtent.height = height;
	copy.imageExtent.depth = 1;
	vkCmdCopyImageToBuffer(commands, image->image, VK_IMAGE_LAYOUT_GENERAL, vk.readback, 1, &copy);
	barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
		VK_ACCESS_HOST_READ_BIT);
	xgpu_vulkan_wait(xgpu_vulkan_fence());
	for (row = 0; row < height; row++)
		memcpy((unsigned char *)pixels + (size_t)row * bytes_per_row, vk.readback_mapped + (size_t)row * width * 4,
			(size_t)width * 4);
	return 1;
}

/* ---------- buffers */

struct xgpu_vulkan_buffer *xgpu_vulkan_buffer_create(uint32_t size)
{
	struct xgpu_vulkan_buffer *buffer = calloc(1, sizeof(*buffer));

	if (!buffer)
		return NULL;
	if (!buffer_make(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1, &buffer->buffer, &buffer->memory, &buffer->mapped))
	{
		free(buffer);
		return NULL;
	}
	buffer->size = size;
	memset(buffer->mapped, 0, size);
	return buffer;
}

VkBuffer xgpu_vulkan_buffer_handle(struct xgpu_vulkan_buffer *buffer)
{
	if (!buffer)
		return VK_NULL_HANDLE;
	/* (read by the work being recorded: writes into it are ordered after) */
	buffer->used = vk.serial;
	return buffer->buffer;
}

const void *xgpu_vulkan_buffer_contents(const struct xgpu_vulkan_buffer *buffer)
{
	return buffer ? buffer->mapped : NULL;
}

void xgpu_vulkan_buffer_write(struct xgpu_vulkan_buffer *buffer, uint32_t offset, const void *data, uint32_t size,
	int unused)
{
	struct xgpu_vulkan_range range;
	VkBufferCopy copy;
	VkCommandBuffer commands;
	void *staging;

	if (!buffer || !size || (uint64_t)offset + size > buffer->size)
		return;
	if (unused)
	{
		memcpy((unsigned char *)buffer->mapped + offset, data, size);
		return;
	}
	staging = xgpu_vulkan_upload(size, 16, &range);
	if (!staging)
		return;
	memcpy(staging, data, size);
	commands = upload_commands(buffer->used);
	memset(&copy, 0, sizeof(copy));
	copy.srcOffset = range.offset;
	copy.dstOffset = offset;
	copy.size = size;
	vkCmdCopyBuffer(commands, range.buffer, buffer->buffer, 1, &copy);
	transfer_done(commands);
}

/* ---------- samplers */

/* NVIDIA's drivers blend between mip levels with anisotropic filtering as
OpenGL and Vulkan alike (a narrowed blend, "brilinear"), but OpenGL's blend
is centred between the levels and Vulkan's begins at the lower: OpenGL's
LOD is Vulkan's less a constant that depends on the most anisotropy, which
this bias takes off, so that this backend's textures are the OpenGL
backend's pixel for pixel. Measured with random textured triangles drawn by
both on an RTX 4090: exact for 2x to 16x, both mip filters, every
anisotropy of the footprint; none without mip levels (OpenGL's GL_LINEAR
minification), and none on other drivers (not measured there). */
static float anisotropic_bias(float anisotropy)
{
	if (!vk.nvidia)
		return 0.0f;
	if (anisotropy >= 8.0f)
		return -64.0f / 256.0f;
	if (anisotropy >= 4.0f)
		return -54.0f / 256.0f;
	return -42.0f / 256.0f;
}

static VkSamplerAddressMode address_mode(uint8_t mode)
{
	switch (mode)
	{
	case XGPU_VULKAN_ADDRESS_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	case XGPU_VULKAN_ADDRESS_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	case XGPU_VULKAN_ADDRESS_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
	}
}

VkSampler xgpu_vulkan_sampler(const struct xgpu_vulkan_sampler_description *description)
{
	VkSamplerCreateInfo info;
	VkSamplerCustomBorderColorCreateInfoEXT custom;
	uint32_t hash = 2166136261u, index, probe;
	const unsigned char *bytes = (const unsigned char *)description;
	VkSampler sampler = VK_NULL_HANDLE;

	for (index = 0; index < sizeof(*description); index++)
		hash = (hash ^ bytes[index]) * 16777619u;
	for (probe = 0; probe < SAMPLER_CACHE; probe++)
	{
		index = (hash + probe) % SAMPLER_CACHE;
		if (!vk.samplers[index].sampler)
			break;
		if (!memcmp(&vk.samplers[index].description, description, sizeof(*description)))
			return vk.samplers[index].sampler;
	}
	if (probe == SAMPLER_CACHE || vk.sampler_count >= SAMPLER_CACHE * 3 / 4)
	{
		/* (never seen: the game uses a few dozen) */
		return vk.stand_in_sampler;
	}
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	/* (a cube map's faces filtered each alone, as OpenGL's are without
	GL_TEXTURE_CUBE_MAP_SEAMLESS, which the OpenGL backend leaves off) */
	if (vk.non_seamless)
		info.flags = VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT;
	info.magFilter = description->magnify == XGPU_VULKAN_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	info.minFilter = description->minify == XGPU_VULKAN_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	info.mipmapMode = description->mip == XGPU_VULKAN_MIP_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR :
		VK_SAMPLER_MIPMAP_MODE_NEAREST;
	info.addressModeU = address_mode(description->address[0]);
	info.addressModeV = address_mode(description->address[1]);
	info.addressModeW = address_mode(description->address[2]);
	info.mipLodBias = description->lod_bias;
	if (description->anisotropy > 1 && vk.capabilities.anisotropy > 1)
	{
		info.anisotropyEnable = VK_TRUE;
		info.maxAnisotropy = (float)(description->anisotropy < vk.capabilities.anisotropy ? description->anisotropy :
			vk.capabilities.anisotropy);
		if (description->mip != XGPU_VULKAN_MIP_NONE)
			info.mipLodBias += anisotropic_bias(info.maxAnisotropy);
	}
	/* (no mip filter: the first level alone, as OpenGL's minification
	without mipmaps samples its base level whatever its least LOD) */
	if (description->mip == XGPU_VULKAN_MIP_NONE)
	{
		info.minLod = 0.0f;
		info.maxLod = 0.25f;
	}
	else
	{
		info.minLod = description->min_lod;
		info.maxLod = VK_LOD_CLAMP_NONE;
	}
	info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	if (description->border_color == 0xff000000u)
		info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
	else if (description->border_color == 0xffffffffu)
		info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
	else if (description->border_color && vk.capabilities.custom_border_colors &&
		(description->address[0] == XGPU_VULKAN_ADDRESS_BORDER || description->address[1] == XGPU_VULKAN_ADDRESS_BORDER ||
		description->address[2] == XGPU_VULKAN_ADDRESS_BORDER))
	{
		memset(&custom, 0, sizeof(custom));
		custom.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT;
		custom.customBorderColor.float32[0] = (float)((description->border_color >> 16) & 0xff) / 255.0f;
		custom.customBorderColor.float32[1] = (float)((description->border_color >> 8) & 0xff) / 255.0f;
		custom.customBorderColor.float32[2] = (float)(description->border_color & 0xff) / 255.0f;
		custom.customBorderColor.float32[3] = (float)(description->border_color >> 24) / 255.0f;
		custom.format = VK_FORMAT_UNDEFINED;
		info.pNext = &custom;
		info.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
	}
	if (!check(vkCreateSampler(vk.device, &info, NULL, &sampler), "vkCreateSampler"))
		return vk.stand_in_sampler;
	vk.samplers[index].description = *description;
	vk.samplers[index].sampler = sampler;
	vk.sampler_count++;
	vk.statistics.samplers_made++;
	return sampler;
}

/* ---------- shaders */

VkShaderModule xgpu_vulkan_shader(const uint32_t *words, size_t count)
{
	uint64_t hash = 14695981039346656037ULL;
	const unsigned char *bytes = (const unsigned char *)words;
	struct shader_entry *entry, **bucket;
	VkShaderModuleCreateInfo info;
	size_t index;

	for (index = 0; index < count * 4; index++)
		hash = (hash ^ bytes[index]) * 1099511628211ULL;
	bucket = &vk.shaders[hash % SHADER_BUCKETS];
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->hash == hash && entry->count == count && !memcmp(entry->words, words, count * 4))
			return entry->module;
	}
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return VK_NULL_HANDLE;
	entry->words = malloc(count * 4);
	if (!entry->words)
	{
		free(entry);
		return VK_NULL_HANDLE;
	}
	memcpy(entry->words, words, count * 4);
	entry->count = count;
	entry->hash = hash;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = count * 4;
	info.pCode = words;
	check(vkCreateShaderModule(vk.device, &info, NULL, &entry->module), "vkCreateShaderModule");
	entry->next = *bucket;
	*bucket = entry;
	return entry->module;
}

/* ---------- pipelines */

static VkCompareOp compare_op(uint8_t function)
{
	static const VkCompareOp operations[8] =
	{
		VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL,
		VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS,
	};

	return operations[function & 7];
}

static VkBlendFactor blend_factor(uint8_t factor)
{
	static const VkBlendFactor factors[] =
	{
		VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
		VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
		VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
		VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
		VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
	};

	return factor < sizeof(factors) / sizeof(factors[0]) ? factors[factor] : VK_BLEND_FACTOR_ONE;
}

static VkBlendOp blend_op(uint8_t operation)
{
	switch (operation)
	{
	case 1: return VK_BLEND_OP_SUBTRACT;
	case 2: return VK_BLEND_OP_REVERSE_SUBTRACT;
	case 3: return VK_BLEND_OP_MIN;
	case 4: return VK_BLEND_OP_MAX;
	default: return VK_BLEND_OP_ADD;
	}
}

static VkStencilOp stencil_op(uint8_t operation)
{
	static const VkStencilOp operations[8] =
	{
		VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_CLAMP,
		VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP,
		VK_STENCIL_OP_DECREMENT_AND_WRAP,
	};

	return operations[operation & 7];
}

static uint32_t description_hash(const struct xgpu_vulkan_pipeline_description *description)
{
	const unsigned char *bytes = (const unsigned char *)description;
	uint32_t hash = 2166136261u;
	size_t index;

	for (index = 0; index < sizeof(*description); index++)
		hash = (hash ^ bytes[index]) * 16777619u;
	return hash;
}

static VkPipeline pipeline_make(const struct xgpu_vulkan_pipeline_description *description)
{
	VkPipelineShaderStageCreateInfo stages[2];
	VkVertexInputBindingDescription bindings[XGPU_VULKAN_BINDINGS];
	VkVertexInputAttributeDescription attributes[16];
	VkPipelineVertexInputStateCreateInfo vertex_input;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineDepthStencilStateCreateInfo depth_stencil;
	VkPipelineColorBlendAttachmentState blend_attachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkPipelineDynamicStateCreateInfo dynamic;
	VkPipelineRenderingCreateInfo rendering;
	VkGraphicsPipelineCreateInfo info;
	VkFormat color_format = vulkan_format(description->color_format);
	VkFormat depth_format = description->depth_format ? vk.depth_format : VK_FORMAT_UNDEFINED;
	static const VkDynamicState dynamic_states[] =
	{
		VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
		VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
	};
	uint32_t binding_count = 0, attribute_count = 0, index, used = 0;
	VkPipeline pipeline = VK_NULL_HANDLE;
	double started = seconds_now();

	memset(stages, 0, sizeof(stages));
	stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = description->vertex_shader;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = description->pixel_shader;
	stages[1].pName = "main";

	for (index = 0; index < 16; index++)
	{
		const struct xgpu_vulkan_attribute *attribute = &description->attributes[index];

		if (attribute->format == VK_FORMAT_UNDEFINED)
			continue;
		attributes[attribute_count].location = index;
		attributes[attribute_count].binding = attribute->binding;
		attributes[attribute_count].format = (VkFormat)attribute->format;
		attributes[attribute_count].offset = attribute->offset;
		attribute_count++;
		used |= 1u << attribute->binding;
	}
	for (index = 0; index < XGPU_VULKAN_BINDINGS; index++)
	{
		if (!(used & (1u << index)))
			continue;
		bindings[binding_count].binding = index;
		bindings[binding_count].stride = description->strides[index];
		bindings[binding_count].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		binding_count++;
	}
	memset(&vertex_input, 0, sizeof(vertex_input));
	vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input.vertexBindingDescriptionCount = binding_count;
	vertex_input.pVertexBindingDescriptions = bindings;
	vertex_input.vertexAttributeDescriptionCount = attribute_count;
	vertex_input.pVertexAttributeDescriptions = attributes;

	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = description->primitive_class == XGPU_VULKAN_CLASS_POINTS ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST :
		description->primitive_class == XGPU_VULKAN_CLASS_LINES ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST :
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	memset(&viewport, 0, sizeof(viewport));
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;

	memset(&raster, 0, sizeof(raster));
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = description->fill == 1 ? VK_POLYGON_MODE_LINE : description->fill == 2 ? VK_POLYGON_MODE_POINT :
		VK_POLYGON_MODE_FILL;
	raster.cullMode = !description->cull ? VK_CULL_MODE_NONE : description->cull_front ? VK_CULL_MODE_FRONT_BIT :
		VK_CULL_MODE_BACK_BIT;
	raster.frontFace = description->front_counter_clockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
	raster.depthBiasEnable = description->depth_bias ? VK_TRUE : VK_FALSE;
	raster.lineWidth = 1.0f;

	memset(&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = description->samples > 1 ? (VkSampleCountFlagBits)description->samples :
		VK_SAMPLE_COUNT_1_BIT;

	memset(&depth_stencil, 0, sizeof(depth_stencil));
	depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	if (depth_format != VK_FORMAT_UNDEFINED)
	{
		/* (depth is written only where it is tested, as OpenGL has it) */
		depth_stencil.depthTestEnable = description->depth_test ? VK_TRUE : VK_FALSE;
		depth_stencil.depthWriteEnable = description->depth_test && description->depth_write ? VK_TRUE : VK_FALSE;
		depth_stencil.depthCompareOp = compare_op(description->depth_function);
		depth_stencil.stencilTestEnable = description->stencil_test ? VK_TRUE : VK_FALSE;
		depth_stencil.front.failOp = stencil_op(description->stencil_fail);
		depth_stencil.front.passOp = stencil_op(description->stencil_pass);
		depth_stencil.front.depthFailOp = stencil_op(description->stencil_depth_fail);
		depth_stencil.front.compareOp = compare_op(description->stencil_function);
		depth_stencil.front.compareMask = description->stencil_read_mask;
		depth_stencil.front.writeMask = description->stencil_write_mask;
		depth_stencil.back = depth_stencil.front;
	}
	depth_stencil.minDepthBounds = 0.0f;
	depth_stencil.maxDepthBounds = 1.0f;

	memset(&blend_attachment, 0, sizeof(blend_attachment));
	blend_attachment.blendEnable = description->blend ? VK_TRUE : VK_FALSE;
	blend_attachment.srcColorBlendFactor = blend_attachment.srcAlphaBlendFactor = blend_factor(description->blend_source);
	blend_attachment.dstColorBlendFactor = blend_attachment.dstAlphaBlendFactor = blend_factor(description->blend_destination);
	blend_attachment.colorBlendOp = blend_attachment.alphaBlendOp = blend_op(description->blend_operation);
	blend_attachment.colorWriteMask = description->color_mask & 15;
	memset(&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = color_format != VK_FORMAT_UNDEFINED ? 1 : 0;
	blend.pAttachments = &blend_attachment;

	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = sizeof(dynamic_states) / sizeof(dynamic_states[0]);
	dynamic.pDynamicStates = dynamic_states;

	memset(&rendering, 0, sizeof(rendering));
	rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	rendering.colorAttachmentCount = color_format != VK_FORMAT_UNDEFINED ? 1 : 0;
	rendering.pColorAttachmentFormats = &color_format;
	rendering.depthAttachmentFormat = depth_format;
	rendering.stencilAttachmentFormat = depth_format;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	info.pNext = &rendering;
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertex_input;
	info.pInputAssemblyState = &assembly;
	info.pViewportState = &viewport;
	info.pRasterizationState = &raster;
	info.pMultisampleState = &multisample;
	info.pDepthStencilState = &depth_stencil;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamic;
	info.layout = vk.pipeline_layout;
	if (!description->vertex_shader || !description->pixel_shader ||
		!check(vkCreateGraphicsPipelines(vk.device, vk.pipeline_cache, 1, &info, NULL, &pipeline), "vkCreateGraphicsPipelines"))
		pipeline = VK_NULL_HANDLE;
	vk.statistics.pipelines_made++;
	vk.statistics.pipeline_seconds += seconds_now() - started;
	return pipeline;
}

VkPipeline xgpu_vulkan_pipeline(const struct xgpu_vulkan_pipeline_description *description)
{
	static struct pipeline_entry *last;
	uint32_t hash;
	struct pipeline_entry **bucket, *entry;

	if (last && !memcmp(&last->description, description, sizeof(*description)))
		return last->pipeline;
	hash = description_hash(description);
	bucket = &vk.pipelines[hash % PIPELINE_BUCKETS];
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->hash == hash && !memcmp(&entry->description, description, sizeof(*description)))
		{
			last = entry;
			return entry->pipeline;
		}
	}
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return VK_NULL_HANDLE;
	entry->hash = hash;
	entry->description = *description;
	entry->pipeline = pipeline_make(description);
	entry->next = *bucket;
	*bucket = entry;
	last = entry;
	return entry->pipeline;
}

/* ---------- passes */

static VkImageView attachment_view(struct xgpu_vulkan_image *image, uint32_t layer)
{
	uint32_t layers = image->description.type == XGPU_VULKAN_IMAGE_2D_ARRAY ? image->description.depth_or_layers : 1;

	if (layer >= layers)
		layer = 0;
	if (layer >= image->attachment_count)
	{
		VkImageView *grown = realloc(image->attachments, layers * sizeof(*grown));

		if (!grown)
			return VK_NULL_HANDLE;
		memset(grown + image->attachment_count, 0, (layers - image->attachment_count) * sizeof(*grown));
		image->attachments = grown;
		image->attachment_count = layers;
	}
	if (!image->attachments[layer])
		image->attachments[layer] = view_make(image, VK_IMAGE_VIEW_TYPE_2D, layer, 1, 0);
	return image->attachments[layer];
}

/* the pass drawing into the bound targets, begun */
static int pass_begin(void)
{
	VkRenderingAttachmentInfo color, depth;
	VkRenderingInfo rendering;
	struct xgpu_vulkan_image *size_of = vk.color ? vk.color : vk.depth;

	if (vk.pass_active)
		return 1;
	if (!size_of)
		return 0;
	recording_begin();
	memset(&rendering, 0, sizeof(rendering));
	rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	rendering.renderArea.extent.width = size_of->description.width;
	rendering.renderArea.extent.height = size_of->description.height;
	rendering.layerCount = 1;
	if (vk.color)
	{
		memset(&color, 0, sizeof(color));
		color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		color.imageView = attachment_view(vk.color, vk.layer);
		color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachments = &color;
		vk.color->used = vk.serial;
		if (vk.depth && (vk.depth->description.width < rendering.renderArea.extent.width ||
			vk.depth->description.height < rendering.renderArea.extent.height))
		{
			rendering.renderArea.extent.width = vk.depth->description.width < rendering.renderArea.extent.width ?
				vk.depth->description.width : rendering.renderArea.extent.width;
			rendering.renderArea.extent.height = vk.depth->description.height < rendering.renderArea.extent.height ?
				vk.depth->description.height : rendering.renderArea.extent.height;
		}
	}
	if (vk.depth)
	{
		memset(&depth, 0, sizeof(depth));
		depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		depth.imageView = attachment_view(vk.depth, vk.layer);
		depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		rendering.pDepthAttachment = &depth;
		rendering.pStencilAttachment = &depth;
		vk.depth->used = vk.serial;
	}
	vkCmdBeginRendering(vk.slot->main, &rendering);
	vk.pass_active = 1;
	vk.pass_width = rendering.renderArea.extent.width;
	vk.pass_height = rendering.renderArea.extent.height;
	vk.statistics.passes++;
	if (vk.test_active)
		test_query_begin();
	return 1;
}

static void pass_end(void)
{
	if (!vk.pass_active)
		return;
	test_query_end();
	vkCmdEndRendering(vk.slot->main);
	barrier(vk.slot->main, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
		VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, AFTER_STAGES, AFTER_ACCESS);
	vk.pass_active = 0;
}

void xgpu_vulkan_set_targets(struct xgpu_vulkan_image *color, struct xgpu_vulkan_image *depth, uint32_t layer)
{
	if (color == vk.color && depth == vk.depth && layer == vk.layer)
		return;
	pass_end();
	vk.color = color;
	vk.depth = depth;
	vk.layer = layer;
}

void xgpu_vulkan_clear(const int32_t (*rectangles)[4], uint32_t count, uint32_t parts, const float color[4], float depth,
	uint32_t stencil)
{
	VkClearAttachment attachments[2];
	VkClearRect *rects;
	uint32_t attachment_count = 0, rect_count = 0, index;

	if (!pass_begin())
		return;
	vk.statistics.clears++;
	if ((parts & XGPU_VULKAN_CLEAR_COLOR) && vk.color)
	{
		if ((parts & XGPU_VULKAN_CLEAR_CHANNELS) != XGPU_VULKAN_CLEAR_CHANNELS)
		{
			static int said;

			/* (a clear of some channels alone: not yet) */
			if (!said)
				host_logf(HOST_LOG_WARN, "vulkan: a clear of channels %x alone is drawn as a clear of all of them",
					parts & XGPU_VULKAN_CLEAR_CHANNELS);
			said = 1;
			vk.statistics.clears_as_draws++;
		}
		if (parts & XGPU_VULKAN_CLEAR_CHANNELS)
		{
			attachments[attachment_count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			attachments[attachment_count].colorAttachment = 0;
			memcpy(attachments[attachment_count].clearValue.color.float32, color, 4 * sizeof(float));
			attachment_count++;
		}
	}
	if ((parts & (XGPU_VULKAN_CLEAR_DEPTH | XGPU_VULKAN_CLEAR_STENCIL)) && vk.depth)
	{
		attachments[attachment_count].aspectMask = ((parts & XGPU_VULKAN_CLEAR_DEPTH) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
			((parts & XGPU_VULKAN_CLEAR_STENCIL) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
		attachments[attachment_count].colorAttachment = 0;
		attachments[attachment_count].clearValue.depthStencil.depth = depth;
		attachments[attachment_count].clearValue.depthStencil.stencil = stencil & 0xff;
		attachment_count++;
	}
	if (!attachment_count || !count)
		return;
	rects = malloc(count * sizeof(*rects));
	if (!rects)
		return;
	for (index = 0; index < count; index++)
	{
		int32_t left = rectangles[index][0], top = rectangles[index][1];
		int32_t right = left + rectangles[index][2], bottom = top + rectangles[index][3];

		/* (inside the targets) */
		if (left < 0)
			left = 0;
		if (top < 0)
			top = 0;
		if (right > (int32_t)vk.pass_width)
			right = (int32_t)vk.pass_width;
		if (bottom > (int32_t)vk.pass_height)
			bottom = (int32_t)vk.pass_height;
		if (right <= left || bottom <= top)
			continue;
		rects[rect_count].rect.offset.x = left;
		rects[rect_count].rect.offset.y = top;
		rects[rect_count].rect.extent.width = (uint32_t)(right - left);
		rects[rect_count].rect.extent.height = (uint32_t)(bottom - top);
		rects[rect_count].baseArrayLayer = 0;
		rects[rect_count].layerCount = 1;
		rect_count++;
	}
	if (rect_count)
		vkCmdClearAttachments(vk.slot->main, attachment_count, attachments, rect_count, rects);
	free(rects);
}

/* ---------- draws */

static VkDescriptorPool descriptor_pool_get(void)
{
	struct slot *slot = vk.slot;

	if (slot->descriptor_pool_used >= slot->descriptor_pool_count)
	{
		VkDescriptorPoolSize size;
		VkDescriptorPoolCreateInfo info;
		VkDescriptorPool *grown = realloc(slot->descriptor_pools, (slot->descriptor_pool_count + 1) * sizeof(*grown));

		if (!grown)
			return VK_NULL_HANDLE;
		slot->descriptor_pools = grown;
		size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		size.descriptorCount = SETS_A_POOL * 4;
		memset(&info, 0, sizeof(info));
		info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		info.maxSets = SETS_A_POOL;
		info.poolSizeCount = 1;
		info.pPoolSizes = &size;
		if (!check(vkCreateDescriptorPool(vk.device, &info, NULL, &slot->descriptor_pools[slot->descriptor_pool_count]),
			"vkCreateDescriptorPool"))
			return VK_NULL_HANDLE;
		slot->descriptor_pool_count++;
	}
	return slot->descriptor_pools[slot->descriptor_pool_used];
}

/* the descriptor set of the stages' textures */
static VkDescriptorSet texture_set(const struct xgpu_vulkan_texture textures[4])
{
	VkDescriptorSetAllocateInfo allocate;
	VkDescriptorImageInfo images[4];
	VkWriteDescriptorSet write;
	VkDescriptorSet set = VK_NULL_HANDLE;
	int stage;

	if (vk.last_texture_valid && !memcmp(vk.last_textures, textures, sizeof(vk.last_textures)))
		return vk.last_texture_set;
	for (;;)
	{
		VkDescriptorPool pool = descriptor_pool_get();
		VkResult result;

		if (!pool)
			return VK_NULL_HANDLE;
		memset(&allocate, 0, sizeof(allocate));
		allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocate.descriptorPool = pool;
		allocate.descriptorSetCount = 1;
		allocate.pSetLayouts = &vk.texture_layout;
		result = vkAllocateDescriptorSets(vk.device, &allocate, &set);
		if (result == VK_SUCCESS)
			break;
		if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
		{
			check(result, "vkAllocateDescriptorSets");
			return VK_NULL_HANDLE;
		}
		vk.slot->descriptor_pool_used++;
	}
	for (stage = 0; stage < 4; stage++)
	{
		const struct xgpu_vulkan_texture *texture = &textures[stage];
		struct xgpu_vulkan_image *image = texture->image && texture->image->sampled ? texture->image :
			vk.stand_ins[texture->kind & 3];

		images[stage].sampler = texture->sampler ? texture->sampler : vk.stand_in_sampler;
		images[stage].imageView = image->sampled;
		images[stage].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		image->used = vk.serial;
	}
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = set;
	write.dstBinding = 0;
	write.descriptorCount = 4;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = images;
	vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);
	memcpy(vk.last_textures, textures, sizeof(vk.last_textures));
	vk.last_texture_set = set;
	vk.last_texture_valid = 1;
	return set;
}

static VkPrimitiveTopology topology_of(uint8_t topology)
{
	switch (topology)
	{
	case XGPU_VULKAN_TOPOLOGY_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	case XGPU_VULKAN_TOPOLOGY_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
	case XGPU_VULKAN_TOPOLOGY_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
	case XGPU_VULKAN_TOPOLOGY_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	case XGPU_VULKAN_TOPOLOGY_TRIANGLE_FAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
	default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	}
}

void xgpu_vulkan_draw(const struct xgpu_vulkan_draw *draw)
{
	const struct xgpu_vulkan_dynamic_state *dynamic = draw->dynamic;
	VkCommandBuffer commands;
	VkViewport viewport;
	VkRect2D scissor;
	VkDescriptorSet sets[2];
	uint32_t offsets[2], index;
	int32_t left, top, right, bottom;

	if (!draw->pipeline || !draw->count || dynamic->viewport[2] <= 0 || dynamic->viewport[3] <= 0)
		return;
	/* (the texture set first: it may begin the recording) */
	recording_begin();
	sets[1] = texture_set(draw->textures);
	if (!sets[1] || !pass_begin())
		return;
	commands = vk.slot->main;
	if (vk.bound_pipeline != draw->pipeline)
	{
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, draw->pipeline);
		vk.bound_pipeline = draw->pipeline;
	}
	viewport.x = (float)dynamic->viewport[0];
	viewport.y = (float)dynamic->viewport[1];
	viewport.width = (float)dynamic->viewport[2];
	viewport.height = (float)dynamic->viewport[3];
	viewport.minDepth = dynamic->depth_range[0];
	viewport.maxDepth = dynamic->depth_range[1];
	vkCmdSetViewport(commands, 0, 1, &viewport);
	/* (the scissor rectangle where it is one, else the whole target, as
	OpenGL's scissor test is enabled; inside the targets) */
	if (dynamic->scissor[2] > 0 && dynamic->scissor[3] > 0)
	{
		left = dynamic->scissor[0];
		top = dynamic->scissor[1];
		right = left + dynamic->scissor[2];
		bottom = top + dynamic->scissor[3];
	}
	else
	{
		left = top = 0;
		right = (int32_t)vk.pass_width;
		bottom = (int32_t)vk.pass_height;
	}
	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;
	if (right > (int32_t)vk.pass_width)
		right = (int32_t)vk.pass_width;
	if (bottom > (int32_t)vk.pass_height)
		bottom = (int32_t)vk.pass_height;
	if (right <= left || bottom <= top)
		return;
	scissor.offset.x = left;
	scissor.offset.y = top;
	scissor.extent.width = (uint32_t)(right - left);
	scissor.extent.height = (uint32_t)(bottom - top);
	vkCmdSetScissor(commands, 0, 1, &scissor);
	vkCmdSetDepthBias(commands, dynamic->depth_bias[1], 0.0f, dynamic->depth_bias[0]);
	vkCmdSetBlendConstants(commands, dynamic->blend_color);
	vkCmdSetStencilReference(commands, VK_STENCIL_FACE_FRONT_AND_BACK, dynamic->stencil_reference & 0xff);
	vkCmdSetPrimitiveTopology(commands, topology_of(draw->topology));
	sets[0] = vk.draw_set;
	offsets[0] = (uint32_t)draw->constants.offset;
	offsets[1] = (uint32_t)draw->uniforms.offset;
	vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout, 0, 2, sets, 2, offsets);
	for (index = 0; index < XGPU_VULKAN_BINDINGS; index++)
	{
		if ((draw->bindings & (1u << index)) && draw->vertices[index].buffer)
			vkCmdBindVertexBuffers(commands, index, 1, &draw->vertices[index].buffer, &draw->vertices[index].offset);
	}
	if (draw->indices.buffer)
	{
		vkCmdBindIndexBuffer(commands, draw->indices.buffer, draw->indices.offset, VK_INDEX_TYPE_UINT16);
		vkCmdDrawIndexed(commands, draw->count, 1, draw->first_index, draw->vertex_offset, 0);
	}
	else
	{
		vkCmdDraw(commands, draw->count, 1, 0, 0);
	}
	vk.statistics.draws++;
}

/* ---------- submissions */

/* the recording submitted, with the semaphores of a present (VK_NULL_HANDLE:
none); returns its number */
static uint64_t submit(VkSemaphore wait_semaphore, VkSemaphore signal_semaphore)
{
	VkSubmitInfo info;
	VkTimelineSemaphoreSubmitInfo timeline;
	VkCommandBuffer buffers[2];
	VkSemaphore signals[2];
	uint64_t values[2] = { 0, 0 }, wait_values[1] = { 0 };
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	uint32_t count = 0;
	uint64_t serial;

	if (!vk.recording)
		return vk.serial - 1;
	pass_end();
	if (vk.early_recording)
	{
		barrier(vk.slot->early, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
		check(vkEndCommandBuffer(vk.slot->early), "vkEndCommandBuffer");
		buffers[count++] = vk.slot->early;
	}
	check(vkEndCommandBuffer(vk.slot->main), "vkEndCommandBuffer");
	buffers[count++] = vk.slot->main;
	serial = vk.serial;
	signals[0] = vk.timeline;
	values[0] = serial;
	signals[1] = signal_semaphore;
	memset(&timeline, 0, sizeof(timeline));
	timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
	timeline.signalSemaphoreValueCount = signal_semaphore ? 2 : 1;
	timeline.pSignalSemaphoreValues = values;
	timeline.waitSemaphoreValueCount = wait_semaphore ? 1 : 0;
	timeline.pWaitSemaphoreValues = wait_values;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	info.pNext = &timeline;
	info.waitSemaphoreCount = wait_semaphore ? 1 : 0;
	info.pWaitSemaphores = &wait_semaphore;
	info.pWaitDstStageMask = &wait_stage;
	info.commandBufferCount = count;
	info.pCommandBuffers = buffers;
	info.signalSemaphoreCount = signal_semaphore ? 2 : 1;
	info.pSignalSemaphores = signals;
	check(vkQueueSubmit(vk.queue, 1, &info, VK_NULL_HANDLE), "vkQueueSubmit");
	vk.slot->ring_end = vk.ring_head;
	vk.recording = 0;
	vk.early_recording = 0;
	vk.serial++;
	vk.statistics.submissions++;
	return serial;
}

uint64_t xgpu_vulkan_serial(void)
{
	return vk.serial;
}

uint64_t xgpu_vulkan_fence(void)
{
	return submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
}

/* the upload ring's bytes the done submissions read, free again */
static void ring_reclaim(void)
{
	uint32_t index;

	for (index = 0; index < SLOTS; index++)
	{
		struct slot *slot = &vk.slots[index];

		if (slot->serial && slot->serial <= vk.completed && slot->ring_end > vk.ring_free_from)
			vk.ring_free_from = slot->ring_end;
	}
}

void xgpu_vulkan_wait(uint64_t fence)
{
	serial_wait(fence);
	ring_reclaim();
}

/* ---------- the window */

static void swapchain_forget(void)
{
	uint32_t index;

	if (!vk.swapchain)
		return;
	vkDeviceWaitIdle(vk.device);
	for (index = 0; index < vk.swapchain_image_count; index++)
	{
		if (vk.presentable[index])
			vkDestroySemaphore(vk.device, vk.presentable[index], NULL);
		vk.presentable[index] = VK_NULL_HANDLE;
	}
	vkDestroySwapchainKHR(vk.device, vk.swapchain, NULL);
	vk.swapchain = VK_NULL_HANDLE;
	vk.swapchain_image_count = 0;
}

static int swapchain_make(void)
{
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR formats[64];
	VkPresentModeKHR modes[16];
	VkSwapchainCreateInfoKHR info;
	VkSemaphoreCreateInfo semaphore;
	uint32_t format_count = 64, mode_count = 16, index;
	VkSwapchainKHR old = vk.swapchain;

	if (!check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical, vk.surface, &capabilities),
		"vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
		return 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical, vk.surface, &format_count, formats);
	vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physical, vk.surface, &mode_count, modes);
	if (!format_count)
		return 0;
	vk.swapchain_format = formats[0].format;
	for (index = 0; index < format_count; index++)
	{
		if (formats[index].format == VK_FORMAT_B8G8R8A8_UNORM || formats[index].format == VK_FORMAT_R8G8B8A8_UNORM)
		{
			vk.swapchain_format = formats[index].format;
			break;
		}
	}
	vk.present_mode = VK_PRESENT_MODE_FIFO_KHR;
	if (getenv("HALO_NO_VSYNC"))
	{
		for (index = 0; index < mode_count; index++)
		{
			if (modes[index] == VK_PRESENT_MODE_IMMEDIATE_KHR || (modes[index] == VK_PRESENT_MODE_MAILBOX_KHR &&
				vk.present_mode == VK_PRESENT_MODE_FIFO_KHR))
				vk.present_mode = modes[index];
		}
	}
	vk.swapchain_extent = capabilities.currentExtent;
	if (vk.swapchain_extent.width == 0xffffffffu)
	{
		vk.swapchain_extent.width = vk.window_width ? vk.window_width : 640;
		vk.swapchain_extent.height = vk.window_height ? vk.window_height : 480;
	}
	if (!vk.swapchain_extent.width || !vk.swapchain_extent.height)
		return 0;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	info.surface = vk.surface;
	info.minImageCount = capabilities.minImageCount + 1;
	if (capabilities.maxImageCount && info.minImageCount > capabilities.maxImageCount)
		info.minImageCount = capabilities.maxImageCount;
	if (info.minImageCount > 8)
		info.minImageCount = 8;
	info.imageFormat = vk.swapchain_format;
	info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	for (index = 0; index < format_count; index++)
	{
		if (formats[index].format == vk.swapchain_format)
		{
			info.imageColorSpace = formats[index].colorSpace;
			break;
		}
	}
	info.imageExtent = vk.swapchain_extent;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = capabilities.currentTransform;
	info.compositeAlpha = (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ?
		VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : (VkCompositeAlphaFlagBitsKHR)(capabilities.supportedCompositeAlpha &
		-capabilities.supportedCompositeAlpha);
	info.presentMode = vk.present_mode;
	info.clipped = VK_TRUE;
	info.oldSwapchain = old;
	if (old)
	{
		vkDeviceWaitIdle(vk.device);
		for (index = 0; index < vk.swapchain_image_count; index++)
		{
			if (vk.presentable[index])
				vkDestroySemaphore(vk.device, vk.presentable[index], NULL);
			vk.presentable[index] = VK_NULL_HANDLE;
		}
	}
	if (!check(vkCreateSwapchainKHR(vk.device, &info, NULL, &vk.swapchain), "vkCreateSwapchainKHR"))
	{
		vk.swapchain = VK_NULL_HANDLE;
		if (old)
			vkDestroySwapchainKHR(vk.device, old, NULL);
		return 0;
	}
	if (old)
		vkDestroySwapchainKHR(vk.device, old, NULL);
	vk.swapchain_image_count = 8;
	vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &vk.swapchain_image_count, vk.swapchain_images);
	memset(&semaphore, 0, sizeof(semaphore));
	semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	for (index = 0; index < vk.swapchain_image_count; index++)
		vkCreateSemaphore(vk.device, &semaphore, NULL, &vk.presentable[index]);
	vk.swapchain_stale = 0;
	host_logf(HOST_LOG_INFO, "vulkan: swapchain %ux%u, %u images, present mode %d", vk.swapchain_extent.width,
		vk.swapchain_extent.height, vk.swapchain_image_count, (int)vk.present_mode);
	return 1;
}

void xgpu_vulkan_window_resized(uint32_t width, uint32_t height)
{
	vk.window_width = width;
	vk.window_height = height;
	vk.swapchain_stale = 1;
}

/* the image shown in the window: letterboxed into a swapchain image
(cleared black), then presented */
static uint64_t present(struct xgpu_vulkan_image *image)
{
	VkSemaphore acquired;
	VkCommandBuffer commands;
	VkImageMemoryBarrier transition;
	VkClearColorValue black;
	VkImageSubresourceRange whole;
	VkImageBlit blit;
	VkPresentInfoKHR info;
	VkResult result;
	uint32_t index, width, height, x, y;

	uint64_t serial;

	if (!vk.surface)
		return 0;
	if (vk.swapchain_stale || !vk.swapchain)
	{
		if (!swapchain_make())
			return 0;
	}
	/* (the slot's semaphore free: the recording begun, which waits for the
	slot's last submission) */
	commands = main_commands();
	acquired = vk.acquired[vk.serial % SLOTS];
	result = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
	if (result == VK_ERROR_OUT_OF_DATE_KHR)
	{
		vk.swapchain_stale = 1;
		return 0;
	}
	if (!check(result, "vkAcquireNextImageKHR"))
		return 0;
	memset(&whole, 0, sizeof(whole));
	whole.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	whole.levelCount = 1;
	whole.layerCount = 1;
	memset(&transition, 0, sizeof(transition));
	transition.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	transition.srcAccessMask = 0;
	transition.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	transition.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	transition.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	transition.srcQueueFamilyIndex = transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	transition.image = vk.swapchain_images[index];
	transition.subresourceRange = whole;
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
		&transition);
	memset(&black, 0, sizeof(black));
	black.float32[3] = 1.0f;
	vkCmdClearColorImage(commands, vk.swapchain_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);
	barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_ACCESS_TRANSFER_WRITE_BIT);
	if (image)
	{
		/* letterboxed to the image's shape */
		width = vk.swapchain_extent.width;
		height = (uint32_t)((uint64_t)width * image->description.height / image->description.width);
		if (height > vk.swapchain_extent.height)
		{
			height = vk.swapchain_extent.height;
			width = (uint32_t)((uint64_t)height * image->description.width / image->description.height);
		}
		x = (vk.swapchain_extent.width - width) / 2;
		y = (vk.swapchain_extent.height - height) / 2;
		memset(&blit, 0, sizeof(blit));
		blit.srcSubresource.aspectMask = blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.layerCount = blit.dstSubresource.layerCount = 1;
		blit.srcOffsets[1].x = (int32_t)image->description.width;
		blit.srcOffsets[1].y = (int32_t)image->description.height;
		blit.srcOffsets[1].z = 1;
		blit.dstOffsets[0].x = (int32_t)x;
		blit.dstOffsets[0].y = (int32_t)y;
		blit.dstOffsets[1].x = (int32_t)(x + width);
		blit.dstOffsets[1].y = (int32_t)(y + height);
		blit.dstOffsets[1].z = 1;
		image->used = vk.serial;
		vkCmdBlitImage(commands, image->image, VK_IMAGE_LAYOUT_GENERAL, vk.swapchain_images[index],
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
	}
	transition.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	transition.dstAccessMask = 0;
	transition.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	transition.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL,
		1, &transition);
	serial = submit(acquired, vk.presentable[index]);
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	info.waitSemaphoreCount = 1;
	info.pWaitSemaphores = &vk.presentable[index];
	info.swapchainCount = 1;
	info.pSwapchains = &vk.swapchain;
	info.pImageIndices = &index;
	result = vkQueuePresentKHR(vk.queue, &info);
	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
		vk.swapchain_stale = 1;
	else
		check(result, "vkQueuePresentKHR");
	return serial;
}

void xgpu_vulkan_frame_end(struct xgpu_vulkan_image *image)
{
	uint64_t serial;

	/* (the frame's last work, its tests' queries with it: the present's
	submission, or one of its own) */
	serial = present(image);
	if (!serial)
	{
		recording_begin();
		serial = submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
	}
	if (vk.queries)
	{
		vk.queries->serial = serial;
		vk.queries = NULL;
	}
	/* (frame k - 2's counts, at frame k's end) */
	if (vk.frame >= 2)
		queries_publish(vk.frame - 2);
	vk.frame++;
	vk.statistics.frames++;
	completed_serial();
	garbage_collect();
}

void xgpu_vulkan_statistics(struct xgpu_vulkan_statistics *statistics)
{
	*statistics = vk.statistics;
	memset(&vk.statistics, 0, sizeof(vk.statistics));
}

void xgpu_vulkan_capabilities(struct xgpu_vulkan_capabilities *capabilities)
{
	*capabilities = vk.capabilities;
}

/* ---------- initialization */

static int load_functions(void)
{
	const char *names[] = { "libvulkan.so.1", "libvulkan.so" };
	size_t index;

	for (index = 0; index < sizeof(names) / sizeof(names[0]) && !vk.library; index++)
		vk.library = dlopen(names[index], RTLD_NOW | RTLD_LOCAL);
	if (!vk.library)
	{
		host_logf(HOST_LOG_ERROR, "vulkan: no Vulkan loader (libvulkan.so.1): %s", dlerror());
		return 0;
	}
	vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(vk.library, "vkGetInstanceProcAddr");
	if (!vkGetInstanceProcAddr)
		return 0;
#define LOAD(name) name = (PFN_##name)vkGetInstanceProcAddr(NULL, #name);
	GLOBAL_FUNCTIONS(LOAD)
#undef LOAD
	return vkCreateInstance != NULL;
}

static int has_layer(const char *name)
{
	VkLayerProperties layers[64];
	uint32_t count = 64, index;

	if (vkEnumerateInstanceLayerProperties(&count, layers) < 0)
		return 0;
	for (index = 0; index < count; index++)
	{
		if (!strcmp(layers[index].layerName, name))
			return 1;
	}
	return 0;
}

static int has_instance_extension(const char *name)
{
	VkExtensionProperties extensions[256];
	uint32_t count = 256, index;

	if (vkEnumerateInstanceExtensionProperties(NULL, &count, extensions) < 0)
		return 0;
	for (index = 0; index < count; index++)
	{
		if (!strcmp(extensions[index].extensionName, name))
			return 1;
	}
	return 0;
}

static int instance_make(const struct xgpu_vulkan_options *options)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo info;
	const char *extensions[32];
	const char *layers[1];
	uint32_t extension_count = 0, index, version = VK_API_VERSION_1_0;
	int debug = 0;

	if (vkEnumerateInstanceVersion)
		vkEnumerateInstanceVersion(&version);
	if (version < VK_API_VERSION_1_3)
	{
		host_logf(HOST_LOG_ERROR, "vulkan: the loader has Vulkan %u.%u; the backend needs 1.3", VK_API_VERSION_MAJOR(version),
			VK_API_VERSION_MINOR(version));
		return 0;
	}
	for (index = 0; index < options->instance_extension_count && extension_count < 30; index++)
		extensions[extension_count++] = options->instance_extensions[index];
	vk.validation = options->validation && has_layer("VK_LAYER_KHRONOS_validation");
	if (options->validation && !vk.validation)
		host_logf(HOST_LOG_WARN, "vulkan: no validation layers (VK_LAYER_KHRONOS_validation)");
	if (vk.validation && has_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
	{
		extensions[extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
		debug = 1;
	}
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "OpenCE";
	application.pEngineName = "OpenCE";
	application.apiVersion = VK_API_VERSION_1_3;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	info.pApplicationInfo = &application;
	info.enabledExtensionCount = extension_count;
	info.ppEnabledExtensionNames = extensions;
	layers[0] = "VK_LAYER_KHRONOS_validation";
	info.enabledLayerCount = vk.validation ? 1 : 0;
	info.ppEnabledLayerNames = layers;
	if (!check(vkCreateInstance(&info, NULL, &vk.instance), "vkCreateInstance"))
		return 0;
#define LOAD(name) name = (PFN_##name)vkGetInstanceProcAddr(vk.instance, #name);
	INSTANCE_FUNCTIONS(LOAD)
	SURFACE_FUNCTIONS(LOAD)
	DEBUG_FUNCTIONS(LOAD)
#undef LOAD
	if (debug && vkCreateDebugUtilsMessengerEXT)
	{
		VkDebugUtilsMessengerCreateInfoEXT messenger;

		memset(&messenger, 0, sizeof(messenger));
		messenger.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
		messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		messenger.pfnUserCallback = validation_message;
		vkCreateDebugUtilsMessengerEXT(vk.instance, &messenger, NULL, &vk.messenger);
	}
	return 1;
}

static int device_has_extension(const char *name)
{
	VkExtensionProperties *extensions;
	uint32_t count = 0, index;
	int found = 0;

	vkEnumerateDeviceExtensionProperties(vk.physical, NULL, &count, NULL);
	extensions = malloc(count * sizeof(*extensions));
	if (!extensions)
		return 0;
	vkEnumerateDeviceExtensionProperties(vk.physical, NULL, &count, extensions);
	for (index = 0; index < count && !found; index++)
		found = !strcmp(extensions[index].extensionName, name);
	free(extensions);
	return found;
}

static int format_has(VkFormat format, int buffer, VkFormatFeatureFlags features)
{
	VkFormatProperties properties;

	vkGetPhysicalDeviceFormatProperties(vk.physical, format, &properties);
	return ((buffer ? properties.bufferFeatures : properties.optimalTilingFeatures) & features) == features;
}

/* a physical device of Vulkan 1.3 with a graphics queue (that can present to
the surface, with one) */
static int physical_choose(void)
{
	VkPhysicalDevice devices[16];
	uint32_t count = 16, index, best_score = 0;
	int chosen = -1;

	if (!check(vkEnumeratePhysicalDevices(vk.instance, &count, devices), "vkEnumeratePhysicalDevices"))
		return 0;
	for (index = 0; index < count; index++)
	{
		VkPhysicalDeviceProperties properties;
		VkQueueFamilyProperties families[16];
		uint32_t family_count = 16, family, score;

		vkGetPhysicalDeviceProperties(devices[index], &properties);
		if (properties.apiVersion < VK_API_VERSION_1_3)
			continue;
		vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &family_count, families);
		for (family = 0; family < family_count; family++)
		{
			VkBool32 present = VK_TRUE;

			if (!(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
				continue;
			if (vk.surface)
				vkGetPhysicalDeviceSurfaceSupportKHR(devices[index], family, vk.surface, &present);
			if (!present)
				continue;
			score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 :
				properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
			if (score > best_score)
			{
				best_score = score;
				chosen = (int)index;
				vk.queue_family = family;
			}
			break;
		}
	}
	if (chosen < 0)
	{
		host_logf(HOST_LOG_ERROR, "vulkan: no device of Vulkan 1.3 with a graphics queue%s", vk.surface ?
			" that presents to the window" : "");
		return 0;
	}
	vk.physical = devices[chosen];
	vkGetPhysicalDeviceProperties(vk.physical, &vk.properties);
	vkGetPhysicalDeviceMemoryProperties(vk.physical, &vk.memory_properties);
	return 1;
}

static int device_make(void)
{
	VkPhysicalDeviceFeatures2 available;
	VkPhysicalDeviceVulkan12Features available12;
	VkPhysicalDeviceVulkan13Features available13;
	VkPhysicalDeviceCustomBorderColorFeaturesEXT available_border;
	VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT available_cube, cube;
	VkPhysicalDeviceFeatures2 features;
	VkPhysicalDeviceVulkan12Features features12;
	VkPhysicalDeviceVulkan13Features features13;
	VkPhysicalDeviceCustomBorderColorFeaturesEXT border;
	VkDeviceQueueCreateInfo queue;
	VkDeviceCreateInfo info;
	const char *extensions[8];
	uint32_t extension_count = 0;
	float priority = 1.0f;
	int custom_border = device_has_extension(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
	int non_seamless = device_has_extension(VK_EXT_NON_SEAMLESS_CUBE_MAP_EXTENSION_NAME);
	void **chain;

	memset(&available, 0, sizeof(available));
	memset(&available12, 0, sizeof(available12));
	memset(&available13, 0, sizeof(available13));
	memset(&available_border, 0, sizeof(available_border));
	available.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	available12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	available13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	available_border.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT;
	memset(&available_cube, 0, sizeof(available_cube));
	available_cube.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT;
	available.pNext = &available12;
	available12.pNext = &available13;
	chain = &available13.pNext;
	if (custom_border)
	{
		*chain = &available_border;
		chain = &available_border.pNext;
	}
	if (non_seamless)
		*chain = &available_cube;
	vkGetPhysicalDeviceFeatures2(vk.physical, &available);
	if (!available13.dynamicRendering || !available12.timelineSemaphore || !available12.hostQueryReset)
	{
		host_logf(HOST_LOG_ERROR, "vulkan: %s has no dynamic rendering, timeline semaphores or host query reset",
			vk.properties.deviceName);
		return 0;
	}

	memset(&features, 0, sizeof(features));
	memset(&features12, 0, sizeof(features12));
	memset(&features13, 0, sizeof(features13));
	memset(&border, 0, sizeof(border));
	features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	border.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT;
	features.pNext = &features12;
	features12.pNext = &features13;
	features.features.fillModeNonSolid = available.features.fillModeNonSolid;
	features.features.samplerAnisotropy = available.features.samplerAnisotropy;
	features.features.largePoints = available.features.largePoints;
	features.features.occlusionQueryPrecise = available.features.occlusionQueryPrecise;
	features.features.textureCompressionBC = available.features.textureCompressionBC;
	features.features.fragmentStoresAndAtomics = available.features.fragmentStoresAndAtomics;
	features.features.shaderImageGatherExtended = VK_FALSE;
	features12.timelineSemaphore = VK_TRUE;
	features12.hostQueryReset = VK_TRUE;
	features13.dynamicRendering = VK_TRUE;
	features13.synchronization2 = VK_FALSE;
	if (vk.surface)
		extensions[extension_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
	chain = &features13.pNext;
	if (custom_border && available_border.customBorderColors && available_border.customBorderColorWithoutFormat)
	{
		extensions[extension_count++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
		border.customBorderColors = VK_TRUE;
		border.customBorderColorWithoutFormat = VK_TRUE;
		*chain = &border;
		chain = &border.pNext;
		vk.capabilities.custom_border_colors = 1;
	}
	if (non_seamless && available_cube.nonSeamlessCubeMap)
	{
		memset(&cube, 0, sizeof(cube));
		cube.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT;
		cube.nonSeamlessCubeMap = VK_TRUE;
		extensions[extension_count++] = VK_EXT_NON_SEAMLESS_CUBE_MAP_EXTENSION_NAME;
		*chain = &cube;
		chain = &cube.pNext;
		vk.non_seamless = 1;
	}

	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = vk.queue_family;
	queue.queueCount = 1;
	queue.pQueuePriorities = &priority;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	info.pNext = &features;
	info.queueCreateInfoCount = 1;
	info.pQueueCreateInfos = &queue;
	info.enabledExtensionCount = extension_count;
	info.ppEnabledExtensionNames = extensions;
	if (!check(vkCreateDevice(vk.physical, &info, NULL, &vk.device), "vkCreateDevice"))
		return 0;
#define LOAD(name) name = (PFN_##name)vkGetDeviceProcAddr(vk.device, #name);
	DEVICE_FUNCTIONS(LOAD)
	if (vk.surface)
	{
		SWAPCHAIN_FUNCTIONS(LOAD)
	}
#undef LOAD
	vkGetDeviceQueue(vk.device, vk.queue_family, 0, &vk.queue);

	vk.capabilities.largest_target = vk.properties.limits.maxImageDimension2D;
	{
		VkSampleCountFlags counts = vk.properties.limits.framebufferColorSampleCounts &
			vk.properties.limits.framebufferDepthSampleCounts;

		vk.capabilities.samples = 1;
		while (vk.capabilities.samples < 16 && (counts & (vk.capabilities.samples * 2)))
			vk.capabilities.samples *= 2;
	}
	vk.capabilities.anisotropy = available.features.samplerAnisotropy ?
		(uint32_t)vk.properties.limits.maxSamplerAnisotropy : 1;
	vk.capabilities.block_compression = available.features.textureCompressionBC;
	vk.capabilities.fragment_stores = available.features.fragmentStoresAndAtomics;
	vk.capabilities.scaled_shorts = format_has(VK_FORMAT_R16G16_SSCALED, 1, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) &&
		format_has(VK_FORMAT_R16G16B16_SSCALED, 1, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) &&
		format_has(VK_FORMAT_R16G16B16A16_SSCALED, 1, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) &&
		format_has(VK_FORMAT_R16_SSCALED, 1, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
	{
		VkPhysicalDeviceVulkan12Properties properties12;
		VkPhysicalDeviceProperties2 properties;

		memset(&properties12, 0, sizeof(properties12));
		memset(&properties, 0, sizeof(properties));
		properties12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
		properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		properties.pNext = &properties12;
		vkGetPhysicalDeviceProperties2(vk.physical, &properties);
		vk.nvidia = properties12.driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY;
	}
	vk.depth_format = format_has(VK_FORMAT_D24_UNORM_S8_UINT, 0, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ?
		VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
	host_logf(HOST_LOG_INFO, "vulkan: %s (Vulkan %u.%u.%u, driver %08x)%s%s%s", vk.properties.deviceName,
		VK_API_VERSION_MAJOR(vk.properties.apiVersion), VK_API_VERSION_MINOR(vk.properties.apiVersion),
		VK_API_VERSION_PATCH(vk.properties.apiVersion), vk.properties.driverVersion,
		vk.capabilities.custom_border_colors ? ", custom border colours" : "",
		vk.capabilities.block_compression ? ", BC" : "", vk.validation ? ", validated" : "");
	return 1;
}

static int layouts_make(void)
{
	VkDescriptorSetLayoutBinding draw[5], textures[4];
	VkDescriptorSetLayoutCreateInfo info;
	VkPipelineLayoutCreateInfo layout;
	VkDescriptorSetLayout sets[2];
	int index;

	memset(draw, 0, sizeof(draw));
	draw[0].binding = 0;
	draw[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	draw[0].descriptorCount = 1;
	draw[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	draw[1].binding = 1;
	draw[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	draw[1].descriptorCount = 1;
	draw[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	draw[2].binding = 2;
	draw[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	draw[2].descriptorCount = 1;
	draw[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	draw[3].binding = 3;
	draw[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	draw[3].descriptorCount = 1;
	draw[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	draw[4].binding = 4;
	draw[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	draw[4].descriptorCount = 1;
	draw[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	info.bindingCount = 5;
	info.pBindings = draw;
	if (!check(vkCreateDescriptorSetLayout(vk.device, &info, NULL, &vk.draw_layout), "vkCreateDescriptorSetLayout"))
		return 0;
	memset(textures, 0, sizeof(textures));
	for (index = 0; index < 4; index++)
	{
		textures[index].binding = (uint32_t)index;
		textures[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		textures[index].descriptorCount = 1;
		textures[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	info.bindingCount = 4;
	info.pBindings = textures;
	if (!check(vkCreateDescriptorSetLayout(vk.device, &info, NULL, &vk.texture_layout), "vkCreateDescriptorSetLayout"))
		return 0;
	sets[0] = vk.draw_layout;
	sets[1] = vk.texture_layout;
	memset(&layout, 0, sizeof(layout));
	layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout.setLayoutCount = 2;
	layout.pSetLayouts = sets;
	return check(vkCreatePipelineLayout(vk.device, &layout, NULL, &vk.pipeline_layout), "vkCreatePipelineLayout");
}

/* the draw set: the ring's ranges (dynamic offsets) of the constants and
uniforms, the lights' block and cells, the visibility counters */
static int draw_set_make(void)
{
	VkDescriptorPoolSize sizes[4];
	VkDescriptorPoolCreateInfo pool;
	VkDescriptorSetAllocateInfo allocate;
	VkDescriptorBufferInfo buffers[4];
	VkDescriptorImageInfo cells;
	VkWriteDescriptorSet writes[5];
	struct xgpu_vulkan_image_description description;
	int index;

	sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	sizes[0].descriptorCount = 2;
	sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	sizes[1].descriptorCount = 1;
	sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sizes[2].descriptorCount = 1;
	sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	sizes[3].descriptorCount = 1;
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.maxSets = 1;
	pool.poolSizeCount = 4;
	pool.pPoolSizes = sizes;
	if (!check(vkCreateDescriptorPool(vk.device, &pool, NULL, &vk.draw_pool), "vkCreateDescriptorPool"))
		return 0;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = vk.draw_pool;
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts = &vk.draw_layout;
	if (!check(vkAllocateDescriptorSets(vk.device, &allocate, &vk.draw_set), "vkAllocateDescriptorSets"))
		return 0;
	vk.lights_block = xgpu_vulkan_buffer_create(6192);
	vk.samples = xgpu_vulkan_buffer_create(4 * XGPU_VULKAN_VISIBILITY_SLOTS);
	memset(&description, 0, sizeof(description));
	description.type = XGPU_VULKAN_IMAGE_3D;
	description.format = XGPU_VULKAN_FORMAT_RGBA32_UINT;
	description.usage = XGPU_VULKAN_USAGE_SAMPLED;
	description.width = 32;
	description.height = 32;
	description.depth_or_layers = 16;
	description.levels = 1;
	vk.lights_cells = xgpu_vulkan_image_create(&description);
	if (!vk.lights_block || !vk.samples || !vk.lights_cells)
		return 0;
	memset(buffers, 0, sizeof(buffers));
	buffers[0].buffer = vk.ring;
	buffers[0].range = 192 * 16;
	buffers[1].buffer = vk.ring;
	/* (the uniforms' size: xgpu_spirv.h's struct xgpu_spirv_uniforms, at
	most) */
	buffers[1].range = 1024;
	buffers[2].buffer = vk.lights_block->buffer;
	buffers[2].range = 6192;
	buffers[3].buffer = vk.samples->buffer;
	buffers[3].range = 4 * XGPU_VULKAN_VISIBILITY_SLOTS;
	cells.sampler = vk.stand_in_sampler;
	cells.imageView = vk.lights_cells->sampled;
	cells.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	memset(writes, 0, sizeof(writes));
	for (index = 0; index < 5; index++)
	{
		writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[index].dstSet = vk.draw_set;
		writes[index].dstBinding = (uint32_t)index;
		writes[index].descriptorCount = 1;
	}
	writes[0].descriptorType = writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	writes[0].pBufferInfo = &buffers[0];
	writes[1].pBufferInfo = &buffers[1];
	writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[2].pBufferInfo = &buffers[2];
	writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[3].pImageInfo = &cells;
	writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[4].pBufferInfo = &buffers[3];
	vkUpdateDescriptorSets(vk.device, 5, writes, 0, NULL);
	return 1;
}

/* the stand-ins sampled by a stage with no image: 1 by 1, (0, 0, 0, 1), as
OpenGL's incomplete textures read */
static int stand_ins_make(void)
{
	static const unsigned char texel[4] = { 0, 0, 0, 255 };
	struct xgpu_vulkan_sampler_description sampler;
	int kind, layer;

	memset(&sampler, 0, sizeof(sampler));
	sampler.anisotropy = 1;
	{
		VkSamplerCreateInfo info;

		memset(&info, 0, sizeof(info));
		info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		info.magFilter = info.minFilter = VK_FILTER_NEAREST;
		info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.maxLod = 0.25f;
		if (!check(vkCreateSampler(vk.device, &info, NULL, &vk.stand_in_sampler), "vkCreateSampler"))
			return 0;
	}
	for (kind = 0; kind < 4; kind++)
	{
		struct xgpu_vulkan_image_description description;

		memset(&description, 0, sizeof(description));
		description.type = (uint8_t)kind;
		description.format = XGPU_VULKAN_FORMAT_BGRA8;
		description.usage = XGPU_VULKAN_USAGE_SAMPLED;
		description.width = description.height = 1;
		description.depth_or_layers = 1;
		description.levels = 1;
		vk.stand_ins[kind] = xgpu_vulkan_image_create(&description);
		if (!vk.stand_ins[kind])
			return 0;
		for (layer = 0; layer < (kind == XGPU_VULKAN_IMAGE_CUBE ? 6 : 1); layer++)
			xgpu_vulkan_image_upload(vk.stand_ins[kind], 0, (uint32_t)layer, 0, 0, 0, 1, 1, 1, texel, 4, 4);
	}
	(void)sampler;
	return 1;
}

static void pipeline_cache_make(const char *path)
{
	VkPipelineCacheCreateInfo info;
	void *data = NULL;
	size_t size = 0;
	FILE *file;

	if (path && *path)
	{
		snprintf(vk.cache_path, sizeof(vk.cache_path), "%s", path);
		file = fopen(path, "rb");
		if (file)
		{
			if (!fseek(file, 0, SEEK_END))
			{
				long length = ftell(file);

				if (length > 0 && length < 256L * 1024 * 1024 && !fseek(file, 0, SEEK_SET) && (data = malloc((size_t)length)))
				{
					if (fread(data, 1, (size_t)length, file) == (size_t)length)
						size = (size_t)length;
				}
			}
			fclose(file);
		}
	}
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	info.initialDataSize = size;
	info.pInitialData = size ? data : NULL;
	/* (one the driver does not take, another device's: an empty one) */
	if (vkCreatePipelineCache(vk.device, &info, NULL, &vk.pipeline_cache) != VK_SUCCESS)
	{
		info.initialDataSize = 0;
		info.pInitialData = NULL;
		vkCreatePipelineCache(vk.device, &info, NULL, &vk.pipeline_cache);
	}
	else if (size)
	{
		host_logf(HOST_LOG_INFO, "vulkan: the pipeline cache's %lu bytes read from %s", (unsigned long)size, path);
	}
	free(data);
}

static void pipeline_cache_save(void)
{
	size_t size = 0;
	void *data;
	char temporary[1100];
	FILE *file;

	if (!vk.pipeline_cache || !vk.cache_path[0])
		return;
	if (vkGetPipelineCacheData(vk.device, vk.pipeline_cache, &size, NULL) != VK_SUCCESS || !size)
		return;
	data = malloc(size);
	if (!data)
		return;
	if (vkGetPipelineCacheData(vk.device, vk.pipeline_cache, &size, data) == VK_SUCCESS)
	{
		snprintf(temporary, sizeof(temporary), "%s.tmp", vk.cache_path);
		file = fopen(temporary, "wb");
		if (file)
		{
			int written = fwrite(data, 1, size, file) == size;

			if (!fclose(file) && written)
				rename(temporary, vk.cache_path);
		}
	}
	free(data);
}

int xgpu_vulkan_initialize(const struct xgpu_vulkan_options *options)
{
	VkSemaphoreTypeCreateInfo type;
	VkSemaphoreCreateInfo semaphore;
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo allocate;
	VkQueryPoolCreateInfo queries;
	int index;

	if (vk.device)
		return 1;
	if (!load_functions() || !instance_make(options))
		return 0;
	if (options->surface_create)
	{
		vk.surface = options->surface_create(vk.instance, options->surface_context);
		if (!vk.surface)
			host_logf(HOST_LOG_WARN, "vulkan: no surface for the window: its frames are not shown");
	}
	if (!physical_choose() || !device_make())
		return 0;
	memset(&type, 0, sizeof(type));
	type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
	type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	type.initialValue = 0;
	memset(&semaphore, 0, sizeof(semaphore));
	semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	semaphore.pNext = &type;
	if (!check(vkCreateSemaphore(vk.device, &semaphore, NULL, &vk.timeline), "vkCreateSemaphore"))
		return 0;
	semaphore.pNext = NULL;
	vk.serial = 1;
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	pool.queueFamilyIndex = vk.queue_family;
	for (index = 0; index < SLOTS; index++)
	{
		VkCommandBuffer buffers[2];

		if (!check(vkCreateCommandPool(vk.device, &pool, NULL, &vk.slots[index].pool), "vkCreateCommandPool"))
			return 0;
		memset(&allocate, 0, sizeof(allocate));
		allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocate.commandPool = vk.slots[index].pool;
		allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate.commandBufferCount = 2;
		if (!check(vkAllocateCommandBuffers(vk.device, &allocate, buffers), "vkAllocateCommandBuffers"))
			return 0;
		vk.slots[index].main = buffers[0];
		vk.slots[index].early = buffers[1];
		if (vk.surface)
			vkCreateSemaphore(vk.device, &semaphore, NULL, &vk.acquired[index]);
	}
	if (!buffer_make(RING_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1, &vk.ring, &vk.ring_memory,
		(void **)&vk.ring_mapped))
		return 0;
	memset(&queries, 0, sizeof(queries));
	queries.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	queries.queryType = VK_QUERY_TYPE_OCCLUSION;
	queries.queryCount = QUERIES_A_FRAME;
	for (index = 0; index < QUERY_FRAMES; index++)
	{
		if (!check(vkCreateQueryPool(vk.device, &queries, NULL, &vk.query_frames[index].pool), "vkCreateQueryPool"))
			return 0;
		vkResetQueryPool(vk.device, vk.query_frames[index].pool, 0, QUERIES_A_FRAME);
	}
	if (!layouts_make() || !stand_ins_make() || !draw_set_make())
		return 0;
	pipeline_cache_make(options->cache_path);
	return 1;
}

void xgpu_vulkan_shutdown(void)
{
	if (!vk.device)
		return;
	if (vk.recording)
		xgpu_vulkan_fence();
	vkDeviceWaitIdle(vk.device);
	pipeline_cache_save();
	if (vk.validation)
		host_logf(HOST_LOG_INFO, "vulkan: %u validation messages", vk.validation_messages);
}
