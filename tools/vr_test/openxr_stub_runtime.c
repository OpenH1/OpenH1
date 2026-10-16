/*
OPENXR_STUB_RUNTIME.C

A stand-in OpenXR runtime for the VR build, for a computer without a headset
or a runtime that can open a session there (SteamVR on Linux needs a
headset's display in direct mode: port/linux/README.md, "SteamVR on a desktop
PC"). It is a headset that is always worn and still, at a standing height,
with no controllers (or two held still, XR_STUB_HANDS): the game draws its eyes, HUD and panels into the
runtime's swapchains as it would for a real one, and the frames are paced at
the headset's refresh rate, then dropped. What it shows is the game's side:
the binding of its GL context, its swapchains, its drawing (the screenshots,
debug.screenshot_every, are of the eyes' and the HUD's images) and its time
for each frame (debug.gpu_stats). Not a compositor's: it composes nothing.
The test hooks (debug.vr_test_head, debug.vr_test_hands) pose the head and
hands.

It offers XR_KHR_opengl_enable with XR_MNDX_egl_enable's binding (the game
takes EGL's context, as with SteamVR and Monado), XR_KHR_composition_layer_
depth, and, as SteamVR does not, no XR_KHR_composition_layer_cylinder (the
HUD is a quad). Its swapchains' images are textures made in the game's GL
context. Settings, from the environment:

	XR_STUB_EYE_SIZE=<width>x<height>   each eye's recommended size (1512x1680,
	                                    SteamVR's null driver's)
	XR_STUB_FPS=<Hz>                    the refresh rate (90)
	XR_STUB_CYLINDER=1                  offer XR_KHR_composition_layer_cylinder
	XR_STUB_HEAD_TURN="<yaw> <pitch> <right> [<frame>]"
	                                    from that frame on (100), the head
	                                    turned left and up (degrees) and moved
	                                    right (metres)
	XR_STUB_HANDS=1                     two controllers, held still before the
	                                    chest, pointing ahead
	XR_STUB_HAND_TURN="<yaw> <pitch> <right> [<frame>]"
	                                    the right one turned and moved so
	XR_STUB_TURN_LATE=1                 the turns located only by a thread
	                                    other than the one that waits for the
	                                    frames (the renderer's, as it draws:
	                                    the late latch), the game's seeing the
	                                    head and hand still

A 64-bit host library, built with the headers the VR build downloads into
build/linux_x86_64/third_party/openxr (configure.py --vr; or fetched there
alone, as openxr_probe.c's comment says), from the root of the repository:

	cc -O2 -shared -fPIC -I build/linux_x86_64/third_party/openxr \
		tools/vr_test/openxr_stub_runtime.c -o build/openxr_stub_runtime.so
	printf '{"file_format_version":"1.0.0","runtime":{"name":"stub",%s}}\n' \
		'"library_path":"./openxr_stub_runtime.so"' > build/openxr_stub_runtime.json
	XR_RUNTIME_JSON=$PWD/build/openxr_stub_runtime.json build/linux_x86_64/halo
*/

#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_EGL
#define XR_USE_GRAPHICS_API_OPENGL

#include <EGL/egl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#define EXPORT __attribute__((visibility("default")))

/* ---------- GL, through the binding's getProcAddress */

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLsizei;
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_RGBA8 0x8058
#define GL_DEPTH_COMPONENT16 0x81A5
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_TEXTURE_BINDING_2D_ARRAY 0x8C1D

