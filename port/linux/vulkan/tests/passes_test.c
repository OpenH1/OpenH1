/*
PASSES_TEST.C

The real Vulkan core over a recording-only Vulkan: no driver or GPU.
Checks attachment load actions, clears inside a pass, multiview layer
selection, sampling an array's first layer as a 2D image, and capability
requirements on devices with optional depth formats or query precision.
Also checks packed vertex bindings and transfer/submission dependencies,
the texture sets a submission reuses, the order of the enhanced dynamic
lights' uploads among the draws, and anti-aliasing's passes: their order,
attachments, barriers, samples and uniforms (xgpu_vulkan_post.c); and the
clears of some of the colour's channels: drawn through their write mask,
over each rectangle, in each view, while every channel's are cleared. And
the window: a lost surface made again, the app in the background, the
present mode of the swap interval, frames turned for the display; the
pipeline cache written only when pipelines were made; and the VR effects'
passes that sample their depth: begun after the depth's writes, storing no
depth or stencil, ended by a draw or clear that writes them.
*/

#include "../xgpu_vulkan.c"
#include "../xgpu_vulkan_post.c"
#include "../xgpu_vulkan_post_shaders.c"
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>

static struct xgpu_vulkan_image color_target, depth_target, stand_in;
static struct slot recording;
static unsigned int begins, clears, created_views;
static VkRenderingInfo began;
static VkRenderingAttachmentInfo began_color, began_depth, began_stencil;
static VkClearAttachment cleared[2];
static VkClearRect cleared_rect;
static VkImageViewCreateInfo last_view;
static VkDescriptorImageInfo written_images[4];
static VkFormatFeatureFlags depth24_features, depth32_features;
static unsigned int capability_errors;
static unsigned int ended_queries;
static unsigned int barriers, copies, bound_vertices;
static unsigned int allocated_sets, set_updates, pool_resets;
static VkMemoryBarrier last_memory;
static VkPipelineStageFlags last_source, last_destination;

static void VKAPI_CALL fake_end_query(VkCommandBuffer command, VkQueryPool pool, uint32_t query)
{
	(void)command; (void)pool; (void)query;
	ended_queries++;
}

void host_logf(int priority, const char *format, ...)
{
	(void)format;
	if (priority == HOST_LOG_ERROR)
		capability_errors++;
}

void host_fatal(const char *format, ...)
{
	(void)format;
	abort();
}

static void VKAPI_CALL fake_begin(VkCommandBuffer command, const VkRenderingInfo *info)
{
	(void)command;
	begins++;
	began = *info;
	if (info->pColorAttachments)
		began_color = *info->pColorAttachments;
	if (info->pDepthAttachment)
		began_depth = *info->pDepthAttachment;
	if (info->pStencilAttachment)
		began_stencil = *info->pStencilAttachment;
}

static void VKAPI_CALL fake_end(VkCommandBuffer command)
{
	(void)command;
}

static void VKAPI_CALL fake_barrier(VkCommandBuffer command, VkPipelineStageFlags source, VkPipelineStageFlags destination,
	VkDependencyFlags dependency, uint32_t memory_count, const VkMemoryBarrier *memory, uint32_t buffer_count,
	const VkBufferMemoryBarrier *buffers, uint32_t image_count, const VkImageMemoryBarrier *images)
{
	(void)command; (void)source; (void)destination; (void)dependency;
	(void)memory_count; (void)memory; (void)buffer_count; (void)buffers; (void)image_count; (void)images;
	barriers++;
	if (memory_count)
		last_memory = memory[0];
	last_source = source;
	last_destination = destination;
}

static void VKAPI_CALL fake_clear(VkCommandBuffer command, uint32_t attachment_count, const VkClearAttachment *attachments,
	uint32_t rect_count, const VkClearRect *rects)
{
	(void)command;
	assert(attachment_count <= 2 && rect_count > 0);
	clears++;
	memcpy(cleared, attachments, attachment_count * sizeof(*attachments));
	cleared_rect = rects[0];
}

