/*
GUEST_METAL.C

The macOS guest's half of the Metal window (HALO_METAL): the host imports
sdl_platform.c calls, declared for the import stubs
(port/macos/host_imports.list; the host's half is host_metal.m), which
translate its arguments with the base register.
*/

int host_metal_window_open(unsigned int window);
void host_metal_battery_saver(int on);
void host_metal_vsync(int on);
