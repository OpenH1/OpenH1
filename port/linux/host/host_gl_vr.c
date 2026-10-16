/*
HOST_GL_VR.C

The VR mode of the OpenGL backend in the host (HALO_VR with XGPU_GL_HOST:
the renderer's command stream played on the host's render thread,
host_gl_stream.c): the VR records' entries (xgpu_stream.h, "the VR mode"),
and what xgpu_gl.c's composition asks of the game's VR mode (vr.h's and
vr_visor.h's functions, the guest's vr.c and vr_visor.c), answered as the
records said, and of OpenXR, which it calls here on the render thread
(host_vr_render.h).

A frame of the runtime's is waited for on the game's thread
(host_vr_frame_wait) and begun here before its first image is acquired (or
at its end, if none is), then ended with the layers the game would submit
(VR_SUBMIT's, computed by vr.c as if every image were acquired), less those
whose image the composition did not acquire or draw: as vr.c's
halo_vr_present does with the guest's own backend.
*/

#include "host.h"
#include "host_render_stream.h"
#include "host_gl_renderer.h"
#include "host_vr_render.h"
#include "xgpu_backend.h"
#include "vr.h"
#include "vr_visor.h"
#include "port_config.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(struct vr_host_layers) == XGPU_STREAM_VR_LAYERS_WORDS * 4, "VR layers");
_Static_assert(sizeof(struct vr_host_pose) == XGPU_STREAM_VR_POSE_WORDS * 4, "VR pose");
_Static_assert(sizeof(struct halo_vr_scope_window) == XGPU_STREAM_VR_SCOPE_WINDOW_WORDS * 4, "VR scope window");
_Static_assert(sizeof(struct halo_vr_cockpit_window) == XGPU_STREAM_VR_COCKPIT_WINDOW_WORDS * 4, "VR cockpit window");
_Static_assert(sizeof(struct halo_vr_cockpit_display) == XGPU_STREAM_VR_COCKPIT_DISPLAY_WORDS * 4,
	"VR cockpit display");
_Static_assert(sizeof(struct vr_visor_hud_image) == XGPU_STREAM_VR_HUD_IMAGE_WORDS * 4, "VR HUD image");
_Static_assert(sizeof(struct vr_visor_panel) == XGPU_STREAM_VR_PANEL_WORDS * 4, "VR panel");
_Static_assert(offsetof(struct vr_visor_image, noise_texture) >= XGPU_STREAM_VR_VISOR_IMAGE_WORDS * 4 &&
	offsetof(struct vr_visor_image, noise_texture) < XGPU_STREAM_VR_VISOR_IMAGE_WORDS * 4 + 8, "VR visor image");

/* (xgpu_gl.c's: vr.c's callers declare it) */
void halo_vr_eye_feed(float squash, float shrink, float bright, float noise, float red, float look, float interference);
_Static_assert(sizeof(struct vr_visor_rim) == XGPU_STREAM_VR_RIM_WORDS * 4, "VR rim");

static struct
{
	/* vr.foveation's inset and its name, as the session was made */
	float foveation_inset;
	char foveation_name[64];
	/* this frame's: the gaze (VR_EYES), the state (VR_STATE), whether the
	game waited for a frame of the runtime's for it (either says) */
	int foveation;
	float centres[2][2];
	struct xgpu_stream_vr_state state;
	BOOL frame;
	/* the images acquired this frame (bits of VR_SWAPCHAIN_*), the eyes'
	depth written */
	unsigned int acquired;
	BOOL depth_written;
	/* the frame being submitted (VR_SUBMIT), and the layers ended with */
	const struct xgpu_stream_vr_submit *submit;
	struct vr_host_layers layers;
	struct vr_host_pose head;
	BOOL submitted;
} vr_frame;

/* ---------- vr.h's, as the records said */

float halo_vr_foveation_inset(void)
{
	return vr_frame.foveation_inset;
}

const char *halo_vr_foveation_name(void)
{
	return vr_frame.foveation_name;
}

int halo_vr_foveation(float centres[2][2])
{
	if (!vr_frame.foveation)
		return 0;
	memcpy(centres, vr_frame.centres, sizeof(vr_frame.centres));
	return vr_frame.foveation;
}

float halo_vr_scope_zoom(float *height)
{
	*height = vr_frame.state.scope_zoom_height;
	return vr_frame.state.scope_zoom;
}

