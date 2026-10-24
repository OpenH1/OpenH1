/*
XGPU_GL.H

The OpenGL backend's own declarations (OpenGL 4.5, OpenGL ES 3.0 to 3.2 and
WebGL 2): what its files share - the backend (xgpu_gl.c),
the GLSL emitter (xgpu_glsl.c), the FXAA and SMAA passes
(xgpu_post.c) - and the texture cache (xbox_textures.c) and the high-res
replacements (hud_hires.c, text_hires.c, menu_files.c), which still make
GL textures themselves.

The backend is built into the game by the build that draws with OpenGL in
the game's own process: the web build. The guest builds (Linux, Windows,
Android, macOS) draw in their host, with Vulkan or Metal, from the
renderer's command stream.
*/

#ifndef __HALO_LINUX_XGPU_GL_H
#define __HALO_LINUX_XGPU_GL_H

#include "xgpu.h"
#include "gl.h"

#ifdef HALO_GLES
/* OpenGL ES features that are optional (xgpu_gl.c gl_initialize) */
struct xgpu_capabilities
{
	BOOL copy_image;
	BOOL border_clamp;
	BOOL anisotropy;
	BOOL s3tc;
	/* ES 3.2, or the EXT/OES_draw_elements_base_vertex extension: glDrawElementsBaseVertex */
	BOOL base_vertex;
	/* ES 3.1 with fragment atomic counters: exact visibility test counts */
	BOOL atomic_counters;
	/* "300 es" or "310 es" */
	const char *shading_language;
};

extern struct xgpu_capabilities xgpu_capabilities;

/* port/android/guest/runtime/guest_host.h */
int host_gl_has_extension(const char *name);
void host_gl_read_buffer(unsigned int buffer, unsigned int offset, unsigned int size, void *data);
void host_gl_buffer_write(unsigned int target, unsigned int offset, unsigned int size, const void *data);
void host_gl_fence_frame(unsigned int slot);
void host_gl_wait_frame(unsigned int slot);
#endif

/* ---------- GL state

The backend caches the GL state it sets for draws (xgpu_gl.c); code that
changes GL state behind it (binding a texture to upload it, deleting one)
must call this afterwards. */

void xgpu_gl_state_invalidate(void);
/* a texture about to be deleted: the binding shadows forget it, since GL
may give its name to a new texture (call before glDeleteTextures) */
void xgpu_gl_texture_deleted(unsigned int texture);

/* a compiled shader, or a linked program of two, or 0 with the log
written */
GLuint xgpu_compile_shader(GLenum type, const char *code, const char *what);
GLuint xgpu_link_program(GLuint vertex_shader, GLuint fragment_shader, const char *what);

/* what the GLSL is written for (xgpu_glsl.c): its #version ("450 core",
"300 es", "310 es"); OpenGL ES (default precisions, clip control done in the
vertex shader, the samplers' LOD bias a uniform); the VR mode's foveated eye
pass (xgpu_foveated); and the pixel shaders' debugging settings
(debug.gpu_debug_expression, debug.gpu_debug_texture0, debug.gpu_debug_flat) */
struct xgpu_glsl_options
{
	const char *version;
	BOOL es;
	BOOL foveated;
	const char *debug_expression;
	BOOL debug_texture0, debug_flat;
};

/* GLSL for a decoded NV2A vertex program (xgpu_shader.h), written for
options. Attributes whose bit is set in packed_attribute_mask are fed as
NORMPACKED3 32-bit integers and unpacked in the shader. With lighting (else
NULL), the normal and world position go to the pixel shader too, which
lights the diffuse color for each pixel (nv2a_pixel_shader_key
per_pixel_lighting). multiview: the views drawn (XGPU_MULTIVIEW_*: xgpu.h).
Returns a malloc'd string. */
char *xgpu_glsl_vertex_shader(const struct xgpu_vertex_program *program, unsigned long packed_attribute_mask,
	const struct nv2a_vertex_lighting *lighting, int multiview, const struct xgpu_glsl_options *options);

struct xgpu_pixel_program;

/* GLSL for a decoded pixel shader (xgpu_shader.h), written for options.
Returns a malloc'd string. */
char *xgpu_glsl_pixel_shader(const struct xgpu_pixel_program *program, const struct xgpu_glsl_options *options);

/* the GL texture for an Xbox texture header, uploading or refreshing it
from guest memory as needed; *target receives GL_TEXTURE_2D etc. */
GLuint xgpu_texture_get(const DWORD *resource, const D3DCOLOR *palette, GLenum *target,
	struct xgpu_texture_description *description);
void xgpu_texture_cache_begin_frame(void);

/* ---------- anti-aliasing

display.anti_aliasing's passes (xgpu_post.c): FXAA or SMAA antialias each
window's 3D view in place before the HUD and menus are drawn over it, so
that their text stays sharp. Supersampling and multisampling are the
front end's and the backend's (d3d8_device.c, xgpu_gl.c). */

/* the programs and textures of FXAA, or of SMAA, made now; FALSE if they
cannot be (once FALSE, it stays so) */
BOOL xgpu_post_prepare(BOOL smaa);

/* FXAA, or SMAA, on the corners x0, y0 to x1, y1 (from row 0) of a render
target's framebuffer, width by height, GL_RGBA8; FALSE if its programs do
not build */
BOOL xgpu_post_anti_alias(BOOL smaa, GLuint framebuffer, unsigned long width, unsigned long height,
	const GLint corners[4]);


#endif
