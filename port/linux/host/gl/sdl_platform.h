/*
SDL_PLATFORM.H (the host's)

The host's stand-in for the guest's sdl_platform.h, for the OpenGL backend
built into the host (platform.h, beside it): the window it draws into, the
host's (port/linux/host/host_gl_stream.c).
*/

#ifndef __HALO_HOST_GL_SDL_PLATFORM_H
#define __HALO_HOST_GL_SDL_PLATFORM_H

/* the window's drawable size in pixels */
void platform_video_drawable_size(int *width, int *height);
/* the frame shown (with vsync, paced by the display: host_gl_stream.c;
without, the guest paces the frames: sdl_platform.c) */
void platform_video_swap(void);

#endif