int halo_vr_scope_smartlink(float reticle[2])
{
	reticle[0] = vr_frame.state.scope_reticle[0];
	reticle[1] = vr_frame.state.scope_reticle[1];
	return (vr_frame.state.answered & XGPU_STREAM_VR_STATE_SMARTLINK) != 0;
}

int halo_vr_scope_window(struct halo_vr_scope_window *window)
{
	if (!(vr_frame.state.answered & XGPU_STREAM_VR_STATE_SCOPE_WINDOW))
		return 0;
	memcpy(window, vr_frame.state.scope_window, sizeof(*window));
	return 1;
}

void halo_vr_scope_readout(float *range, int *rounds_loaded, int *rounds_carried)
{
	*range = vr_frame.state.scope_range;
	*rounds_loaded = vr_frame.state.scope_rounds_loaded;
	*rounds_carried = vr_frame.state.scope_rounds_carried;
}

void halo_vr_scope_animation(float *opened_seconds, float *closing, float *switched_seconds)
{
	*opened_seconds = vr_frame.state.scope_animation[0];
	*closing = vr_frame.state.scope_animation[1];
	*switched_seconds = vr_frame.state.scope_animation[2];
}

int halo_vr_scope_gun(void)
{
	return vr_frame.state.scope_gun;
}

int halo_vr_cockpit_window(struct halo_vr_cockpit_window *window, struct halo_vr_cockpit_display *display)
{
	if (!(vr_frame.state.answered & XGPU_STREAM_VR_STATE_COCKPIT))
		return 0;
	memcpy(window, vr_frame.state.cockpit_window, sizeof(*window));
	memcpy(display, vr_frame.state.cockpit_display, sizeof(*display));
	return 1;
}

float halo_vr_vignette(float fov[2][4])
{
	memcpy(fov, vr_frame.state.vignette_fov, sizeof(vr_frame.state.vignette_fov));
	return vr_frame.state.vignette;
}

int halo_vr_depth_wanted(void)
{
	return (vr_frame.state.answered & XGPU_STREAM_VR_STATE_DEPTH) != 0;
}

void halo_vr_depth_written(void)
{
	vr_frame.depth_written = TRUE;
}

void halo_vr_panel_size(int panel, int *width, int *height)
{
	const struct vr_host_info *info = host_vr_info();

	*width = info && panel >= 0 && panel < VR_PANEL_COUNT ? info->panel_width[panel] : 0;
	*height = info && panel >= 0 && panel < VR_PANEL_COUNT ? info->panel_height[panel] : 0;
}

/* the swapchain's image for this frame, acquired (the frame begun first) */
unsigned int halo_vr_image(int swapchain)
{
	const struct vr_host_info *info = host_vr_info();
	int index;

	if (!vr_frame.frame || !info || swapchain < 0 || swapchain >= VR_SWAPCHAIN_COUNT || !host_vr_frame_begin())
		return 0;
	index = host_vr_acquire(swapchain);
	if (index < 0 || index >= info->image_count[swapchain])
		return 0;
	vr_frame.acquired |= 1u << swapchain;
	return info->images[swapchain][index];
}

/* the frame ended with the game's layers, less those not acquired or not
drawn (vr.c's halo_vr_present's tests, here) */
void halo_vr_present(int eyes_drawn, int hud_drawn, int scope_drawn)
{
	const struct xgpu_stream_vr_submit *submit = vr_frame.submit;
	struct vr_host_layers *layers = &vr_frame.layers;
	int panel;

	if (!submit || !submit->waited)
		return;
	memcpy(layers, submit->layers, sizeof(*layers));
	memcpy(&vr_frame.head, submit->head, sizeof(vr_frame.head));
	layers->projection = layers->projection && eyes_drawn && (vr_frame.acquired & (1u << VR_SWAPCHAIN_EYES));
	layers->hud = layers->hud && hud_drawn && (vr_frame.acquired & (1u << VR_SWAPCHAIN_HUD));
	layers->depth = layers->depth && layers->projection && vr_frame.depth_written &&
		(vr_frame.acquired & (1u << VR_SWAPCHAIN_DEPTH));
	layers->scope = layers->scope && layers->projection && scope_drawn &&
		(vr_frame.acquired & (1u << VR_SWAPCHAIN_SCOPE));
	for (panel = 0; panel < VR_PANEL_COUNT; panel++)
		layers->panels[panel].shown = layers->panels[panel].shown &&
			(vr_frame.acquired & (1u << (VR_SWAPCHAIN_PANEL + panel)));
	/* (a frame none of whose images was acquired is begun to be ended) */
	if (host_vr_frame_begin())
		host_vr_frame_end(layers);
	vr_frame.submitted = TRUE;
}

