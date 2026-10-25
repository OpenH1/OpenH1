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
/* (port/macos/host_links.c, iOS's too) the links the app was opened with:
the oldest not taken yet, 1; else 0 */
int host_app_take_link(char *link, int size);
