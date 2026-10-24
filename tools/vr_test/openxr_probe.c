/*
OPENXR_PROBE.C

What an OpenXR runtime offers the VR build (port/linux/host/host_vr.c): its
instance extensions, whether Vulkan's binding is among them
(XR_KHR_vulkan_enable2, without which the game plays flat), and whether it
has a headset (xrGetSystem). The runtime is loaded as host_vr.c loads it:
from a manifest's library_path, through xrNegotiateLoaderRuntimeInterface,
without the OpenXR loader.

	openxr_probe [--extensions-only] [MANIFEST]

MANIFEST, else $XR_RUNTIME_JSON, else the active runtime's
($XDG_CONFIG_HOME or ~/.config, then /etc/xdg: openxr/1/active_runtime.json).
--extensions-only stops before xrCreateInstance (which may start the
runtime's server: SteamVR's does). The exit status is 0 when the runtime was
loaded and listed its extensions.

A 64-bit host tool, built with the headers the VR build downloads into
build/linux_x86_64/third_party/openxr (configure.py --vr; or fetched there
alone, tools/linux_guest_build.py's fetch_openxr_headers), from the root of
the repository:

	python3 -c 'from pathlib import Path; from tools.linux_guest_build import fetch_openxr_headers; \
		fetch_openxr_headers(Path("build/linux_x86_64/third_party/openxr"))'
	cc -O1 -I build/linux_x86_64/third_party/openxr tools/vr_test/openxr_probe.c -o build/openxr_probe -ldl
*/

#define XR_NO_PROTOTYPES

#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

/* (openxr_platform.h's name, which would need Vulkan's headers) */
#define XR_KHR_VULKAN_ENABLE2 "XR_KHR_vulkan_enable2"

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

int main(int argc, char **argv)
{
	const char *manifest = NULL;
	int extensions_only = 0, index, has_vulkan = 0;
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
	const char *enabled[1] = { XR_KHR_VULKAN_ENABLE2 };

	for (index = 1; index < argc; index++)
	{
		if (!strcmp(argv[index], "--extensions-only"))
			extensions_only = 1;
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
	if (!extensions)
	{
		printf("extensions: out of memory for %u\n", (unsigned)count);
		return 1;
	}
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
		has_vulkan |= !strcmp(extensions[extension].extensionName, XR_KHR_VULKAN_ENABLE2);
	}
	free(extensions);
	printf("%s: %s\n", XR_KHR_VULKAN_ENABLE2, has_vulkan ? "yes" : "no (the game plays flat)");
	if (extensions_only)
		return 0;

	/* the instance (with Vulkan's binding, as the game's), and its system */
	if (!create_instance)
	{
		printf("instance: no xrCreateInstance\n");
		return 0;
	}
	strcpy(create.applicationInfo.applicationName, "openxr_probe");
	create.applicationInfo.applicationVersion = 1;
	strcpy(create.applicationInfo.engineName, "halo-ce-universal");
	create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	create.enabledExtensionCount = has_vulkan ? 1 : 0;
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
	}
	else
	{
		printf("system: xrGetSystem: %s\n", result_text(to_string, instance, result));
	}
	if (destroy_instance)
		destroy_instance(instance);
	return 0;
}
