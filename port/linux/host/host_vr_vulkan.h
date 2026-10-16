/*
HOST_VR_VULKAN.H

The host's Vulkan binding to OpenXR (XR_KHR_vulkan_enable2). No Vulkan
handle crosses the 32-bit guest ABI: vr_host_info carries sizes and image
counts only, and the render thread gets the acquired VkImage here.
*/

#ifndef __HALO_HOST_VR_VULKAN_H
#define __HALO_HOST_VR_VULKAN_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

/* Before the Vulkan backend starts, only when VR is requested: creates
an OpenXR instance, selects the headset and reads graphics requirements. */
int host_vr_vulkan_prepare(void);
/* Whether the guest requested VR while choosing its window binding. */
int host_vr_vulkan_requested(void);
/* Vulkan creation callbacks: the runtime adds the extensions and device
features it needs. The backend still chooses its queue and features. */
VkResult host_vr_vulkan_create_instance(PFN_vkGetInstanceProcAddr get_proc,
	const VkInstanceCreateInfo *create, const VkAllocationCallbacks *allocator, VkInstance *instance);
VkPhysicalDevice host_vr_vulkan_physical_device(VkInstance instance);
VkResult host_vr_vulkan_create_device(PFN_vkGetInstanceProcAddr get_proc, VkPhysicalDevice physical,
	const VkDeviceCreateInfo *create, const VkAllocationCallbacks *allocator, VkDevice *device);
void host_vr_vulkan_bind(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
	uint32_t queue_family, uint32_t queue_index);

/* After host_vr_initialize: actual runtime images, indexed by
host_vr_acquire's result. Invalid indices return VK_NULL_HANDLE. */
VkImage host_vr_vulkan_image(int swapchain, int index);
VkFormat host_vr_vulkan_format(int swapchain);
/* The backend's depth format; choose it only if the runtime offers it. */
void host_vr_vulkan_set_depth_format(VkFormat format);
/* After the renderer has finished using the images, before it destroys
the device: destroys the OpenXR session and its swapchains. */
void host_vr_vulkan_shutdown(void);

#endif
