/* The OpenXR bridge against a fake runtime: native Vulkan handles and the
acquire/wait/release contract, without a headset or graphics device. */
#define HALO_VULKAN 1
#include "../host/host_vr.c"
#include <assert.h>

static int acquisitions, waits, releases;
static XrResult wait_result = XR_SUCCESS;
static XrResult create_result = XR_SUCCESS;
static VkResult create_vk_result = VK_SUCCESS;
static VkPhysicalDevice chosen_physical;

void host_logf(int priority, const char *format, ...)
{
	(void)priority;
	(void)format;
}

static XrResult XRAPI_CALL acquire_image(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo *info,
	uint32_t *index)
{
	assert(swapchain == vr.swapchains[VR_SWAPCHAIN_EYES]);
	assert(info->type == XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
	acquisitions++;
	*index = 3;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL wait_image(XrSwapchain swapchain, const XrSwapchainImageWaitInfo *info)
{
	assert(swapchain == vr.swapchains[VR_SWAPCHAIN_EYES]);
	assert(info->type == XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
	waits++;
	return wait_result;
}

static XrResult XRAPI_CALL release_image(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *info)
{
	assert(swapchain == vr.swapchains[VR_SWAPCHAIN_EYES]);
	assert(info->type == XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
	releases++;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL create_instance(XrInstance instance, const XrVulkanInstanceCreateInfoKHR *info,
	VkInstance *made, VkResult *result)
{
	assert(instance == vr.instance && info->systemId == vr.system);
	assert(info->vulkanCreateInfo->pApplicationInfo->apiVersion == VK_API_VERSION_1_3);
	*made = (VkInstance)(uintptr_t)0x123456789abcdef0ull;
	*result = create_vk_result;
	return create_result;
}

static XrResult XRAPI_CALL get_device(XrInstance instance, const XrVulkanGraphicsDeviceGetInfoKHR *info,
	VkPhysicalDevice *physical)
{
	assert(instance == vr.instance && info->systemId == vr.system);
	assert(info->vulkanInstance == (VkInstance)(uintptr_t)0x123456789abcdef0ull);
	*physical = chosen_physical;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL create_device(XrInstance instance, const XrVulkanDeviceCreateInfoKHR *info,
	VkDevice *made, VkResult *result)
{
	assert(instance == vr.instance && info->systemId == vr.system);
	assert(info->vulkanPhysicalDevice == chosen_physical);
	*made = (VkDevice)(uintptr_t)0xabcdef0123456789ull;
	*result = create_vk_result;
	return create_result;
}

static XrResult XRAPI_CALL create_session(XrInstance instance, const XrSessionCreateInfo *info, XrSession *session)
{
	const XrGraphicsBindingVulkan2KHR *binding = info->next;

	assert(instance == vr.instance && info->systemId == vr.system);
	assert(binding->type == XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR);
	assert(binding->instance == (VkInstance)(uintptr_t)0x123456789abcdef0ull);
	assert(binding->physicalDevice == chosen_physical);
	assert(binding->device == (VkDevice)(uintptr_t)0xabcdef0123456789ull);
	assert(binding->queueFamilyIndex == 7 && binding->queueIndex == 0);
	*session = (XrSession)(uintptr_t)0x100000055ull;
	return XR_SUCCESS;
}

static int64_t offered_formats[4];
static uint32_t offered_count;
static int64_t created_format;

static XrResult XRAPI_CALL enumerate_formats(XrSession session, uint32_t capacity, uint32_t *count, int64_t *formats)
{
	(void)session;
	/* (the two-call idiom: the count, then as many as were asked for) */
	*count = offered_count;
	if (!capacity)
	{
		assert(!formats);
		return XR_SUCCESS;
	}
	if (capacity < offered_count)
		return XR_ERROR_SIZE_INSUFFICIENT;
	memcpy(formats, offered_formats, offered_count * sizeof(*formats));
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL create_swapchain(XrSession session, const XrSwapchainCreateInfo *info, XrSwapchain *swapchain)
{
	(void)session;
	assert(info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT);
	created_format = info->format;
	*swapchain = (XrSwapchain)(uintptr_t)0x100000066ull;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL enumerate_images(XrSwapchain swapchain, uint32_t capacity, uint32_t *count,
	XrSwapchainImageBaseHeader *images)
{
	XrSwapchainImageVulkan2KHR *native = (XrSwapchainImageVulkan2KHR *)images;
	uint32_t index;

	(void)swapchain;
	*count = 6;
	if (!capacity)
		return XR_SUCCESS;
	assert(capacity == 6);
	for (index = 0; index < 6; index++)
	{
		assert(native[index].type == XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR);
		native[index].image = (VkImage)(uintptr_t)(0x1122334455667783ull + index);
	}
	return XR_SUCCESS;
}

int main(void)
{
	VkApplicationInfo application = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	VkInstanceCreateInfo instance_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	VkDeviceCreateInfo device_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	VkInstance instance;
	VkDevice device;
	VkImage image = (VkImage)(uintptr_t)0x1122334455667788ull;

	xrAcquireSwapchainImage = acquire_image;
	xrWaitSwapchainImage = wait_image;
	xrReleaseSwapchainImage = release_image;
	vr.swapchains[VR_SWAPCHAIN_EYES] = (XrSwapchain)(uintptr_t)0x100000011ull;
	vr.acquired[VR_SWAPCHAIN_EYES] = -1;
	/* A timeout leaves one pending acquisition. It may neither be drawn
	nor released, and the next call must wait for that same image. */
	wait_result = XR_TIMEOUT_EXPIRED;
	assert(host_vr_acquire(VR_SWAPCHAIN_EYES) == -1);
	host_vr_release(VR_SWAPCHAIN_EYES);
	assert(acquisitions == 1 && waits == 1 && releases == 0);
	assert(host_vr_acquire(VR_SWAPCHAIN_EYES) == -1);
	assert(acquisitions == 1 && waits == 2);
	wait_result = XR_SUCCESS;
	assert(host_vr_acquire(VR_SWAPCHAIN_EYES) == 3);
	assert(host_vr_acquire(VR_SWAPCHAIN_EYES) == 3);
	assert(acquisitions == 1 && waits == 3);
	host_vr_release(VR_SWAPCHAIN_EYES);
	host_vr_release(VR_SWAPCHAIN_EYES);
	assert(releases == 1);
	/* Errors also retain ownership; session-loss pending is an available
	image according to the OpenXR graphics binding contract. */
	wait_result = XR_ERROR_RUNTIME_FAILURE;
	assert(host_vr_acquire(VR_SWAPCHAIN_EYES) == -1);
	assert(acquisitions == 2);
	wait_result = XR_SESSION_LOSS_PENDING;
	assert(host_vr_acquire(VR_SWAPCHAIN_EYES) == 3);
	host_vr_release(VR_SWAPCHAIN_EYES);
	assert(releases == 2 && acquisitions == 2);

	/* The runtime can return more images than the guest's GL array and
	handles whose upper bits are significant. */
	xrEnumerateSwapchainFormats = enumerate_formats;
	xrCreateSwapchain = create_swapchain;
	xrEnumerateSwapchainImages = enumerate_images;
	offered_count = 3;
	offered_formats[0] = VK_FORMAT_R8G8B8A8_SRGB;
	offered_formats[1] = VK_FORMAT_B8G8R8A8_UNORM;
	offered_formats[2] = VK_FORMAT_B8G8R8A8_SRGB;
	assert(swapchain_create(VR_SWAPCHAIN_EYES, 64, 64, 2, &vr.info));
	assert(created_format == VK_FORMAT_B8G8R8A8_SRGB);
	assert(vr.info.image_count[VR_SWAPCHAIN_EYES] == 6);
	assert(host_vr_vulkan_image(VR_SWAPCHAIN_EYES, 5) == image);
	assert(host_vr_vulkan_image(VR_SWAPCHAIN_EYES, 6) == VK_NULL_HANDLE);
	assert(host_vr_vulkan_image(-1, 0) == VK_NULL_HANDLE);
	assert(host_vr_vulkan_image(VR_SWAPCHAIN_EYES, -1) == VK_NULL_HANDLE);
	assert(vr.info.images[VR_SWAPCHAIN_EYES][0] == 0);
	/* Raw copies require matching sRGB channels. UNORM-only runtimes
	cannot present the game's encoded bytes without changing their colour. */
	offered_count = 1;
	offered_formats[0] = VK_FORMAT_B8G8R8A8_UNORM;
	assert(!swapchain_create(VR_SWAPCHAIN_HUD, 64, 64, 1, &vr.info));
	offered_formats[0] = VK_FORMAT_R8G8B8A8_SRGB;
	assert(swapchain_create(VR_SWAPCHAIN_HUD, 64, 64, 1, &vr.info));
	assert(created_format == VK_FORMAT_R8G8B8A8_SRGB);
	/* Depth copies require the device's exact chosen depth/stencil format. */
	host_vr_vulkan_set_depth_format(VK_FORMAT_D24_UNORM_S8_UINT);
	offered_formats[0] = VK_FORMAT_D32_SFLOAT;
	assert(!swapchain_create(VR_SWAPCHAIN_DEPTH, 64, 64, 2, &vr.info));
	offered_formats[0] = VK_FORMAT_D24_UNORM_S8_UINT;
	assert(swapchain_create(VR_SWAPCHAIN_DEPTH, 64, 64, 2, &vr.info));
	assert(created_format == VK_FORMAT_D24_UNORM_S8_UINT);

	/* Flat mode does not probe or initialize OpenXR. The instance made
	for device selection is reused when the render thread makes a session. */
	vr.binding = VR_BINDING_VULKAN;
	assert(!host_vr_vulkan_requested());
	vr.probed = 1;
	assert(host_vr_vulkan_requested());
	vulkan.prepared = 1;
	vulkan.requirements.minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
	vr.instance = (XrInstance)(uintptr_t)0x100000022ull;
	vr.system = 42;
	assert(instance_create());
	assert(vr.instance == (XrInstance)(uintptr_t)0x100000022ull);
	chosen_physical = (VkPhysicalDevice)(uintptr_t)0x1234567887654321ull;
	xrCreateVulkanInstanceKHR = create_instance;
	xrGetVulkanGraphicsDevice2KHR = get_device;
	xrCreateVulkanDeviceKHR = create_device;
	xrCreateSession = create_session;
	application.apiVersion = VK_API_VERSION_1_3;
	instance_info.pApplicationInfo = &application;
	assert(host_vr_vulkan_create_instance(NULL, &instance_info, NULL, &instance) == VK_SUCCESS);
	assert(host_vr_vulkan_physical_device(instance) == chosen_physical);
	assert(host_vr_vulkan_create_device(NULL, chosen_physical, &device_info, NULL, &device) == VK_SUCCESS);
	host_vr_vulkan_bind(instance, chosen_physical, device, 7, 0);
	assert(session_create());
	/* Preserve Vulkan failures even when the OpenXR call succeeded, and
	preserve OpenXR failures even when a runtime supplied VK_SUCCESS. */
	create_vk_result = VK_ERROR_EXTENSION_NOT_PRESENT;
	assert(host_vr_vulkan_create_device(NULL, chosen_physical, &device_info, NULL, &device) ==
		VK_ERROR_EXTENSION_NOT_PRESENT);
	create_vk_result = VK_SUCCESS;
	create_result = XR_ERROR_RUNTIME_FAILURE;
	assert(host_vr_vulkan_create_instance(NULL, &instance_info, NULL, &instance) == VK_ERROR_INITIALIZATION_FAILED);
	free(vulkan.images[VR_SWAPCHAIN_EYES]);
	free(vulkan.images[VR_SWAPCHAIN_HUD]);
	free(vulkan.images[VR_SWAPCHAIN_DEPTH]);
	puts("ok");
	return 0;
}
