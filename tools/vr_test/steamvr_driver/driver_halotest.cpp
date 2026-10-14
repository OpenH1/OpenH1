/*
DRIVER_HALOTEST.CPP

A headset for SteamVR on Linux without a headset or a display: an OpenVR
driver whose HMD is in driver direct mode (IVRDriverDirectModeComponent), as
a streamed headset's is, so that SteamVR's compositor needs no display of
its own (on Linux it otherwise shows a headset only in direct mode, even the
null driver's). The frames come to the driver as layers of SteamVR's shared
Vulkan images (IVRIPCResourceManagerClient): the application's eyes, and
SteamVR's system layer (its overlays, an OpenXR application's quad layers
among them) over them. The driver paces them at its refresh rate and drops
them, but every captureEvery frames writes each layer, both eyes side by
side, to captureDirectory as a PAM file (RGBA), read back through a Vulkan
device of its own; and every 450 frames it logs SteamVR's frame timing
(GetFrameTimings) to vrserver.txt. The headset is still, at a standing
height (but for a tenth of a millimetre of sway, without which SteamVR puts
it into standby), with 1512x1680 eyes (SteamVR's null driver's) at 90 Hz,
and has no controllers.

tools/vr_test/steamvr_driver.sh builds it and runs a command with SteamVR
and the driver in a configuration folder of its own; refer to
tools/vr_test/README.md.
*/

#include <openvr_driver.h>
#include <vulkan/vulkan.h>

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

using namespace vr;

namespace
{
const uint32_t eye_width = 1512, eye_height = 1680; /* the null driver's */
const float frequency = 90.0f;

void DriverLog(const char *format, ...)
{
	char text[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	if (VRDriverLog())
		VRDriverLog()->Log(text);
}

double seconds_now()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

IVRIPCResourceManagerClient *resources;

/* ---------- the frames' capture: SteamVR's shared images, imported into a
Vulkan device of the driver's own and copied to the host as PPM files
(driver_halotest's captureDirectory and captureEvery settings) */

struct capture
{
	int tried, ready;
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	VkCommandPool pool;
} vk;

int capture_initialize()
{
	VkApplicationInfo application = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	VkInstanceCreateInfo instance = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	VkDeviceQueueCreateInfo queue = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	VkDeviceCreateInfo device = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	VkCommandPoolCreateInfo pool = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	const char *extensions[] = { VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME };
	uint32_t count = 1;
	float priority = 1.0f;

	if (vk.tried)
		return vk.ready;
	vk.tried = 1;
	application.apiVersion = VK_API_VERSION_1_1;
	instance.pApplicationInfo = &application;
	if (vkCreateInstance(&instance, nullptr, &vk.instance) != VK_SUCCESS ||
		vkEnumeratePhysicalDevices(vk.instance, &count, &vk.physical) < 0 || !count)
	{
		DriverLog("halotest: capture: no Vulkan device\n");
		return 0;
	}
	vk.family = 0;
	queue.queueFamilyIndex = vk.family;
	queue.queueCount = 1;
	queue.pQueuePriorities = &priority;
	device.queueCreateInfoCount = 1;
	device.pQueueCreateInfos = &queue;
	device.enabledExtensionCount = 2;
	device.ppEnabledExtensionNames = extensions;
	if (vkCreateDevice(vk.physical, &device, nullptr, &vk.device) != VK_SUCCESS)
	{
		device.enabledExtensionCount = 1;
		if (vkCreateDevice(vk.physical, &device, nullptr, &vk.device) != VK_SUCCESS)
		{
			DriverLog("halotest: capture: cannot make a Vulkan device\n");
			return 0;
		}
	}
	vkGetDeviceQueue(vk.device, vk.family, 0, &vk.queue);
	pool.queueFamilyIndex = vk.family;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	vkCreateCommandPool(vk.device, &pool, nullptr, &vk.pool);
	vk.ready = 1;
	return 1;
}

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags wanted)
{
	VkPhysicalDeviceMemoryProperties properties;
	uint32_t index;

	vkGetPhysicalDeviceMemoryProperties(vk.physical, &properties);
	for (index = 0; index < properties.memoryTypeCount; index++)
	{
		if ((bits & (1u << index)) && (properties.memoryTypes[index].propertyFlags & wanted) == wanted)
			return index;
	}
	return 0;
}

/* the shared images imported so far: each once (its file descriptor can
be had once), kept until its swap texture set goes */
struct imported_image
{
	SharedTextureHandle_t handle;
	VkImage image;
	VkDeviceMemory memory;
} imported[64];

void imported_release(SharedTextureHandle_t handle)
{
	int index;

	for (index = 0; index < 64; index++)
	{
		if (imported[index].handle == handle && handle)
		{
			vkDestroyImage(vk.device, imported[index].image, nullptr);
			vkFreeMemory(vk.device, imported[index].memory, nullptr);
			resources->UnrefResource(handle);
			memset(&imported[index], 0, sizeof(imported[index]));
		}
	}
}

/* one shared image's pixels (RGBA8), into pixels (width * height * 4) */
int capture_image(SharedTextureHandle_t handle, uint32_t format, uint32_t width, uint32_t height, unsigned char *pixels)
{
	VkExternalMemoryImageCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
	VkImageCreateInfo create = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	VkImportMemoryFdInfoKHR import = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR };
	VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkBufferCreateInfo buffer_create = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	VkCommandBufferAllocateInfo command_allocate = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	VkBufferImageCopy region = {};
	VkMemoryRequirements requirements;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE, host_memory = VK_NULL_HANDLE;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkCommandBuffer commands = VK_NULL_HANDLE;
	VkResult result;
	uint64_t ipc = 0;
	int fd = -1, success = 0;
	off_t size;
	void *mapped;

