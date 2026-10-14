/*
OPENXR_PROBE.C

What an OpenXR runtime offers the VR build (port/linux/host/host_vr.c): its
instance extensions, the GL bindings among them (XR_KHR_opengl_enable, and
XR_MNDX_egl_enable, without which the session takes GLX's binding), and
whether it has a headset (xrGetSystem). The runtime is loaded as host_vr.c
loads it: from a manifest's library_path, through
xrNegotiateLoaderRuntimeInterface, without the OpenXR loader.

	openxr_probe [--extensions-only | --egl-session] [MANIFEST]

MANIFEST, else $XR_RUNTIME_JSON, else the active runtime's
($XDG_CONFIG_HOME or ~/.config, then /etc/xdg: openxr/1/active_runtime.json).
--extensions-only stops before xrCreateInstance (which may start the
runtime's server: SteamVR's does). --egl-session goes on to open a session
with XR_MNDX_egl_enable's binding, on a desktop OpenGL 4.5 context of EGL's
default display without a surface (the game's has its window's), with
libEGL.so.1 loaded at run time. The exit status is 0 when the runtime was
loaded and listed its extensions.

A 64-bit host tool, built with the headers the VR build downloads
(tools/linux_guest_build.py, fetch_openxr_headers), from the root of the
repository:

	python3 -c 'from pathlib import Path; from tools.linux_guest_build import fetch_openxr_headers; \
		fetch_openxr_headers(Path("build/openxr"))'
	cc -O1 -I build/openxr tools/vr_test/openxr_probe.c -o build/openxr_probe -ldl

(EGL's header from libegl-dev or Mesa; the probe needs no EGL library but
for --egl-session.)
*/

#define XR_NO_PROTOTYPES

#define XR_USE_PLATFORM_EGL
#define XR_USE_GRAPHICS_API_OPENGL

#include <EGL/egl.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#define XR_KHR_OPENGL_ENABLE XR_KHR_OPENGL_ENABLE_EXTENSION_NAME
#define XR_MNDX_EGL_ENABLE XR_MNDX_EGL_ENABLE_EXTENSION_NAME

static PFN_xrGetInstanceProcAddr get_instance_proc_address;

/* the manifest's library_path, made absolute from its folder (host_vr.c's
manifest_library) */
static int manifest_library(const char *manifest, char *library, size_t size)
{
	char real[PATH_MAX], text[8192], *key, *value, *end, *slash;
	FILE *file;
	size_t length;

	if (!realpath(manifest, real) || !(file = fopen(real, "rb")))
		return 0;
	length = fread(text, 1, sizeof(text) - 1, file);
	fclose(file);
	text[length] = 0;
	if (!(key = strstr(text, "\"library_path\"")) || !(value = strchr(key + 14, ':')) ||
		!(value = strchr(value, '"')) || !(end = strchr(++value, '"')))
	{
		return 0;
	}
	*end = 0;
	if (value[0] == '/')
	{
		snprintf(library, size, "%s", value);
	}
	else
	{
		slash = strrchr(real, '/');
		if (slash)
			*slash = 0;
		snprintf(library, size, "%s/%s", real, value);
	}
	return 1;
}