static VkResult VKAPI_CALL fake_view(VkDevice device, const VkImageViewCreateInfo *info,
	const VkAllocationCallbacks *allocator, VkImageView *view)
{
	(void)device; (void)allocator;
	last_view = *info;
	*view = (VkImageView)(uintptr_t)(++created_views + 100);
	return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_allocate(VkDevice device, const VkDescriptorSetAllocateInfo *info, VkDescriptorSet *set)
{
	(void)device; (void)info;
	*set = (VkDescriptorSet)(uintptr_t)(50 + allocated_sets++);
	return VK_SUCCESS;
}

static void VKAPI_CALL fake_update(VkDevice device, uint32_t count, const VkWriteDescriptorSet *writes,
	uint32_t copies, const VkCopyDescriptorSet *copy)
{
	uint32_t index;

	(void)device; (void)copies; (void)copy;
	assert(count == 4);
	set_updates++;
	for (index = 0; index < count; index++)
	{
		assert(writes[index].dstBinding == index && writes[index].descriptorCount == 1);
		assert(writes[index].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
		written_images[index] = *writes[index].pImageInfo;
	}
}

static void VKAPI_CALL fake_format_properties(VkPhysicalDevice physical, VkFormat format, VkFormatProperties *properties)
{
	(void)physical;
	memset(properties, 0, sizeof(*properties));
	if (format == VK_FORMAT_D24_UNORM_S8_UINT)
		properties->optimalTilingFeatures = depth24_features;
	else if (format == VK_FORMAT_D32_SFLOAT_S8_UINT)
		properties->optimalTilingFeatures = depth32_features;
	else
		assert(0);
}

static void capabilities_test(void)
{
	VkPhysicalDeviceFeatures2 features = { 0 };
	VkPhysicalDeviceVulkan12Features features12 = { 0 };
	VkPhysicalDeviceVulkan13Features features13 = { 0 };
	VkFormatFeatureFlags attachment = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
	VkFormatFeatureFlags sampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

	vkGetPhysicalDeviceFormatProperties = fake_format_properties;
	depth24_features = depth32_features = attachment | sampled;
	assert(depth_format_choose() && vk.depth_format == VK_FORMAT_D24_UNORM_S8_UINT);
	/* D24 may draw correctly but not support the scope's sampled reads. */
	depth24_features = attachment;
	assert(depth_format_choose() && vk.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT);
	depth24_features = 0;
	assert(depth_format_choose() && vk.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT);
	depth24_features = attachment | sampled;
	depth32_features = 0;
	assert(depth_format_choose() && vk.depth_format == VK_FORMAT_D24_UNORM_S8_UINT);
	capability_errors = 0;
	depth24_features = attachment;
	depth32_features = sampled;
	assert(!depth_format_choose() && vk.depth_format == VK_FORMAT_UNDEFINED);
	assert(capability_errors == 1);
	depth24_features = depth32_features = 0;
	assert(!depth_format_choose() && capability_errors == 2);

	features.features.occlusionQueryPrecise = VK_TRUE;
	features12.timelineSemaphore = features12.hostQueryReset = VK_TRUE;
	features13.dynamicRendering = VK_TRUE;
	assert(required_features(&features, &features12, &features13));
	features.features.occlusionQueryPrecise = VK_FALSE;
	assert(!required_features(&features, &features12, &features13));
	features.features.occlusionQueryPrecise = VK_TRUE;
	features12.timelineSemaphore = VK_FALSE;
	assert(!required_features(&features, &features12, &features13));
	features12.timelineSemaphore = VK_TRUE;
	features12.hostQueryReset = VK_FALSE;
	assert(!required_features(&features, &features12, &features13));
	features12.hostQueryReset = VK_TRUE;
	features13.dynamicRendering = VK_FALSE;
	assert(!required_features(&features, &features12, &features13));
	assert(capability_errors == 6);
}

static void reset(void)
{
	int index;

	free(color_target.attachments);
	free(depth_target.attachments);
	free(vk.garbage);
	for (index = 0; index < SLOTS; index++)
		free(vk.slots[index].texture_sets);
	free(recording.texture_sets);
	memset(&vk, 0, sizeof(vk));
	memset(&color_target, 0, sizeof(color_target));
	memset(&depth_target, 0, sizeof(depth_target));
	memset(&recording, 0, sizeof(recording));
	memset(&began, 0, sizeof(began));
	memset(&began_color, 0, sizeof(began_color));
	memset(&began_depth, 0, sizeof(began_depth));
	memset(&began_stencil, 0, sizeof(began_stencil));
	begins = clears = created_views = 0;
	color_target.description.width = depth_target.description.width = 64;
	color_target.description.height = depth_target.description.height = 48;
	color_target.description.type = depth_target.description.type = XGPU_VULKAN_IMAGE_2D_ARRAY;
	color_target.description.depth_or_layers = depth_target.description.depth_or_layers = 4;
	color_target.description.levels = depth_target.description.levels = 1;
	color_target.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	depth_target.aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	vk.recording = 1;
	vk.serial = 11;
	vk.slot = &recording;
	vk.color = &color_target;
	vk.depth = &depth_target;
	vk.views = 1;
	stand_in.sampled = (VkImageView)(uintptr_t)20;
	for (index = 0; index < 4; index++)
		vk.stand_ins[index] = &stand_in;
	vkCmdBeginRendering = fake_begin;
	vkCmdEndRendering = fake_end;
	vkCmdEndQuery = fake_end_query;
	vkCmdClearAttachments = fake_clear;
	vkCmdPipelineBarrier = fake_barrier;
	vkCreateImageView = fake_view;
	vkAllocateDescriptorSets = fake_allocate;
	vkUpdateDescriptorSets = fake_update;
}

static VkResult VKAPI_CALL fake_pipeline(VkDevice device, VkPipelineCache cache, uint32_t count,
	const VkGraphicsPipelineCreateInfo *info, const VkAllocationCallbacks *allocator, VkPipeline *pipeline)
{
	const VkPipelineVertexInputStateCreateInfo *input = info->pVertexInputState;
	const uint32_t expected[4] = { 1, 0, 2, 2 };
	uint32_t index;

	(void)device; (void)cache; (void)allocator;
	assert(count == 1 && input->vertexBindingDescriptionCount == 3 && input->vertexAttributeDescriptionCount == 4);
	for (index = 0; index < 3; index++)
		assert(input->pVertexBindingDescriptions[index].binding == index);
	assert(input->pVertexBindingDescriptions[0].stride == 12);
	assert(input->pVertexBindingDescriptions[1].stride == 32);
	assert(input->pVertexBindingDescriptions[2].stride == 0);
	for (index = 0; index < 4; index++)
	{
		assert(input->pVertexAttributeDescriptions[index].location == index);
		assert(input->pVertexAttributeDescriptions[index].binding == expected[index]);
		assert(input->pVertexAttributeDescriptions[index].offset == index * 16);
	}
	*pipeline = (VkPipeline)(uintptr_t)10;
	return VK_SUCCESS;
}

static void VKAPI_CALL fake_bind_vertices(VkCommandBuffer command, uint32_t first, uint32_t count,
	const VkBuffer *buffers, const VkDeviceSize *offsets)
{
	const uint32_t sources[3] = { 3, 15, XGPU_VULKAN_BINDING_CONSTANT };

	(void)command;
	assert(count == 1 && first == bound_vertices && first < 3);
	assert(buffers[0] == (VkBuffer)(uintptr_t)(100 + sources[first]));
	assert(offsets[0] == sources[first] * 16);
	bound_vertices++;
}

static void VKAPI_CALL fake_viewport(VkCommandBuffer c, uint32_t first, uint32_t count, const VkViewport *v)
{ (void)c; (void)first; (void)count; (void)v; }
static void VKAPI_CALL fake_scissor(VkCommandBuffer c, uint32_t first, uint32_t count, const VkRect2D *r)
{ (void)c; (void)first; (void)count; (void)r; }
static void VKAPI_CALL fake_bias(VkCommandBuffer c, float constant, float clamp, float slope)
{ (void)c; (void)constant; (void)clamp; (void)slope; }
static void VKAPI_CALL fake_blend(VkCommandBuffer c, const float color[4])
{ (void)c; (void)color; }
static void VKAPI_CALL fake_stencil(VkCommandBuffer c, VkStencilFaceFlags face, uint32_t reference)
{ (void)c; (void)face; (void)reference; }
static void VKAPI_CALL fake_topology(VkCommandBuffer c, VkPrimitiveTopology topology)
{ (void)c; (void)topology; }
static void VKAPI_CALL fake_bind_sets(VkCommandBuffer c, VkPipelineBindPoint point, VkPipelineLayout layout,
	uint32_t first, uint32_t count, const VkDescriptorSet *sets, uint32_t dynamic_count, const uint32_t *offsets)
{ (void)c; (void)point; (void)layout; (void)first; (void)count; (void)sets; (void)dynamic_count; (void)offsets; }
static void VKAPI_CALL fake_draw(VkCommandBuffer c, uint32_t vertices, uint32_t instances, uint32_t first, uint32_t instance)
{ (void)c; (void)vertices; (void)instances; (void)first; (void)instance; assert(bound_vertices == 3); }

static void vertex_input_test(void)
{
	struct xgpu_vulkan_pipeline_description description = { 0 };
	struct xgpu_vulkan_draw draw = { 0 };
	struct xgpu_vulkan_dynamic_state dynamic = { 0 };
	VkDescriptorPool pool = (VkDescriptorPool)(uintptr_t)1;
	const uint32_t sources[4] = { 15, 3, XGPU_VULKAN_BINDING_CONSTANT, XGPU_VULKAN_BINDING_CONSTANT };
	VkResult result;
	double elapsed;
	uint32_t index;

	reset();
	description.vertex_shader = description.pixel_shader = (VkShaderModule)(uintptr_t)1;
	for (index = 0; index < 4; index++)
	{
		description.attributes[index].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		description.attributes[index].binding = sources[index];
		description.attributes[index].offset = index * 16;
		draw.bindings |= 1u << sources[index];
		draw.vertices[sources[index]].buffer = (VkBuffer)(uintptr_t)(100 + sources[index]);
		draw.vertices[sources[index]].offset = sources[index] * 16;
	}
	description.strides[3] = 12;
	description.strides[15] = 32;
	vkCreateGraphicsPipelines = fake_pipeline;
	draw.pipeline = pipeline_build(&description, &result, &elapsed);
	assert(draw.pipeline && result == VK_SUCCESS);
	vk.bound_pipeline = draw.pipeline;
	dynamic.viewport[2] = 64;
	dynamic.viewport[3] = 48;
	draw.dynamic = &dynamic;
	draw.count = 3;
	recording.descriptor_pools = &pool;
	recording.descriptor_pool_count = 1;
	vkCmdBindVertexBuffers = fake_bind_vertices;
	vkCmdSetViewport = fake_viewport;
	vkCmdSetScissor = fake_scissor;
	vkCmdSetDepthBias = fake_bias;
	vkCmdSetBlendConstants = fake_blend;
	vkCmdSetStencilReference = fake_stencil;
	vkCmdSetPrimitiveTopology = fake_topology;
	vkCmdBindDescriptorSets = fake_bind_sets;
	vkCmdDraw = fake_draw;
	bound_vertices = 0;
	xgpu_vulkan_draw(&draw);
	assert(bound_vertices == 3);
	/* A draw that reads the dynamic lights orders their next upload after
	it; others leave it free to go before the submission's draws. */
	{
		static struct xgpu_vulkan_buffer block;
		static struct xgpu_vulkan_image cells;

		vk.lights_block = &block;
		vk.lights_cells = &cells;
		bound_vertices = 0;
		xgpu_vulkan_draw(&draw);
		assert(block.used == 0 && cells.used == 0);
		draw.dynamic_lights = 1;
		bound_vertices = 0;
		xgpu_vulkan_draw(&draw);
		assert(block.used == vk.serial && cells.used == vk.serial);
	}
	/* The VR mode's effects sample the depth attachment they are tested
	against: in a pass of their own that writes neither depth nor stencil
	(and stores neither), begun after the depth's writes, which draws that
	write neither share; a draw that writes depth ends it, as a clear of
	depth does. */
	{
		const float black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		unsigned int before;

		draw.dynamic_lights = 0;
		draw.writes_depth = 1;
		bound_vertices = 0;
		xgpu_vulkan_draw(&draw);
		before = begins;
		assert(vk.pass_active && !vk.pass_reads_depth);
		draw.writes_depth = 0;
		draw.reads_depth = 1;
		bound_vertices = 0;
		xgpu_vulkan_draw(&draw);
		assert(begins == before + 1 && vk.pass_reads_depth);
		assert(began_depth.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && began_depth.storeOp == VK_ATTACHMENT_STORE_OP_NONE);
		assert(began_stencil.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && began_stencil.storeOp == VK_ATTACHMENT_STORE_OP_NONE);
		assert(began_color.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && began_color.storeOp == VK_ATTACHMENT_STORE_OP_STORE);
		bound_vertices = 0;
		xgpu_vulkan_draw(&draw);
		draw.reads_depth = 0;
		bound_vertices = 0;
		xgpu_vulkan_draw(&draw);
		assert(begins == before + 1 && vk.pass_reads_depth);
		draw.writes_depth = 1;
		bound_vertices = 0;
		xgpu_vulkan_draw(&draw);
		assert(begins == before + 2 && !vk.pass_reads_depth && began_depth.storeOp == VK_ATTACHMENT_STORE_OP_STORE);
		draw.writes_depth = 0;
		draw.reads_depth = 1;
		bound_vertices = 0;
		xgpu_vulkan_draw(&draw);
		assert(begins == before + 3 && vk.pass_reads_depth);
		xgpu_vulkan_clear((const int32_t[][4]){ { 0, 0, 8, 8 } }, 1, XGPU_VULKAN_CLEAR_DEPTH, black, 1.0f, 0);
		assert(begins == before + 4 && !vk.pass_reads_depth && began_depth.storeOp == VK_ATTACHMENT_STORE_OP_STORE);
		/* (targets changed: the next pass is an ordinary one) */
		bound_vertices = 0;
		xgpu_vulkan_draw(&draw);
		xgpu_vulkan_set_target_views(&color_target, NULL, 0, 1);
		assert(!vk.pass_active && !vk.pass_reads_depth);
		draw.reads_depth = 0;
	}
}

static unsigned int light_copies, light_image_copies;
static VkCommandBuffer light_commands;

static void VKAPI_CALL fake_copy_lights(VkCommandBuffer command, VkBuffer source, VkBuffer destination,
	uint32_t count, const VkBufferCopy *regions)
{
	(void)source; (void)destination;
	assert(count == 1 && regions[0].dstOffset == 0 && regions[0].size == 6192);
	light_commands = command;
	light_copies++;
}

static void VKAPI_CALL fake_copy_cells(VkCommandBuffer command, VkBuffer source, VkImage destination,
	VkImageLayout layout, uint32_t count, const VkBufferImageCopy *regions)
{
	(void)source; (void)destination;
	assert(layout == VK_IMAGE_LAYOUT_GENERAL && count == 1);
	assert(regions[0].imageExtent.width == 32 && regions[0].imageExtent.height == 32 &&
		regions[0].imageExtent.depth == 16);
	assert(regions[0].imageSubresource.baseArrayLayer == 0 && regions[0].imageSubresource.layerCount == 1);
	assert(command == light_commands);
	light_image_copies++;
}

/* The enhanced dynamic lights' block and cells: written before the
submission's draws where none of them read the lights, in their order
where one did. */
static void dynamic_lights_test(void)
{
	static struct xgpu_vulkan_buffer block;
	static struct xgpu_vulkan_image cells;
	static unsigned char ring[6192 + 32 * 32 * 16 * 16 + 64], data[6192], grid[32 * 32 * 16 * 16];

	reset();
	vk.slot->main = (VkCommandBuffer)(uintptr_t)1;
	vk.slot->early = (VkCommandBuffer)(uintptr_t)2;
	vk.early_recording = 1;
	vk.ring_mapped = ring;
	block.size = sizeof(data);
	block.buffer = (VkBuffer)(uintptr_t)3;
	cells.description.type = XGPU_VULKAN_IMAGE_3D;
	cells.description.format = XGPU_VULKAN_FORMAT_RGBA32_UINT;
	cells.description.width = cells.description.height = 32;
	cells.description.depth_or_layers = 16;
	cells.description.levels = 1;
	cells.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	vk.lights_block = &block;
	vk.lights_cells = &cells;
	data[0] = 7;
	grid[sizeof(grid) - 1] = 9;
	vkCmdCopyBuffer = fake_copy_lights;
	vkCmdCopyBufferToImage = fake_copy_cells;
	light_copies = light_image_copies = 0;
	xgpu_vulkan_dynamic_lights_upload(data, sizeof(data), grid);
	assert(light_copies == 1 && light_image_copies == 1 && light_commands == vk.slot->early);
	assert(vk.statistics.uploads_early == 2 && vk.statistics.uploads_ordered == 0);
	assert(ring[0] == 7 && ring[6192 + sizeof(grid) - 1] == 9);
	block.used = cells.used = vk.serial;
	vk.ring_head = 0;
	xgpu_vulkan_dynamic_lights_upload(data, sizeof(data), grid);
	assert(light_copies == 2 && light_image_copies == 2 && light_commands == vk.slot->main);
	assert(vk.statistics.uploads_ordered == 2);
}

static void VKAPI_CALL fake_copy_buffer(VkCommandBuffer command, VkBuffer source, VkBuffer destination,
	uint32_t count, const VkBufferCopy *regions)
{
	(void)command; (void)source; (void)destination;
	assert(count == 1 && regions[0].dstOffset == 0 && regions[0].size == 16);
	/* A second write to the same bytes must follow the first's barrier. */
	assert(barriers == copies);
	copies++;
}

static void uploads_test(void)
{
	struct xgpu_vulkan_buffer buffer = { 0 };
	unsigned char ring[64], data[16] = { 1 };

	reset();
	vk.slot->main = (VkCommandBuffer)(uintptr_t)1;
	vk.slot->early = (VkCommandBuffer)(uintptr_t)2;
	vk.early_recording = 1;
	vk.ring_mapped = ring;
	buffer.size = sizeof(ring);
	buffer.buffer = (VkBuffer)(uintptr_t)3;
	vkCmdCopyBuffer = fake_copy_buffer;
	barriers = copies = 0;
	xgpu_vulkan_buffer_write(&buffer, 0, data, sizeof(data), 0);
	xgpu_vulkan_buffer_write(&buffer, 0, data, sizeof(data), 0);
	assert(copies == 2 && barriers == 2);
	assert(last_source == VK_PIPELINE_STAGE_TRANSFER_BIT && last_destination == VK_PIPELINE_STAGE_TRANSFER_BIT);
	assert(last_memory.srcAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT);
	assert(last_memory.dstAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT);
	assert(!memcmp(ring, data, sizeof(data)) && !memcmp(ring + 16, data, sizeof(data)));
	buffer.used = vk.serial;
	xgpu_vulkan_buffer_write(&buffer, 0, data, sizeof(data), 0);
	assert(copies == 3 && barriers == 3 && last_destination == AFTER_STAGES);
	assert(last_memory.dstAccessMask == AFTER_ACCESS);
}

static VkResult VKAPI_CALL fake_reset_pool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags)
{ (void)device; (void)pool; (void)flags; return VK_SUCCESS; }

static VkResult VKAPI_CALL fake_begin_commands(VkCommandBuffer command, const VkCommandBufferBeginInfo *info)
{ (void)command; (void)info; return VK_SUCCESS; }

static void submissions_test(void)
{
	reset();
	vk.recording = 0;
	vkResetCommandPool = fake_reset_pool;
	vkBeginCommandBuffer = fake_begin_commands;
	barriers = 0;
	recording_begin();
	/* Even without an early command buffer, the next frame cannot write
	its target while the previous presentation blit still reads it. */
	assert(barriers == 1 && vk.recording && !vk.early_recording);
	assert(last_source == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
	assert(last_destination == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
	assert(last_memory.srcAccessMask == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT));
	assert(last_memory.dstAccessMask == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT));
	recording_begin();
	assert(barriers == 1);
}

static VkResult VKAPI_CALL fake_reset_descriptors(VkDevice device, VkDescriptorPool pool, VkDescriptorPoolResetFlags flags)
{ (void)device; (void)pool; (void)flags; pool_resets++; return VK_SUCCESS; }

static VkResult VKAPI_CALL fake_counter(VkDevice device, VkSemaphore semaphore, uint64_t *value)
{ (void)device; (void)semaphore; *value = vk.serial; return VK_SUCCESS; }

/* Draws that come back to earlier textures reuse their submission's sets.
Another sampler, an image made where a destroyed one was, or the slot's
pools reset never give back a set written before. */
static void texture_sets_test(void)
{
	static struct xgpu_vulkan_image first, second;
	struct xgpu_vulkan_texture a[4], b[4];
	VkDescriptorPool pools[2] = { (VkDescriptorPool)(uintptr_t)7, (VkDescriptorPool)(uintptr_t)8 };
	VkDescriptorSet set_a, set_b, set;
	struct slot *slot;

	reset();
	vk.recording = 0;
	vkResetCommandPool = fake_reset_pool;
	vkBeginCommandBuffer = fake_begin_commands;
	vkResetDescriptorPool = fake_reset_descriptors;
	vkGetSemaphoreCounterValue = fake_counter;
	slot = &vk.slots[vk.serial % SLOTS];
	slot->descriptor_pools = pools;
	slot->descriptor_pool_count = 2;
	pool_resets = 0;
	recording_begin();
	assert(vk.slot == slot && !pool_resets);
	memset(&first, 0, sizeof(first));
	memset(&second, 0, sizeof(second));
	first.sampled = (VkImageView)(uintptr_t)31;
	first.number = 1;
	second.sampled = (VkImageView)(uintptr_t)32;
	second.number = 2;
	memset(a, 0, sizeof(a));
	memset(b, 0, sizeof(b));
	a[0].image = &first;
	b[0].image = &second;
	a[0].kind = b[0].kind = XGPU_VULKAN_IMAGE_2D;
	a[0].sampler = b[0].sampler = (VkSampler)(uintptr_t)5;
	allocated_sets = set_updates = 0;
	set_a = texture_set(a);
	set_b = texture_set(b);
	assert(set_a && set_b && set_a != set_b && set_updates == 2);
	assert(texture_set(a) == set_a && texture_set(b) == set_b);
	assert(texture_set(a) == set_a && set_updates == 2 && allocated_sets == 2);
	assert(vk.statistics.texture_sets_written == 2 && vk.statistics.texture_sets_reused == 3);
	assert(written_images[0].imageView == second.sampled);

	a[0].sampler = (VkSampler)(uintptr_t)6;
	set = texture_set(a);
	assert(set != set_a && set != set_b && set_updates == 3);
	assert(written_images[0].sampler == (VkSampler)(uintptr_t)6);
	a[0].sampler = (VkSampler)(uintptr_t)5;
	assert(texture_set(a) == set_a && set_updates == 3);

	/* (first destroyed and another image made at its address, as
	xgpu_vulkan_image_create numbers it; the last set is b's, as
	xgpu_vulkan_image_destroy forgets the last set when it had first) */
	assert(texture_set(b) == set_b);
	first.number = 3;
	first.sampled = (VkImageView)(uintptr_t)33;
	set = texture_set(a);
	assert(set != set_a && set != set_b && set_updates == 4);
	assert(written_images[0].imageView == (VkImageView)(uintptr_t)33);
	assert(texture_set(b) == set_b && texture_set(a) == set && set_updates == 4);

	/* The slot's next submission: only the pool allocated from is reset,
	and its sets are forgotten with it. */
	vk.recording = 0;
	vk.serial += SLOTS;
	recording_begin();
	assert(vk.slot == slot && pool_resets == 1 && !slot->texture_set_count && !slot->descriptor_pool_dirty);
	assert(texture_set(b) && set_updates == 5 && allocated_sets == 5);
	assert(texture_set(a) && set_updates == 6);
	assert(texture_set(b) && set_updates == 6);
	slot->descriptor_pools = NULL;
	slot->descriptor_pool_count = 0;
	reset();
}

/* ---------- anti-aliasing's passes */

/* what the passes record, in order: C a copy, X a barrier, B a pass begun
(c its colour cleared, l loaded), D a draw, E a pass ended */
static char post_events[64];
static VkImageCopy post_copied;
static VkImageView post_attachments[4];
static VkRenderingAttachmentInfo post_began[4];
static uint32_t post_begins, post_draws, post_updates, post_masks[4], post_pipelines;
static VkDescriptorImageInfo post_sampled[4][4];
static VkViewport post_viewports[4];
static VkRect2D post_scissors[4];
static uint32_t post_offsets[4][2];
static VkPipelineStageFlags post_barrier_sources[8], post_barrier_destinations[8];
static VkAccessFlags post_barrier_access[8];
static uint32_t post_barriers;
static unsigned char post_ring[65536];
static VkDescriptorPool post_pool = (VkDescriptorPool)(uintptr_t)7;

static void post_event(char event)
{
	size_t length = strlen(post_events);

	assert(length + 1 < sizeof(post_events));
	post_events[length] = event;
}

static void VKAPI_CALL post_copy(VkCommandBuffer command, VkImage source, VkImageLayout source_layout,
	VkImage destination, VkImageLayout destination_layout, uint32_t count, const VkImageCopy *regions)
{
	(void)command;
	assert(source == (VkImage)(uintptr_t)1 && destination == (VkImage)(uintptr_t)2);
	assert(source_layout == VK_IMAGE_LAYOUT_GENERAL && destination_layout == VK_IMAGE_LAYOUT_GENERAL && count == 1);
	post_copied = regions[0];
	post_event('C');
}

static void VKAPI_CALL post_barrier(VkCommandBuffer command, VkPipelineStageFlags source, VkPipelineStageFlags destination,
	VkDependencyFlags dependency, uint32_t memory_count, const VkMemoryBarrier *memory, uint32_t buffer_count,
	const VkBufferMemoryBarrier *buffers, uint32_t image_count, const VkImageMemoryBarrier *images)
{
	(void)command; (void)dependency; (void)buffer_count; (void)buffers; (void)image_count; (void)images;
	assert(memory_count == 1 && post_barriers < 8);
	post_barrier_sources[post_barriers] = source;
	post_barrier_destinations[post_barriers] = destination;
	post_barrier_access[post_barriers] = memory[0].dstAccessMask;
	post_barriers++;
	post_event('X');
}

static void VKAPI_CALL post_begin(VkCommandBuffer command, const VkRenderingInfo *info)
{
	(void)command;
	assert(post_begins < 4 && info->colorAttachmentCount == 1 && !info->pDepthAttachment && !info->viewMask);
	post_began[post_begins] = info->pColorAttachments[0];
	post_attachments[post_begins] = info->pColorAttachments[0].imageView;
	post_begins++;
	post_event('B');
	post_event(info->pColorAttachments[0].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? 'c' : 'l');
}

static void VKAPI_CALL post_end(VkCommandBuffer command)
{
	(void)command;
	post_event('E');
}

static VkResult VKAPI_CALL post_pipeline(VkDevice device, VkPipelineCache cache, uint32_t count,
	const VkGraphicsPipelineCreateInfo *info, const VkAllocationCallbacks *allocator, VkPipeline *pipeline)
{
	const VkPipelineRenderingCreateInfo *rendering = info->pNext;

	(void)device; (void)cache; (void)allocator;
	/* one triangle from the vertex's index, into BGRA8 alone, nothing
	culled, tested or blended */
	assert(count == 1 && post_pipelines < 4);
	assert(!info->pVertexInputState->vertexBindingDescriptionCount &&
		!info->pVertexInputState->vertexAttributeDescriptionCount);
	assert(info->pInputAssemblyState->topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	assert(info->pRasterizationState->cullMode == VK_CULL_MODE_NONE);
	assert(!info->pDepthStencilState->depthTestEnable && !info->pDepthStencilState->stencilTestEnable);
	assert(!info->pColorBlendState->pAttachments[0].blendEnable);
	assert(rendering->colorAttachmentCount == 1 && rendering->pColorAttachmentFormats[0] == VK_FORMAT_B8G8R8A8_UNORM);
	assert(rendering->depthAttachmentFormat == VK_FORMAT_UNDEFINED && !rendering->viewMask);
	assert(info->pMultisampleState->rasterizationSamples == VK_SAMPLE_COUNT_1_BIT);
	post_masks[post_pipelines] = info->pColorBlendState->pAttachments[0].colorWriteMask;
	*pipeline = (VkPipeline)(uintptr_t)(200 + post_pipelines++);
	return VK_SUCCESS;
}

static void VKAPI_CALL post_bind_pipeline(VkCommandBuffer command, VkPipelineBindPoint point, VkPipeline pipeline)
{
	(void)command; (void)point; (void)pipeline;
}

static void VKAPI_CALL post_update(VkDevice device, uint32_t count, const VkWriteDescriptorSet *writes,
	uint32_t copies, const VkCopyDescriptorSet *copy)
{
	uint32_t index;

	(void)device; (void)copies; (void)copy;
	assert(count == 4 && post_updates < 4);
	for (index = 0; index < count; index++)
		post_sampled[post_updates][index] = *writes[index].pImageInfo;
	post_updates++;
}

static void VKAPI_CALL post_viewport(VkCommandBuffer c, uint32_t first, uint32_t count, const VkViewport *v)
{
	(void)c;
	assert(first == 0 && count == 1);
	post_viewports[post_draws] = v[0];
}

static void VKAPI_CALL post_scissor(VkCommandBuffer c, uint32_t first, uint32_t count, const VkRect2D *r)
{
	(void)c;
	assert(first == 0 && count == 1);
	post_scissors[post_draws] = r[0];
}

static void VKAPI_CALL post_bind_sets(VkCommandBuffer c, VkPipelineBindPoint point, VkPipelineLayout layout,
	uint32_t first, uint32_t count, const VkDescriptorSet *sets, uint32_t dynamic_count, const uint32_t *offsets)
{
	(void)c; (void)point; (void)layout; (void)sets;
	assert(first == 0 && count == 2 && dynamic_count == 2);
	post_offsets[post_draws][0] = offsets[0];
	post_offsets[post_draws][1] = offsets[1];
}

static void VKAPI_CALL post_draw(VkCommandBuffer c, uint32_t vertices, uint32_t instances, uint32_t first, uint32_t instance)
{
	(void)c;
	assert(vertices == 3 && instances == 1 && first == 0 && instance == 0 && post_draws < 4);
	post_draws++;
	post_event('D');
}

static void post_image(struct xgpu_vulkan_image *image, uintptr_t number, uint8_t format, uint32_t width, uint32_t height)
{
	free(image->attachments);
	memset(image, 0, sizeof(*image));
	image->image = (VkImage)number;
	image->sampled = (VkImageView)(number + 40);
	image->number = (uint32_t)number;
	image->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	image->description.type = XGPU_VULKAN_IMAGE_2D;
	image->description.format = format;
	image->description.width = width;
	image->description.height = height;
	image->description.depth_or_layers = 1;
	image->description.levels = 1;
}

static void post_reset(void)
{
	reset();
	memset(post_events, 0, sizeof(post_events));
	post_begins = post_draws = post_updates = post_barriers = 0;
	vk.color = vk.depth = NULL;
	vk.ring_mapped = post_ring;
	vk.ring = (VkBuffer)(uintptr_t)9;
	recording.main = (VkCommandBuffer)(uintptr_t)1;
	recording.descriptor_pools = &post_pool;
	recording.descriptor_pool_count = 1;
	vkCmdCopyImage = post_copy;
	vkCmdPipelineBarrier = post_barrier;
	vkCmdBeginRendering = post_begin;
	vkCmdEndRendering = post_end;
	vkUpdateDescriptorSets = post_update;
	vkCmdSetViewport = post_viewport;
	vkCmdSetScissor = post_scissor;
	vkCmdBindDescriptorSets = post_bind_sets;
	vkCmdDraw = post_draw;
	vkCreateGraphicsPipelines = post_pipeline;
	vkCmdBindPipeline = post_bind_pipeline;
}

/* SMAA's three passes and FXAA's one over a window's rectangle: the
target's pixels copied first (the copy's barrier before the first pass
samples them), each pass ended by the barrier the next one's samples
need, SMAA's intermediates cleared whole as their passes begin, the target
drawn last, loaded, its alpha kept; each pass's viewport and scissor the
rectangle, its uniforms the target's size and FXAA's bounds the
rectangle's outermost texel centres */
static void post_test(void)
{
	static struct xgpu_vulkan_image target, color, edges, weights, area, search;
	static const int32_t corners[4] = { 8, 4, 40, 44 };
	static const int32_t beyond[4] = { -4, 4, 70, 44 }, empty[4] = { 70, 4, 80, 44 };
	struct xgpu_vulkan_image *images[6];
	const struct xgpu_vulkan_post_uniforms *uniforms;
	uint32_t index;

	post_reset();
	post_image(&target, 1, XGPU_VULKAN_FORMAT_BGRA8, 64, 48);
	post_image(&color, 2, XGPU_VULKAN_FORMAT_BGRA8, 64, 48);
	post_image(&edges, 3, XGPU_VULKAN_FORMAT_BGRA8, 64, 48);
	post_image(&weights, 4, XGPU_VULKAN_FORMAT_BGRA8, 64, 48);
	post_image(&area, 5, XGPU_VULKAN_FORMAT_RGBA8, 160, 560);
	post_image(&search, 6, XGPU_VULKAN_FORMAT_RGBA8, 64, 16);
	memset(&post, 0, sizeof(post));
	post.vertex = (VkShaderModule)(uintptr_t)1;
	for (index = 0; index < XGPU_VULKAN_POST_PROGRAM_COUNT; index++)
		post.pixel[index] = (VkShaderModule)(uintptr_t)(2 + index);
	post.sampler = (VkSampler)(uintptr_t)5;
	post.area = &area;
	post.search = &search;
	post.color = &color;
	post.edges = &edges;
	post.weights = &weights;
	post.width = 64;
	post.height = 48;
	post_pipelines = 0;

	assert(xgpu_vulkan_post_anti_alias(1, &target, 0, corners));
	assert(!strcmp(post_events, "CXBcDEXBcDEXBlD"));
	assert(post_pipelines == 3 && post_masks[0] == 15 && post_masks[1] == 15 && post_masks[2] == 7);
	assert(post_copied.srcSubresource.baseArrayLayer == 0 && post_copied.dstSubresource.baseArrayLayer == 0);
	assert(post_copied.srcOffset.x == 8 && post_copied.srcOffset.y == 4 && post_copied.dstOffset.x == 8);
	assert(post_copied.dstOffset.y == 4 && post_copied.extent.width == 32 && post_copied.extent.height == 40);
	/* (the copy, and each pass, made visible to the samples after) */
	assert(post_barriers == 3 && post_barrier_sources[0] == VK_PIPELINE_STAGE_TRANSFER_BIT);
	for (index = 0; index < 3; index++)
	{
		assert(post_barrier_destinations[index] & VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		assert(post_barrier_access[index] & VK_ACCESS_SHADER_READ_BIT);
	}
	assert(post_barrier_sources[1] & VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	assert(post_barrier_sources[2] & VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	for (index = 0; index < 2; index++)
	{
		assert(post_began[index].clearValue.color.float32[0] == 0.0f);
		assert(post_began[index].clearValue.color.float32[3] == 0.0f);
		assert(post_began[index].storeOp == VK_ATTACHMENT_STORE_OP_STORE);
	}
	assert(post_attachments[0] == edges.attachments[0] && post_attachments[1] == weights.attachments[0]);
	assert(post_attachments[2] == target.attachments[0]);
	/* edges from the copy; weights from the edges, the area and search
	textures; the blend from the copy and the weights */
	assert(post_updates == 3);
	assert(post_sampled[0][0].imageView == color.sampled && post_sampled[0][0].sampler == post.sampler);
	assert(post_sampled[1][0].imageView == edges.sampled && post_sampled[1][1].imageView == area.sampled);
	assert(post_sampled[1][2].imageView == search.sampled);
	assert(post_sampled[2][0].imageView == color.sampled && post_sampled[2][1].imageView == weights.sampled);
	for (index = 0; index < 3; index++)
	{
		assert(post_viewports[index].x == 8.0f && post_viewports[index].y == 4.0f);
		assert(post_viewports[index].width == 32.0f && post_viewports[index].height == 40.0f);
		assert(post_scissors[index].offset.x == 8 && post_scissors[index].offset.y == 4);
		assert(post_scissors[index].extent.width == 32 && post_scissors[index].extent.height == 40);
		assert(post_offsets[index][0] == post_offsets[index][1] && post_offsets[index][1] % 256 == 0);
		uniforms = (const struct xgpu_vulkan_post_uniforms *)(post_ring + post_offsets[index][1]);
		assert(uniforms->metrics[0] == 1.0f / 64.0f && uniforms->metrics[1] == 1.0f / 48.0f);
		assert(uniforms->metrics[2] == 64.0f && uniforms->metrics[3] == 48.0f);
	}
	assert(vk.color == &target);

	/* FXAA in a layered target's second layer, beyond its corners */
	post_reset();
	free(target.attachments);
	target.attachments = NULL;
	target.attachment_count = 0;
	target.description.type = XGPU_VULKAN_IMAGE_2D_ARRAY;
	target.description.depth_or_layers = 2;
	assert(xgpu_vulkan_post_anti_alias(0, &target, 1, beyond));
	assert(!strcmp(post_events, "CXBlD"));
	assert(post_pipelines == 4 && post_masks[3] == 7);
	assert(post_copied.srcSubresource.baseArrayLayer == 1 && post_copied.dstSubresource.baseArrayLayer == 0);
	assert(post_copied.srcOffset.x == 0 && post_copied.extent.width == 64 && post_copied.extent.height == 40);
	assert(post_attachments[0] == target.attachments[4] && last_view.subresourceRange.baseArrayLayer == 1);
	assert(post_sampled[0][0].imageView == color.sampled);
	uniforms = (const struct xgpu_vulkan_post_uniforms *)(post_ring + post_offsets[0][1]);
	assert(uniforms->bounds[0] == 0.5f / 64.0f && uniforms->bounds[1] == 4.5f / 48.0f);
	assert(uniforms->bounds[2] == 63.5f / 64.0f && uniforms->bounds[3] == 43.5f / 48.0f);
	assert(post_viewports[0].x == 0.0f && post_viewports[0].width == 64.0f);

	/* nothing of an empty rectangle, nor of a multisampled target */
	post_reset();
	assert(xgpu_vulkan_post_anti_alias(0, &target, 0, empty) && !post_events[0]);
	target.description.samples = 4;
	assert(!xgpu_vulkan_post_anti_alias(0, &target, 0, corners) && !post_events[0]);

	images[0] = &target;
	images[1] = &color;
	images[2] = &edges;
	images[3] = &weights;
	images[4] = &area;
	images[5] = &search;
	for (index = 0; index < 6; index++)
	{
		free(images[index]->attachments);
		images[index]->attachments = NULL;
	}
	memset(&post, 0, sizeof(post));
	recording.descriptor_pools = NULL;
	recording.descriptor_pool_count = 0;
	reset();
}

/* ---------- clears of some of the colour's channels */

/* what the clears record, in order: B a pass begun, A attachments cleared,
D a draw, Q a query begun, q ended */
static char masked_events[64];
static unsigned char masked_ring[65536];
static uint32_t masked_pipelines, masked_draws;
static struct
{
	VkColorComponentFlags mask;
	VkBool32 blend, depth_test, depth_write, stencil_test;
	VkFormat color_format, depth_format;
	VkSampleCountFlagBits samples;
	uint32_t view_mask, bindings, attributes;
	VkPrimitiveTopology topology;
	VkCullModeFlags cull;
} masked_made;
static VkViewport masked_viewports[4];
static VkRect2D masked_scissors[4];
static uint32_t masked_offsets[4];
static VkClearAttachment masked_cleared[2];
static uint32_t masked_cleared_count;

static void masked_event(char event)
{
	size_t length = strlen(masked_events);

	assert(length + 1 < sizeof(masked_events));
	masked_events[length] = event;
}

static void VKAPI_CALL masked_begin(VkCommandBuffer command, const VkRenderingInfo *info)
{
	fake_begin(command, info);
	masked_event('B');
}

static void VKAPI_CALL masked_clear(VkCommandBuffer command, uint32_t attachment_count,
	const VkClearAttachment *attachments, uint32_t rect_count, const VkClearRect *rects)
{
	fake_clear(command, attachment_count, attachments, rect_count, rects);
	memcpy(masked_cleared, attachments, attachment_count * sizeof(*attachments));
	masked_cleared_count = attachment_count;
	masked_event('A');
}

static VkResult VKAPI_CALL masked_pipeline(VkDevice device, VkPipelineCache cache, uint32_t count,
	const VkGraphicsPipelineCreateInfo *info, const VkAllocationCallbacks *allocator, VkPipeline *pipeline)
{
	const VkPipelineRenderingCreateInfo *rendering = info->pNext;

	(void)device; (void)cache; (void)allocator;
	assert(count == 1 && info->pColorBlendState->attachmentCount == 1 && rendering->colorAttachmentCount == 1);
	masked_made.mask = info->pColorBlendState->pAttachments[0].colorWriteMask;
	masked_made.blend = info->pColorBlendState->pAttachments[0].blendEnable;
	masked_made.depth_test = info->pDepthStencilState->depthTestEnable;
	masked_made.depth_write = info->pDepthStencilState->depthWriteEnable;
	masked_made.stencil_test = info->pDepthStencilState->stencilTestEnable;
	masked_made.color_format = rendering->pColorAttachmentFormats[0];
	masked_made.depth_format = rendering->depthAttachmentFormat;
	masked_made.samples = info->pMultisampleState->rasterizationSamples;
	masked_made.view_mask = rendering->viewMask;
	masked_made.bindings = info->pVertexInputState->vertexBindingDescriptionCount;
	masked_made.attributes = info->pVertexInputState->vertexAttributeDescriptionCount;
	masked_made.topology = info->pInputAssemblyState->topology;
	masked_made.cull = info->pRasterizationState->cullMode;
	*pipeline = (VkPipeline)(uintptr_t)(300 + masked_pipelines++);
	return VK_SUCCESS;
}

static void VKAPI_CALL masked_viewport(VkCommandBuffer c, uint32_t first, uint32_t count, const VkViewport *v)
{
	(void)c;
	assert(first == 0 && count == 1 && masked_draws < 4);
	masked_viewports[masked_draws] = v[0];
}

static void VKAPI_CALL masked_scissor(VkCommandBuffer c, uint32_t first, uint32_t count, const VkRect2D *r)
{
	(void)c;
	assert(first == 0 && count == 1 && masked_draws < 4);
	masked_scissors[masked_draws] = r[0];
}

static void VKAPI_CALL masked_bind_sets(VkCommandBuffer c, VkPipelineBindPoint point, VkPipelineLayout layout,
	uint32_t first, uint32_t count, const VkDescriptorSet *sets, uint32_t dynamic_count, const uint32_t *offsets)
{
	(void)c; (void)point; (void)layout; (void)sets;
	assert(first == 0 && count == 2 && dynamic_count == 2 && offsets[0] == offsets[1]);
	masked_offsets[masked_draws] = offsets[1];
}

static void VKAPI_CALL masked_draw(VkCommandBuffer c, uint32_t vertices, uint32_t instances, uint32_t first,
	uint32_t instance)
{
	(void)c;
	assert(vertices == 3 && instances == 1 && first == 0 && instance == 0 && masked_draws < 4);
	masked_draws++;
	masked_event('D');
}

static void VKAPI_CALL masked_begin_query(VkCommandBuffer c, VkQueryPool pool, uint32_t query, VkQueryControlFlags flags)
{
	(void)c; (void)pool; (void)query; (void)flags;
	masked_event('Q');
}

static void VKAPI_CALL masked_end_query(VkCommandBuffer c, VkQueryPool pool, uint32_t query)
{
	(void)c; (void)pool; (void)query;
	masked_event('q');
}

static void masked_reset(void)
{
	reset();
	memset(masked_events, 0, sizeof(masked_events));
	memset(&masked_made, 0, sizeof(masked_made));
	masked_draws = masked_cleared_count = 0;
	vk.ring_mapped = masked_ring;
	vk.ring = (VkBuffer)(uintptr_t)9;
	vk.depth_format = VK_FORMAT_D24_UNORM_S8_UINT;
	color_target.description.format = XGPU_VULKAN_FORMAT_BGRA8;
	depth_target.description.format = XGPU_VULKAN_FORMAT_D24S8;
	/* (the clears' shaders, made already) */
	vk.clear_vertex = (VkShaderModule)(uintptr_t)61;
	vk.clear_pixel = (VkShaderModule)(uintptr_t)62;
	recording.main = (VkCommandBuffer)(uintptr_t)1;
	recording.descriptor_pools = &post_pool;
	recording.descriptor_pool_count = 1;
	vkCmdBeginRendering = masked_begin;
	vkCmdClearAttachments = masked_clear;
	vkCreateGraphicsPipelines = masked_pipeline;
	vkCmdBindPipeline = post_bind_pipeline;
	vkCmdSetViewport = masked_viewport;
	vkCmdSetScissor = masked_scissor;
	vkCmdSetDepthBias = fake_bias;
	vkCmdSetBlendConstants = fake_blend;
	vkCmdSetStencilReference = fake_stencil;
	vkCmdSetPrimitiveTopology = fake_topology;
	vkCmdBindDescriptorSets = masked_bind_sets;
	vkCmdDraw = masked_draw;
	vkCmdBeginQuery = masked_begin_query;
	vkCmdEndQuery = masked_end_query;
}

/* A clear of some of the colour's channels (the game's are of alpha alone)
is a triangle over each rectangle, through a pipeline whose write mask is
those channels, testing and writing no depth or stencil, of the targets'
formats, samples and views; depth and stencil are cleared as before (a
whole attachment's as the pass begins). A clear of every channel draws
nothing. */
static void masked_clear_test(void)
{
	static const int32_t full[1][4] = { { 0, 0, 64, 48 } };
	static const int32_t two[2][4] = { { 4, 5, 10, 12 }, { -20, -20, 100, 100 } };
	static const int32_t outside[1][4] = { { 70, 0, 8, 8 } };
	static const float color[4] = { 0.25f, 0.5f, 0.75f, 0.125f };
	struct query_frame queries;
	const float *uniforms;
	uint64_t count;

	/* a fog screen's: alpha alone and depth, the whole target, before a
	pass: depth cleared as the pass begins, the colour loaded and drawn */
	masked_reset();
	masked_pipelines = 0;
	xgpu_vulkan_clear(full, 1, XGPU_VULKAN_CLEAR_COLOR | XGPU_VULKAN_CLEAR_ALPHA | XGPU_VULKAN_CLEAR_DEPTH, color,
		1.0f, 0);
	assert(!strcmp(masked_events, "BD"));
	assert(began_color.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && began_depth.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
	assert(began_stencil.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && began_depth.clearValue.depthStencil.depth == 1.0f);
	assert(vk.statistics.attachment_clears == 1 && vk.statistics.clears_as_draws == 1);
	assert(masked_pipelines == 1 && masked_made.mask == VK_COLOR_COMPONENT_A_BIT && !masked_made.blend);
	assert(!masked_made.depth_test && !masked_made.depth_write && !masked_made.stencil_test);
	assert(masked_made.color_format == VK_FORMAT_B8G8R8A8_UNORM);
	assert(masked_made.depth_format == VK_FORMAT_D24_UNORM_S8_UINT);
	assert(masked_made.samples == VK_SAMPLE_COUNT_1_BIT && !masked_made.view_mask);
	assert(!masked_made.bindings && !masked_made.attributes && masked_made.cull == VK_CULL_MODE_NONE);
	assert(masked_made.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	assert(masked_viewports[0].x == 0.0f && masked_viewports[0].y == 0.0f);
	assert(masked_viewports[0].width == 64.0f && masked_viewports[0].height == 48.0f);
	assert(masked_viewports[0].minDepth == 0.0f && masked_viewports[0].maxDepth == 1.0f);
	assert(masked_scissors[0].extent.width == 64 && masked_scissors[0].extent.height == 48);
	assert(masked_offsets[0] % 256 == 0);
	uniforms = (const float *)(masked_ring + masked_offsets[0]);
	assert(uniforms[0] == 0.25f && uniforms[1] == 0.5f && uniforms[2] == 0.75f && uniforms[3] == 0.125f);
	/* (the same targets again: the same pipeline) */
	count = xgpu_vulkan_pipeline_count();
	xgpu_vulkan_clear(full, 1, XGPU_VULKAN_CLEAR_COLOR | XGPU_VULKAN_CLEAR_ALPHA, color, 1.0f, 0);
	assert(!strcmp(masked_events, "BDD") && xgpu_vulkan_pipeline_count() == count && masked_pipelines == 1);

	/* red, green and blue and the stencil, in a pass, over two rectangles
	(one beyond the target): the stencil cleared, the colour drawn over
	each, inside the target */
	masked_reset();
	xgpu_vulkan_clear(two, 1, XGPU_VULKAN_CLEAR_DEPTH, color, 0.5f, 0);
	xgpu_vulkan_clear(two, 2, XGPU_VULKAN_CLEAR_COLOR | 7 | XGPU_VULKAN_CLEAR_STENCIL, color, 0.5f, 9);
	assert(!strcmp(masked_events, "BAADD"));
	assert(masked_cleared_count == 1 && masked_cleared[0].aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT);
	assert(masked_cleared[0].clearValue.depthStencil.stencil == 9);
	assert(masked_made.mask == (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT));
	assert(masked_viewports[0].x == 4.0f && masked_viewports[0].y == 5.0f);
	assert(masked_viewports[0].width == 10.0f && masked_viewports[0].height == 12.0f);
	assert(masked_scissors[0].offset.x == 4 && masked_scissors[0].offset.y == 5);
	assert(masked_scissors[0].extent.width == 10 && masked_scissors[0].extent.height == 12);
	assert(masked_viewports[1].x == 0.0f && masked_viewports[1].y == 0.0f);
	assert(masked_viewports[1].width == 64.0f && masked_viewports[1].height == 48.0f);
	assert(masked_scissors[1].extent.width == 64 && masked_scissors[1].extent.height == 48);

	/* every channel: cleared, never drawn (the whole target as the pass
	begins, a rectangle in it with vkCmdClearAttachments) */
	masked_reset();
	count = xgpu_vulkan_pipeline_count();
	xgpu_vulkan_clear(full, 1, XGPU_VULKAN_CLEAR_COLOR | XGPU_VULKAN_CLEAR_CHANNELS, color, 1.0f, 0);
	xgpu_vulkan_clear(two, 1, XGPU_VULKAN_CLEAR_COLOR | XGPU_VULKAN_CLEAR_CHANNELS, color, 1.0f, 0);
	assert(!strcmp(masked_events, "BA") && began_color.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
	assert(masked_cleared_count == 1 && masked_cleared[0].aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
	assert(xgpu_vulkan_pipeline_count() == count && !vk.statistics.clears_as_draws && !masked_draws);
	/* no channel, or a rectangle outside the target: nothing drawn */
	xgpu_vulkan_clear(full, 1, XGPU_VULKAN_CLEAR_COLOR, color, 1.0f, 0);
	xgpu_vulkan_clear(outside, 1, XGPU_VULKAN_CLEAR_COLOR | XGPU_VULKAN_CLEAR_ALPHA, color, 1.0f, 0);
	assert(!strcmp(masked_events, "BA") && !masked_draws);

	/* a layered target's two views (multiview), and a multisampled one */
	masked_reset();
	xgpu_vulkan_set_target_views(&color_target, &depth_target, 2, 2);
	xgpu_vulkan_clear(full, 1, XGPU_VULKAN_CLEAR_COLOR | XGPU_VULKAN_CLEAR_ALPHA, color, 1.0f, 0);
	assert(!strcmp(masked_events, "BD") && began.viewMask == 3 && masked_made.view_mask == 3);
	assert(last_view.subresourceRange.baseArrayLayer == 2 && last_view.subresourceRange.layerCount == 2);
	masked_reset();
	color_target.description.samples = depth_target.description.samples = 4;
	vk.depth = NULL;
	xgpu_vulkan_clear(full, 1, XGPU_VULKAN_CLEAR_COLOR | XGPU_VULKAN_CLEAR_ALPHA, color, 1.0f, 0);
	assert(!strcmp(masked_events, "BD") && masked_made.samples == VK_SAMPLE_COUNT_4_BIT);
	assert(masked_made.depth_format == VK_FORMAT_UNDEFINED && !masked_made.view_mask);

	/* in a visibility test: its query ended over the triangle, which counts
	no samples, and begun again after it */
	masked_reset();
	memset(&queries, 0, sizeof(queries));
	vk.queries = &queries;
	xgpu_vulkan_visibility_begin();
	xgpu_vulkan_clear(full, 1, XGPU_VULKAN_CLEAR_COLOR | XGPU_VULKAN_CLEAR_ALPHA, color, 1.0f, 0);
	assert(!strcmp(masked_events, "BQqDQ") && vk.test_query_open && vk.test_query_count == 2);
	vk.queries = NULL;

	/* queued as a map is prepared: the targets' pipeline, the one the clear
	draws with after (none for every channel) */
	masked_reset();
	masked_pipelines = 0;
	count = xgpu_vulkan_pipeline_count();
	assert(xgpu_vulkan_clear_prepare(XGPU_VULKAN_CLEAR_COLOR | XGPU_VULKAN_CLEAR_CHANNELS));
	assert(xgpu_vulkan_pipeline_count() == count);
	assert(xgpu_vulkan_clear_prepare(XGPU_VULKAN_CLEAR_COLOR | XGPU_VULKAN_CLEAR_RED | XGPU_VULKAN_CLEAR_BLUE));
	assert(xgpu_vulkan_pipeline_count() == count + 1 && masked_pipelines == 1);
	assert(masked_made.mask == (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_B_BIT));
	xgpu_vulkan_clear(full, 1, XGPU_VULKAN_CLEAR_COLOR | XGPU_VULKAN_CLEAR_RED | XGPU_VULKAN_CLEAR_BLUE, color, 1.0f, 0);
	assert(!strcmp(masked_events, "BD") && xgpu_vulkan_pipeline_count() == count + 1 && masked_pipelines == 1);

	recording.descriptor_pools = NULL;
	recording.descriptor_pool_count = 0;
	vkCmdEndQuery = fake_end_query;
	reset();
}

/* ---------- the window: its surface lost and made again, the app in the
background, the present mode of the swap interval, the frames turned for
the display, and the pipeline cache written when there is something new */

static VkSurfaceCapabilitiesKHR window_capabilities;
static VkResult window_capabilities_result, window_acquire_result, window_present_result;
static VkPresentModeKHR window_modes[4];
static uint32_t window_mode_count;
static VkSwapchainCreateInfoKHR window_made;
static unsigned int window_swapchains, window_surfaces_made, window_surfaces_destroyed, window_surface_calls;
static unsigned int window_presents, window_capability_queries, window_turns, window_rotations;
static int window_gone;
static VkImage window_blit_source;
static struct xgpu_vulkan_image window_frame, window_turned;

static VkResult VKAPI_CALL window_get_capabilities(VkPhysicalDevice physical, VkSurfaceKHR surface,
	VkSurfaceCapabilitiesKHR *capabilities)
{
	(void)physical;
	assert(surface);
	window_capability_queries++;
	*capabilities = window_capabilities;
	return window_capabilities_result;
}

static VkResult VKAPI_CALL window_get_formats(VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t *count,
	VkSurfaceFormatKHR *formats)
{
	(void)physical; (void)surface;
	assert(*count >= 1);
	*count = 1;
	formats[0].format = VK_FORMAT_B8G8R8A8_UNORM;
	formats[0].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	return VK_SUCCESS;
}

static VkResult VKAPI_CALL window_get_modes(VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t *count,
	VkPresentModeKHR *modes)
{
	(void)physical; (void)surface;
	assert(*count >= window_mode_count);
	*count = window_mode_count;
	memcpy(modes, window_modes, window_mode_count * sizeof(*modes));
	return VK_SUCCESS;
}

static VkResult VKAPI_CALL window_get_support(VkPhysicalDevice physical, uint32_t family, VkSurfaceKHR surface,
	VkBool32 *supported)
{
	(void)physical; (void)family;
	assert(surface);
	*supported = VK_TRUE;
	return VK_SUCCESS;
}

static VkSurfaceKHR window_surface_create(VkInstance instance, void *context)
{
	(void)instance;
	assert(context == &window_gone);
	window_surface_calls++;
	if (window_gone)
		return VK_NULL_HANDLE;
	return (VkSurfaceKHR)(uintptr_t)(300 + ++window_surfaces_made);
}

static void VKAPI_CALL window_destroy_surface(VkInstance instance, VkSurfaceKHR surface,
	const VkAllocationCallbacks *allocator)
{
	(void)instance; (void)allocator;
	assert(surface);
	window_surfaces_destroyed++;
}

static VkResult VKAPI_CALL window_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
	const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain)
{
	(void)device; (void)allocator;
	assert(info->surface == vk.surface);
	window_made = *info;
	*swapchain = (VkSwapchainKHR)(uintptr_t)(400 + ++window_swapchains);
	return VK_SUCCESS;
}

static void VKAPI_CALL window_destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain,
	const VkAllocationCallbacks *allocator)
{
	(void)device; (void)swapchain; (void)allocator;
}

static VkResult VKAPI_CALL window_get_images(VkDevice device, VkSwapchainKHR swapchain, uint32_t *count, VkImage *images)
{
	(void)device; (void)swapchain;
	assert(*count >= 2);
	*count = 2;
	images[0] = (VkImage)(uintptr_t)501;
	images[1] = (VkImage)(uintptr_t)502;
	return VK_SUCCESS;
}

static VkResult VKAPI_CALL window_create_semaphore(VkDevice device, const VkSemaphoreCreateInfo *info,
	const VkAllocationCallbacks *allocator, VkSemaphore *semaphore)
{
	(void)device; (void)info; (void)allocator;
	*semaphore = (VkSemaphore)(uintptr_t)600;
	return VK_SUCCESS;
}

static void VKAPI_CALL window_destroy_semaphore(VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *allocator)
{
	(void)device; (void)semaphore; (void)allocator;
}

static VkResult VKAPI_CALL window_wait_idle(VkDevice device)
{
	(void)device;
	return VK_SUCCESS;
}

static VkResult VKAPI_CALL window_acquire(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
	VkSemaphore semaphore, VkFence fence, uint32_t *index)
{
	(void)device; (void)timeout; (void)semaphore; (void)fence;
	assert(swapchain == vk.swapchain);
	*index = 1;
	return window_acquire_result;
}

static VkResult VKAPI_CALL window_present(VkQueue queue, const VkPresentInfoKHR *info)
{
	(void)queue;
	assert(info->swapchainCount == 1 && info->pImageIndices[0] == 1);
	window_presents++;
	return window_present_result;
}

static VkResult VKAPI_CALL window_submit(VkQueue queue, uint32_t count, const VkSubmitInfo *info, VkFence fence)
{
	(void)queue; (void)count; (void)info; (void)fence;
	return VK_SUCCESS;
}

static VkResult VKAPI_CALL window_end_commands(VkCommandBuffer commands)
{
	(void)commands;
	return VK_SUCCESS;
}

static void VKAPI_CALL window_clear_image(VkCommandBuffer commands, VkImage image, VkImageLayout layout,
	const VkClearColorValue *color, uint32_t count, const VkImageSubresourceRange *ranges)
{
	(void)commands; (void)image; (void)layout; (void)color; (void)count; (void)ranges;
}

static void VKAPI_CALL window_blit(VkCommandBuffer commands, VkImage source, VkImageLayout source_layout,
	VkImage destination, VkImageLayout destination_layout, uint32_t count, const VkImageBlit *regions,
	VkFilter filter)
{
	(void)commands; (void)source_layout; (void)destination; (void)destination_layout; (void)count; (void)regions;
	(void)filter;
	window_blit_source = source;
}

static struct xgpu_vulkan_image *window_rotate(struct xgpu_vulkan_image *image, int quarter_turns)
{
	assert(image == &window_frame);
	window_rotations++;
	window_turns = (unsigned int)quarter_turns;
	return &window_turned;
}

/* a present of the frame: its serial (0: nothing presented) */
static uint64_t window_present_frame(void)
{
	window_blit_source = VK_NULL_HANDLE;
	return present(&window_frame);
}

static void window_reset(void)
{
	reset();
	vk.device = (VkDevice)(uintptr_t)1;
	vk.instance = (VkInstance)(uintptr_t)2;
	vk.physical = (VkPhysicalDevice)(uintptr_t)3;
	vk.surface_create = window_surface_create;
	vk.surface_context = &window_gone;
	vk.interval = 1;
	vk.color = vk.depth = NULL;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR = window_get_capabilities;
	vkGetPhysicalDeviceSurfaceFormatsKHR = window_get_formats;
	vkGetPhysicalDeviceSurfacePresentModesKHR = window_get_modes;
	vkGetPhysicalDeviceSurfaceSupportKHR = window_get_support;
	vkDestroySurfaceKHR = window_destroy_surface;
	vkCreateSwapchainKHR = window_create_swapchain;
	vkDestroySwapchainKHR = window_destroy_swapchain;
	vkGetSwapchainImagesKHR = window_get_images;
	vkCreateSemaphore = window_create_semaphore;
	vkDestroySemaphore = window_destroy_semaphore;
	vkDeviceWaitIdle = window_wait_idle;
	vkAcquireNextImageKHR = window_acquire;
	vkQueuePresentKHR = window_present;
	vkQueueSubmit = window_submit;
	vkEndCommandBuffer = window_end_commands;
	vkResetCommandPool = fake_reset_pool;
	vkBeginCommandBuffer = fake_begin_commands;
	vkCmdClearColorImage = window_clear_image;
	vkCmdBlitImage = window_blit;
	memset(&window_capabilities, 0, sizeof(window_capabilities));
	window_capabilities.minImageCount = 2;
	window_capabilities.currentExtent.width = 1280;
	window_capabilities.currentExtent.height = 720;
	window_capabilities.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	window_capabilities.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	window_capabilities.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	window_capabilities_result = window_acquire_result = window_present_result = VK_SUCCESS;
	window_modes[0] = VK_PRESENT_MODE_FIFO_KHR;
	window_modes[1] = VK_PRESENT_MODE_IMMEDIATE_KHR;
	window_modes[2] = VK_PRESENT_MODE_MAILBOX_KHR;
	window_mode_count = 3;
	window_swapchains = window_surfaces_made = window_surfaces_destroyed = window_surface_calls = 0;
	window_presents = window_capability_queries = window_turns = window_rotations = 0;
	window_gone = 0;
	memset(&window_frame, 0, sizeof(window_frame));
	window_frame.image = (VkImage)(uintptr_t)700;
	window_frame.description.width = 640;
	window_frame.description.height = 480;
	memset(&window_turned, 0, sizeof(window_turned));
	window_turned.image = (VkImage)(uintptr_t)701;
	window_turned.description.width = 480;
	window_turned.description.height = 640;
}

static void window_test(void)
{
	static const VkPresentModeKHR fifo_only[1] = { VK_PRESENT_MODE_FIFO_KHR };
	static const VkPresentModeKHR no_mailbox[2] = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR };
	static const VkPresentModeKHR all[3] = { VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
		VK_PRESENT_MODE_FIFO_KHR };
	unsigned int index, queries;

	/* the swap interval's present mode: FIFO waits; else MAILBOX, else
	IMMEDIATE, else FIFO */
	assert(present_mode_choose(all, 3, 1) == VK_PRESENT_MODE_FIFO_KHR);
	assert(present_mode_choose(all, 3, 0) == VK_PRESENT_MODE_MAILBOX_KHR);
	assert(present_mode_choose(no_mailbox, 2, 0) == VK_PRESENT_MODE_IMMEDIATE_KHR);
	assert(present_mode_choose(fifo_only, 1, 0) == VK_PRESENT_MODE_FIFO_KHR);

	/* the first present makes the surface and the swapchain (vsync: FIFO),
	and blits the frame itself */
	window_reset();
	assert(window_present_frame() && window_surfaces_made == 1 && window_swapchains == 1 && window_presents == 1);
	assert(window_made.presentMode == VK_PRESENT_MODE_FIFO_KHR && window_blit_source == window_frame.image);
	assert(window_made.preTransform == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR);
	assert(window_made.imageExtent.width == 1280 && window_made.imageExtent.height == 720);
	assert(window_present_frame() && window_swapchains == 1);

	/* display.vsync off: the swapchain made again, MAILBOX; on: FIFO */
	xgpu_vulkan_window_interval(0);
	assert(window_present_frame() && window_swapchains == 2 && window_made.presentMode == VK_PRESENT_MODE_MAILBOX_KHR);
	assert(window_made.oldSwapchain == (VkSwapchainKHR)(uintptr_t)401);
	assert(window_present_frame() && window_swapchains == 2);
	xgpu_vulkan_window_interval(1);
	assert(window_present_frame() && window_swapchains == 3 && window_made.presentMode == VK_PRESENT_MODE_FIFO_KHR);

	/* a resize (from another thread) makes it again, once */
	xgpu_vulkan_window_resized(800, 600);
	assert(window_present_frame() && window_swapchains == 4);
	assert(window_present_frame() && window_swapchains == 4);

	/* VK_ERROR_SURFACE_LOST_KHR from the acquire: nothing presented, the
	surface let go of; the next frame makes a new one and presents */
	window_acquire_result = VK_ERROR_SURFACE_LOST_KHR;
	index = window_presents;
	assert(!window_present_frame() && window_presents == index);
	assert(!vk.surface && !vk.swapchain && window_surfaces_destroyed == 1);
	window_acquire_result = VK_SUCCESS;
	assert(window_present_frame() && window_surfaces_made == 2 && window_swapchains == 5);
	assert(window_made.surface == (VkSurfaceKHR)(uintptr_t)302 && !window_made.oldSwapchain);

	/* ... from the present */
	window_present_result = VK_ERROR_SURFACE_LOST_KHR;
	assert(window_present_frame() && !vk.surface && window_surfaces_destroyed == 2);
	window_present_result = VK_SUCCESS;
	assert(window_present_frame() && window_surfaces_made == 3 && vk.surface);

	/* ... from the surface's capabilities, as the swapchain is made */
	xgpu_vulkan_window_resized(800, 600);
	window_capabilities_result = VK_ERROR_SURFACE_LOST_KHR;
	assert(!window_present_frame() && !vk.surface && window_surfaces_destroyed == 3);
	window_capabilities_result = VK_SUCCESS;
	assert(window_present_frame() && window_surfaces_made == 4);

	/* the app in the background: the surface let go of and not made
	again, the frames not presented (nor waited for) */
	xgpu_vulkan_window_suspend(1);
	assert(!vk.surface && !vk.swapchain && window_surfaces_destroyed == 4 && vk.window_suspended);
	window_gone = 1;
	index = window_surface_calls;
	assert(!window_present_frame() && !window_present_frame() && window_surface_calls == index);
	/* back, before Android gives the window back: tried at each frame */
	xgpu_vulkan_window_suspend(0);
	assert(!vk.window_suspended && !vk.surface && window_surface_calls == index + 1);
	assert(!window_present_frame() && window_surface_calls == index + 2);
	window_gone = 0;
	assert(window_present_frame() && vk.surface && window_surfaces_made == 5 && window_presents);

	/* a hidden window (VR's, while the headset shows the game): nothing
	presented, nothing waited for; shown, presented again */
	xgpu_vulkan_window_hidden(1);
	index = window_presents;
	assert(!window_present_frame() && window_presents == index && vk.surface);
	xgpu_vulkan_window_hidden(0);
	assert(window_present_frame() && window_presents == index + 1);

	/* held sideways (a quarter turn), with a way to turn the frames: the
	swapchain is the display's way up (the extent swapped, preTransform
	the surface's), and the frame is drawn turned into it */
	window_reset();
	vk.rotate = window_rotate;
	window_capabilities.currentExtent.width = 2400;
	window_capabilities.currentExtent.height = 1080;
	window_capabilities.currentTransform = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR;
	window_capabilities.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR |
		VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR |
		VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
	assert(window_present_frame() && window_made.preTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR);
	assert(window_made.imageExtent.width == 1080 && window_made.imageExtent.height == 2400);
	assert(window_rotations == 1 && window_turns == 1 && window_blit_source == window_turned.image);
	/* turned the other way: a SUBOPTIMAL present finds the transform
	changed, and the swapchain is made again for it */
	window_capabilities.currentTransform = VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
	window_present_result = VK_SUBOPTIMAL_KHR;
	assert(window_present_frame() && vk.swapchain_stale);
	window_present_result = VK_SUCCESS;
	assert(window_present_frame() && window_made.preTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR);
	assert(window_turns == 3 && window_made.imageExtent.width == 1080);
	/* upside down: no swap */
	window_capabilities.currentTransform = VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR;
	window_capabilities.currentExtent.width = 1080;
	window_capabilities.currentExtent.height = 2400;
	xgpu_vulkan_window_resized(1080, 2400);
	assert(window_present_frame() && window_turns == 2 && window_made.imageExtent.width == 1080 &&
		window_made.imageExtent.height == 2400);

	/* a SUBOPTIMAL that making the swapchain again cannot mend (the
	surface unchanged) neither makes it again nor asks the surface at
	each frame */
	queries = window_capability_queries;
	index = window_swapchains;
	window_present_result = VK_SUBOPTIMAL_KHR;
	for (index = 0; index < 120; index++)
		assert(window_present_frame() && !vk.swapchain_stale);
	assert(window_capability_queries - queries == 2);

	/* without a way to turn the frames: the window's way up, which the
	compositor turns */
	window_reset();
	window_capabilities.currentTransform = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR;
	window_capabilities.supportedTransforms |= VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR;
	window_capabilities.currentExtent.width = 2400;
	window_capabilities.currentExtent.height = 1080;
	assert(window_present_frame() && window_made.preTransform == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR);
	assert(window_made.imageExtent.width == 2400 && !window_rotations && window_blit_source == window_frame.image);
	reset();
}

/* the present step's turn (xgpu_vulkan_post_rotate): one triangle over
the whole turned image (the frame's width and height swapped for a
quarter turn), sampling the frame, its uniforms the turned image's size
and the turn's matrix */
static void rotate_test(void)
{
	static struct xgpu_vulkan_image frame, turned;
	const struct xgpu_vulkan_post_uniforms *uniforms;

	post_reset();
	post_image(&frame, 1, XGPU_VULKAN_FORMAT_BGRA8, 64, 48);
	post_image(&turned, 2, XGPU_VULKAN_FORMAT_BGRA8, 48, 64);
	memset(&post, 0, sizeof(post));
	post.sampler = (VkSampler)(uintptr_t)5;
	assert(!xgpu_vulkan_post_rotate(&frame, 1));
	post.rotate = (VkPipeline)(uintptr_t)210;
	post.turned = &turned;
	assert(!xgpu_vulkan_post_rotate(&frame, 0) && !xgpu_vulkan_post_rotate(&frame, 4));
	assert(xgpu_vulkan_post_rotate(&frame, 1) == &turned);
	assert(!strcmp(post_events, "BlD") && post_attachments[0] == turned.attachments[0]);
	assert(post_sampled[0][0].imageView == frame.sampled && post_sampled[0][0].sampler == post.sampler);
	assert(post_viewports[0].x == 0.0f && post_viewports[0].width == 48.0f && post_viewports[0].height == 64.0f);
	assert(post_scissors[0].extent.width == 48 && post_scissors[0].extent.height == 64);
	uniforms = (const struct xgpu_vulkan_post_uniforms *)(post_ring + post_offsets[0][1]);
	assert(uniforms->metrics[0] == 1.0f / 48.0f && uniforms->metrics[1] == 1.0f / 64.0f);
	/* (a quarter turn clockwise: the turned image's x is the frame's y,
	its y the frame's -x, about the centre) */
	assert(uniforms->bounds[0] == 0.0f && uniforms->bounds[1] == 1.0f);
	assert(uniforms->bounds[2] == -1.0f && uniforms->bounds[3] == 0.0f);
	post_reset();
	assert(xgpu_vulkan_post_rotate(&frame, 3) == &turned);
	uniforms = (const struct xgpu_vulkan_post_uniforms *)(post_ring + post_offsets[0][1]);
	assert(uniforms->bounds[1] == -1.0f && uniforms->bounds[2] == 1.0f);
	free(frame.attachments);
	free(turned.attachments);
	memset(&post, 0, sizeof(post));
	recording.descriptor_pools = NULL;
	recording.descriptor_pool_count = 0;
	reset();
}

static unsigned int cache_reads;

static VkResult VKAPI_CALL cache_data(VkDevice device, VkPipelineCache cache, size_t *size, void *data)
{
	(void)device;
	assert(cache);
	cache_reads++;
	if (data)
		memcpy(data, "cache", 5);
	*size = 5;
	return VK_SUCCESS;
}

static void pipeline_cache_test(void)
{
	char path[] = "/tmp/openh1_passes_cacheXXXXXX", temporary[64];
	FILE *file;
	char read[8] = { 0 };
	int descriptor = mkstemp(path);

	assert(descriptor >= 0);
	close(descriptor);
	remove(path);
	reset();
	vk.pipeline_cache = (VkPipelineCache)(uintptr_t)5;
	snprintf(vk.cache_path, sizeof(vk.cache_path), "%s", path);
	vkGetPipelineCacheData = cache_data;
	/* nothing made since the cache was read: not written */
	xgpu_vulkan_pipeline_cache_save();
	assert(!cache_reads && access(path, F_OK));
	/* a pipeline made: written whole (through a temporary file, gone) */
	pipeline_publish(&(struct pipeline_entry){ 0 }, (VkPipeline)(uintptr_t)9, VK_SUCCESS, 0.0);
	xgpu_vulkan_pipeline_cache_save();
	assert(cache_reads == 2);
	file = fopen(path, "rb");
	assert(file && fread(read, 1, sizeof(read), file) == 5 && !memcmp(read, "cache", 5));
	fclose(file);
	snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	assert(access(temporary, F_OK));
	/* and not again until another is made */
	xgpu_vulkan_pipeline_cache_save();
	assert(cache_reads == 2);
	remove(path);
	reset();
}

int main(void)
{
	static const int32_t full[1][4] = { { 0, 0, 64, 48 } };
	static const int32_t partial[1][4] = { { 4, 5, 10, 12 } };
	static const int32_t beyond[2][4] = { { 70, 0, 1, 1 }, { -20, -20, 100, 100 } };
	static const float color[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
	uint32_t all = XGPU_VULKAN_CLEAR_COLOR | XGPU_VULKAN_CLEAR_CHANNELS |
		XGPU_VULKAN_CLEAR_DEPTH | XGPU_VULKAN_CLEAR_STENCIL;

	capabilities_test();
	vertex_input_test();
	uploads_test();
	dynamic_lights_test();
	submissions_test();
	texture_sets_test();
	post_test();
	masked_clear_test();
	window_test();
	rotate_test();
	pipeline_cache_test();
	reset();
	xgpu_vulkan_clear(full, 0, all, color, 0.75f, 0x123);
	assert(!begins && !clears);

	reset();
	xgpu_vulkan_clear(full, 1, all, color, 0.75f, 0x123);
	assert(begins == 1 && !clears);
	assert(began_color.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && began_depth.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
	assert(began_stencil.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
	assert(began_color.clearValue.color.float32[0] == 0.25f);
	assert(began_depth.clearValue.depthStencil.depth == 0.75f);
	assert(began_stencil.clearValue.depthStencil.stencil == 0x23);
	assert(began_color.storeOp == VK_ATTACHMENT_STORE_OP_STORE && began_depth.storeOp == VK_ATTACHMENT_STORE_OP_STORE);
	assert(began_stencil.storeOp == VK_ATTACHMENT_STORE_OP_STORE);
	assert(vk.statistics.attachment_clears == 3 && vk.statistics.attachment_loads == 0);
	/* A later clear must remain in the pass: it cannot overwrite an
	earlier draw's initial attachment values retroactively. */
	xgpu_vulkan_clear(full, 1, all, color, 0.5f, 7);
	assert(begins == 1 && clears == 1);
	assert(cleared[1].clearValue.depthStencil.depth == 0.5f);

	reset();
	xgpu_vulkan_clear(full, 1, XGPU_VULKAN_CLEAR_DEPTH, color, 0.75f, 0);
	assert(begins == 1 && !clears);
	assert(began_depth.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
	assert(began_color.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && began_stencil.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
	assert(vk.statistics.attachment_clears == 1 && vk.statistics.attachment_loads == 2);

	reset();
	xgpu_vulkan_clear(full, 1, XGPU_VULKAN_CLEAR_STENCIL, color, 0.75f, 19);
	assert(began_stencil.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && began_depth.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
	assert(began_stencil.clearValue.depthStencil.stencil == 19 && !clears);

	reset();
	xgpu_vulkan_clear(partial, 1, all, color, 0.75f, 3);
	assert(begins == 1 && clears == 1);
	assert(began_color.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && began_depth.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
	assert(began_stencil.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
	assert(cleared_rect.rect.offset.x == 4 && cleared_rect.rect.extent.height == 12);
	assert(vk.statistics.attachment_clears == 0 && vk.statistics.attachment_loads == 3);

	reset();
	xgpu_vulkan_clear(beyond, 2, all, color, 0.75f, 3);
	assert(begins == 1 && !clears && vk.statistics.attachment_clears == 3);

	reset();
	xgpu_vulkan_set_target_views(&color_target, &depth_target, 2, 2);
	xgpu_vulkan_clear(full, 1, all, color, 0.75f, 3);
	assert(began.viewMask == 3 && began.layerCount == 1);
	assert(last_view.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY);
	assert(last_view.subresourceRange.baseArrayLayer == 2 && last_view.subresourceRange.layerCount == 2);

	reset();
	depth_target.description.width = 32;
	depth_target.description.height = 24;
	xgpu_vulkan_clear((const int32_t[][4]){ { 0, 0, 32, 24 } }, 1, all, color, 0.75f, 3);
	assert(began.renderArea.extent.width == 32 && began.renderArea.extent.height == 24);
	assert(vk.statistics.attachment_clears == 3 && !clears);

	reset();
	xgpu_vulkan_set_target_views(&color_target, &depth_target, 3, 2);
	xgpu_vulkan_clear(full, 1, all, color, 0.75f, 3);
	assert(!begins && !clears);

	reset();
	{
		struct xgpu_vulkan_image *array = calloc(1, sizeof(*array));
		struct xgpu_vulkan_texture textures[4] = { 0 };
		VkDescriptorPool pool = (VkDescriptorPool)(uintptr_t)7;
		VkImageView first_layer;
		unsigned int index, views_freed = 0;

		assert(array);
		array->description = color_target.description;
		array->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		array->sampled = (VkImageView)(uintptr_t)21;
		textures[0].image = array;
		textures[0].kind = XGPU_VULKAN_IMAGE_2D;
		recording.descriptor_pools = &pool;
		recording.descriptor_pool_count = 1;
		assert(texture_set(textures));
		first_layer = written_images[0].imageView;
		assert(created_views == 1 && first_layer == array->sampled_2d);
		assert(first_layer != array->sampled && last_view.viewType == VK_IMAGE_VIEW_TYPE_2D);
		assert(last_view.subresourceRange.baseArrayLayer == 0 && last_view.subresourceRange.layerCount == 1);
		assert(texture_set(textures) && created_views == 1);
		textures[0].kind = XGPU_VULKAN_IMAGE_2D_ARRAY;
		assert(texture_set(textures) && written_images[0].imageView == array->sampled && created_views == 1);
		xgpu_vulkan_image_destroy(array);
		for (index = 0; index < vk.garbage_count; index++)
		{
			if (vk.garbage[index].kind == _garbage_view)
			{
				views_freed++;
				assert(vk.garbage[index].serial == 11);
			}
		}
		assert(views_freed == 2);
	}
	reset();
	{
		struct xgpu_vulkan_image *loading_image = calloc(1, sizeof(*loading_image));
		struct loading_presentation_frame frame = { 0 };
		struct query_frame queries = { 0 };
		unsigned int index;
		float first_color;

		assert(loading_image);
		loading_image->description = color_target.description;
		loading_image->description.type = XGPU_VULKAN_IMAGE_2D;
		loading_image->description.format = XGPU_VULKAN_FORMAT_BGRA8;
		loading_image->description.depth_or_layers = 1;
		loading_image->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		vk.loading_image = loading_image;
		xgpu_vulkan_set_target_views(&color_target, &depth_target, 2, 2);
		xgpu_vulkan_clear(full, 1, all, color, 0.75f, 3);
		/* Simulate a game query spanning a preparation wait. Loading
		passes close it once, never allocate more query segments, and
		leave the game's target, test and frame number intact. */
		vk.test_active = vk.test_query_open = 1;
		vk.test_query_count = 1;
		vk.test_number = 37;
		vk.queries = &queries;
		ended_queries = 0;
		frame.active = 1;
		loading_presentation_build(&frame);
		first_color = frame.rectangles[0].color[0];
		for (index = 0; index < 128; index++)
		{
			frame.elapsed_ns = (uint64_t)index * 16666667;
			loading_presentation_build(&frame);
			assert(xgpu_vulkan_loading_draw(&frame, 64, 48, XGPU_VULKAN_FORMAT_BGRA8) == loading_image);
			assert(vk.color == &color_target && vk.depth == &depth_target && vk.layer == 2 && vk.views == 2);
			assert(vk.test_active && !vk.test_query_open && vk.test_query_count == 1 && vk.test_number == 37);
			assert(!vk.statistics.frames && !vk.pass_active && queries.count == 0 && ended_queries == 1);
		}
		assert(first_color != frame.rectangles[0].color[0]);
		frame.active = 0;
		assert(!xgpu_vulkan_loading_draw(&frame, 0, 0, 0) && !vk.loading_image);
		vk.queries = NULL;
	}
	reset();
	puts("Vulkan passes, image views, vertex bindings, synchronization, anti-aliasing and the window passed");
	return 0;
}
