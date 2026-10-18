/*
PASSES_TEST.C

The real Vulkan core over a recording-only Vulkan: no driver or GPU.
Checks attachment load actions, clears inside a pass, multiview layer
selection, sampling an array's first layer as a 2D image, and capability
requirements on devices with optional depth formats or query precision.
Also checks packed vertex bindings and transfer/submission dependencies,
and the texture sets a submission reuses.
*/

#include "../xgpu_vulkan.c"
#include <assert.h>
#include <stdarg.h>

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
	submissions_test();
	texture_sets_test();
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
	xgpu_vulkan_clear(full, 1, XGPU_VULKAN_CLEAR_COLOR | XGPU_VULKAN_CLEAR_RED, color, 0.0f, 0);
	assert(began_color.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && clears == 1);
	assert(vk.statistics.attachment_clears == 0);

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
	puts("Vulkan passes, image views, vertex bindings and synchronization passed");
	return 0;
}