static int runtime_load(const char *argument)
{
	char manifest[PATH_MAX], library[PATH_MAX];
	const char *config = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME"), *override = getenv("XR_RUNTIME_JSON");
	PFN_xrNegotiateLoaderRuntimeInterface negotiate;
	XrNegotiateLoaderInfo loader = { 0 };
	XrNegotiateRuntimeRequest request = { 0 };
	void *handle;
	XrResult result;

	if (argument)
		snprintf(manifest, sizeof(manifest), "%s", argument);
	else if (override && *override)
		snprintf(manifest, sizeof(manifest), "%s", override);
	else if (config && *config)
		snprintf(manifest, sizeof(manifest), "%s/openxr/1/active_runtime.json", config);
	else
		snprintf(manifest, sizeof(manifest), "%s/.config/openxr/1/active_runtime.json", home ? home : "");
	if (!manifest_library(manifest, library, sizeof(library)))
	{
		if (argument || (override && *override))
		{
			printf("manifest: %s: unreadable, or no library_path\n", manifest);
			return 0;
		}
		snprintf(manifest, sizeof(manifest), "/etc/xdg/openxr/1/active_runtime.json");
		if (!manifest_library(manifest, library, sizeof(library)))
		{
			printf("manifest: no active OpenXR runtime\n");
			return 0;
		}
	}
	printf("manifest: %s\nlibrary: %s\n", manifest, library);
	if (!(handle = dlopen(library, RTLD_NOW | RTLD_LOCAL)))
	{
		printf("dlopen: %s\n", dlerror());
		return 0;
	}
	if (!(negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)dlsym(handle, "xrNegotiateLoaderRuntimeInterface")))
	{
		printf("negotiation: no xrNegotiateLoaderRuntimeInterface\n");
		return 0;
	}
	loader.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
	loader.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
	loader.structSize = sizeof(loader);
	loader.minInterfaceVersion = 1;
	loader.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
	loader.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
	loader.maxApiVersion = XR_MAKE_VERSION(1, 1, 0xffffffff);
	request.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
	request.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
	request.structSize = sizeof(request);
	result = negotiate(&loader, &request);
	if (result != XR_SUCCESS || !request.getInstanceProcAddr)
	{
		printf("negotiation: refused (%d)\n", (int)result);
		return 0;
	}
	printf("negotiation: interface %u, API %u.%u.%u\n", (unsigned)request.runtimeInterfaceVersion,
		(unsigned)XR_VERSION_MAJOR(request.runtimeApiVersion), (unsigned)XR_VERSION_MINOR(request.runtimeApiVersion),
		(unsigned)XR_VERSION_PATCH(request.runtimeApiVersion));
	get_instance_proc_address = request.getInstanceProcAddr;
	return 1;
}

static void *function(XrInstance instance, const char *name)
{
	PFN_xrVoidFunction address = NULL;

	if (get_instance_proc_address(instance, name, &address) != XR_SUCCESS)
		return NULL;
	return (void *)address;
}

static const char *result_text(PFN_xrResultToString to_string, XrInstance instance, XrResult result)
{
	static char text[XR_MAX_RESULT_STRING_SIZE];

	if (!to_string || to_string(instance, result, text) != XR_SUCCESS)
		snprintf(text, sizeof(text), "XrResult %d", (int)result);
	return text;
}