int halo_vr_submitted(struct vr_host_layers *layers, struct vr_host_pose *head)
{
	if (!vr_frame.submitted)
		return 0;
	*layers = vr_frame.layers;
	*head = vr_frame.head;
	return 1;
}

/* ---------- vr_visor.h's, as the records said */

void vr_visor_hud_image(struct vr_visor_hud_image *image)
{
	memcpy(image, vr_frame.state.hud_image, sizeof(*image));
}

int vr_visor_panels(struct vr_visor_panel panels[VR_PANEL_COUNT])
{
	if (!(vr_frame.state.answered & XGPU_STREAM_VR_STATE_PANELS))
		return 0;
	memcpy(panels, vr_frame.state.panels, VR_PANEL_COUNT * sizeof(panels[0]));
	return 1;
}

void vr_visor_image(struct vr_visor_image *image)
{
	memset(image, 0, sizeof(*image));
	memcpy(image, vr_frame.state.visor_image, sizeof(vr_frame.state.visor_image));
	/* (guest addresses: xgpu_host_texture_header) */
	image->noise_texture[0] = (void *)(uintptr_t)vr_frame.state.visor_noise[0];
	image->noise_texture[1] = (void *)(uintptr_t)vr_frame.state.visor_noise[1];
	image->time = vr_frame.state.visor_time;
}

int vr_visor_rim(struct vr_visor_rim *rim)
{
	if (!(vr_frame.state.answered & XGPU_STREAM_VR_STATE_RIM))
	{
		memset(rim, 0, sizeof(*rim));
		return 0;
	}
	memcpy(rim, vr_frame.state.rim, sizeof(*rim));
	return 1;
}

/* ---------- the screenshots (xgpu.h; d3d8_device.c's, in the guest) */

static unsigned long screenshot_number;

void xgpu_write_bmp(const char *name, const unsigned char *pixels, unsigned long width, unsigned long height,
	BOOL bottom_up)
{
	const char *directory = config_string("debug.screenshot_directory");
	unsigned char header[54] = { 'B', 'M' };
	uint32_t image_size = (uint32_t)(width * height * 4);
	unsigned long row;
	char path[512];
	FILE *file;

	snprintf(path, sizeof(path), "%s/%s%05lu.bmp", directory, name, screenshot_number);
	file = fopen(path, "wb");
	if (!file)
		return;
	*(uint32_t *)(header + 2) = 54 + image_size;
	*(uint32_t *)(header + 10) = 54;
	*(uint32_t *)(header + 14) = 40;
	*(int32_t *)(header + 18) = (int32_t)width;
	*(int32_t *)(header + 22) = bottom_up ? (int32_t)height : -(int32_t)height;
	*(uint16_t *)(header + 26) = 1;
	*(uint16_t *)(header + 28) = 32;
	*(uint32_t *)(header + 34) = image_size;
	fwrite(header, 1, sizeof(header), file);
	for (row = 0; row < height; row++)
		fwrite(pixels + row * width * 4, 1, width * 4, file);
	fclose(file);
}

/* ---------- the records */

static int stream_vr_supported(void *context)
{
	(void)context;
	return xgpu_backend_vr_supported() != FALSE;
}

static void stream_vr_initialize(void *context, struct xgpu_stream_vr_info *wire)
{
	struct xgpu_vr_info info;

	(void)context;
	vr_frame.foveation_inset = wire->foveation_inset;
	memcpy(vr_frame.foveation_name, wire->foveation_name, sizeof(vr_frame.foveation_name));
	vr_frame.foveation_name[sizeof(vr_frame.foveation_name) - 1] = 0;
	memset(&info, 0, sizeof(info));
	info.eye_width = wire->eye_width;
	info.eye_height = wire->eye_height;
	info.hud_width = wire->hud_width;
	info.hud_height = wire->hud_height;
	info.scope_size = wire->scope_size;
	info.statistics = wire->statistics != 0;
	xgpu_backend_vr_initialize(&info);
	wire->foveated = info.foveated != 0;
	wire->fovea_width = info.fovea_width;
	wire->fovea_height = info.fovea_height;
	wire->sample_counting = xgpu_backend_capabilities.sample_counting != 0;
	/* (the shaders are written for the foveated eye pass: as the front
	end's xgpu_foveated, which it sets from this answer) */
	xgpu_foveated = info.foveated ? 1 : 0;
}

