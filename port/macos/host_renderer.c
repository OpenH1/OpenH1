/*
HOST_RENDERER.C

The OpenGL services the guest's renderer would ask the host for, which
macOS does not have. The guest's renderer draws with OpenGL ES through the
host (port/android/host/host_gl.c) elsewhere; on macOS its backend is
Metal's, in the host (xgpu_metal.m, host_metal.m), which the guest's frames
reach as the command stream: the guest has no OpenGL backend and calls none
of these, and every OpenGL entry point the guest imports resolves to the
loader's "not available" stub.
*/

#include "host.h"

static void no_renderer(void)
{
	host_fatal("the macOS build has no OpenGL (its renderer is Metal's)");
}

void *host_gl_resolve(const char *name)
{
	(void)name;
	return (void *)no_renderer;
}

void host_gl_get_string(uint32_t name, int index, char *buffer, uint32_t size)
{
	(void)name;
	(void)index;
	if (size)
		buffer[0] = 0;
}

int host_gl_has_extension(const char *name)
{
	(void)name;
	return 0;
}

void host_gl_read_buffer(uint32_t buffer, uint32_t offset, uint32_t size, void *data)
{
	(void)buffer;
	(void)offset;
	(void)size;
	(void)data;
	no_renderer();
}

void host_gl_buffer_write(uint32_t target, uint32_t offset, uint32_t size, const void *data)
{
	(void)target;
	(void)offset;
	(void)size;
	(void)data;
	no_renderer();
}

void host_gl_fence_frame(uint32_t slot)
{
	(void)slot;
	no_renderer();
}

void host_gl_wait_frame(uint32_t slot)
{
	(void)slot;
	no_renderer();
}

int host_gl_map_results(uint32_t buffer, uint32_t size)
{
	(void)buffer;
	(void)size;
	no_renderer();
	return 0;
}

void host_gl_read_results(void *data, uint32_t size)
{
	(void)data;
	(void)size;
	no_renderer();
}
