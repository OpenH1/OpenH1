/*
HOST_RENDERER.C

The window import of the OpenGL backend the Linux hosts have
(port/linux/host/host_gl_stream.c), which the Android app does not: its
renderer is Vulkan's, in the host (port/linux/vulkan), which the guest's
frames reach as the command stream, and its window is Vulkan's
(host_vulkan_window_open). The guest does not call it. As
port/macos/host_renderer.c; outside port/android/host, which the Linux and
macOS hosts build too.
*/

#include "host.h"

int host_gl_window(uint32_t window, int interval)
{
	(void)window;
	(void)interval;
	return 0;
}
