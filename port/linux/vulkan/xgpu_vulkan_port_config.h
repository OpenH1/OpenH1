/*
XGPU_VULKAN_PORT_CONFIG.H

The host's stand-in for the guest's settings (port/linux/src/port_config.h)
where the VR mode's host files (port/linux/host/host_vr_stream.c,
host_vulkan_vr.c) are built beside the renderer's shared files: the
debugging settings they read, from their HALO_* environment variables alone
(host_vr_stream.c), as config.toml is the guest's. tools/vulkan_build.py
copies it beside them as port_config.h.
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_PORT_CONFIG_H
#define __HALO_LINUX_XGPU_VULKAN_PORT_CONFIG_H

int config_boolean(const char *name);
long config_integer(const char *name);
/* never NULL; "" when unset */
const char *config_string(const char *name);

#endif
