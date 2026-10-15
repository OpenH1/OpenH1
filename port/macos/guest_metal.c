/*
GUEST_METAL.C

The macOS guest's half of the Metal window (HALO_METAL): the host import
sdl_platform.c calls, declared for the import stubs
(port/macos/host_imports.list; the host's half is host_metal.m), and the
null renderer's window.

Until the Metal backend draws the game (the renderer's command stream), the
macOS build runs with the null renderer, which makes no window.
debug.metal_window makes one anyway: the guest image is linked with
--wrap=Direct3D_CreateDevice (tools/macos_build.py), so that the device's
creation here also opens the window, which the host then presents at each
of the display's refreshes. The game's own events and the window's keys
(F11's fullscreen) are pumped as with a renderer.
*/

/* ---------- the host's half (host_metal.m, host_imports.list) */

int host_metal_window_open(unsigned int window);

/* ---------- the null renderer's window */

/* port_config.h, sdl_platform.c */
int config_boolean(const char *name);
int platform_video_initialize(unsigned long width, unsigned long height);

/* d3d8_gl.c's (HRESULT, D3DDEVTYPE, DWORD, D3DPRESENT_PARAMETERS *,
D3DDevice **) */
long __real_Direct3D_CreateDevice(unsigned int adapter, int device_type, void *unused, unsigned long behavior_flags,
	void *presentation_parameters, void **returned_device);

long __wrap_Direct3D_CreateDevice(unsigned int adapter, int device_type, void *unused, unsigned long behavior_flags,
	void *presentation_parameters, void **returned_device)
{
	long result = __real_Direct3D_CreateDevice(adapter, device_type, unused, behavior_flags, presentation_parameters,
		returned_device);

	if (config_boolean("debug.null_renderer") && config_boolean("debug.metal_window"))
		platform_video_initialize(640, 480);
	return result;
}
