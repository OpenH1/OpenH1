/* The production session capability against a runtime that rejects haptics
once its session ends or is destroyed. No GPU or headset is needed. */
#define HALO_VULKAN 1
#include "../host/host_vr.c"
#include <assert.h>
#include <errno.h>

static pthread_mutex_t runtime_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t runtime_changed = PTHREAD_COND_INITIALIZER;
static int alive, entered, release_call, active_call, ending;
static unsigned calls, ended, destroyed;
static XrResult begin_result = XR_SUCCESS;

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

static XrResult XRAPI_CALL begin_session(XrSession session, const XrSessionBeginInfo *info)
{
	assert(session == (XrSession)(uintptr_t)1);
	assert(info->primaryViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
	alive = begin_result == XR_SUCCESS;
	return begin_result;
}

static XrResult XRAPI_CALL end_session(XrSession session)
{
	assert(session == (XrSession)(uintptr_t)1 && !active_call);
	alive = 0;
	ended++;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL destroy_session(XrSession session)
{
	assert(session == (XrSession)(uintptr_t)1 && !active_call);
	alive = 0;
	destroyed++;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL apply_haptic(XrSession session, const XrHapticActionInfo *info,
	const XrHapticBaseHeader *base)
{
	const XrHapticVibration *vibration = (const void *)base;

	assert(alive && session == (XrSession)(uintptr_t)1);
	assert(info->action == (XrAction)(uintptr_t)2 && info->subactionPath == 4);
	assert(vibration->amplitude == 0.5f && vibration->duration > 0);
	pthread_mutex_lock(&runtime_lock);
	active_call = entered = 1;
	calls++;
	pthread_cond_broadcast(&runtime_changed);
	while (!release_call)
		pthread_cond_wait(&runtime_changed, &runtime_lock);
	active_call = 0;
	pthread_mutex_unlock(&runtime_lock);
	return XR_SUCCESS;
}

static void *send_haptic(void *unused)
{
	(void)unused;
	host_vr_haptic(1, 0.5f, 0.1f);
	return NULL;
}

static void *end_runtime(void *destroy)
{
	pthread_mutex_lock(&runtime_lock);
	ending = 1;
	pthread_cond_broadcast(&runtime_changed);
	pthread_mutex_unlock(&runtime_lock);
	if (destroy)
		host_vr_vulkan_shutdown();
	else
		session_end();
	return NULL;
}

int main(void)
{
	pthread_t sender, stopper;
	int destroy;
	unsigned before;

	xrBeginSession = begin_session;
	xrEndSession = end_session;
	xrDestroySession = destroy_session;
	xrApplyHapticFeedback = apply_haptic;
	vr.session = (XrSession)(uintptr_t)1;
	vr.actions[_action_haptic] = (XrAction)(uintptr_t)2;
	vr.hands[0] = 3;
	vr.hands[1] = 4;
	host_vr_haptic(1, 0.5f, 0.1f);
	assert(!calls);
	begin_result = XR_ERROR_RUNTIME_FAILURE;
	session_begin();
	host_vr_haptic(1, 0.5f, 0.1f);
	assert(!calls && !vr.running);
	begin_result = XR_SUCCESS;
	for (destroy = 0; destroy < 2; destroy++)
	{
		vr.session = (XrSession)(uintptr_t)1;
		session_begin();
		assert(vr.running);
		entered = release_call = ending = 0;
		host_vr_haptic(-1, 0.5f, 0.1f);
		host_vr_haptic(2, 0.5f, 0.1f);
		assert(pthread_create(&sender, NULL, send_haptic, NULL) == 0);
		pthread_mutex_lock(&runtime_lock);
		while (!entered)
			pthread_cond_wait(&runtime_changed, &runtime_lock);
		pthread_mutex_unlock(&runtime_lock);
		assert(pthread_mutex_trylock(&haptics.lock) == EBUSY);
		assert(pthread_create(&stopper, NULL, end_runtime, (void *)(uintptr_t)destroy) == 0);
		pthread_mutex_lock(&runtime_lock);
		while (!ending)
			pthread_cond_wait(&runtime_changed, &runtime_lock);
		assert(alive && active_call);
		release_call = 1;
		pthread_cond_broadcast(&runtime_changed);
		pthread_mutex_unlock(&runtime_lock);
		pthread_join(sender, NULL);
		pthread_join(stopper, NULL);
		assert(!alive && !vr.running);
		before = calls;
		host_vr_haptic(1, 0.5f, 0.1f);
		assert(calls == before);
	}
	assert(calls == 2 && ended == 1 && destroyed == 1);
	puts("ok");
	return 0;
}
