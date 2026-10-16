/*
HOST_VR_RENDER.H

OpenXR on the host's render thread (host_vr.c), where the renderer's
backend is the host's (the command stream: host_gl_vr.c). The calls of
OpenXR that use the GL context (the session made, the swapchains' images
acquired and released, a frame begun and ended) are made on the thread the
context is current on, the render thread; the game's thread waits for the
runtime's frames (host_vr_frame_wait, which then only waits) and locates the
views and controllers, as before.
*/

#ifndef __HALO_HOST_VR_RENDER_H
#define __HALO_HOST_VR_RENDER_H

#include "vr_host.h"

/* from now on: host_vr_initialize runs on the render thread
(host_render_stream_call), host_vr_frame_wait waits alone, and the render
thread begins each frame waited for (host_vr_frame_begin) */
void host_vr_render_thread(void);
/* (the render thread) the oldest frame waited for and not yet begun,
begun, unless one is already; 1 if one is begun */
int host_vr_frame_begin(void);
/* the session's images and sizes, as host_vr_initialize made them (NULL
before) */
const struct vr_host_info *host_vr_info(void);

#endif