static void stream_vr_eyes(void *context, const struct xgpu_stream_vr_eyes *record)
{
	(void)context;
	vr_frame.frame = TRUE;
	vr_frame.foveation = record->foveation;
	memcpy(vr_frame.centres, record->centres, sizeof(vr_frame.centres));
	/* (the game's image is the host's to acquire) */
	xgpu_backend_vr_eyes(record->eye_target, halo_vr_image(VR_SWAPCHAIN_EYES));
}

static void stream_vr_pass(void *context, int32_t pass)
{
	(void)context;
	xgpu_backend_vr_pass(pass);
}

static void stream_vr_groups_reset(void *context)
{
	(void)context;
	xgpu_backend_vr_groups_reset();
}

static void stream_vr_timing(void *context, int32_t mark)
{
	(void)context;
	xgpu_backend_vr_timing(mark);
}

static void frame_convert(const struct xgpu_stream_vr_frame *wire, struct xgpu_vr_frame *frame)
{
	memset(frame, 0, sizeof(*frame));
	frame->eyes_drawn = wire->eyes_drawn != 0;
	frame->hud_drawn = wire->hud_drawn != 0;
	frame->scope_drawn = wire->scope_drawn != 0;
	frame->hud_panels = wire->hud_panels != 0;
	frame->eye_target = wire->eye_target;
	frame->eye_depth = wire->eye_depth;
	frame->scope = wire->scope;
	frame->hud = wire->hud;
	frame->eye_depth_layers = wire->eye_depth_layers;
}

static void stream_vr_state(void *context, const struct xgpu_stream_vr_state *record)
{
	(void)context;
	vr_frame.state = *record;
	if (record->answered & XGPU_STREAM_VR_STATE_FRAME)
		vr_frame.frame = TRUE;
}

static void stream_vr_compose(void *context, const struct xgpu_stream_vr_frame *wire)
{
	struct xgpu_vr_frame frame;

	(void)context;
	frame_convert(wire, &frame);
	xgpu_backend_vr_compose(&frame);
}

static void stream_vr_submit(void *context, const struct xgpu_stream_vr_submit *record)
{
	struct xgpu_vr_frame frame;

	(void)context;
	frame_convert(&record->record.frame, &frame);
	vr_frame.submit = record;
	vr_frame.submitted = FALSE;
	screenshot_number = record->shot;
	/* (halo_vr_present, above, ends the frame) */
	xgpu_backend_vr_submit(&frame, record->record.screenshot != 0);
	vr_frame.submit = NULL;
	vr_frame.submitted = FALSE;
	vr_frame.frame = FALSE;
	vr_frame.foveation = 0;
	vr_frame.acquired = 0;
	vr_frame.depth_written = FALSE;
}

static void stream_vr_eye_transforms(void *context, const float transforms[2][16])
{
	(void)context;
	halo_vr_set_eye_transforms(transforms);
}

static void stream_vr_screen_corrections(void *context, const float corrections[2][4])
{
	(void)context;
	halo_vr_set_screen_corrections(corrections);
}

static void stream_vr_sky(void *context, int32_t sky)
{
	(void)context;
	halo_vr_sky(sky);
}

static void stream_vr_eye_feed(void *context, const float feed[7])
{
	(void)context;
	halo_vr_eye_feed(feed[0], feed[1], feed[2], feed[3], feed[4], feed[5], feed[6]);
}

static void stream_vr_eye_tint(void *context, const float tint[3])
{
	(void)context;
	halo_vr_eye_tint(tint[0], tint[1], tint[2]);
}

void host_gl_vr_backend(struct host_render_backend *backend)
{
	backend->vr_supported = stream_vr_supported;
	backend->vr_initialize = stream_vr_initialize;
	backend->vr_eyes = stream_vr_eyes;
	backend->vr_pass = stream_vr_pass;
	backend->vr_groups_reset = stream_vr_groups_reset;
	backend->vr_timing = stream_vr_timing;
	backend->vr_compose = stream_vr_compose;
	backend->vr_submit = stream_vr_submit;
	backend->vr_state = stream_vr_state;
	backend->vr_eye_transforms = stream_vr_eye_transforms;
	backend->vr_screen_corrections = stream_vr_screen_corrections;
	backend->vr_sky = stream_vr_sky;
	backend->vr_eye_feed = stream_vr_eye_feed;
	backend->vr_eye_tint = stream_vr_eye_tint;
	/* (OpenXR's calls that use the context from now on on the render
	thread: host_vr_render.h) */
	host_vr_render_thread();
}
