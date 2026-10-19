/*
HOST_RENDERER.C

The window import of the OpenGL backend the hosts once had
(host_gl_window, in every guest's import list), which no host has now: the
renderer of the Android app and of the Linux builds is Vulkan's, in the host
(port/linux/vulkan), which the guest's frames reach as the command stream,
and their window is Vulkan's (host_vulkan_window_open). The guest does not
call it. As port/macos/host_renderer.c; outside port/android/host, which
the Linux and macOS hosts build too.
*/

#include "host.h"

int host_gl_window(uint32_t window, int interval)
{
	(void)window;
	(void)interval;
	return 0;
}
