/*
VR_VISOR.H

The HUD on the helmet's visor (HALO_VR, vr.hud = "visor"; vr_visor.c): what
vr.c and the renderer (d3d8_device.c, port/linux/vulkan/host_vulkan_vr.c)
ask of it. The game's side is vr.h's halo_vr_visor_state.
*/

#ifndef __HALO_VR_VISOR_H
#define __HALO_VR_VISOR_H

#include "vr_host.h"

/* how the HUD's layer shows the game's HUD image (host_vulkan_vr.c draws it so) */
struct vr_visor_hud_image
{
	/* 0: as it is (vr.hud = "flat", the menus); 1: curved about the eyes, in
	a quad's image; 2: in a cylinder's */
	int curve;
	/* the HUD's half width and half height on the visor (radians) */
	float half_angles[2];
	/* the layer's half extents: a quad's in tangents across and up, a
	cylinder's in radians around and the tangent up */
	float extent[2];
	/* the HUD's opacity (its flicker as the shields break) and its glow (0
	none) */
	float opacity;
	float glow;
	/* the glass's look (vr.hud_glass, 0 none to 1): the HUD's light on the
	glass rather than over the world, the glass's tint in it and in its
	bloom, and its faint reflection */
	float glass;
	/* the world's light (vr.hud_react, 0 none to 1): the HUD's contrast
	with what is behind it, from a map of the world's light worked out from
	the eyes' images (host_vulkan_vr.c): how far it reaches from the middle of the
	head (tangents: left, right, up, down), how much of the frame's light it
	takes in (its smoothing over time: 1 the frame's alone), and each eye's
	transform from the head and field of view (as the rim's) */
	float react;
	float light_reach[4];
	float light_blend;
	float eye[2][3][4];
	float eye_fov[2][4];
	/* a hit on the shields: how much the HUD is disturbed (0 none to 1),
	and its pattern (a whole number, a new one every few frames) */
	float hit;
	float hit_pattern;
	/* the low shields' warning's pulse (0 to 1), in the HUD's bloom */
	float warning;
	/* the HUD's groups each in an image of their own, for the panels
	(vr.hud_depth); else all in the HUD's */
	int panels;
};

/* a panel of the HUD (vr.hud_depth): a group of its elements on a quad of
its own at its own depth, curved in its image as the HUD's layer is (from
the middle of the head, each pixel is a direction, whose angles across and
up are where it is in the HUD) */
struct vr_visor_panel
{
	int group; /* vr.h's HALO_VR_HUD_GROUP_* */
	/* the HUD's region it shows (0 to 1 across, 0 to 1 down) */
	float region[4];
	/* its plane at a unit's distance from the middle of the head, in the
	HUD's frame (x right, y up, -z ahead): its middle, its axes across and
	up, and its half extents along them */
	float centre[3], right[3], up[3];
	float half_size[2];
	/* metres from the middle of the head */
	float distance;
	/* along the visor's curve (vr.hud_wrap): its region turned across and
	up (radians), and bent, its angle up less by this times the square of
	its angle across (the angles from the HUD's middle; 0 for the world's
	panel) */
	float wrap[3];
};

/* the helmet's rim (vr.helmet_rim): the faceplate's lip about the visor,
drawn in each eye where it is, before it (host_vulkan_vr.c): its strength (0
none), how far before the middle of the head it is (metres:
vr.helmet_rim_depth), how far in from the edges of the eyes' views on that
plane its lip begins (a fraction of them: vr.helmet_rim_reach), the edges
(metres: left, right, up, down), and each eye's transform from the head
(rows: x, y, z, with the translation as w) and field of view (tangents:
left, right, up, down) */
struct vr_visor_rim
{
	float strength;
	float distance;
	float reach;
	float edges[4];
	float eye[2][3][4];
	float fov[2][4];
};

