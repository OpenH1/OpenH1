/* The production OpenXR frame owner against a runtime that checks ordering.
No graphics device, guest simulation, or headset is needed. */
#define HALO_VULKAN 1
#include "../host/host_vr.c"
#include <assert.h>

static unsigned runtime_waits, runtime_begins, runtime_ends, runtime_images;
static int runtime_waited, runtime_begun, runtime_render = 1;
static int guest_stop;
static int runtime_hold, runtime_entered;
static unsigned guest_samples, runtime_sessions_ended, runtime_sessions_destroyed;
static XrSessionState runtime_event;
static XrTime runtime_time, located_time;
static XrResult runtime_wait_result = XR_SUCCESS;

void host_logf(int priority, const char *format, ...)
{
	(void)priority;
	(void)format;
}

void host_exit(int code)
{
	(void)code;
	abort();
}

static XrResult XRAPI_CALL poll_event(XrInstance instance, XrEventDataBuffer *event)
{
	(void)instance;
	if (runtime_event)
	{
		XrEventDataSessionStateChanged *changed = (void *)event;

		changed->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
		changed->session = vr.session;
		changed->state = runtime_event;
		runtime_event = 0;
		return XR_SUCCESS;
	}
	return XR_EVENT_UNAVAILABLE;
}

static XrResult XRAPI_CALL begin_session(XrSession session, const XrSessionBeginInfo *info)
{
	(void)session;
	assert(info->primaryViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
	assert(!runtime_waited && !runtime_begun);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL end_session(XrSession session)
{
	(void)session;
	assert(!loading.waiter_started && !runtime_waited && !runtime_begun);
	runtime_sessions_ended++;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL destroy_session(XrSession session)
{
	(void)session;
	assert(!loading.active && !loading.waiter_started && !runtime_waited && !runtime_begun);
	runtime_sessions_destroyed++;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL wait_frame(XrSession session, const XrFrameWaitInfo *info, XrFrameState *state)
{
	(void)session;
	assert(info->type == XR_TYPE_FRAME_WAIT_INFO);
	assert(!runtime_waited && !runtime_begun);
	if (__atomic_load_n(&runtime_hold, __ATOMIC_ACQUIRE))
	{
		__atomic_store_n(&runtime_entered, 1, __ATOMIC_RELEASE);
		while (__atomic_load_n(&runtime_hold, __ATOMIC_ACQUIRE))
		{
			struct timespec delay = {0, 1000000};
			nanosleep(&delay, NULL);
		}
	}
	if (runtime_wait_result != XR_SUCCESS)
		return runtime_wait_result;
	runtime_waited = 1;
	runtime_waits++;
	runtime_time += 10000000;
	state->predictedDisplayTime = runtime_time;
	state->predictedDisplayPeriod = 10000000;
	state->shouldRender = runtime_render;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL begin_frame(XrSession session, const XrFrameBeginInfo *info)
{
	(void)session;
	assert(info->type == XR_TYPE_FRAME_BEGIN_INFO);
	assert(runtime_waited && !runtime_begun);
	runtime_waited = 0;
	runtime_begun = 1;
	runtime_begins++;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL end_frame(XrSession session, const XrFrameEndInfo *info)
{
	(void)session;
	assert(info->type == XR_TYPE_FRAME_END_INFO);
	assert(runtime_begun && info->displayTime == runtime_time);
	if (info->layerCount)
	{
		const XrCompositionLayerQuad *quad = (const void *)info->layers[0];

		assert(info->layerCount == 1 && quad->type == XR_TYPE_COMPOSITION_LAYER_QUAD);
		assert(quad->space == vr.play_space && quad->space != vr.view_space);
		assert(quad->subImage.swapchain == vr.swapchains[VR_SWAPCHAIN_HUD]);
		assert(quad->size.width == 1.6f && quad->size.height == 0.8f);
	}
	runtime_begun = 0;
	runtime_ends++;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL locate_views(XrSession session, const XrViewLocateInfo *info,
	XrViewState *state, uint32_t capacity, uint32_t *count, XrView *views)
{
	unsigned index;

	(void)session;
	assert(capacity == 2 && info->displayTime == runtime_time);
	located_time = info->displayTime;
	*count = 2;
	state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT;
	for (index = 0; index < 2; index++)
	{
		views[index].pose.orientation.w = 1.0f;
		views[index].pose.position.x = (float)runtime_waits;
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL locate_space(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation *location)
{
	(void)space;
	assert(base == vr.play_space && time == runtime_time);
	location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
	location->pose.orientation.w = 1.0f;
	location->pose.position.x = (float)runtime_waits;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL acquire_image(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo *info,
	uint32_t *index)
{
	(void)info;
	assert(runtime_begun && runtime_render && swapchain == vr.swapchains[VR_SWAPCHAIN_HUD]);
	runtime_images++;
	*index = 0;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL wait_image(XrSwapchain swapchain, const XrSwapchainImageWaitInfo *info)
{
	(void)swapchain;
	(void)info;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL release_image(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *info)
{
	(void)info;
	assert(runtime_begun && swapchain == vr.swapchains[VR_SWAPCHAIN_HUD]);
	return XR_SUCCESS;
}

static void *guest_snapshot(void *unused)
{
	struct vr_host_views views;

	(void)unused;
	while (!__atomic_load_n(&guest_stop, __ATOMIC_ACQUIRE))
	{
		assert(host_vr_frame_wait(&views));
		assert(views.display_elapsed == 0.0f);
		assert(host_vr_locate(&views));
		assert(views.head.valid);
		__atomic_add_fetch(&guest_samples, 1, __ATOMIC_RELEASE);
	}
	return NULL;
}

static void wait_for_loading_ticket(void)
{
	pthread_mutex_lock(&render_thread.lock);
	while (!render_thread.count && !loading.waiter_failed)
		pthread_cond_wait(&loading.changed, &render_thread.lock);
	pthread_mutex_unlock(&render_thread.lock);
}

int main(void)
{
	struct vr_host_views views;
	struct loading_presentation_frame frame = {0};
	pthread_t guest;
	unsigned index, before;
	XrTime previous;

	xrPollEvent = poll_event;
	xrBeginSession = begin_session;
	xrEndSession = end_session;
	xrDestroySession = destroy_session;
	xrWaitFrame = wait_frame;
	xrBeginFrame = begin_frame;
	xrEndFrame = end_frame;
	xrLocateViews = locate_views;
	xrLocateSpace = locate_space;
	xrAcquireSwapchainImage = acquire_image;
	xrWaitSwapchainImage = wait_image;
	xrReleaseSwapchainImage = release_image;
	vr.session = (XrSession)(uintptr_t)1;
	vr.running = 1;
	vr.play_space = (XrSpace)(uintptr_t)2;
	vr.view_space = (XrSpace)(uintptr_t)3;
	vr.swapchains[VR_SWAPCHAIN_HUD] = (XrSwapchain)(uintptr_t)4;
	for (index = 0; index < VR_SWAPCHAIN_COUNT; index++)
		vr.acquired[index] = -1;
	render_thread.on = 1;

	/* A waited game frame must end before loading takes ownership. */
	assert(host_vr_frame_wait(&views));
	host_vr_loading_set(1);
	assert(runtime_ends == 1);
	assert(!host_vr_frame_begin());
	wait_for_loading_ticket();
	assert(host_vr_loading_begin(&frame, &views) == 0);
	host_vr_loading_end(&frame);
	assert(views.head.position[0] == 2.0f);
	assert(pthread_create(&guest, NULL, guest_snapshot, NULL) == 0);
	for (index = 0; index < 20; index++)
	{
		previous = located_time;
		frame.serial++;
		wait_for_loading_ticket();
		assert(host_vr_loading_begin(&frame, &views) == 0);
		assert(located_time > previous);
		host_vr_loading_end(&frame);
	}
	while (!__atomic_load_n(&guest_samples, __ATOMIC_ACQUIRE))
	{
		struct timespec delay = {0, 1000000};
		nanosleep(&delay, NULL);
	}
	__atomic_store_n(&guest_stop, 1, __ATOMIC_RELEASE);
	pthread_join(guest, NULL);
	/* Readiness obtains a new prediction before the first game image. */
	previous = located_time;
	host_vr_loading_set(2);
	assert(host_vr_locate_late(&views));
	assert(located_time > previous && loading.suspended);
	assert(host_vr_loading_begin(&frame, &views) == -1);
	host_vr_frame_end(NULL);
	host_vr_loading_set(0);
	assert(host_vr_frame_wait(&views) && views.display_elapsed == 0.0f);
	assert(host_vr_frame_begin());
	host_vr_frame_end(NULL);

	/* Repeated maps, non-rendering frames, and failed waits own no image. */
	runtime_render = 0;
	host_vr_loading_set(1);
	before = runtime_images;
	wait_for_loading_ticket();
	assert(host_vr_loading_begin(&frame, &views) == -1);
	assert(runtime_images == before);
	host_vr_loading_set(0);
	assert(!runtime_begun && !runtime_waited);
	runtime_render = 1;
	runtime_wait_result = XR_ERROR_SESSION_LOST;
	host_vr_loading_set(1);
	wait_for_loading_ticket();
	assert(host_vr_loading_begin(&frame, &views) == -1);
	assert(!loading.presenting && !runtime_begun);
	host_vr_loading_set(0);
	runtime_wait_result = XR_SUCCESS;
	host_vr_loading_set(1);
	wait_for_loading_ticket();
	assert(host_vr_loading_begin(&frame, &views) == 0);
	host_vr_loading_end(&frame);
	host_vr_loading_set(0);
	assert(!loading.waiter_started && !runtime_waited && !runtime_begun);
	/* A runtime still waiting must not block the render owner's poll.
	The eventual ticket transfers to gameplay without another wait. */
	__atomic_store_n(&runtime_hold, 1, __ATOMIC_RELEASE);
	host_vr_loading_set(1);
	while (!__atomic_load_n(&runtime_entered, __ATOMIC_ACQUIRE))
	{
		struct timespec delay = {0, 1000000};
		nanosleep(&delay, NULL);
	}
	assert(host_vr_loading_begin(&frame, &views) == -1);
	__atomic_store_n(&runtime_hold, 0, __ATOMIC_RELEASE);
	wait_for_loading_ticket();
	before = runtime_waits;
	host_vr_loading_set(2);
	assert(host_vr_locate_late(&views));
	assert(runtime_waits == before);
	host_vr_frame_end(NULL);
	host_vr_loading_set(0);
	assert(!loading.waiter_started && !runtime_waited && !runtime_begun);
	/* Session stopping drains the producer's ticket before ending the
	session; ready resumes the same loading epoch with a new producer. */
	host_vr_loading_set(1);
	wait_for_loading_ticket();
	runtime_event = XR_SESSION_STATE_STOPPING;
	assert(host_vr_loading_begin(&frame, &views) == -1);
	assert(runtime_sessions_ended == 1 && !vr.running && loading.active);
	runtime_event = XR_SESSION_STATE_READY;
	events_poll();
	assert(vr.running);
	loading_wait_request();
	wait_for_loading_ticket();
	assert(host_vr_loading_begin(&frame, &views) == 0);
	host_vr_loading_end(&frame);
	/* Shutdown must join a producer with a pending ticket before the
	runtime's session and its swapchains can be destroyed. */
	wait_for_loading_ticket();
	host_vr_vulkan_shutdown();
	assert(runtime_sessions_destroyed == 1 && !vr.session);
	assert(runtime_waits == runtime_begins && runtime_begins == runtime_ends);
	puts("ok");
	return 0;
}
