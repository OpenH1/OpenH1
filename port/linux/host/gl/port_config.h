/*
PORT_CONFIG.H (the host's)

The host's stand-in for the guest's settings (port/linux/src/port_config.h),
for the OpenGL backend built into the host (platform.h, beside it): the
debugging settings it reads, from their HALO_* environment variables alone
(port/linux/host/host_gl_stream.c), as config.toml is the guest's.
*/

#ifndef __HALO_HOST_GL_PORT_CONFIG_H
#define __HALO_HOST_GL_PORT_CONFIG_H

int config_boolean(const char *name);
long config_integer(const char *name);
/* never NULL; "" when unset */
const char *config_string(const char *name);

#endif