/* the visor's glass, VR_GLASS_DISTANCE before the middle of the head, just
behind the rim's lip: the edges of the view darkened, its tint, the
warning's glow on its rim, and the shields' energy across it (host_vulkan_vr.c
draws it into each eye's image, as the rim) */
#define VR_GLASS_DISTANCE 0.105f
struct vr_visor_image
{
	int shown;
	/* how far it reaches, what the eyes see of the glass along their
	views' edges (their middles: where its shade is), and all that they see
	of it (every corner of their views: past it, out to its reach, the glass
	fades to nothing), from the middle of the head (tangents at the glass's
	distance: left, right, up, down) */
	float extent[4];
	float fov[4];
	float clear[4];
	/* each eye's transform from the head and its field of view, as the
	rim's */
	float eye[2][3][4];
	float eye_fov[2][4];
	/* the frame about the visor (vr.visor_frame, 0 to 1) */
	float frame;
	/* the warning's glow on the rim (linear, premultiplied: added) */
	float glow[3];
	/* the shields' energy: the game's plasma (vr_visor_state's look), as
	bright as the armour's (glow), and as the shields recharge, filling the
	glass from below to their level (charge, 0 to 1; charge_level, 0 to 1),
	and its flash as they are full (0 to 1) */
	int plasma;
	float plasma_glow;
	/* how strong the energy is (vr.visor_energy) */
	float energy;
	float charge, charge_level, full;
	float perpendicular[3], parallel[3];
	float noise_scale[2], noise_period[2], noise_direction[2][3];
	void *noise_texture[2];
	float time;
};

/* vr.hud = "visor" */
int vr_visor_enabled(void);
/* the panels' images' sizes (vr_host_info's, before the session) */
void vr_visor_panel_sizes(int width[VR_PANEL_COUNT], int height[VR_PANEL_COUNT]);
/* the runtime's: whether the HUD can be a cylinder layer */
void vr_visor_initialize(const struct vr_host_info *info);
/* once a frame, the head, the eyes and their fields of view located for
the frame's draws: the HUD's orientation follows the head (vr.hud_lag); the
frame's seconds, and the HUD's distance (metres) and width (degrees) */
void vr_visor_follow(const struct vr_host_pose *head, const struct vr_host_pose eyes[2], const float fov[2][4],
	float seconds, float distance, float size);
/* the HUD's orientation (play space; the head's without a lag), and
whether it follows the head (not locked to it) */
int vr_visor_orientation(float orientation[4]);
/* the HUD's layers for a frame of the game (not the menus), before the
head followed (the visor's glass is no layer: it is drawn into the eyes'
images) */
void vr_visor_layers(struct vr_host_layers *layers);
/* the menus are up (halo_vr_menus): their layer is as it is, and the
visor is not there */
void vr_visor_menus(int active);
/* what the renderer draws for the frame */
void vr_visor_hud_image(struct vr_visor_hud_image *image);
/* the HUD's panels (VR_PANEL_COUNT; the n'th shows the group n + 1);
0 if there are none (vr.hud_depth 0, the HUD not on the visor, the menus) */
int vr_visor_panels(struct vr_visor_panel panels[VR_PANEL_COUNT]);
void vr_visor_image(struct vr_visor_image *image);
/* 0 if there is no rim to draw */
int vr_visor_rim(struct vr_visor_rim *rim);
/* debug.screenshot_every: the frame as the headset shows it, the layers
over each eye's image (BGRA, rows from the bottom), from the layers as
submitted; the HUD's and the scope's images as BGRA, rows from the
bottom */
void vr_visor_composite(unsigned char *eyes[2], int width, int height, const struct vr_host_layers *layers,
	const struct vr_host_pose *head, const unsigned char *hud, int hud_width, int hud_height,
	unsigned char *const panels[VR_PANEL_COUNT],
	const int panel_width[VR_PANEL_COUNT], const int panel_height[VR_PANEL_COUNT], const unsigned char *scope,
	int scope_size);

/* vr.c's: the layers of the frame presented last, and the head they were
placed before; 0 if there is none */
int halo_vr_submitted(struct vr_host_layers *layers, struct vr_host_pose *head);

#endif
