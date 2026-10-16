/*
HOST_GL_RENDERER.H

The OpenGL backend in the host (XGPU_GL_HOST, port/linux/src/xgpu_gl.h):
the renderer's command stream played into the backend of xgpu_gl.c, built
into the host (host_gl_stream.c), and its texture cache
(host_gl_textures.c). Its files are compiled with the backend's shared
headers and the host's stand-ins for the guest's (port/linux/host/gl).
*/

#ifndef __HALO_HOST_GL_RENDERER_H
#define __HALO_HOST_GL_RENDERER_H

#include "xgpu_gl.h"
#include "xgpu_stream.h"

struct host_render_backend;

/* the backend's table (port/android/host/host_render_stream.h), the
streams' from the window's handing over on (host_gl_window) */
const struct host_render_backend *host_gl_stream_backend(void);
/* (HALO_VR: host_gl_vr.c) the VR mode's entries in it, and OpenXR's calls
that use the context made on the render thread from now on */
void host_gl_vr_backend(struct host_render_backend *backend);

/* the GL texture of a texture stage's Xbox texture or image, read or made
again as needed (0: none); *target receives GL_TEXTURE_2D etc. */
GLuint host_gl_textures_get(const struct xgpu_stream_texture_stage *stage, GLenum *target);

#endif
