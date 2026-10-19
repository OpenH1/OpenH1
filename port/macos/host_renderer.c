/*
HOST_RENDERER.C

The window import of the OpenGL backend the hosts once had (host_gl_window,
in every guest's import list), which macOS does not have: its renderer is
Metal's, in the host (xgpu_metal.m, host_metal.m), which the guest's frames
reach as the command stream, and its window is Metal's (host_metal_window_
open). The guest does not call it. As port/android/host_renderer.c, which
the Android and Linux hosts build.
*/

#include "host.h"

int host_gl_window(uint32_t window, int interval)
{
	(void)window;
	(void)interval;
	return 0;
}
