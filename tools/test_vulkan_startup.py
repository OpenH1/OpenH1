"""Vulkan startup handles Android device enumeration and surface rotation."""

from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]


def test_physical_device_enumeration(tmp_path):
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        pytest.skip("needs a C compiler")
    text = (ROOT / "port/linux/vulkan/xgpu_vulkan.c").read_text()
    function = text[text.index("static int physical_choose("):
                    text.index("static int device_make(")]
    source = tmp_path / "devices.c"
    source.write_text(r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define VK_API_VERSION_1_3 13
#define VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU 2
#define VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU 1
#define VK_QUEUE_GRAPHICS_BIT 1
#define VK_TRUE 1
#define HOST_LOG_ERROR 1
typedef unsigned VkPhysicalDevice;
typedef unsigned VkBool32;
typedef struct {
    unsigned apiVersion, deviceType;
    struct { float timestampPeriod; } limits;
} VkPhysicalDeviceProperties;
typedef struct { unsigned queueFlags, timestampValidBits; } VkQueueFamilyProperties;
struct xgpu_vulkan_options { VkPhysicalDevice (*choose_physical)(unsigned); };
static struct {
    unsigned instance, surface, physical, queue_family, timestamp_bits, memory_properties;
    VkPhysicalDeviceProperties properties;
    struct { int gpu_timestamps; } capabilities;
} vk;
static unsigned available = 1, count_calls, fetch_calls, warnings;
static int count_failure, fetch_failure, unsupported;
static int check(int result, const char *name) { assert(name); return result >= 0; }
static void host_logf(int priority, const char *format, ...)
{ assert(priority == HOST_LOG_ERROR && format); warnings++; }
static int vkEnumeratePhysicalDevices(unsigned instance, unsigned *count, VkPhysicalDevice *devices)
{
    unsigned index;
    assert(instance == 42);
    if (!devices) {
        count_calls++;
        *count = available;
        return count_failure ? -1 : 0;
    }
    fetch_calls++;
    /* The phone's driver leaves an oversized count unchanged. Android's
       loader then reads unwritten handles and crashes before returning. */
    assert(count_calls && *count > 0 && *count <= available && *count <= 16);
    if (fetch_failure) return -1;
    for (index = 0; index < *count; index++) devices[index] = index + 1;
    return *count < available ? 5 : 0; /* VK_INCOMPLETE */
}
static void vkGetPhysicalDeviceProperties(VkPhysicalDevice device, VkPhysicalDeviceProperties *properties)
{
    assert(device > 0 && device <= available);
    properties->apiVersion = unsupported ? 12 : 13;
    properties->deviceType = device == 2 ? 2 : 1;
    properties->limits.timestampPeriod = 1.0f;
}
static void vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice device, unsigned *count,
    VkQueueFamilyProperties *families)
{
    assert(device && *count >= 1);
    *count = 1;
    families[0].queueFlags = VK_QUEUE_GRAPHICS_BIT;
    families[0].timestampValidBits = 64;
}
static void vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice device, unsigned family,
    unsigned surface, VkBool32 *present)
{ assert(device && !family && surface); *present = VK_TRUE; }
static void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice device, unsigned *properties)
{ assert(device); *properties = 1; }
static VkPhysicalDevice runtime_device(unsigned instance) { assert(instance == 42); return 1; }
''' + function + r'''
static void reset(unsigned devices)
{
    memset(&vk, 0, sizeof(vk));
    vk.instance = 42;
    available = devices;
    count_calls = fetch_calls = warnings = 0;
    count_failure = fetch_failure = unsupported = 0;
}
int main(void)
{
    struct xgpu_vulkan_options options = { NULL };
    reset(1);
    assert(physical_choose(&options) && vk.physical == 1);
    assert(count_calls == 1 && fetch_calls == 1 && !warnings);
    assert(vk.capabilities.gpu_timestamps && vk.timestamp_bits == 64);
    reset(2);
    vk.surface = 1;
    assert(physical_choose(&options) && vk.physical == 2);
    reset(20);
    assert(physical_choose(&options) && vk.physical == 2);
    reset(0);
    assert(!physical_choose(&options) && count_calls == 1 && !fetch_calls && warnings == 1);
    reset(1);
    count_failure = 1;
    assert(!physical_choose(&options) && !fetch_calls);
    reset(1);
    fetch_failure = 1;
    assert(!physical_choose(&options) && fetch_calls == 1 && !vk.physical);
    reset(1);
    unsupported = 1;
    assert(!physical_choose(&options) && warnings == 1);
    reset(1);
    options.choose_physical = runtime_device;
    assert(physical_choose(&options) && vk.physical == 1 && !count_calls && !fetch_calls);
    return 0;
}
''')
    binary = tmp_path / "devices"
    subprocess.run([compiler, "-std=gnu89", "-Wall", "-Wextra", "-Werror",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)


def test_swapchain_rotation(tmp_path):
    compiler = shutil.which("clang") or shutil.which("cc")
    candidates = [ROOT / "build/android/third_party/vulkan-headers",
                  ROOT / "build/linux_vulkan/third_party/vulkan-headers", Path("/usr/include")]
    headers = next((path for path in candidates if (path / "vulkan/vulkan_core.h").exists()), None)
    if not compiler or not headers:
        pytest.skip("needs a C compiler and Vulkan headers (an Android build, or configure --linux-vulkan)")
    text = (ROOT / "port/linux/vulkan/xgpu_vulkan.c").read_text()
    functions = text[text.index("static int swapchain_make("):
                     text.index("/* the image shown in the window:")]
    source = tmp_path / "swapchain.c"
    source.write_text(r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#define HOST_LOG_INFO 0
#define VK_HANDLE(type, value) ((type)(uintptr_t)(value))
static struct {
    VkDevice device;
    VkPhysicalDevice physical;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat swapchain_format;
    VkExtent2D swapchain_extent;
    VkSurfaceTransformFlagBitsKHR surface_transform;
    VkPresentModeKHR present_mode;
    unsigned window_width, window_height, swapchain_image_count;
    VkImage swapchain_images[8];
    VkSemaphore presentable[8];
    int swapchain_stale;
} vk;
static VkSurfaceCapabilitiesKHR current;
static VkSwapchainCreateInfoKHR made;
static int capability_failure;
static int check(VkResult result, const char *name) { assert(name); return result >= 0; }
static void host_logf(int priority, const char *format, ...) { (void)priority; (void)format; }
static VkResult vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physical, VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR *capabilities)
{ (void)physical; (void)surface; *capabilities = current; return capability_failure ? -1 : VK_SUCCESS; }
static void vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physical, VkSurfaceKHR surface,
    unsigned *count, VkSurfaceFormatKHR *formats)
{
    (void)physical; (void)surface; assert(*count >= 1); *count = 1;
    formats[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    formats[0].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
}
static void vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physical, VkSurfaceKHR surface,
    unsigned *count, VkPresentModeKHR *modes)
{ (void)physical; (void)surface; assert(*count >= 1); *count = 1; modes[0] = VK_PRESENT_MODE_FIFO_KHR; }
static void vkDeviceWaitIdle(VkDevice device) { (void)device; }
static void vkDestroySemaphore(VkDevice device, VkSemaphore semaphore, const void *allocator)
{ (void)device; (void)semaphore; (void)allocator; }
static void vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const void *allocator)
{ (void)device; (void)swapchain; (void)allocator; }
static VkResult vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *info,
    const void *allocator, VkSwapchainKHR *swapchain)
{ (void)device; (void)allocator; made = *info; *swapchain = VK_HANDLE(VkSwapchainKHR, 1); return VK_SUCCESS; }
static void vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, unsigned *count, VkImage *images)
{ (void)device; (void)swapchain; assert(*count >= 1); *count = 1; images[0] = VK_HANDLE(VkImage, 1); }
static void vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *info,
    const void *allocator, VkSemaphore *semaphore)
{ (void)device; (void)info; (void)allocator; *semaphore = VK_HANDLE(VkSemaphore, 1); }
''' + functions + r'''
int main(void)
{
    unsigned index;
    current.minImageCount = 2;
    current.currentExtent.width = 2410;
    current.currentExtent.height = 1080;
    current.currentTransform = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR;
    current.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR | current.currentTransform;
    current.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    assert(swapchain_make());
    assert(made.preTransform == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR);
    assert(made.imageExtent.width == 2410 && made.imageExtent.height == 1080);
    /* Repeated SUBOPTIMAL presents with unchanged surface properties must
       not recreate the identity swapchain on every frame. */
    for (index = 0; index < 100; index++) assert(!swapchain_changed());
    current.currentExtent.width = 1080;
    current.currentExtent.height = 2410;
    assert(swapchain_changed());
    assert(swapchain_make() && !swapchain_changed());
    current.currentTransform = VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
    assert(swapchain_changed());
    assert(swapchain_make() && !swapchain_changed());
    current.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    assert(swapchain_make() && made.preTransform == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR);
    current.currentTransform = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR;
    current.supportedTransforms = current.currentTransform;
    assert(swapchain_make() && made.preTransform == current.currentTransform);
    current.currentExtent.width = current.currentExtent.height = 0xffffffffu;
    xgpu_vulkan_window_resized(800, 600);
    assert(vk.swapchain_stale && swapchain_make());
    assert(made.imageExtent.width == 800 && made.imageExtent.height == 600 && !swapchain_changed());
    capability_failure = 1;
    assert(swapchain_changed() && !swapchain_make());
    return 0;
}
''')
    binary = tmp_path / "swapchain"
    subprocess.run([compiler, "-std=gnu89", "-Wall", "-Wextra", "-Werror", f"-I{headers}",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
