/*
HOST_WINDOWS.C

The host's half of what the 64-bit Windows build's guest needs beyond the
Linux desktop guest's (guest_windows.c is the guest's half,
host_imports.list lists them): the Windows functions the platform layer
calls under _WIN32 in the 32-bit build, here performed for the guest.

Until the Vulkan renderer is built in (port/linux/vulkan/host_vulkan.c),
the window has no renderer: host_vulkan_window_open refuses it, and the
game runs without drawing (debug.null_renderer, a server).
*/

#include "host_win.h"

/* port/windows/src/win32_p2p.c */
int posix_hardware_id_source(char *text, int size);

int host_windows_hardware_id(char *text, int size)
{
	return posix_hardware_id_source(text, size);
}

int host_vulkan_window_open(uint32_t window)
{
	static int logged;

	(void)window;
	if (!logged)
	{
		logged = 1;
		host_logf(HOST_LOG_WARN, "this build has no renderer yet: the window stays empty");
	}
	return 0;
}
