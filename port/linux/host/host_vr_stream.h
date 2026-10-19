/*
HOST_VR_STREAM.H

The VR records and late latch of the host's renderer (implemented in
host_vr_stream.c). The renderer owns its GPU work; this layer locates poses,
acquires the runtime's images and submits the frame's layers.
*/

#ifndef __HALO_HOST_VR_STREAM_H
#define __HALO_HOST_VR_STREAM_H

#include "xgpu_backend.h"
#include "xgpu_stream.h"

struct host_render_backend;

struct host_vr_renderer
{
	BOOL (*supported)(void);
	void (*initialize)(struct xgpu_vr_info *info);
	BOOL (*sample_counting)(void);
	void (*eyes)(xgpu_target eye_target, unsigned int image);
	void (*pass)(int pass);
	void (*groups_reset)(void);
	void (*timing)(int mark);
	void (*compose)(const struct xgpu_vr_frame *frame);
	void (*submit)(const struct xgpu_vr_frame *frame, BOOL screenshot);
	void (*eye_transforms)(const float transforms[2][16]);
	void (*screen_corrections)(const float corrections[2][4]);
	void (*sky)(int sky);
	void (*eye_feed)(float squash, float shrink, float bright, float noise, float red, float look, float interference);
	void (*eye_tint)(float red, float green, float blue);
	/* An acquired runtime image as the renderer names it (a Vulkan host
	token). This name never crosses the guest ABI. */
	unsigned int (*image)(int swapchain, int index);
};

/* Registers VR's stream callbacks, sharing their state and late latch.
The operations table must remain alive while the renderer is used. */
void host_vr_stream_backend(struct host_render_backend *backend, const struct host_vr_renderer *renderer);

#endif
