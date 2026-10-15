/*
HOST_RENDERER.C

The OpenGL services the guest's renderer would ask the host for, which
macOS does not have. The guest's renderer (port/linux/src/d3d8_gl.c) draws
with OpenGL ES through the host (port/android/host/host_gl.c) elsewhere;
on macOS the renderer is Metal's, in the host (xgpu_metal.m, host_metal.m),
which the game does not draw through yet. Until then the game runs with
the null renderer (debug.null_renderer), which calls none of these, and
every OpenGL entry point the guest imports resolves to the loader's "not
available" stub.
*/

#include "host.h"

static void no_renderer(void)
{
	host_fatal("this build has no renderer yet: run it with HALO_NULL_RENDERER=1");
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