	struct imported_image *cached = nullptr;
	int index;

	if (!resources || !capture_initialize())
		return 0;
	for (index = 0; index < 64 && !cached; index++)
	{
		if (imported[index].handle == handle)
			cached = &imported[index];
	}
	if (cached)
	{
		image = cached->image;
		goto copy;
	}
	for (index = 0; index < 64 && !cached; index++)
	{
		if (!imported[index].handle)
			cached = &imported[index];
	}
	if (!cached)
		return 0;
	if (!resources->RefResource(handle, &ipc) || !resources->ReceiveSharedFd(ipc, &fd) || fd < 0)
	{
		DriverLog("halotest: capture: no file descriptor for the shared image\n");
		resources->UnrefResource(handle);
		return 0;
	}
	size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	create.pNext = &external;
	create.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	create.imageType = VK_IMAGE_TYPE_2D;
	create.format = (VkFormat)format;
	create.extent = { width, height, 1 };
	create.mipLevels = 1;
	create.arrayLayers = 1;
	create.samples = VK_SAMPLE_COUNT_1_BIT;
	create.tiling = VK_IMAGE_TILING_OPTIMAL;
	create.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if ((result = vkCreateImage(vk.device, &create, nullptr, &image)) != VK_SUCCESS)
	{
		DriverLog("halotest: capture: vkCreateImage %d\n", (int)result);
		goto done;
	}
	vkGetImageMemoryRequirements(vk.device, image, &requirements);
	import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	import.fd = fd;
	dedicated.image = image;
	import.pNext = &dedicated;
	allocate.pNext = &import;
	allocate.allocationSize = size > 0 ? (VkDeviceSize)size : requirements.size;
	allocate.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if ((result = vkAllocateMemory(vk.device, &allocate, nullptr, &memory)) != VK_SUCCESS)
	{
		DriverLog("halotest: capture: importing the image's memory (%lld bytes, %lld wanted): %d\n", (long long)size,
			(long long)requirements.size, (int)result);
		goto done;
	}
	fd = -1; /* (the memory owns it) */
	vkBindImageMemory(vk.device, image, memory, 0);
	cached->handle = handle;
	cached->image = image;
	cached->memory = memory;
copy:
	buffer_create.size = (VkDeviceSize)width * height * 4;
	buffer_create.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	vkCreateBuffer(vk.device, &buffer_create, nullptr, &buffer);
	vkGetBufferMemoryRequirements(vk.device, buffer, &requirements);
	allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = memory_type(requirements.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	vkAllocateMemory(vk.device, &allocate, nullptr, &host_memory);
	vkBindBufferMemory(vk.device, buffer, host_memory, 0);
	command_allocate.commandPool = vk.pool;
	command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command_allocate.commandBufferCount = 1;
	vkAllocateCommandBuffers(vk.device, &command_allocate, &commands);
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(commands, &begin);
	/* (the compositor leaves its images in the general layout, or one whose
	contents the transition keeps on this driver) */
	barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	barrier.dstQueueFamilyIndex = vk.family;
	barrier.image = image;
	barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
		nullptr, 1, &barrier);
	region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.imageExtent = { width, height, 1 };
	vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
	vkEndCommandBuffer(commands);
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &commands;
	vkQueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE);
	vkQueueWaitIdle(vk.queue);
	if (vkMapMemory(vk.device, host_memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS)
	{
		memcpy(pixels, mapped, (size_t)width * height * 4);
		vkUnmapMemory(vk.device, host_memory);
		success = 1;
	}
done:
	if (commands)
		vkFreeCommandBuffers(vk.device, vk.pool, 1, &commands);
	if (buffer)
		vkDestroyBuffer(vk.device, buffer, nullptr);
	if (host_memory)
		vkFreeMemory(vk.device, host_memory, nullptr);
	if (!cached || cached->handle != handle)
	{
		/* (the import failed) */
		if (image)
			vkDestroyImage(vk.device, image, nullptr);
		if (memory)
			vkFreeMemory(vk.device, memory, nullptr);
		resources->UnrefResource(handle);
	}
	if (fd >= 0)
		close(fd);
	return success;
}