/* a session with EGL's binding (--egl-session): 1 if xrCreateSession took it */
static int egl_session(XrInstance instance, XrSystemId system, PFN_xrResultToString to_string)
{
	void *library = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
	EGLDisplay (*get_display)(EGLNativeDisplayType);
	EGLBoolean (*initialize)(EGLDisplay, EGLint *, EGLint *);
	EGLBoolean (*bind_api)(EGLenum);
	EGLBoolean (*choose_config)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
	EGLContext (*create_context)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
	EGLBoolean (*make_current)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
	EGLBoolean (*destroy_context)(EGLDisplay, EGLContext);
	EGLBoolean (*terminate)(EGLDisplay);
	PFN_xrEglGetProcAddressMNDX get_proc_address;
	PFN_xrGetOpenGLGraphicsRequirementsKHR requirements_get;
	PFN_xrCreateSession create_session;
	PFN_xrDestroySession destroy_session;
	static const EGLint config_attributes[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
	static const EGLint context_attributes[] = { 0x3098 /* EGL_CONTEXT_MAJOR_VERSION */, 4,
		0x30FB /* EGL_CONTEXT_MINOR_VERSION */, 5, EGL_NONE };
	XrGraphicsRequirementsOpenGLKHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
	XrGraphicsBindingEGLMNDX binding = { XR_TYPE_GRAPHICS_BINDING_EGL_MNDX };
	XrSessionCreateInfo create = { XR_TYPE_SESSION_CREATE_INFO };
	XrSession session = XR_NULL_HANDLE;
	EGLDisplay display;
	EGLConfig config = NULL;
	EGLContext context;
	EGLint count = 0, major = 0, minor = 0;
	XrResult result;

	if (!library)
	{
		printf("session: no libEGL.so.1\n");
		return 0;
	}
	get_display = (EGLDisplay (*)(EGLNativeDisplayType))dlsym(library, "eglGetDisplay");
	initialize = (EGLBoolean (*)(EGLDisplay, EGLint *, EGLint *))dlsym(library, "eglInitialize");
	bind_api = (EGLBoolean (*)(EGLenum))dlsym(library, "eglBindAPI");
	choose_config = (EGLBoolean (*)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))dlsym(library,
		"eglChooseConfig");
	create_context = (EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint *))dlsym(library,
		"eglCreateContext");
	make_current = (EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext))dlsym(library, "eglMakeCurrent");
	destroy_context = (EGLBoolean (*)(EGLDisplay, EGLContext))dlsym(library, "eglDestroyContext");
	terminate = (EGLBoolean (*)(EGLDisplay))dlsym(library, "eglTerminate");
	get_proc_address = (PFN_xrEglGetProcAddressMNDX)dlsym(library, "eglGetProcAddress");
	if (!get_display || !initialize || !bind_api || !choose_config || !create_context || !make_current ||
		!destroy_context || !terminate || !get_proc_address)
	{
		printf("session: libEGL.so.1 lacks a function\n");
		return 0;
	}
	display = get_display(EGL_DEFAULT_DISPLAY);
	if (display == EGL_NO_DISPLAY || !initialize(display, &major, &minor) || !bind_api(EGL_OPENGL_API) ||
		!choose_config(display, config_attributes, &config, 1, &count) || count < 1)
	{
		printf("session: no EGL display or desktop OpenGL config\n");
		return 0;
	}
	context = create_context(display, config, EGL_NO_CONTEXT, context_attributes);
	if (context == EGL_NO_CONTEXT || !make_current(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
	{
		printf("session: no current OpenGL 4.5 context on EGL %d.%d\n", (int)major, (int)minor);
		return 0;
	}
	printf("session: EGL %d.%d, an OpenGL 4.5 context\n", (int)major, (int)minor);
	requirements_get = (PFN_xrGetOpenGLGraphicsRequirementsKHR)function(instance,
		"xrGetOpenGLGraphicsRequirementsKHR");
	create_session = (PFN_xrCreateSession)function(instance, "xrCreateSession");
	destroy_session = (PFN_xrDestroySession)function(instance, "xrDestroySession");
	if (requirements_get && requirements_get(instance, system, &requirements) == XR_SUCCESS)
	{
		printf("session: OpenGL %u.%u to %u.%u\n", (unsigned)XR_VERSION_MAJOR(requirements.minApiVersionSupported),
			(unsigned)XR_VERSION_MINOR(requirements.minApiVersionSupported),
			(unsigned)XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
			(unsigned)XR_VERSION_MINOR(requirements.maxApiVersionSupported));
	}
	binding.getProcAddress = get_proc_address;
	binding.display = display;
	binding.config = config;
	binding.context = context;
	create.next = &binding;
	create.systemId = system;
	result = create_session ? create_session(instance, &create, &session) : XR_ERROR_FUNCTION_UNSUPPORTED;
	printf("session: xrCreateSession (EGL): %s\n", result_text(to_string, instance, result));
	if (result == XR_SUCCESS && destroy_session)
		destroy_session(session);
	make_current(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	destroy_context(display, context);
	terminate(display);
	return result == XR_SUCCESS;
}

int main(int argc, char **argv)
{
	const char *manifest = NULL;
	int extensions_only = 0, session = 0, index, has_opengl = 0, has_egl = 0;
	PFN_xrEnumerateInstanceExtensionProperties enumerate;
	PFN_xrCreateInstance create_instance;
	PFN_xrDestroyInstance destroy_instance;
	PFN_xrResultToString to_string;
	PFN_xrGetInstanceProperties instance_properties;
	PFN_xrGetSystem get_system;
	PFN_xrGetSystemProperties system_properties;
	XrExtensionProperties *extensions;
	uint32_t count = 0, extension;
	XrResult result;
	XrInstanceCreateInfo create = { XR_TYPE_INSTANCE_CREATE_INFO };
	XrInstanceProperties properties = { XR_TYPE_INSTANCE_PROPERTIES };
	XrSystemGetInfo system_info = { XR_TYPE_SYSTEM_GET_INFO };
	XrSystemProperties system = { XR_TYPE_SYSTEM_PROPERTIES };
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId system_id = XR_NULL_SYSTEM_ID;
	const char *enabled[2] = { XR_KHR_OPENGL_ENABLE, XR_MNDX_EGL_ENABLE };

	for (index = 1; index < argc; index++)
	{
		if (!strcmp(argv[index], "--extensions-only"))
			extensions_only = 1;
		else if (!strcmp(argv[index], "--egl-session"))
			session = 1;
		else
			manifest = argv[index];
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (!runtime_load(manifest))
		return 1;
	enumerate = (PFN_xrEnumerateInstanceExtensionProperties)function(XR_NULL_HANDLE,
		"xrEnumerateInstanceExtensionProperties");
	create_instance = (PFN_xrCreateInstance)function(XR_NULL_HANDLE, "xrCreateInstance");
	if (!enumerate || (result = enumerate(NULL, 0, &count, NULL)) != XR_SUCCESS)
	{
		printf("extensions: cannot list them (%d)\n", enumerate ? (int)result : 0);
		return 1;
	}
	extensions = calloc(count ? count : 1, sizeof(*extensions));
	for (extension = 0; extension < count; extension++)
		extensions[extension].type = XR_TYPE_EXTENSION_PROPERTIES;
	if ((result = enumerate(NULL, count, &count, extensions)) != XR_SUCCESS)
	{
		printf("extensions: cannot list them (%d)\n", (int)result);
		return 1;
	}
	printf("extensions: %u\n", (unsigned)count);
	for (extension = 0; extension < count; extension++)
	{
		printf("  %s %u\n", extensions[extension].extensionName, (unsigned)extensions[extension].extensionVersion);
		has_opengl |= !strcmp(extensions[extension].extensionName, XR_KHR_OPENGL_ENABLE);
		has_egl |= !strcmp(extensions[extension].extensionName, XR_MNDX_EGL_ENABLE);
	}
	free(extensions);
	printf("%s: %s\n%s: %s\n", XR_KHR_OPENGL_ENABLE, has_opengl ? "yes" : "no", XR_MNDX_EGL_ENABLE,
		has_egl ? "yes" : "no");
	printf("binding: %s\n", !has_opengl ? "none (no OpenGL)" : has_egl ? "EGL (XrGraphicsBindingEGLMNDX)" :
		"GLX (XrGraphicsBindingOpenGLXlibKHR)");
	if (extensions_only)
		return 0;

	/* the instance (with OpenGL's extension, as the game's), and its system */
	if (!create_instance)
	{
		printf("instance: no xrCreateInstance\n");
		return 0;
	}
	strcpy(create.applicationInfo.applicationName, "openxr_probe");
	create.applicationInfo.applicationVersion = 1;
	strcpy(create.applicationInfo.engineName, "halo-ce-universal");
	create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	create.enabledExtensionCount = !has_opengl ? 0 : session && has_egl ? 2 : 1;
	create.enabledExtensionNames = enabled;
	result = create_instance(&create, &instance);
	if (result != XR_SUCCESS)
	{
		printf("instance: xrCreateInstance: %s\n", result_text(NULL, XR_NULL_HANDLE, result));
		return 0;
	}
	to_string = (PFN_xrResultToString)function(instance, "xrResultToString");
	destroy_instance = (PFN_xrDestroyInstance)function(instance, "xrDestroyInstance");
	instance_properties = (PFN_xrGetInstanceProperties)function(instance, "xrGetInstanceProperties");
	get_system = (PFN_xrGetSystem)function(instance, "xrGetSystem");
	system_properties = (PFN_xrGetSystemProperties)function(instance, "xrGetSystemProperties");
	if (instance_properties && instance_properties(instance, &properties) == XR_SUCCESS)
	{
		printf("instance: %s %u.%u.%u\n", properties.runtimeName,
			(unsigned)XR_VERSION_MAJOR(properties.runtimeVersion), (unsigned)XR_VERSION_MINOR(properties.runtimeVersion),
			(unsigned)XR_VERSION_PATCH(properties.runtimeVersion));
	}
	else
	{
		printf("instance: made\n");
	}
	system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	result = get_system ? get_system(instance, &system_info, &system_id) : XR_ERROR_FUNCTION_UNSUPPORTED;
	if (result == XR_SUCCESS)
	{
		if (system_properties && system_properties(instance, system_id, &system) == XR_SUCCESS)
			printf("system: %s (vendor %u)\n", system.systemName, (unsigned)system.vendorId);
		else
			printf("system: %llu\n", (unsigned long long)system_id);
		if (session && !has_egl)
			printf("session: no XR_MNDX_egl_enable\n");
		else if (session)
			egl_session(instance, system_id, to_string);
	}
	else
	{
		printf("system: xrGetSystem: %s\n", result_text(to_string, instance, result));
	}
	if (destroy_instance)
		destroy_instance(instance);
	return 0;
}
