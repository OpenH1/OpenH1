/*
PASSES_TEST.C

The real Vulkan core over a recording-only Vulkan: no driver or GPU.
Checks attachment load actions, clears inside a pass, multiview layer
selection, sampling an array's first layer as a 2D image, and capability
requirements on devices with optional depth formats or query precision.
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
	*set = (VkDescriptorSet)(uintptr_t)50;
	return VK_SUCCESS;
}

static void VKAPI_CALL fake_update(VkDevice device, uint32_t count, const VkWriteDescriptorSet *writes,
	uint32_t copies, const VkCopyDescriptorSet *copy)
{
	(void)device; (void)copies; (void)copy;
	assert(count == 1 && writes[0].descriptorCount == 4);
	memcpy(written_images, writes[0].pImageInfo, sizeof(written_images));
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

int main(void)
{
	static const int32_t full[1][4] = { { 0, 0, 64, 48 } };
	static const int32_t partial[1][4] = { { 4, 5, 10, 12 } };
	static const int32_t beyond[2][4] = { { 70, 0, 1, 1 }, { -20, -20, 100, 100 } };
	static const float color[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
	uint32_t all = XGPU_VULKAN_CLEAR_COLOR | XGPU_VULKAN_CLEAR_CHANNELS |
		XGPU_VULKAN_CLEAR_DEPTH | XGPU_VULKAN_CLEAR_STENCIL;

	capabilities_test();
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
	puts("Vulkan passes: full/partial/aspect clears, multiview and sampled array views passed");
	return 0;
}
