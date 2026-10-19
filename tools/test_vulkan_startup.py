"""Vulkan startup handles Android device enumeration (the swapchain's rotation, surface
loss and present modes: port/linux/vulkan/tests/passes_test.c, tools/test_vulkan_passes.py)."""

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