/* both eyes' images side by side, as a PAM (RGBA) */
void capture_write(const char *path, unsigned char *const eyes[2], uint32_t width, uint32_t height)
{
	FILE *file = fopen(path, "wb");
	unsigned char *row;
	uint32_t y, x;
	int eye;

	if (!file)
		return;
	row = new unsigned char[(size_t)width * 2 * 4];
	fprintf(file, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", width * 2, height);
	for (y = 0; y < height; y++)
	{
		for (eye = 0; eye < 2; eye++)
		{
			for (x = 0; x < width; x++)
				memcpy(&row[((size_t)eye * width + x) * 4], &eyes[eye][((size_t)y * width + x) * 4], 4);
		}
		fwrite(row, 1, (size_t)width * 2 * 4, file);
	}
	delete[] row;
	fclose(file);
}

class hmd : public ITrackedDeviceServerDriver, public IVRDisplayComponent, public IVRDriverDirectModeComponent
{
public:
	uint32_t index = k_unTrackedDeviceIndexInvalid;
	double start = seconds_now();
	uint64_t presents = 0;
	std::mutex lock;
	struct set
	{
		SharedTextureHandle_t handles[3];
		uint32_t next, format, width, height;
	} sets[64] = {};
	SharedTextureHandle_t submitted[8][2] = {}, frame_layers[8][2] = {};
	int layers = 0, layer_count = 0, layers_logged = 0;
	char capture_directory[512] = "";
	int capture_every = 0;

	EVRInitError Activate(uint32_t object) override
	{
		PropertyContainerHandle_t props;

		index = object;
		props = VRProperties()->TrackedDeviceToPropertyContainer(object);
		VRProperties()->SetStringProperty(props, Prop_ModelNumber_String, "halotest");
		VRProperties()->SetStringProperty(props, Prop_ManufacturerName_String, "halotest");
		VRProperties()->SetStringProperty(props, Prop_TrackingSystemName_String, "halotest");
		VRProperties()->SetFloatProperty(props, Prop_UserIpdMeters_Float, 0.064f);
		VRProperties()->SetFloatProperty(props, Prop_UserHeadToEyeDepthMeters_Float, 0.0f);
		VRProperties()->SetFloatProperty(props, Prop_DisplayFrequency_Float, frequency);
		VRProperties()->SetFloatProperty(props, Prop_SecondsFromVsyncToPhotons_Float, 0.011f);
		VRProperties()->SetUint64Property(props, Prop_CurrentUniverseId_Uint64, 2);
		VRProperties()->SetBoolProperty(props, Prop_IsOnDesktop_Bool, false);
		VRProperties()->SetBoolProperty(props, Prop_DisplayDebugMode_Bool, false);
		VRProperties()->SetBoolProperty(props, Prop_HasDisplayComponent_Bool, true);
		VRProperties()->SetBoolProperty(props, Prop_HasDriverDirectModeComponent_Bool, true);
		return VRInitError_None;
	}
	void Deactivate() override { index = k_unTrackedDeviceIndexInvalid; }
	void EnterStandby() override {}
	void *GetComponent(const char *name) override
	{
		if (!strcmp(name, IVRDisplayComponent_Version))
			return static_cast<IVRDisplayComponent *>(this);
		if (!strcmp(name, IVRDriverDirectModeComponent_Version))
			return static_cast<IVRDriverDirectModeComponent *>(this);
		return nullptr;
	}

	/* IVRDriverDirectModeComponent */
	void CreateSwapTextureSet(uint32_t, const SwapTextureSetDesc_t *desc, SwapTextureSet_t *out) override
	{
		std::lock_guard<std::mutex> guard(lock);
		int which, image;

		memset(out, 0, sizeof(*out));
		/* (the resource manager is made after the driver's Init: asked for
		here, as the header's accessor does) */
		if (!resources)
			resources = VRIPCResourceManager();
		for (which = 0; which < 64 && sets[which].handles[0]; which++)
			;
		if (which == 64 || !resources)
			return;
		for (image = 0; image < 3; image++)
		{
			if (!resources->NewSharedVulkanImage(desc->nFormat, desc->nWidth, desc->nHeight, true, false, false, 1, 1, 0, 0,
				&out->rSharedTextureHandles[image]))
			{
				DriverLog("halotest: NewSharedVulkanImage(%u, %ux%u) failed\n", desc->nFormat, desc->nWidth, desc->nHeight);
			}
			sets[which].handles[image] = out->rSharedTextureHandles[image];
		}
		sets[which].next = 0;
		sets[which].format = desc->nFormat;
		sets[which].width = desc->nWidth;
		sets[which].height = desc->nHeight;
		DriverLog("halotest: swap texture set %d: format %u, %ux%u\n", which, desc->nFormat, desc->nWidth, desc->nHeight);
	}
	void DestroySwapTextureSet(SharedTextureHandle_t handle) override
	{
		std::lock_guard<std::mutex> guard(lock);
		int which, image;

		for (which = 0; which < 64; which++)
		{
			for (image = 0; image < 3; image++)
			{
				if (sets[which].handles[image] == handle)
				{
					for (image = 0; image < 3; image++)
					{
						if (vk.ready)
							imported_release(sets[which].handles[image]);
						resources->UnrefResource(sets[which].handles[image]);
					}
					memset(&sets[which], 0, sizeof(sets[which]));
					return;
				}
			}
		}
	}
	void DestroyAllSwapTextureSets(uint32_t) override {}
	void GetNextSwapTextureSetIndex(SharedTextureHandle_t handles[2], uint32_t (*indices)[2]) override
	{
		std::lock_guard<std::mutex> guard(lock);
		int eye, which, image;

		for (eye = 0; eye < 2; eye++)
		{
			(*indices)[eye] = 0;
			for (which = 0; which < 64; which++)
			{
				for (image = 0; image < 3; image++)
				{
					if (handles[eye] && sets[which].handles[image] == handles[eye])
					{
						(*indices)[eye] = (image + 1) % 3;
					}
				}
			}
		}
	}
	void SubmitLayer(const SubmitLayerPerEye_t (&eyes)[2]) override
	{
		/* (the frame's layers: the application's, as the compositor passes
		them on) */
		if (layers < 8)
		{
			frame_layers[layers][0] = eyes[0].hTexture;
			frame_layers[layers][1] = eyes[1].hTexture;
		}
		layers++;
	}
	const set *set_of(SharedTextureHandle_t handle)
	{
		int which, image;

		for (which = 0; which < 64; which++)
		{
			for (image = 0; image < 3; image++)
			{
				if (handle && sets[which].handles[image] == handle)
					return &sets[which];
			}
		}
		return nullptr;
	}
	void Present(SharedTextureHandle_t) override
	{
		if (layers != layers_logged)
		{
			DriverLog("halotest: frames arrive: %d layers a frame\n", layers);
			layers_logged = layers;
		}
		layer_count = layers < 8 ? layers : 8;
		memcpy(submitted, frame_layers, sizeof(submitted));
		layers = 0;
		presents++;
		if (presents % 450 == 0)
			timing_log();
		if (capture_every > 0 && capture_directory[0] && presents % (uint64_t)capture_every == 0)
			capture_frame();
	}
	/* SteamVR's timing of the last 90 frames: its compositor's (in driver
	direct mode it measures no GPU time of its own), and the GPU's for all
	of the frame */
	void timing_log()
	{
		Compositor_FrameTiming timings[90];
		uint32_t count, index, dropped = 0, reprojected = 0;
		double render_gpu = 0, render_cpu = 0, total_gpu = 0, interval = 0, idle = 0;

		timings[0].m_nSize = sizeof(Compositor_FrameTiming);
		for (index = 1; index < 90; index++)
			timings[index].m_nSize = sizeof(Compositor_FrameTiming);
		count = VRServerDriverHost()->GetFrameTimings(timings, 90);
		for (index = 0; index < count; index++)
		{
			render_gpu += timings[index].m_flCompositorRenderGpuMs;
			render_cpu += timings[index].m_flCompositorRenderCpuMs;
			total_gpu += timings[index].m_flTotalRenderGpuMs;
			interval += timings[index].m_flClientFrameIntervalMs;
			idle += timings[index].m_flCompositorIdleCpuMs;
			dropped += timings[index].m_nNumDroppedFrames;
			reprojected += (timings[index].m_nReprojectionFlags & 0x0F) != 0;
		}
		if (count)
		{
			DriverLog("halotest: timing over %u frames: compositor GPU %.2f ms, compositor CPU %.2f ms, total GPU %.2f ms, "
				"app frame interval %.2f ms, compositor idle %.2f ms, %u dropped, %u reprojected\n", count,
				render_gpu / count, render_cpu / count, total_gpu / count, interval / count, idle / count, dropped,
				reprojected);
		}
	}
	/* each layer of the last frame (the application's eyes, SteamVR's
	system layer), both eyes side by side. (This takes the server's thread a
	fifth of a second: the next frame or two are late) */
	void capture_frame()
	{
		int layer, eye, captured;
		char path[600];

		for (layer = 0; layer < layer_count; layer++)
		{
			const set *eyes[2] = { set_of(submitted[layer][0]), set_of(submitted[layer][1]) };
			unsigned char *pixels[2];

			if (!eyes[0] || !eyes[1] || eyes[0]->width != eyes[1]->width || eyes[0]->height != eyes[1]->height ||
				eyes[0]->width < 64)
			{
				continue;
			}
			for (eye = 0; eye < 2; eye++)
				pixels[eye] = new unsigned char[(size_t)eyes[eye]->width * eyes[eye]->height * 4];
			captured = 1;
			for (eye = 0; eye < 2 && captured; eye++)
				captured = capture_image(submitted[layer][eye], eyes[eye]->format, eyes[eye]->width, eyes[eye]->height,
					pixels[eye]);
			if (captured)
			{
				snprintf(path, sizeof(path), "%s/steamvr%06llu_layer%d.pam", capture_directory,
					(unsigned long long)presents, layer);
				capture_write(path, pixels, eyes[0]->width, eyes[0]->height);
				DriverLog("halotest: captured %s (%ux%u)\n", path, eyes[0]->width, eyes[0]->height);
			}
			for (eye = 0; eye < 2; eye++)
				delete[] pixels[eye];
		}
	}
	void PostPresent(const Throttling_t *) override
	{
		/* the next vsync */
		double period = 1.0 / frequency, elapsed = seconds_now() - start;
		double next = (std::floor(elapsed / period) + 1.0) * period;

		std::this_thread::sleep_for(std::chrono::duration<double>(next - elapsed));
		VRServerDriverHost()->VsyncEvent(0.0);
	}
	void GetFrameTiming(DriverDirectMode_FrameTiming *timing) override
	{
		timing->m_nReprojectionFlags = 0;
		timing->m_nNumFramePresents = 1;
		timing->m_nNumMisPresented = 0;
		timing->m_nNumDroppedFrames = 0;
	}
	void DebugRequest(const char *, char *response, uint32_t size) override
	{
		if (size)
			response[0] = 0;
	}
	DriverPose_t GetPose() override
	{
		DriverPose_t pose = {};

		pose.poseIsValid = true;
		pose.result = TrackingResult_Running_OK;
		pose.deviceIsConnected = true;
		pose.qWorldFromDriverRotation.w = pose.qDriverFromHeadRotation.w = pose.qRotation.w = 1.0;
		/* (a worn headset never holds still: SteamVR puts one that does into
		standby after some seconds, and the application out of sight. A
		tenth of a millimetre and a hundredth of a degree, at 1 Hz) */
		{
			double phase = std::sin((seconds_now() - start) * 2.0 * 3.14159265358979);

			pose.vecPosition[0] = 0.0001 * phase;
			pose.qRotation.y = 0.00009 * phase;
			pose.qRotation.w = std::sqrt(1.0 - pose.qRotation.y * pose.qRotation.y);
		}
		pose.vecPosition[1] = 1.7;
		return pose;
	}

	/* IVRDisplayComponent: no window; both eyes side by side */
	void GetWindowBounds(int32_t *x, int32_t *y, uint32_t *width, uint32_t *height) override
	{
		*x = *y = 0;
		*width = eye_width * 2;
		*height = eye_height;
	}
	bool IsDisplayOnDesktop() override { return false; }
	bool IsDisplayRealDisplay() override { return false; }
	void GetRecommendedRenderTargetSize(uint32_t *width, uint32_t *height) override
	{
		*width = eye_width;
		*height = eye_height;
	}
	void GetEyeOutputViewport(EVREye eye, uint32_t *x, uint32_t *y, uint32_t *width, uint32_t *height) override
	{
		*x = eye == Eye_Left ? 0 : eye_width;
		*y = 0;
		*width = eye_width;
		*height = eye_height;
	}
	void GetProjectionRaw(EVREye eye, float *left, float *right, float *top, float *bottom) override
	{
		/* about 100 degrees each way, a little more outward */
		*left = eye == Eye_Left ? -1.25f : -1.05f;
		*right = eye == Eye_Left ? 1.05f : 1.25f;
		*top = -1.2f;
		*bottom = 1.2f;
	}
	DistortionCoordinates_t ComputeDistortion(EVREye, float u, float v) override
	{
		DistortionCoordinates_t c;

		c.rfRed[0] = c.rfGreen[0] = c.rfBlue[0] = u;
		c.rfRed[1] = c.rfGreen[1] = c.rfBlue[1] = v;
		return c;
	}
	bool ComputeInverseDistortion(HmdVector2_t *, EVREye, uint32_t, float, float) override { return false; }
};

class provider : public IServerTrackedDeviceProvider
{
public:
	hmd headset;

	EVRInitError Init(IVRDriverContext *context) override
	{
		VR_INIT_SERVER_DRIVER_CONTEXT(context);
		/* (no standby: nobody wears this headset to wake it) */
		VRSettings()->SetFloat(k_pch_Power_Section, k_pch_Power_TurnOffScreensTimeout_Float, 86400.0f);
		VRSettings()->SetBool(k_pch_Power_Section, k_pch_Power_PauseCompositorOnStandby_Bool, false);
		VRSettings()->GetString("driver_halotest", "captureDirectory", headset.capture_directory,
			sizeof(headset.capture_directory));
		headset.capture_every = VRSettings()->GetInt32("driver_halotest", "captureEvery");
		/* (or from the environment SteamVR's server starts in: the first
		OpenVR or OpenXR application's) */
		if (getenv("HALOTEST_CAPTURE_DIRECTORY"))
			snprintf(headset.capture_directory, sizeof(headset.capture_directory), "%s", getenv("HALOTEST_CAPTURE_DIRECTORY"));
		if (getenv("HALOTEST_CAPTURE_EVERY"))
			headset.capture_every = atoi(getenv("HALOTEST_CAPTURE_EVERY"));
		DriverLog("halotest: capture every %d frames to \"%s\"\n", headset.capture_every, headset.capture_directory);
		VRServerDriverHost()->TrackedDeviceAdded("halotest-hmd", TrackedDeviceClass_HMD, &headset);
		return VRInitError_None;
	}
	void Cleanup() override { VR_CLEANUP_SERVER_DRIVER_CONTEXT(); }
	const char *const *GetInterfaceVersions() override { return k_InterfaceVersions; }
	void RunFrame() override
	{
		if (headset.index != k_unTrackedDeviceIndexInvalid)
			VRServerDriverHost()->TrackedDevicePoseUpdated(headset.index, headset.GetPose(), sizeof(DriverPose_t));
	}
	bool ShouldBlockStandbyMode() override { return true; }
	void EnterStandby() override {}
	void LeaveStandby() override {}
};

provider server;
}

extern "C" __attribute__((visibility("default"))) void *HmdDriverFactory(const char *name, int *code)
{
	if (!strcmp(name, IServerTrackedDeviceProvider_Version))
		return &server;
	if (code)
		*code = VRInitError_Init_InterfaceNotFound;
	return nullptr;
}