static struct
{
	void (*GenTextures)(GLsizei, GLuint *);
	void (*DeleteTextures)(GLsizei, const GLuint *);
	void (*BindTexture)(GLenum, GLuint);
	void (*GetIntegerv)(GLenum, int *);
	void (*TexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
	void (*TexStorage3D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei);
} gl;

/* ---------- the state: one instance, system and session */

#define IMAGES 3
#define MAX_SWAPCHAINS 32
#define MAX_PATHS 512
#define MAX_EVENTS 8
#define MAX_ACTIONS 256

struct swapchain
{
	int used;
	GLuint images[IMAGES];
	uint32_t next;
};

static struct
{
	int instance, session;
	int eye_width, eye_height, cylinder;
	double fps;
	XrSessionState state;
	XrSessionState events[MAX_EVENTS];
	int event_count;
	XrTime next_display;
	struct swapchain swapchains[MAX_SWAPCHAINS];
	char *paths[MAX_PATHS];
	int path_count;
	uint64_t handles;
	/* XR_STUB_HEAD_TURN, XR_STUB_HAND_TURN: the head's and the right
	hand's turn (a quaternion) and move, from which frame, located by which
	threads; the frames waited for, by which */
	struct turn
	{
		int set;
		float q[4], move;
		unsigned long frame;
	} head_turn, hand_turn;
	int turn_late;
	unsigned long frames;
	pthread_t waiter;
	/* XR_STUB_HANDS; the actions' names and the action spaces' actions and
	hands, by their handles' numbers */
	int hands;
	char action_names[MAX_ACTIONS][XR_MAX_ACTION_NAME_SIZE];
	struct
	{
		uint64_t action;
		XrPath hand;
	} action_spaces[MAX_ACTIONS];
} stub;

#define INSTANCE ((XrInstance)(uintptr_t)1)
#define SESSION ((XrSession)(uintptr_t)2)
#define SYSTEM ((XrSystemId)1)

/* the spaces: their kind in their handle's low bits */
enum
{
	_space_view = 1, _space_local, _space_stage, _space_action
};
#define SPACE(kind) ((XrSpace)(uintptr_t)(((++stub.handles) << 4) | (kind)))
#define SPACE_KIND(space) ((int)((uintptr_t)(space) & 15))

static XrTime now(void)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (XrTime)time.tv_sec * 1000000000 + time.tv_nsec;
}

static void event_queue(XrSessionState state)
{
	if (stub.event_count < MAX_EVENTS)
		stub.events[stub.event_count++] = state;
}

static const char *const extension_names[] =
{
	XR_KHR_OPENGL_ENABLE_EXTENSION_NAME, XR_MNDX_EGL_ENABLE_EXTENSION_NAME,
	XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME,
};

/* ---------- the functions */

