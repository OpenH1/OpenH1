/*
PACING_TEST.C

display.low_latency's pacing (xgpu_vulkan_pacing.c) over a fake device
whose present wait says nothing until a present is shown, or nothing ever
(the Steam Deck's gamescope, where build 940 showed no frame at all): the
first frames are presented unpaced, every present returns at once, a
display that never answers is given up on, and one that answers paces the
frames from the present after its first answer.
*/

#include "../xgpu_vulkan_pacing.c"
#include <assert.h>
#include <stdarg.h>

static int stream_paced, paced_calls;
/* the fake display: present wait answers (1), or never does (0) */
static int answers;
static uint64_t presents;

void host_logf(int priority, const char *format, ...)
{
	va_list arguments;

	(void)priority;
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
}

void host_render_stream_display_paced(int paced)
{
	stream_paced = paced;
	paced_calls++;
}

void host_render_stream_frames_taken_reset(void)
{
}

void host_render_stream_frame_displayed(uint64_t input, uint64_t finished, uint64_t latched, uint64_t shown,
	uint64_t period)
{
	(void)input;
	(void)finished;
	(void)latched;
	(void)shown;
	(void)period;
}

void host_render_stream_frame_finished(uint64_t input, uint64_t finished)
{
	(void)input;
	(void)finished;
}

void host_render_stream_pacing_statistics(struct host_render_pacing_statistics *statistics)
{
	memset(statistics, 0, sizeof(*statistics));
}

uint64_t host_render_stream_frame_input(void)
{
	return now_ns();
}

static VKAPI_ATTR VkResult VKAPI_CALL fake_wait_for_present(VkDevice device, VkSwapchainKHR swapchain,
	uint64_t id, uint64_t timeout)
{
	(void)device;
	(void)swapchain;
	if (answers && id <= __atomic_load_n(&presents, __ATOMIC_ACQUIRE))
		return VK_SUCCESS;
	sleep_ns(timeout < 1000000ull ? timeout : 1000000ull);
	return VK_TIMEOUT;
}

static VKAPI_ATTR VkResult VKAPI_CALL fake_queue_present(VkQueue queue, const VkPresentInfoKHR *info)
{
	(void)queue;
	(void)info;
	__atomic_add_fetch(&presents, 1, __ATOMIC_RELEASE);
	return VK_SUCCESS;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_proc_addr(VkDevice device, const char *name)
{
	(void)device;
	if (!strcmp(name, "vkWaitForPresentKHR"))
		return (PFN_vkVoidFunction)fake_wait_for_present;
	return NULL;
}

/* a present as the backend makes one; returns how long it took (ns) */
static uint64_t present(void)
{
	VkPresentInfoKHR info;
	VkSwapchainKHR swapchain = (VkSwapchainKHR)(uintptr_t)1;
	uint32_t index = 0;
	uint64_t start = now_ns();

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	info.swapchainCount = 1;
	info.pSwapchains = &swapchain;
	info.pImageIndices = &index;
	assert(xgpu_vulkan_pacing_present(fake_queue_present, VK_NULL_HANDLE, &info, 1, 0) == VK_SUCCESS);
	return now_ns() - start;
}

static void start(int answering)
{
	struct xgpu_vulkan_pacing_device device;

	memset(&device, 0, sizeof(device));
	device.device = (VkDevice)(uintptr_t)1;
	device.get_device_proc_addr = fake_proc_addr;
	device.present_wait = 1;
	answers = answering;
	/* (present numbers go on from the last start) */
	presents = pacing.next_id;
	stream_paced = paced_calls = 0;
	assert(xgpu_vulkan_pacing_start(&device));
	/* (display.low_latency on, FIFO, 60 Hz) */
	assert(!xgpu_vulkan_pacing_mode(1, 1, 1, 16666667ull));
	xgpu_vulkan_pacing_swapchain((VkSwapchainKHR)(uintptr_t)1);
	assert(!stream_paced);
}

int main(void)
{
	int frame;

	/* a display that never says when it took a present: every present
	returns at once, the game is never paced, and the presents are given up
	on once SILENT_PRESENTS in a row went unanswered */
	start(0);
	for (frame = 0; frame < 40; frame++)
	{
		assert(present() < 5000000ull);
		assert(!stream_paced);
		sleep_ns(33000000ull);
	}
	pthread_mutex_lock(&pacing.lock);
	assert(pacing.silent && !pacing.proven);
	pthread_mutex_unlock(&pacing.lock);
	assert(!xgpu_vulkan_pacing_mode(1, 1, 1, 16666667ull));
	xgpu_vulkan_pacing_stop();

	/* one that answers once a present is shown: the first present unpaced,
	then paced from a present after the first answer */
	start(1);
	assert(present() < 5000000ull && !stream_paced);
	for (frame = 0; frame < 20 && !stream_paced; frame++)
	{
		sleep_ns(17000000ull);
		assert(present() < 5000000ull);
	}
	assert(stream_paced && paced_calls == 1);
	pthread_mutex_lock(&pacing.lock);
	assert(pacing.proven && !pacing.silent);
	pthread_mutex_unlock(&pacing.lock);

	/* the swapchain stalls (xgpu_vulkan.c's acquire): pacing let go of */
	xgpu_vulkan_pacing_silence();
	assert(!stream_paced);
	assert(!xgpu_vulkan_pacing_mode(1, 1, 1, 16666667ull));
	xgpu_vulkan_pacing_stop();
	puts("pacing: ok");
	return 0;
}