static XrResult XRAPI_CALL enumerate_extensions(const char *layer, uint32_t capacity, uint32_t *count,
	XrExtensionProperties *properties)
{
	uint32_t total = stub.cylinder ? 4 : 3, index;

	if (layer)
		return XR_ERROR_API_LAYER_NOT_PRESENT;
	*count = total;
	if (!capacity)
		return XR_SUCCESS;
	if (capacity < total)
		return XR_ERROR_SIZE_INSUFFICIENT;
	for (index = 0; index < total; index++)
	{
		snprintf(properties[index].extensionName, sizeof(properties[index].extensionName), "%s",
			extension_names[index]);
		properties[index].extensionVersion = 1;
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL create_instance(const XrInstanceCreateInfo *info, XrInstance *instance)
{
	(void)info;
	stub.instance = 1;
	*instance = INSTANCE;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL destroy_instance(XrInstance instance)
{
	(void)instance;
	stub.instance = 0;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL get_instance_properties(XrInstance instance, XrInstanceProperties *properties)
{
	(void)instance;
	snprintf(properties->runtimeName, sizeof(properties->runtimeName), "OpenXR stub runtime");
	properties->runtimeVersion = XR_MAKE_VERSION(1, 0, 0);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL result_to_string(XrInstance instance, XrResult result, char *text)
{
	(void)instance;
	snprintf(text, XR_MAX_RESULT_STRING_SIZE, "XrResult %d", (int)result);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL get_system(XrInstance instance, const XrSystemGetInfo *info, XrSystemId *system)
{
	(void)instance;
	if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
		return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
	*system = SYSTEM;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL get_system_properties(XrInstance instance, XrSystemId system,
	XrSystemProperties *properties)
{
	(void)instance;
	(void)system;
	properties->systemId = SYSTEM;
	properties->vendorId = 0;
	snprintf(properties->systemName, sizeof(properties->systemName), "stub headset");
	properties->graphicsProperties.maxSwapchainImageWidth = 8192;
	properties->graphicsProperties.maxSwapchainImageHeight = 8192;
	properties->graphicsProperties.maxLayerCount = 16;
	properties->trackingProperties.orientationTracking = XR_TRUE;
	properties->trackingProperties.positionTracking = XR_TRUE;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL enumerate_view_configuration_views(XrInstance instance, XrSystemId system,
	XrViewConfigurationType type, uint32_t capacity, uint32_t *count, XrViewConfigurationView *views)
{
	uint32_t index;

	(void)instance;
	(void)system;
	if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
		return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
	*count = 2;
	if (!capacity)
		return XR_SUCCESS;
	if (capacity < 2)
		return XR_ERROR_SIZE_INSUFFICIENT;
	for (index = 0; index < 2; index++)
	{
		views[index].recommendedImageRectWidth = (uint32_t)stub.eye_width;
		views[index].recommendedImageRectHeight = (uint32_t)stub.eye_height;
		views[index].maxImageRectWidth = 8192;
		views[index].maxImageRectHeight = 8192;
		views[index].recommendedSwapchainSampleCount = 1;
		views[index].maxSwapchainSampleCount = 1;
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL get_opengl_requirements(XrInstance instance, XrSystemId system,
	XrGraphicsRequirementsOpenGLKHR *requirements)
{
	(void)instance;
	(void)system;
	requirements->minApiVersionSupported = XR_MAKE_VERSION(4, 3, 0);
	requirements->maxApiVersionSupported = XR_MAKE_VERSION(4, 6, 0);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL create_session(XrInstance instance, const XrSessionCreateInfo *info, XrSession *session)
{
	const XrBaseInStructure *next;
	const XrGraphicsBindingEGLMNDX *egl = NULL;

	(void)instance;
	for (next = info->next; next; next = next->next)
	{
		if (next->type == XR_TYPE_GRAPHICS_BINDING_EGL_MNDX)
			egl = (const XrGraphicsBindingEGLMNDX *)next;
	}
	if (!egl || !egl->getProcAddress || !egl->context)
	{
		fprintf(stderr, "openxr stub: xrCreateSession without an EGL binding\n");
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	}
#define GL_LOAD(name) gl.name = (void *)egl->getProcAddress("gl" #name)
	GL_LOAD(GenTextures);
	GL_LOAD(DeleteTextures);
	GL_LOAD(BindTexture);
	GL_LOAD(GetIntegerv);
	GL_LOAD(TexStorage2D);
	GL_LOAD(TexStorage3D);
#undef GL_LOAD
	if (!gl.GenTextures || !gl.BindTexture || !gl.TexStorage2D || !gl.TexStorage3D || !gl.GetIntegerv)
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	stub.session = 1;
	stub.state = XR_SESSION_STATE_IDLE;
	event_queue(XR_SESSION_STATE_IDLE);
	event_queue(XR_SESSION_STATE_READY);
	*session = SESSION;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL destroy_session(XrSession session)
{
	(void)session;
	stub.session = 0;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL begin_session(XrSession session, const XrSessionBeginInfo *info)
{
	(void)session;
	(void)info;
	stub.next_display = 0;
	event_queue(XR_SESSION_STATE_SYNCHRONIZED);
	event_queue(XR_SESSION_STATE_VISIBLE);
	event_queue(XR_SESSION_STATE_FOCUSED);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL end_session(XrSession session)
{
	(void)session;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL poll_event(XrInstance instance, XrEventDataBuffer *buffer)
{
	XrEventDataSessionStateChanged *changed = (XrEventDataSessionStateChanged *)buffer;
	int index;

	(void)instance;
	if (!stub.event_count)
		return XR_EVENT_UNAVAILABLE;
	stub.state = stub.events[0];
	for (index = 1; index < stub.event_count; index++)
		stub.events[index - 1] = stub.events[index];
	stub.event_count--;
	memset(changed, 0, sizeof(*changed));
	changed->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
	changed->session = SESSION;
	changed->state = stub.state;
	changed->time = now();
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL create_reference_space(XrSession session, const XrReferenceSpaceCreateInfo *info,
	XrSpace *space)
{
	(void)session;
	switch (info->referenceSpaceType)
	{
	case XR_REFERENCE_SPACE_TYPE_VIEW:
		*space = SPACE(_space_view);
		return XR_SUCCESS;
	case XR_REFERENCE_SPACE_TYPE_LOCAL:
		*space = SPACE(_space_local);
		return XR_SUCCESS;
	case XR_REFERENCE_SPACE_TYPE_STAGE:
		*space = SPACE(_space_stage);
		return XR_SUCCESS;
	default:
		return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
	}
}

static XrResult XRAPI_CALL create_action_space(XrSession session, const XrActionSpaceCreateInfo *info,
	XrSpace *space)
{
	uint64_t number;

	(void)session;
	*space = SPACE(_space_action);
	number = (uint64_t)(uintptr_t)*space >> 4;
	if (number < MAX_ACTIONS)
	{
		stub.action_spaces[number].action = (uint64_t)(uintptr_t)info->action;
		stub.action_spaces[number].hand = info->subactionPath;
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL destroy_space(XrSpace space)
{
	(void)space;
	return XR_SUCCESS;
}

/* a space's origin's height above the floor: the head's, standing, and the
local space's (the head's place as the session began), or the stage's, the
floor */
static float origin_height(XrSpace space)
{
	return SPACE_KIND(space) == _space_stage ? 0.0f : 1.7f;
}

/* a pose turned and moved as XR_STUB_HEAD_TURN (or _HAND_TURN) has it,
now and on this thread: turned about a point at height above the space's
origin (the head's middle, the hand's grip) */
static void turned(const struct turn *turn, XrPosef *pose, float height)
{
	const float *q = turn->q;
	float v[3] = { pose->position.x, pose->position.y - height, pose->position.z }, c[3], cc[3], r[4];
	const float p[4] = { pose->orientation.x, pose->orientation.y, pose->orientation.z, pose->orientation.w };
	int axis;

	if (!turn->set || stub.frames < turn->frame || (stub.turn_late && pthread_equal(pthread_self(), stub.waiter)))
		return;
	c[0] = q[1] * v[2] - q[2] * v[1];
	c[1] = q[2] * v[0] - q[0] * v[2];
	c[2] = q[0] * v[1] - q[1] * v[0];
	cc[0] = q[1] * c[2] - q[2] * c[1];
	cc[1] = q[2] * c[0] - q[0] * c[2];
	cc[2] = q[0] * c[1] - q[1] * c[0];
	for (axis = 0; axis < 3; axis++)
		v[axis] += 2.0f * (q[3] * c[axis] + cc[axis]);
	pose->position.x = v[0] + turn->move;
	pose->position.y = v[1] + height;
	pose->position.z = v[2];
	r[0] = q[3] * p[0] + q[0] * p[3] + q[1] * p[2] - q[2] * p[1];
	r[1] = q[3] * p[1] - q[0] * p[2] + q[1] * p[3] + q[2] * p[0];
	r[2] = q[3] * p[2] + q[0] * p[1] - q[1] * p[0] + q[2] * p[3];
	r[3] = q[3] * p[3] - q[0] * p[0] - q[1] * p[1] - q[2] * p[2];
	pose->orientation.x = r[0];
	pose->orientation.y = r[1];
	pose->orientation.z = r[2];
	pose->orientation.w = r[3];
}

static XrResult XRAPI_CALL locate_space(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation *location)
{
	(void)time;
	memset(&location->pose, 0, sizeof(location->pose));
	location->pose.orientation.w = 1.0f;
	location->locationFlags = 0;
	if (SPACE_KIND(space) == _space_action && SPACE_KIND(base) != _space_action && stub.hands)
	{
		/* (a hand's pose: grip and aim alike, 1.3 m above the floor, 0.2 m to
		its side and 0.35 m ahead) */
		uint64_t number = (uint64_t)(uintptr_t)space >> 4;
		XrPath hand = number < MAX_ACTIONS ? stub.action_spaces[number].hand : 0;
		const char *name = hand && hand <= (XrPath)stub.path_count ? stub.paths[hand - 1] : "";
		int right = !strcmp(name, "/user/hand/right");

		if (strcmp(name, "/user/hand/left") && !right)
			return XR_SUCCESS;
		location->pose.position.x = right ? 0.2f : -0.2f;
		location->pose.position.y = 1.3f - origin_height(base);
		location->pose.position.z = -0.35f;
		if (right)
			turned(&stub.hand_turn, &location->pose, location->pose.position.y);
		location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
			XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
		return XR_SUCCESS;
	}
	if (SPACE_KIND(space) == _space_action || SPACE_KIND(base) == _space_action)
		return XR_SUCCESS; /* (no controllers) */
	location->pose.position.y = origin_height(space) - origin_height(base);
	if (SPACE_KIND(space) == _space_view && SPACE_KIND(base) != _space_view)
		turned(&stub.head_turn, &location->pose, location->pose.position.y);
	location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
		XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL locate_views(XrSession session, const XrViewLocateInfo *info, XrViewState *state,
	uint32_t capacity, uint32_t *count, XrView *views)
{
	uint32_t eye;

	(void)session;
	*count = 2;
	if (!capacity)
		return XR_SUCCESS;
	if (capacity < 2)
		return XR_ERROR_SIZE_INSUFFICIENT;
	state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
		XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
	for (eye = 0; eye < 2; eye++)
	{
		memset(&views[eye].pose, 0, sizeof(views[eye].pose));
		views[eye].pose.orientation.w = 1.0f;
		views[eye].pose.position.x = eye ? 0.032f : -0.032f;
		views[eye].pose.position.y = 1.7f - origin_height(info->space);
		/* about 105 degrees across each eye, wider outward, as a headset's */
		views[eye].fov.angleLeft = eye ? -0.80f : -0.98f;
		views[eye].fov.angleRight = eye ? 0.98f : 0.80f;
		views[eye].fov.angleUp = 0.92f;
		views[eye].fov.angleDown = -0.92f;
		turned(&stub.head_turn, &views[eye].pose, 1.7f - origin_height(info->space));
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL enumerate_swapchain_formats(XrSession session, uint32_t capacity, uint32_t *count,
	int64_t *formats)
{
	static const int64_t offered[] = { GL_SRGB8_ALPHA8, GL_RGBA8, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT24,
		GL_DEPTH_COMPONENT16 };
	uint32_t index, total = (uint32_t)(sizeof(offered) / sizeof(offered[0]));

	(void)session;
	*count = total;
	if (!capacity)
		return XR_SUCCESS;
	if (capacity < total)
		return XR_ERROR_SIZE_INSUFFICIENT;
	for (index = 0; index < total; index++)
		formats[index] = offered[index];
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL create_swapchain(XrSession session, const XrSwapchainCreateInfo *info,
	XrSwapchain *handle)
{
	struct swapchain *swapchain = NULL;
	GLenum target = info->arraySize > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
	int which, image, bound = 0;

	(void)session;
	for (which = 0; which < MAX_SWAPCHAINS && !swapchain; which++)
	{
		if (!stub.swapchains[which].used)
			swapchain = &stub.swapchains[which];
	}
	if (!swapchain || !info->width || !info->height || info->faceCount != 1 || info->sampleCount != 1)
		return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	gl.GetIntegerv(target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D : GL_TEXTURE_BINDING_2D_ARRAY, &bound);
	gl.GenTextures(IMAGES, swapchain->images);
	for (image = 0; image < IMAGES; image++)
	{
		gl.BindTexture(target, swapchain->images[image]);
		if (target == GL_TEXTURE_2D_ARRAY)
		{
			gl.TexStorage3D(target, (GLsizei)info->mipCount, (GLenum)info->format, (GLsizei)info->width,
				(GLsizei)info->height, (GLsizei)info->arraySize);
		}
		else
		{
			gl.TexStorage2D(target, (GLsizei)info->mipCount, (GLenum)info->format, (GLsizei)info->width,
				(GLsizei)info->height);
		}
	}
	gl.BindTexture(target, (GLuint)bound);
	swapchain->used = 1;
	swapchain->next = 0;
	*handle = (XrSwapchain)(uintptr_t)(swapchain - stub.swapchains + 1);
	return XR_SUCCESS;
}

static struct swapchain *swapchain_of(XrSwapchain handle)
{
	uintptr_t index = (uintptr_t)handle;

	return index >= 1 && index <= MAX_SWAPCHAINS && stub.swapchains[index - 1].used ? &stub.swapchains[index - 1] :
		NULL;
}

static XrResult XRAPI_CALL destroy_swapchain(XrSwapchain handle)
{
	struct swapchain *swapchain = swapchain_of(handle);

	if (!swapchain)
		return XR_ERROR_HANDLE_INVALID;
	if (gl.DeleteTextures)
		gl.DeleteTextures(IMAGES, swapchain->images);
	memset(swapchain, 0, sizeof(*swapchain));
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL enumerate_swapchain_images(XrSwapchain handle, uint32_t capacity, uint32_t *count,
	XrSwapchainImageBaseHeader *images)
{
	struct swapchain *swapchain = swapchain_of(handle);
	XrSwapchainImageOpenGLKHR *gl_images = (XrSwapchainImageOpenGLKHR *)images;
	uint32_t index;

	if (!swapchain)
		return XR_ERROR_HANDLE_INVALID;
	*count = IMAGES;
	if (!capacity)
		return XR_SUCCESS;
	if (capacity < IMAGES)
		return XR_ERROR_SIZE_INSUFFICIENT;
	for (index = 0; index < IMAGES; index++)
		gl_images[index].image = swapchain->images[index];
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL acquire_swapchain_image(XrSwapchain handle, const XrSwapchainImageAcquireInfo *info,
	uint32_t *index)
{
	struct swapchain *swapchain = swapchain_of(handle);

	(void)info;
	if (!swapchain)
		return XR_ERROR_HANDLE_INVALID;
	*index = swapchain->next;
	swapchain->next = (swapchain->next + 1) % IMAGES;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL wait_swapchain_image(XrSwapchain handle, const XrSwapchainImageWaitInfo *info)
{
	(void)info;
	return swapchain_of(handle) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

static XrResult XRAPI_CALL release_swapchain_image(XrSwapchain handle, const XrSwapchainImageReleaseInfo *info)
{
	(void)info;
	return swapchain_of(handle) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

/* the frames: each waits for the display's next refresh, as a runtime's
compositor paces them */
static XrResult XRAPI_CALL wait_frame(XrSession session, const XrFrameWaitInfo *info, XrFrameState *state)
{
	XrTime period = (XrTime)(1e9 / stub.fps), time = now();

	(void)session;
	(void)info;
	stub.waiter = pthread_self();
	stub.frames++;
	if (!stub.next_display || stub.next_display < time - period)
		stub.next_display = time + period;
	else
		stub.next_display += period;
	if (stub.next_display - period > time)
	{
		struct timespec pause;
		XrTime wait = stub.next_display - period - time;

		pause.tv_sec = (time_t)(wait / 1000000000);
		pause.tv_nsec = (long)(wait % 1000000000);
		nanosleep(&pause, NULL);
	}
	state->predictedDisplayTime = stub.next_display;
	state->predictedDisplayPeriod = period;
	state->shouldRender = stub.state == XR_SESSION_STATE_VISIBLE || stub.state == XR_SESSION_STATE_FOCUSED;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL begin_frame(XrSession session, const XrFrameBeginInfo *info)
{
	(void)session;
	(void)info;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL end_frame(XrSession session, const XrFrameEndInfo *info)
{
	uint32_t layer;

	(void)session;
	for (layer = 0; layer < info->layerCount; layer++)
	{
		if (info->layers[layer]->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR && !stub.cylinder)
			return XR_ERROR_LAYER_INVALID;
	}
	return XR_SUCCESS;
}

/* the paths, kept as their strings */
static XrResult XRAPI_CALL string_to_path(XrInstance instance, const char *text, XrPath *path)
{
	int index;

	(void)instance;
	for (index = 0; index < stub.path_count; index++)
	{
		if (!strcmp(stub.paths[index], text))
		{
			*path = (XrPath)(index + 1);
			return XR_SUCCESS;
		}
	}
	if (stub.path_count == MAX_PATHS || !(stub.paths[stub.path_count] = strdup(text)))
		return XR_ERROR_PATH_COUNT_EXCEEDED;
	*path = (XrPath)++stub.path_count;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL path_to_string(XrInstance instance, XrPath path, uint32_t capacity, uint32_t *count,
	char *text)
{
	const char *string;

	(void)instance;
	if (!path || path > (XrPath)stub.path_count)
		return XR_ERROR_PATH_INVALID;
	string = stub.paths[path - 1];
	*count = (uint32_t)strlen(string) + 1;
	if (!capacity)
		return XR_SUCCESS;
	if (capacity < *count)
		return XR_ERROR_SIZE_INSUFFICIENT;
	memcpy(text, string, *count);
	return XR_SUCCESS;
}

/* the actions: made, bound and synced, never active (no controllers) */
static XrResult XRAPI_CALL create_action_set(XrInstance instance, const XrActionSetCreateInfo *info,
	XrActionSet *set)
{
	(void)instance;
	(void)info;
	*set = (XrActionSet)(uintptr_t)++stub.handles;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL create_action(XrActionSet set, const XrActionCreateInfo *info, XrAction *action)
{
	(void)set;
	*action = (XrAction)(uintptr_t)++stub.handles;
	if (stub.handles < MAX_ACTIONS)
		snprintf(stub.action_names[stub.handles], sizeof(stub.action_names[0]), "%s", info->actionName);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL suggest_bindings(XrInstance instance,
	const XrInteractionProfileSuggestedBinding *suggestion)
{
	(void)instance;
	(void)suggestion;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL attach_action_sets(XrSession session, const XrSessionActionSetsAttachInfo *info)
{
	(void)session;
	(void)info;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL sync_actions(XrSession session, const XrActionsSyncInfo *info)
{
	(void)session;
	(void)info;
	return stub.state == XR_SESSION_STATE_FOCUSED ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
}

static XrResult XRAPI_CALL get_boolean(XrSession session, const XrActionStateGetInfo *info,
	XrActionStateBoolean *state)
{
	(void)session;
	(void)info;
	state->currentState = XR_FALSE;
	state->changedSinceLastSync = XR_FALSE;
	state->lastChangeTime = 0;
	state->isActive = XR_FALSE;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL get_float(XrSession session, const XrActionStateGetInfo *info, XrActionStateFloat *state)
{
	(void)session;
	(void)info;
	state->currentState = 0.0f;
	state->changedSinceLastSync = XR_FALSE;
	state->lastChangeTime = 0;
	state->isActive = XR_FALSE;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL get_vector2f(XrSession session, const XrActionStateGetInfo *info,
	XrActionStateVector2f *state)
{
	(void)session;
	(void)info;
	state->currentState.x = state->currentState.y = 0.0f;
	state->changedSinceLastSync = XR_FALSE;
	state->lastChangeTime = 0;
	state->isActive = XR_FALSE;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL get_pose(XrSession session, const XrActionStateGetInfo *info, XrActionStatePose *state)
{
	uint64_t action = (uint64_t)(uintptr_t)info->action;

	(void)session;
	state->isActive = stub.hands && action < MAX_ACTIONS && strstr(stub.action_names[action], "pose") ? XR_TRUE : XR_FALSE;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL apply_haptic(XrSession session, const XrHapticActionInfo *info,
	const XrHapticBaseHeader *haptic)
{
	(void)session;
	(void)info;
	(void)haptic;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL get_interaction_profile(XrSession session, XrPath user, XrInteractionProfileState *state)
{
	(void)session;
	(void)user;
	state->interactionProfile = XR_NULL_PATH;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL get_instance_proc_address(XrInstance instance, const char *name,
	PFN_xrVoidFunction *function)
{
	static const struct
	{
		const char *name;
		PFN_xrVoidFunction function;
	} functions[] =
	{
#define F(name, function) { name, (PFN_xrVoidFunction)function }
		F("xrEnumerateInstanceExtensionProperties", enumerate_extensions),
		F("xrCreateInstance", create_instance),
		F("xrDestroyInstance", destroy_instance),
		F("xrGetInstanceProperties", get_instance_properties),
		F("xrResultToString", result_to_string),
		F("xrGetSystem", get_system),
		F("xrGetSystemProperties", get_system_properties),
		F("xrEnumerateViewConfigurationViews", enumerate_view_configuration_views),
		F("xrGetOpenGLGraphicsRequirementsKHR", get_opengl_requirements),
		F("xrCreateSession", create_session),
		F("xrDestroySession", destroy_session),
		F("xrBeginSession", begin_session),
		F("xrEndSession", end_session),
		F("xrPollEvent", poll_event),
		F("xrCreateReferenceSpace", create_reference_space),
		F("xrCreateActionSpace", create_action_space),
		F("xrDestroySpace", destroy_space),
		F("xrLocateSpace", locate_space),
		F("xrLocateViews", locate_views),
		F("xrEnumerateSwapchainFormats", enumerate_swapchain_formats),
		F("xrCreateSwapchain", create_swapchain),
		F("xrDestroySwapchain", destroy_swapchain),
		F("xrEnumerateSwapchainImages", enumerate_swapchain_images),
		F("xrAcquireSwapchainImage", acquire_swapchain_image),
		F("xrWaitSwapchainImage", wait_swapchain_image),
		F("xrReleaseSwapchainImage", release_swapchain_image),
		F("xrWaitFrame", wait_frame),
		F("xrBeginFrame", begin_frame),
		F("xrEndFrame", end_frame),
		F("xrStringToPath", string_to_path),
		F("xrPathToString", path_to_string),
		F("xrCreateActionSet", create_action_set),
		F("xrCreateAction", create_action),
		F("xrSuggestInteractionProfileBindings", suggest_bindings),
		F("xrAttachSessionActionSets", attach_action_sets),
		F("xrSyncActions", sync_actions),
		F("xrGetActionStateBoolean", get_boolean),
		F("xrGetActionStateFloat", get_float),
		F("xrGetActionStateVector2f", get_vector2f),
		F("xrGetActionStatePose", get_pose),
		F("xrApplyHapticFeedback", apply_haptic),
		F("xrGetCurrentInteractionProfile", get_interaction_profile),
		F("xrGetInstanceProcAddr", get_instance_proc_address),
#undef F
	};
	size_t index;

	(void)instance;
	*function = NULL;
	for (index = 0; index < sizeof(functions) / sizeof(functions[0]); index++)
	{
		if (!strcmp(functions[index].name, name))
		{
			*function = functions[index].function;
			return XR_SUCCESS;
		}
	}
	return XR_ERROR_FUNCTION_UNSUPPORTED;
}

/* "<yaw> <pitch> <right> [<frame>]": the yaw about the play space's up,
then the pitch about the pose's right (degrees), the move right (metres),
from the frame (100) */
static void turn_read(const char *text, struct turn *turn)
{
	float yaw = 0.0f, pitch = 0.0f, half, a[4], b[4];

	turn->frame = 100;
	turn->set = text && sscanf(text, "%f %f %f %lu", &yaw, &pitch, &turn->move, &turn->frame) >= 1;
	half = yaw * 3.14159265f / 360.0f;
	a[0] = 0.0f, a[1] = sinf(half), a[2] = 0.0f, a[3] = cosf(half);
	half = pitch * 3.14159265f / 360.0f;
	b[0] = sinf(half), b[1] = 0.0f, b[2] = 0.0f, b[3] = cosf(half);
	turn->q[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	turn->q[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	turn->q[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	turn->q[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

EXPORT XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo *loader,
	XrNegotiateRuntimeRequest *request)
{
	const char *size = getenv("XR_STUB_EYE_SIZE"), *fps = getenv("XR_STUB_FPS"), *cylinder = getenv("XR_STUB_CYLINDER");
	const char *head_turn = getenv("XR_STUB_HEAD_TURN"), *hand_turn = getenv("XR_STUB_HAND_TURN");
	const char *turn_late = getenv("XR_STUB_TURN_LATE"), *hands = getenv("XR_STUB_HANDS");

	if (!loader || !request || loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
		request->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST)
	{
		return XR_ERROR_INITIALIZATION_FAILED;
	}
	stub.eye_width = 1512;
	stub.eye_height = 1680;
	if (size && sscanf(size, "%dx%d", &stub.eye_width, &stub.eye_height) != 2)
		stub.eye_width = 1512, stub.eye_height = 1680;
	stub.fps = fps ? atof(fps) : 90.0;
	if (!(stub.fps >= 10.0 && stub.fps <= 500.0))
		stub.fps = 90.0;
	stub.cylinder = cylinder && *cylinder == '1';
	turn_read(head_turn, &stub.head_turn);
	turn_read(hand_turn, &stub.hand_turn);
	stub.turn_late = turn_late && *turn_late == '1';
	stub.hands = hands && *hands == '1';
	request->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
	request->runtimeApiVersion = XR_MAKE_VERSION(1, 0, 0);
	request->getInstanceProcAddr = get_instance_proc_address;
	return XR_SUCCESS;
}
