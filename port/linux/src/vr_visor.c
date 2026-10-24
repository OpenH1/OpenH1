/*
VR_VISOR.C

The HUD on the helmet's visor (HALO_VR, vr.hud = "visor"; vr_visor.h,
port/linux/README.md "VR"): the HUD as Master Chief's helmet projects it,
not a screen floating before the face.

The HUD is curved about the eyes, each of its elements as large and as
square at the edges as in the middle, all of it at one distance: the game's
640x480 HUD is laid out in angles, its width vr.hud_size degrees around and
its height in proportion, so that the shields' meter, the motion tracker,
the ammunition and the messages are where the game puts them, toward the
edges of the view, but no farther out than the eyes turn comfortably. On a
cylinder layer about the head where the runtime has them
(XR_KHR_composition_layer_cylinder), else on a quad whose image is drawn
curved (host_vulkan_vr.c): from the eyes the two look the same, the cylinder's
edges merely as near as its middle. The HUD's camera projects through the
same curve (vr_render.c, vr_render_hud_to_screen), so that the waypoints are
where they belong.

The HUD's orientation follows the head's a little behind it (vr.hud_lag):
smoothed, never more than FOLLOW_LIMIT behind, and never past it (no
spring, no overshoot), so that it is still while the head is. The visor's
rim does not lag: it is the helmet, and turns with the head exactly.

The HUD's elements have depths of their own (vr.hud_depth), as a helmet's
projection would give them, not one surface: each group of them (vr.h's
HALO_VR_HUD_GROUP_*, by where the game anchors them: hud_draw.c, hud.c) is
drawn into an image of its own (host_vulkan_vr.c) and shown on a panel of its own,
a quad layer at its own distance, curved in its image as the HUD is, so
that each element is where it was in the HUD seen from the middle of the
head, only nearer or farther: the motion tracker nearest, tilted back as a
console is, the shields and the weapon's panels before the HUD's middle,
the waypoints and the players' names beyond it, toward the world they mark.
The corners' panels follow the visor's curve (vr.hud_wrap): turned out
toward the edges of the view, where a glance finds them, and bent as the
visor's brow and chin are (the shields' meter falls away toward its outer
end), each element as large as ever.

It looks to be on the glass (vr.hud_glass): light on it, the world seen
through it, the glass's gold in it and its bloom, its faint reflection in
the glass's other surface; and it reacts to the world as a helmet's display
would (vr.hud_react): more contrast before bright light, dimmer in the
dark, and a moment's disturbance as the shields are hit (host_vulkan_vr.c draws
it so, from the eyes' images' light). But its layers stay where they are,
at 1.45 to 2.5 m, as a real visor's display is collimated beyond the glass:
the glass itself is too near for the eyes to cross on it or focus there.
Every panel is placed with the HUD's layer, a moment behind the head. Quad
layers, which every runtime has (SteamVR on the Steam Frame has no
cylinders): the compositor gives each its stereo depth, and the HUD's
pixels are not drawn into the eyes' images, whose depth is the world's.

The helmet (vr.helmet_rim): the faceplate's frame about the visor, a mesh
drawn into each eye's image (host_vulkan_vr.c, rim), before the eyes as a
helmet is, where only the edges of each eye's view see it: the left eye its
left side, the right eye its right, both its brow and its chin. It does not
lag: it is the helmet, and turns with the head exactly.

The visor's glass (vr.visor_frame, just within the rim): the edges of the
view darkened, and the glass a faint gold (no line where the visor ends: a
line fixed in the view reads as a smudge on the lenses, or a scratch across
it). It is drawn into each eye's image, as the rim is (host_vulkan_vr.c,
visor), each pixel where that eye's ray meets the glass's plane: no
layer of its own. As a quad layer, head-locked a hand's breadth before the
eyes and reaching past both their views (out to 80 degrees), it was the
compositor's to draw, and on the Steam Frame each eye saw a straight line
across it, a diagonal, in a different place in each eye (the two eyes cannot
fuse it): SteamVR draws so near and so wide a quad wrongly (its two
triangles meeting, or the quad clipped at its near plane). Past every
corner of both eyes' views, as the runtime gives them, it fades to nothing
(visor_extent), in case a view is wider than the runtime says. And the
shields on it
(vr.visor_effects), from the player's unit as the game has it
(halo_vr_visor_state): their energy across the glass, the game's own (the
unit's modifier shader, the plasma the armour glows with as the shields are
hit: its colours, its noise maps and their motion), as bright as the
armour's, and as they recharge, rising across the glass to their level,
flashing as they are full; the glass's rim pulses red while they are low or
gone, and the HUD flickers as they break and comes up as the player takes
control. More at the glass's edges than its middle, so that nothing is
hidden. Other effects on the glass (water, dirt, cracks, reflections) would
be drawn with it (host_vulkan_vr.c, visor).
*/

#ifdef HALO_VR
#include "platform.h"
#include "port_config.h"
#include "vr.h"
#include "vr_host.h"
#include "vr_visor.h"

#include <math.h>
#include <string.h>

/* the HUD's turn behind the head's (vr.hud_lag), at most (radians) */
#define FOLLOW_LIMIT (1.5f * (float)M_PI / 180.0f)
/* the visor's image: past all that the eyes see of the glass by a band in
which it fades out, this much of the tangent it is from the head's forward
and this much more (tangents at the glass's distance) */
#define VISOR_FADE 0.12f
#define VISOR_FADE_BASE 0.10f
/* the HUD's image's pixels across (vr.c's), for the panels' */
#define HUD_PIXELS_ACROSS 1280.0f
/* the panels' distances: never nearer (metres) */
#define PANEL_NEAREST 1.0f
/* the HUD's height for its width (the 640x480 screen) */
#define HUD_ASPECT 0.75f
/* the shields' effects: the flash as they are full again, and the body's
(red) as it is hurt, each fading over this time constant (seconds); the low shields' warning pulse (a second); the flicker
as they break, and the HUD coming up as the player takes control (seconds) */
#define FULL_FADE 0.35f
#define WOUND_FADE 0.30f
#define WARNING_PULSE 1.6f
#define WARNING_EASE 0.30f
#define CHARGE_EASE 0.25f
#define FLICKER_SECONDS 0.45f
#define BOOT_SECONDS 0.40f
#define PRESENCE_EASE 0.25f
/* the HUD's disturbance as the shields are hit (vr.hud_react): fading over
this time constant (seconds), its pattern a new one this often (seconds:
slower than the frames, not a strobe) */
#define HIT_FADE 0.12f
#define HIT_PATTERN_SECONDS 0.045f
/* the world's light behind the HUD: smoothed over this time constant
(seconds), as eyes adapt and as a helmet's display would follow it, so
that a glance across a bright window does not flash it; its map reaches
this far from the middle of the head (tangents across and up) */
#define LIGHT_EASE 0.35f
#define LIGHT_REACH_ACROSS 1.1f
#define LIGHT_REACH_UP 0.9f
/* the shields are gone at this (the game's 0, or a frame into recharging) */
#define SHIELD_GONE 0.03f

static struct
{
	unsigned long read_at;
	int visor;
	float lag;
	float frame;
	int effects;
	float glow;
	float glass;
	float react;
	float depth;
	/* each panel's distance from the HUD's at vr.hud_depth 1 (metres), the
	tracker's tilt (radians) */
	float panel_offset[VR_PANEL_COUNT];
	float tracker_tilt;
	float wrap;
	float rim, rim_depth, rim_reach;
	float energy;
} settings = { (unsigned long)-1 };

/* the HUD's panels: the group each shows, the HUD's region it is in (0 to
1, across and down), and the setting of its distance from the HUD's (at
vr.hud_depth 1, metres: before it less than 0). The regions are the anchors'
corners of the game's 640x480 HUD, where hud_draw.c places their elements,
with room to spare. The tracker's panel tilts back (vr.hud_tracker_tilt). */
static const struct
{
	int group;
	float region[4];
	const char *offset;
} panel_layout[VR_PANEL_COUNT] =
{
	{ HALO_VR_HUD_GROUP_WORLD, { 0.0f, 0.0f, 1.0f, 1.0f }, "vr.hud_depth_world" },
	{ HALO_VR_HUD_GROUP_WEAPON, { 0.0f, 0.0f, 0.5f, 0.45f }, "vr.hud_depth_weapon" },
	{ HALO_VR_HUD_GROUP_STATUS, { 0.5f, 0.0f, 1.0f, 0.45f }, "vr.hud_depth_status" },
	{ HALO_VR_HUD_GROUP_TRACKER, { 0.0f, 0.5f, 0.45f, 1.0f }, "vr.hud_depth_tracker" },
};
/* the tracker's panel's, the one tilted */
#define TRACKER_PANEL 3
/* the most a panel tilts (radians), for its image's size */
#define TILT_MOST 0.8f
/* the corners' panels along the visor's curve (vr.hud_wrap, at 1): turned
outward from the HUD's middle by these angles across and up or down
(radians), and bent as the visor's brow and chin are, falling away (or
rising, below) toward the sides by this much of the square of their angle
across (radians for a radian squared); the most of vr.hud_wrap */
#define WRAP_ACROSS (7.0f * (float)M_PI / 180.0f)
#define WRAP_UP (5.0f * (float)M_PI / 180.0f)
#define WRAP_BEND 0.25f
#define WRAP_MOST 2.0f

static struct
{
	int cylinder;
	int logged;
	int rim_logged;
	int extent_logged;
	int menus;
	/* the frame's head and eyes' fields of view, its seconds, and the HUD's
	distance and half angles (radians) */
	struct vr_host_pose head;
	float fov[2][4];
	/* the eyes from the middle of the head (in its frame), when known */
	float eye_position[2][3];
	float eye_orientation[2][4];
	int eyes_known;
	float seconds;
	float distance;
	float half_angles[2];
	/* the HUD's orientation, following the head's */
	int orientation_valid;
	float orientation[4];
	/* the player's unit, as last told */
	int active;
	int known;
	int has_shield;
	float shield, body;
	/* the effects: the visor there (0 to 1, eased in and out), the flares,
	the warning and the recharge (0 to 1), the warning's pulse (seconds),
	the flicker's and the HUD's coming up (seconds from their start) */
	float presence;
	float full, wound;
	float warning, charge;
	float pulse;
	float flicker;
	float boot;
	/* the HUD's disturbance as the shields are hit (0 to 1), and its
	seconds since the last hit */
	float hit;
	float hit_time;
	/* the shields' look on the armour (halo_vr_visor_state) */
	struct halo_vr_visor_state look;
} visor;

static float clamp(float value, float low, float high)
{
	return value < low ? low : value > high ? high : value;
}

static void settings_read(void)
{
	if (settings.read_at == config_changes())
		return;
	settings.read_at = config_changes();
	settings.visor = strcmp(config_string("vr.hud"), "flat") != 0;
	settings.lag = clamp((float)config_real("vr.hud_lag"), 0.0f, 0.2f);
	settings.frame = clamp((float)config_real("vr.visor_frame"), 0.0f, 1.0f);
	settings.effects = config_boolean("vr.visor_effects");
	settings.glow = clamp((float)config_real("vr.hud_glow"), 0.0f, 1.0f);
	settings.glass = clamp((float)config_real("vr.hud_glass"), 0.0f, 1.0f);
	settings.react = clamp((float)config_real("vr.hud_react"), 0.0f, 1.0f);
	settings.depth = clamp((float)config_real("vr.hud_depth"), 0.0f, 2.0f);
	{
		int index;

		for (index = 0; index < VR_PANEL_COUNT; index++)
			settings.panel_offset[index] = clamp((float)config_real(panel_layout[index].offset), -1.0f, 2.0f);
	}
	settings.tracker_tilt = clamp((float)config_real("vr.hud_tracker_tilt"), -45.0f, 45.0f) * (float)M_PI / 180.0f;
	settings.wrap = clamp((float)config_real("vr.hud_wrap"), 0.0f, WRAP_MOST);
	settings.rim = clamp((float)config_real("vr.helmet_rim"), 0.0f, 1.0f);
	settings.rim_depth = clamp((float)config_real("vr.helmet_rim_depth"), 0.05f, 0.25f);
	settings.rim_reach = clamp((float)config_real("vr.helmet_rim_reach"), 0.0f, 0.2f);
	settings.energy = clamp((float)config_real("vr.visor_energy"), 0.0f, 2.0f);
}

int vr_visor_enabled(void)
{
	settings_read();
	return settings.visor;
}

void vr_visor_initialize(const struct vr_host_info *info)
{
	settings_read();
	visor.cylinder = info->cylinder;
	/* (no flicker, the HUD up, until the game says otherwise) */
	visor.flicker = FLICKER_SECONDS;
	visor.boot = BOOT_SECONDS;
	visor.distance = 2.0f;
	visor.half_angles[0] = 0.5f;
	visor.half_angles[1] = 0.5f * HUD_ASPECT;
	if (!settings.visor)
		platform_log("vr: the HUD on a flat quad (vr.hud = \"flat\")");
	else if (visor.cylinder)
		platform_log("vr: the HUD on the visor: a cylinder layer (XR_KHR_composition_layer_cylinder), the glass "
			"in the eyes' images");
	else
		platform_log("vr: the HUD on the visor: a quad layer, curved in its image (no "
			"XR_KHR_composition_layer_cylinder), the glass in the eyes' images");
}

/* ---------- maths: OpenXR's axes (x right, y up, z backwards) */

static void quaternion_rotate(const float q[4], const float v[3], float out[3])
{
	/* v + 2w(q x v) + 2 q x (q x v) */
	float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
	float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
	float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);

	out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
	out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
	out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

static void quaternion_multiply(const float a[4], const float b[4], float out[4])
{
	float r[4];

	r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
	memcpy(out, r, sizeof(r));
}

static float quaternion_dot(const float a[4], const float b[4])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

/* the angle (radians) from one orientation to the other */
static float quaternion_angle(const float a[4], const float b[4])
{
	return 2.0f * acosf(clamp(fabsf(quaternion_dot(a, b)), 0.0f, 1.0f));
}

/* q turned toward target by a fraction t of the way (the angles are small:
along the chord, normalized) */
static void quaternion_towards(float q[4], const float target[4], float t)
{
	float sign = quaternion_dot(q, target) < 0.0f ? -1.0f : 1.0f;
	float length = 0.0f;
	int i;

	for (i = 0; i < 4; i++)
	{
		q[i] += (sign * target[i] - q[i]) * t;
		length += q[i] * q[i];
	}
	length = sqrtf(length);
	for (i = 0; i < 4; i++)
		q[i] /= length;
}

/* ---------- the HUD following the head */

void vr_visor_follow(const struct vr_host_pose *head, const struct vr_host_pose eyes[2], const float fov[2][4],
	float seconds, float distance, float size)
{
	settings_read();
	visor.head = *head;
	memcpy(visor.fov, fov, sizeof(visor.fov));
	visor.eyes_known = head->valid && eyes[0].valid && eyes[1].valid;
	if (visor.eyes_known)
	{
		float inverse[4];
		int eye, axis;

		inverse[0] = -head->orientation[0];
		inverse[1] = -head->orientation[1];
		inverse[2] = -head->orientation[2];
		inverse[3] = head->orientation[3];
		/* the eyes in the head's frame */
		for (eye = 0; eye < 2; eye++)
		{
			float offset[3];

			for (axis = 0; axis < 3; axis++)
				offset[axis] = eyes[eye].position[axis] - head->position[axis];
			quaternion_rotate(inverse, offset, visor.eye_position[eye]);
			quaternion_multiply(inverse, eyes[eye].orientation, visor.eye_orientation[eye]);
		}
	}
	visor.seconds = clamp(seconds, 0.0f, 0.1f);
	visor.distance = distance;
	/* (a quad's tangents grow without bound toward 90 degrees) */
	visor.half_angles[0] = clamp(size, 20.0f, 120.0f) * 0.5f * (float)M_PI / 180.0f;
	visor.half_angles[1] = visor.half_angles[0] * HUD_ASPECT;
	if (!head->valid)
		return;
	if (!visor.orientation_valid || settings.lag <= 0.0f || visor.seconds <= 0.0f)
	{
		memcpy(visor.orientation, head->orientation, sizeof(visor.orientation));
		visor.orientation_valid = 1;
		return;
	}
	{
		float angle;

		/* smoothed, as a first-order lag (no overshoot), and held within
		FOLLOW_LIMIT of the head */
		quaternion_towards(visor.orientation, head->orientation, 1.0f - expf(-visor.seconds / settings.lag));
		angle = quaternion_angle(visor.orientation, head->orientation);
		if (angle > FOLLOW_LIMIT)
			quaternion_towards(visor.orientation, head->orientation, 1.0f - FOLLOW_LIMIT / angle);
	}
}

int vr_visor_orientation(float orientation[4])
{
	if (!visor.orientation_valid || !visor.head.valid)
	{
		orientation[0] = orientation[1] = orientation[2] = 0.0f;
		orientation[3] = 1.0f;
		return 0;
	}
	memcpy(orientation, visor.orientation, sizeof(visor.orientation));
	return settings.lag > 0.0f;
}

/* how far a plane before the head reaches, for what the eyes see of it: a
tangent from the middle of the head (about 80 degrees: a plane's tangents
grow without bound toward 90) */
#define PLANE_TANGENT_LIMIT 6.0f

/* where an eye's direction (tangents in its view: x right, y up) meets a
plane before the head (distance metres along its -z), in tangents from the
middle of the head; a direction along the plane or away from it, or past
PLANE_TANGENT_LIMIT, as far as the plane goes its way, and 0 returned */
static int plane_point(int eye, float x, float y, float distance, float point[2])
{
	float origin[3] = { eye ? 0.032f : -0.032f, 0.0f, 0.0f }, local[3], direction[3], t, across;

	local[0] = x;
	local[1] = y;
	local[2] = -1.0f;
	/* (unknown, the eyes beside the middle of the head, as they nearly
	are) */
	memcpy(direction, local, sizeof(direction));
	if (visor.eyes_known)
	{
		memcpy(origin, visor.eye_position[eye], sizeof(origin));
		quaternion_rotate(visor.eye_orientation[eye], local, direction);
	}
	t = direction[2] < -1e-6f ? (-distance - origin[2]) / direction[2] : -1.0f;
	if (t > 0.0f)
	{
		point[0] = (origin[0] + direction[0] * t) / distance;
		point[1] = (origin[1] + direction[1] * t) / distance;
		if (fabsf(point[0]) < PLANE_TANGENT_LIMIT && fabsf(point[1]) < PLANE_TANGENT_LIMIT)
			return 1;
	}
	across = fabsf(direction[0]) > fabsf(direction[1]) ? fabsf(direction[0]) : fabsf(direction[1]);
	point[0] = across > 0.0f ? direction[0] / across * PLANE_TANGENT_LIMIT : 0.0f;
	point[1] = across > 0.0f ? direction[1] / across * PLANE_TANGENT_LIMIT : 0.0f;
	return 0;
}

/* what the eyes see of a plane before the head (distance metres along its
-z): the edges of their views on it together (metres in the head's frame:
left, right, up, down), along the middles of the edges, where the views
reach. Near the eyes, each side is one eye's: the left eye's left edge, the
right eye's right. */
static void footprint(float distance, float edges[4])
{
	int eye, edge;

	edges[0] = edges[1] = edges[2] = edges[3] = 0.0f;
	for (eye = 0; eye < 2; eye++)
	{
		for (edge = 0; edge < 4; edge++)
		{
			float point[2];

			plane_point(eye, edge < 2 ? visor.fov[eye][edge] : 0.0f, edge >= 2 ? visor.fov[eye][edge] : 0.0f,
				distance, point);
			if (edge == 0 && point[0] * distance < edges[0])
				edges[0] = point[0] * distance;
			if (edge == 1 && point[0] * distance > edges[1])
				edges[1] = point[0] * distance;
			if (edge == 2 && point[1] * distance > edges[2])
				edges[2] = point[1] * distance;
			if (edge == 3 && point[1] * distance < edges[3])
				edges[3] = point[1] * distance;
		}
	}
}

/* all that the eyes see on a plane before the head (distance metres along
its -z), whatever their views' shapes and turns: the bounds of where the
corners of each eye's view meet it (the views are pyramids, so what each
sees of a plane is the polygon of its corners), in tangents from the middle
of the head (left, right, up, down); 0 if a corner is past where the plane
reaches (plane_point) */
static int view_bounds(float distance, float bounds[4])
{
	static const int corners[4][2] = { { 0, 2 }, { 1, 2 }, { 0, 3 }, { 1, 3 } };
	int eye, corner, whole = 1;

	bounds[0] = bounds[1] = bounds[2] = bounds[3] = 0.0f;
	for (eye = 0; eye < 2; eye++)
	{
		for (corner = 0; corner < 4; corner++)
		{
			float point[2];

			if (!plane_point(eye, visor.fov[eye][corners[corner][0]], visor.fov[eye][corners[corner][1]], distance,
				point))
				whole = 0;
			if (point[0] < bounds[0])
				bounds[0] = point[0];
			if (point[0] > bounds[1])
				bounds[1] = point[0];
			if (point[1] > bounds[2])
				bounds[2] = point[1];
			if (point[1] < bounds[3])
				bounds[3] = point[1];
		}
	}
	return whole;
}

/* what the eyes see of the glass, how far it reaches about it and where
it fades out (tangents at the glass's distance from the middle of the head:
left, right, up, down). It must never end in an edge, at any setting, on
any headset: it reaches past all that the eyes see (view_bounds, every
corner of both eyes' views) by a band in which it fades to nothing, so that
even a view a little wider than the runtime says (or one past where the
plane can reach: then the band is within the view, and the glass fades out
softly) shows no edge. */
static void visor_extent(float fov[4], float extent[4], float clear[4])
{
	int edge, whole;

	footprint(VR_GLASS_DISTANCE, fov);
	for (edge = 0; edge < 4; edge++)
		fov[edge] /= VR_GLASS_DISTANCE;
	whole = view_bounds(VR_GLASS_DISTANCE, clear);
	for (edge = 0; edge < 4; edge++)
	{
		float side = edge == 0 || edge == 3 ? -1.0f : 1.0f;
		float seen = fabsf(clear[edge]);
		float band = VISOR_FADE_BASE + VISOR_FADE * seen;

		if (seen + band > PLANE_TANGENT_LIMIT)
		{
			/* (the band within the plane's reach, the fade before its edge) */
			seen = PLANE_TANGENT_LIMIT - band;
			whole = 0;
		}
		clear[edge] = side * seen;
		extent[edge] = side * (seen + band);
	}
	if (!whole && !visor.extent_logged)
	{
		platform_log("vr: visor: the eyes' views reach past %.0f degrees from the head's forward; the glass fades out "
			"before its edges within them", atanf(PLANE_TANGENT_LIMIT) * 180.0f / (float)M_PI);
		visor.extent_logged = 1;
	}
}

/* ---------- the HUD's panels */

/* the direction (x right, y up, -z ahead) at angles across and up */
static void angles_direction(float across, float up, float out[3])
{
	out[0] = sinf(across) * cosf(up);
	out[1] = sinf(up);
	out[2] = -cosf(across) * cosf(up);
}

static void cross(const float a[3], const float b[3], float out[3])
{
	float r[3];

	r[0] = a[1] * b[2] - a[2] * b[1];
	r[1] = a[2] * b[0] - a[0] * b[2];
	r[2] = a[0] * b[1] - a[1] * b[0];
	memcpy(out, r, sizeof(r));
}

static float dot(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void normalize(float v[3])
{
	float length = sqrtf(dot(v, v));
	int axis;

	if (length > 0.0f)
	{
		for (axis = 0; axis < 3; axis++)
			v[axis] /= length;
	}
}

/* how a panel follows the visor's curve (vr.hud_wrap): its turn across and
up from where the game has it, and its bend (vr_visor_panel's wrap). The
waypoints' and the names' panel is the world's, and stays on what it
marks; the others are the corners', each turned toward its own. */
static void panel_wrap(int index, float wrap, float out[3])
{
	const float *region = panel_layout[index].region;
	float across = region[0] + region[2] < 1.0f ? -1.0f : 1.0f, up = region[1] + region[3] < 1.0f ? 1.0f : -1.0f;

	out[0] = out[1] = out[2] = 0.0f;
	if (panel_layout[index].group == HALO_VR_HUD_GROUP_WORLD)
		return;
	out[0] = across * wrap * WRAP_ACROSS;
	out[1] = up * wrap * WRAP_UP;
	out[2] = up * wrap * WRAP_BEND;
}

/* the direction of a point of the HUD (its angles across and up) on a
panel that follows the visor's curve (panel_wrap): turned, then bent, its
angle up less (or more, below) by the bend times the square of its angle
across, as the visor's brow falls away toward the sides; the copy (host_vulkan_vr.c)
undoes it */
static void wrapped_direction(float across, float up, const float wrap[3], float out[3])
{
	angles_direction(across + wrap[0], up + wrap[1] - wrap[2] * across * across, out);
}

/* a panel's plane, for the HUD's half angles (radians), its distance
(metres), vr.hud_depth, its tilt (radians) and vr.hud_wrap */
static void panel_geometry(int index, const float half_angles[2], float distance, float depth, float tilt,
	float wrap, struct vr_visor_panel *panel)
{
	static const float world_up[3] = { 0.0f, 1.0f, 0.0f };
	const float *region = panel_layout[index].region;
	float across, up, normal[3], half[2] = { 0.0f, 0.0f };
	int axis, edge, step;

	memset(panel, 0, sizeof(*panel));
	panel->group = panel_layout[index].group;
	memcpy(panel->region, region, sizeof(panel->region));
	panel_wrap(index, wrap, panel->wrap);
	/* its middle, facing the head, tilted back (its top farther) */
	across = (region[0] + region[2] - 1.0f) * half_angles[0];
	up = (1.0f - region[1] - region[3]) * half_angles[1];
	wrapped_direction(across, up, panel->wrap, panel->centre);
	cross(panel->centre, world_up, panel->right);
	normalize(panel->right);
	cross(panel->right, panel->centre, panel->up);
	normalize(panel->up);
	for (axis = 0; axis < 3; axis++)
		panel->up[axis] = panel->up[axis] * cosf(tilt) + panel->centre[axis] * sinf(tilt);
	cross(panel->right, panel->up, normal);
	/* as large as the region's edges' directions meet it */
	for (edge = 0; edge < 4; edge++)
	{
		for (step = 0; step <= 16; step++)
		{
			float f = (float)step / 16.0f, u, v, direction[3], point[3], t, denominator;

			u = edge == 0 ? region[0] : edge == 1 ? region[2] : region[0] + (region[2] - region[0]) * f;
			v = edge == 2 ? region[1] : edge == 3 ? region[3] : region[1] + (region[3] - region[1]) * f;
			wrapped_direction((2.0f * u - 1.0f) * half_angles[0], (1.0f - 2.0f * v) * half_angles[1], panel->wrap,
				direction);
			denominator = dot(direction, normal);
			if (fabsf(denominator) < 1e-4f)
				continue;
			t = dot(panel->centre, normal) / denominator;
			for (axis = 0; axis < 3; axis++)
				point[axis] = direction[axis] * t - panel->centre[axis];
			if (fabsf(dot(point, panel->right)) > half[0])
				half[0] = fabsf(dot(point, panel->right));
			if (fabsf(dot(point, panel->up)) > half[1])
				half[1] = fabsf(dot(point, panel->up));
		}
	}
	panel->half_size[0] = half[0];
	panel->half_size[1] = half[1];
	panel->distance = distance + settings.panel_offset[index] * depth;
	if (panel->distance < PANEL_NEAREST)
		panel->distance = PANEL_NEAREST;
}

void vr_visor_panel_sizes(int width[VR_PANEL_COUNT], int height[VR_PANEL_COUNT])
{
	float half_angles[2], density;
	int index;

	settings_read();
	half_angles[0] = clamp((float)config_real("vr.hud_size"), 20.0f, 120.0f) * 0.5f * (float)M_PI / 180.0f;
	half_angles[1] = half_angles[0] * HUD_ASPECT;
	/* as many pixels for an angle as the HUD's */
	density = HUD_PIXELS_ACROSS / (2.0f * half_angles[0]);
	for (index = 0; index < VR_PANEL_COUNT; index++)
	{
		struct vr_visor_panel panel;

		width[index] = height[index] = 0;
		if (!settings.visor)
			continue;
		/* (the most the tilt, the region's curve and its bend may take) */
		panel_geometry(index, half_angles, 2.0f, 1.0f, index == TRACKER_PANEL ? TILT_MOST : 0.0f, WRAP_MOST, &panel);
		width[index] = ((int)(2.0f * panel.half_size[0] * density) + 15) & ~15;
		height[index] = ((int)(2.0f * panel.half_size[1] * density) + 15) & ~15;
		if (width[index] > 2048)
			width[index] = 2048;
		if (height[index] > 2048)
			height[index] = 2048;
	}
}

int vr_visor_panels(struct vr_visor_panel panels[VR_PANEL_COUNT])
{
	int index;

	settings_read();
	if (!settings.visor || visor.menus || settings.depth <= 0.0f)
		return 0;
	for (index = 0; index < VR_PANEL_COUNT; index++)
		panel_geometry(index, visor.half_angles, visor.distance, settings.depth,
			index == TRACKER_PANEL ? settings.tracker_tilt : 0.0f, settings.wrap, &panels[index]);
	return 1;
}

/* the rotation whose axes are these (orthonormal) */
static void quaternion_from_axes(const float x[3], const float y[3], const float z[3], float q[4])
{
	float trace = x[0] + y[1] + z[2], s;

	if (trace > 0.0f)
	{
		s = 0.5f / sqrtf(trace + 1.0f);
		q[3] = 0.25f / s;
		q[0] = (y[2] - z[1]) * s;
		q[1] = (z[0] - x[2]) * s;
		q[2] = (x[1] - y[0]) * s;
	}
	else if (x[0] > y[1] && x[0] > z[2])
	{
		s = 2.0f * sqrtf(1.0f + x[0] - y[1] - z[2]);
		q[3] = (y[2] - z[1]) / s;
		q[0] = 0.25f * s;
		q[1] = (y[0] + x[1]) / s;
		q[2] = (z[0] + x[2]) / s;
	}
	else if (y[1] > z[2])
	{
		s = 2.0f * sqrtf(1.0f + y[1] - x[0] - z[2]);
		q[3] = (z[0] - x[2]) / s;
		q[0] = (y[0] + x[1]) / s;
		q[1] = 0.25f * s;
		q[2] = (z[1] + y[2]) / s;
	}
	else
	{
		s = 2.0f * sqrtf(1.0f + z[2] - x[0] - y[1]);
		q[3] = (x[1] - y[0]) / s;
		q[0] = (z[0] + x[2]) / s;
		q[1] = (z[1] + y[2]) / s;
		q[2] = 0.25f * s;
	}
}

void vr_visor_layers(struct vr_host_layers *layers)
{
	static const float forward[3] = { 0.0f, 0.0f, -1.0f };
	float d = visor.distance, tangent_up = tanf(visor.half_angles[1]);
	int follows = settings.lag > 0.0f && visor.orientation_valid && visor.head.valid;
	int axis;
	float base[3];

	if (!visor.logged)
	{
		platform_log("vr: the HUD's layer: %s, %.2f m, %.0f by %.0f degrees, %s", visor.cylinder ? "a cylinder" :
			"a quad", d, visor.half_angles[0] * 2.0f * 180.0f / (float)M_PI,
			visor.half_angles[1] * 2.0f * 180.0f / (float)M_PI, follows ? "following the head" : "on the head");
		visor.logged = 1;
	}
	/* before the head, turned as it was a moment ago (vr.hud_lag), else on
	it (the view space: exactly with it, whatever the frame's prediction) */
	layers->hud_head_locked = !follows;
	memset(&layers->hud_pose, 0, sizeof(layers->hud_pose));
	layers->hud_pose.orientation[3] = 1.0f;
	if (follows)
	{
		memcpy(layers->hud_pose.orientation, visor.orientation, sizeof(visor.orientation));
		memcpy(layers->hud_pose.position, visor.head.position, sizeof(visor.head.position));
	}
	/* (the middle of the head: the panels' origin) */
	memcpy(base, layers->hud_pose.position, sizeof(base));
	if (visor.cylinder)
	{
		/* about the middle of the head */
		layers->hud_cylinder = 1;
		layers->hud_radius = d;
		layers->hud_angle = 2.0f * visor.half_angles[0];
		layers->hud_size[0] = layers->hud_angle * d;
		layers->hud_size[1] = 2.0f * d * tangent_up;
	}
	else
	{
		/* tall enough for the curve's corners, which are higher on the quad
		than its middle's top (host_vulkan_vr.c, copy_hud_image) */
		float offset[3];

		quaternion_rotate(layers->hud_pose.orientation, forward, offset);
		for (axis = 0; axis < 3; axis++)
			layers->hud_pose.position[axis] += offset[axis] * d;
		layers->hud_size[0] = 2.0f * d * tanf(visor.half_angles[0]);
		layers->hud_size[1] = 2.0f * d * tangent_up / cosf(visor.half_angles[0]);
	}
	/* the HUD's panels, placed as its layer is (vr.c marks those drawn) */
	{
		struct vr_visor_panel panels[VR_PANEL_COUNT];
		int index, any = vr_visor_panels(panels);

		for (index = 0; index < VR_PANEL_COUNT; index++)
		{
			const struct vr_visor_panel *panel = &panels[index];
			float offset[3], back[3], orientation[4];

			if (!any || !layers->panels[index].shown)
			{
				memset(&layers->panels[index], 0, sizeof(layers->panels[index]));
				continue;
			}
			for (axis = 0; axis < 3; axis++)
				offset[axis] = panel->centre[axis] * panel->distance;
			quaternion_rotate(layers->hud_pose.orientation, offset, offset);
			for (axis = 0; axis < 3; axis++)
				layers->panels[index].pose.position[axis] = base[axis] + offset[axis];
			cross(panel->right, panel->up, back);
			quaternion_from_axes(panel->right, panel->up, back, orientation);
			quaternion_multiply(layers->hud_pose.orientation, orientation, layers->panels[index].pose.orientation);
			layers->panels[index].pose.valid = 1;
			layers->panels[index].size[0] = 2.0f * panel->half_size[0] * panel->distance;
			layers->panels[index].size[1] = 2.0f * panel->half_size[1] * panel->distance;
			layers->panels[index].over_hud = panel->distance < d;
		}
	}
	/* (the visor's glass is drawn into the eyes' images: no layer) */
	layers->visor = 0;
}

/* ---------- the shields on the visor */

/* toward target over a time constant */
static float ease(float value, float target, float seconds, float time_constant)
{
	return value + (target - value) * (1.0f - expf(-seconds / time_constant));
}

void halo_vr_visor_state(const struct halo_vr_visor_state *state)
{
	float seconds = visor.seconds;

	settings_read();
	visor.presence = ease(visor.presence, state->active ? 1.0f : 0.0f, seconds, PRESENCE_EASE);
	visor.full *= expf(-seconds / FULL_FADE);
	visor.wound *= expf(-seconds / WOUND_FADE);
	visor.flicker += seconds;
	visor.boot += seconds;
	visor.pulse += seconds;
	visor.hit *= expf(-seconds / HIT_FADE);
	visor.hit_time += seconds;
	if (!state->active)
	{
		/* (dead: a last flare of red on the glass as it goes) */
		if (state->dead && visor.active)
		{
			platform_log("vr: visor: dead");
			visor.wound = 1.0f;
		}
		visor.known = 0;
		visor.active = 0;
		visor.look.glow = 0.0f;
		visor.warning = ease(visor.warning, 0.0f, seconds, WARNING_EASE);
		visor.charge = ease(visor.charge, 0.0f, seconds, CHARGE_EASE);
		return;
	}
	if (!visor.active)
	{
		/* the player takes control: the HUD comes up */
		visor.boot = 0.0f;
		visor.active = 1;
	}
	if (visor.known && state->has_shield)
	{
		/* broken (or as good as: a frame later they may be recharging) */
		if (visor.shield > SHIELD_GONE && state->shield <= SHIELD_GONE)
		{
			platform_log("vr: visor: the shields are down (from %.2f)", visor.shield);
			visor.flicker = 0.0f;
		}
		/* full again, after recharging */
		if (visor.shield < 1.0f && state->shield >= 1.0f && visor.charge > 0.1f)
		{
			platform_log("vr: visor: the shields are full");
			visor.full = 1.0f;
		}
	}
	if (visor.known && visor.body - state->body > 0.001f)
		visor.wound = clamp(visor.wound + 0.4f + 3.0f * (visor.body - state->body), 0.0f, 1.0f);
	{
		/* a hit, on the shields or (without them) the body: the HUD's
		projection is disturbed for a moment, more the harder the hit (a
		recharge, which raises them, is none) */
		float shield = visor.known && state->has_shield && visor.shield > state->shield ? visor.shield - state->shield :
			0.0f;
		float body = visor.known && visor.body > state->body ? visor.body - state->body : 0.0f;

		if (shield > 0.004f || body > 0.001f)
		{
			visor.hit = clamp(visor.hit + 0.45f + 5.0f * (shield + body), 0.0f, 1.0f);
			visor.hit_time = 0.0f;
		}
	}
	{
		/* the warning: the shields low (more as they fall), or gone, or the
		body's health low without them */
		float warning = 0.0f;

		if (state->has_shield && state->shield < 0.25f)
			warning = state->shield <= SHIELD_GONE ? 1.0f : 0.4f + 0.6f * (0.25f - state->shield) / 0.25f;
		if (state->body < 0.25f && (!state->has_shield || state->shield <= SHIELD_GONE))
			warning = 1.0f;
		if (warning > 0.0f && visor.warning < 0.05f)
			visor.pulse = 0.0f;
		visor.warning = ease(visor.warning, warning, seconds, WARNING_EASE);
	}
	visor.charge = ease(visor.charge, state->has_shield && state->charging && state->shield < 1.0f ? 1.0f : 0.0f,
		seconds, CHARGE_EASE);
	visor.has_shield = state->has_shield;
	visor.shield = state->shield;
	visor.body = state->body;
	visor.known = 1;
	visor.look = *state;
}

static void eye_frames(float frames[2][3][4], float fov[2][4]);

/* the HUD's opacity as the shields break: three dips, each shallower, then
none (three in under half a second, then still: not a strobe) */
static float flicker_opacity(float t)
{
	static const float centres[3] = { 0.03f, 0.16f, 0.32f };
	static const float depths[3] = { 0.65f, 0.45f, 0.25f };
	float opacity = 1.0f;
	int dip;

	if (t >= FLICKER_SECONDS)
		return 1.0f;
	for (dip = 0; dip < 3; dip++)
	{
		float x = (t - centres[dip]) / 0.045f;

		if (x > -1.0f && x < 1.0f)
			opacity -= depths[dip] * (1.0f - x * x);
	}
	return clamp(opacity, 0.0f, 1.0f);
}

void vr_visor_menus(int active)
{
	visor.menus = active;
}

void vr_visor_hud_image(struct vr_visor_hud_image *image)
{
	settings_read();
	memset(image, 0, sizeof(*image));
	image->opacity = 1.0f;
	if (!settings.visor || visor.menus)
		return;
	image->curve = visor.cylinder ? 2 : 1;
	image->half_angles[0] = visor.half_angles[0];
	image->half_angles[1] = visor.half_angles[1];
	if (visor.cylinder)
	{
		image->extent[0] = visor.half_angles[0];
		image->extent[1] = tanf(visor.half_angles[1]);
	}
	else
	{
		image->extent[0] = tanf(visor.half_angles[0]);
		image->extent[1] = tanf(visor.half_angles[1]) / cosf(visor.half_angles[0]);
	}
	/* its own light on the glass, brighter as the shields recharge */
	image->glow = settings.glow * (1.0f + 0.5f * visor.charge * (settings.effects ? 1.0f : 0.0f));
	image->glass = settings.glass;
	/* the world's light behind it, and the shields' hits and warning */
	image->react = settings.react;
	image->light_reach[0] = -LIGHT_REACH_ACROSS;
	image->light_reach[1] = LIGHT_REACH_ACROSS;
	image->light_reach[2] = LIGHT_REACH_UP;
	image->light_reach[3] = -LIGHT_REACH_UP;
	image->light_blend = 1.0f - expf(-visor.seconds / LIGHT_EASE);
	eye_frames(image->eye, image->eye_fov);
	if (settings.react > 0.0f && settings.effects && visor.active)
	{
		image->hit = settings.react * visor.hit;
		image->hit_pattern = floorf(visor.hit_time / HIT_PATTERN_SECONDS);
		image->warning = settings.react * visor.warning * (1.0f - visor.charge) *
			(0.55f + 0.45f * sinf(2.0f * (float)M_PI * WARNING_PULSE * visor.pulse));
	}
	if (settings.effects && visor.active)
	{
		/* coming up: from nothing, with a flicker, as the projector warms */
		if (visor.boot < BOOT_SECONDS)
		{
			float t = visor.boot / BOOT_SECONDS;

			image->opacity = t * t * (3.0f - 2.0f * t) * (t < 0.3f || t > 0.42f ? 1.0f : 0.55f);
		}
		image->opacity *= flicker_opacity(visor.flicker);
	}
}

/* each eye's frame from the head's, as rows of x' = R (x - position) (the
eye's place and turn from the middle of the head; before the runtime has
them, eyes 64 mm apart looking ahead), and its field of view (tangents:
left, right, up, down) */
static void eye_frames(float frames[2][3][4], float fov[2][4])
{
	int eye, row;

	for (eye = 0; eye < 2; eye++)
	{
		float inverse[4], axis[3], position[3], column[3][3];
		int i;

		if (visor.eyes_known)
		{
			inverse[0] = -visor.eye_orientation[eye][0];
			inverse[1] = -visor.eye_orientation[eye][1];
			inverse[2] = -visor.eye_orientation[eye][2];
			inverse[3] = visor.eye_orientation[eye][3];
			memcpy(position, visor.eye_position[eye], sizeof(position));
		}
		else
		{
			inverse[0] = inverse[1] = inverse[2] = 0.0f;
			inverse[3] = 1.0f;
			position[0] = eye ? 0.032f : -0.032f;
			position[1] = position[2] = 0.0f;
		}
		for (i = 0; i < 3; i++)
		{
			float unit[3] = { 0.0f, 0.0f, 0.0f };

			unit[i] = 1.0f;
			quaternion_rotate(inverse, unit, column[i]);
		}
		quaternion_rotate(inverse, position, axis);
		for (row = 0; row < 3; row++)
		{
			frames[eye][row][0] = column[0][row];
			frames[eye][row][1] = column[1][row];
			frames[eye][row][2] = column[2][row];
			frames[eye][row][3] = -axis[row];
		}
		memcpy(fov[eye], visor.fov[eye], sizeof(fov[eye]));
	}
}

void vr_visor_image(struct vr_visor_image *image)
{
	static const float red[3] = { 1.00f, 0.10f, 0.05f };
	float effects, warning, glow = 0.0f;
	int channel;

	settings_read();
	memset(image, 0, sizeof(*image));
	if (!settings.visor || visor.menus || visor.presence < 0.01f)
		return;
	visor_extent(image->fov, image->extent, image->clear);
	eye_frames(image->eye, image->eye_fov);
	image->frame = settings.frame * visor.presence;
	effects = settings.effects ? visor.presence : 0.0f;
	/* the warning's pulse: a sine, never off while it lasts; it gives way
	to the shields' own energy as they recharge */
	warning = visor.warning * (1.0f - visor.charge) *
		(0.55f + 0.45f * sinf(2.0f * (float)M_PI * WARNING_PULSE * visor.pulse));
	for (channel = 0; channel < 3; channel++)
	{
		image->glow[channel] = effects * red[channel] * (visor.wound * 0.18f + warning * 0.13f);
		glow += image->glow[channel];
	}
	/* the shields' energy on the glass: the game's plasma, with its noise
	map (no map, no energy) */
	if (effects > 0.0f && visor.look.look && visor.look.noise_texture[0] && visor.look.noise_texture[1])
	{
		image->plasma = 1;
		image->plasma_glow = effects * clamp(visor.look.glow, 0.0f, 1.0f);
		image->energy = settings.energy;
		image->charge = effects * visor.charge;
		/* (an overshield charges from 1 to 3) */
		image->charge_level = clamp(visor.shield <= 1.0f ? visor.shield : (visor.shield - 1.0f) * 0.5f, 0.0f, 1.0f);
		image->full = effects * visor.full;
		memcpy(image->perpendicular, visor.look.perpendicular, sizeof(image->perpendicular));
		memcpy(image->parallel, visor.look.parallel, sizeof(image->parallel));
		memcpy(image->noise_scale, visor.look.noise_scale, sizeof(image->noise_scale));
		memcpy(image->noise_period, visor.look.noise_period, sizeof(image->noise_period));
		memcpy(image->noise_direction, visor.look.noise_direction, sizeof(image->noise_direction));
		image->noise_texture[0] = visor.look.noise_texture[0];
		image->noise_texture[1] = visor.look.noise_texture[1];
		image->time = visor.look.time;
		glow += image->plasma_glow + image->charge + image->full;
	}
	image->shown = image->frame > 0.001f || glow > 0.001f;
}

int vr_visor_rim(struct vr_visor_rim *rim)
{
	int eye;

	settings_read();
	memset(rim, 0, sizeof(*rim));
	if (!settings.visor || visor.menus || settings.rim <= 0.0f || visor.presence < 0.01f || !visor.head.valid)
		return 0;
	rim->strength = settings.rim * visor.presence;
	rim->distance = settings.rim_depth;
	rim->reach = settings.rim_reach;
	footprint(rim->distance, rim->edges);
	eye_frames(rim->eye, rim->fov);
	if (!visor.rim_logged && visor.eyes_known)
	{
		/* the view it leaves wholly clear: within its lip's inner edge (its
		reach in from the edges), from each eye, outward, up and down
		(degrees), of the eye's own */
		float inner = 1.0f - rim->reach;
		float clear[2][3], whole[2][3];

		for (eye = 0; eye < 2; eye++)
		{
			int side;

			for (side = 0; side < 3; side++)
			{
				float point[3], local[3];
				int axis;

				point[0] = side == 0 ? inner * rim->edges[eye ? 1 : 0] : visor.eye_position[eye][0];
				point[1] = side == 0 ? visor.eye_position[eye][1] : inner * rim->edges[side == 1 ? 2 : 3];
				point[2] = -rim->distance;
				for (axis = 0; axis < 3; axis++)
				{
					local[axis] = rim->eye[eye][axis][0] * point[0] + rim->eye[eye][axis][1] * point[1] +
						rim->eye[eye][axis][2] * point[2] + rim->eye[eye][axis][3];
				}
				clear[eye][side] = fabsf(atan2f(side == 0 ? local[0] : local[1], -local[2])) * 180.0f / (float)M_PI;
				whole[eye][side] = fabsf(atanf(visor.fov[eye][side == 0 ? eye : side == 1 ? 2 : 3])) * 180.0f /
					(float)M_PI;
			}
		}
		platform_log("vr: helmet rim: clear to %.0f of %.0f degrees outward, %.0f of %.0f up and %.0f of %.0f down "
			"(left eye), %.0f of %.0f, %.0f of %.0f and %.0f of %.0f (right)", clear[0][0], whole[0][0], clear[0][1],
			whole[0][1], clear[0][2], whole[0][2], clear[1][0], whole[1][0], clear[1][1], whole[1][1], clear[1][2],
			whole[1][2]);
		visor.rim_logged = 1;
	}
	return 1;
}

/* ---------- debug.screenshot_every: what the headset shows */

/* a layer's pose in the play space: those on the head are from its pose */
static void layer_pose(const struct vr_host_pose *pose, int on_head, const struct vr_host_pose *head,
	float position[3], float orientation[4])
{
	int axis;

	if (!on_head)
	{
		memcpy(position, pose->position, sizeof(pose->position));
		memcpy(orientation, pose->orientation, sizeof(pose->orientation));
		return;
	}
	quaternion_rotate(head->orientation, pose->position, position);
	for (axis = 0; axis < 3; axis++)
		position[axis] += head->position[axis];
	quaternion_multiply(head->orientation, pose->orientation, orientation);
}

/* where a ray (play space) meets a quad (size[0] by size[1] about its
pose's origin, facing +z) or a cylinder (about its pose's y, radius, angle
around): the image's coordinates (0 to 1, v up); 0 if it misses */
static int layer_hit(const float origin[3], const float direction[3], const float position[3],
	const float orientation[4], int cylinder, float radius, float angle, const float size[2], float uv[2])
{
	float inverse[4] = { -orientation[0], -orientation[1], -orientation[2], orientation[3] };
	float o[3], d[3], v[3], t;
	int axis;

	for (axis = 0; axis < 3; axis++)
		v[axis] = origin[axis] - position[axis];
	quaternion_rotate(inverse, v, o);
	quaternion_rotate(inverse, direction, d);
	if (!cylinder)
	{
		if (fabsf(d[2]) < 1e-6f)
			return 0;
		t = -o[2] / d[2];
		if (t <= 0.0f)
			return 0;
		uv[0] = (o[0] + d[0] * t) / size[0] + 0.5f;
		uv[1] = (o[1] + d[1] * t) / size[1] + 0.5f;
	}
	else
	{
		/* (from inside: the far root) */
		float a = d[0] * d[0] + d[2] * d[2], b = 2.0f * (o[0] * d[0] + o[2] * d[2]);
		float c = o[0] * o[0] + o[2] * o[2] - radius * radius, discriminant = b * b - 4.0f * a * c;
		float x, z;

		if (a < 1e-9f || discriminant < 0.0f)
			return 0;
		t = (-b + sqrtf(discriminant)) / (2.0f * a);
		if (t <= 0.0f)
			return 0;
		x = o[0] + d[0] * t;
		z = o[2] + d[2] * t;
		uv[0] = atan2f(x, -z) / angle + 0.5f;
		uv[1] = (o[1] + d[1] * t) / size[1] + 0.5f;
	}
	return uv[0] >= 0.0f && uv[0] < 1.0f && uv[1] >= 0.0f && uv[1] < 1.0f;
}

/* an sRGB value's light, and back (near enough for a picture) */
static float linear(float value)
{
	return powf(value, 2.2f);
}

static void layer_over(unsigned char *pixel, const unsigned char *image, int width, int height, const float uv[2])
{
	const unsigned char *texel = image + ((size_t)(int)(uv[1] * height) * width + (int)(uv[0] * width)) * 4;
	float alpha = texel[3] / 255.0f;
	int channel;

	for (channel = 0; channel < 3; channel++)
	{
		float light = linear(texel[channel] / 255.0f) + linear(pixel[channel] / 255.0f) * (1.0f - alpha);

		pixel[channel] = (unsigned char)(powf(clamp(light, 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f + 0.5f);
	}
}

void vr_visor_composite(unsigned char *eyes[2], int width, int height, const struct vr_host_layers *layers,
	const struct vr_host_pose *head, const unsigned char *hud, int hud_width, int hud_height,
	unsigned char *const panels[VR_PANEL_COUNT],
	const int panel_width[VR_PANEL_COUNT], const int panel_height[VR_PANEL_COUNT], const unsigned char *scope,
	int scope_size)
{
	float hud_position[3], hud_orientation[4];
	float panel_position[VR_PANEL_COUNT][3], panel_orientation[VR_PANEL_COUNT][4];
	float scope_extent[2] = { layers->scope_size, layers->scope_size };
	int eye, x, y, panel, over;

	layer_pose(&layers->hud_pose, layers->hud_head_locked, head, hud_position, hud_orientation);
	for (panel = 0; panel < VR_PANEL_COUNT; panel++)
	{
		layer_pose(&layers->panels[panel].pose, layers->hud_head_locked, head, panel_position[panel],
			panel_orientation[panel]);
	}
	for (eye = 0; eye < 2; eye++)
	{
		const float *fov = layers->fov[eye];

		for (y = 0; y < height; y++)
		{
			for (x = 0; x < width; x++)
			{
				unsigned char *pixel = eyes[eye] + ((size_t)y * width + x) * 4;
				float local[3], direction[3], uv[2];

				local[0] = fov[0] + (fov[1] - fov[0]) * ((float)x + 0.5f) / (float)width;
				local[1] = fov[3] + (fov[2] - fov[3]) * ((float)y + 0.5f) / (float)height;
				local[2] = -1.0f;
				quaternion_rotate(layers->eye[eye].orientation, local, direction);
				/* (in the order the host submits them: host_vr.c) */
				if (layers->scope && scope && (layers->scope_eye < 0 || eye == layers->scope_eye) && layer_hit(layers->eye[eye].position,
					direction, layers->scope_pose.position, layers->scope_pose.orientation, 0, 0.0f, 0.0f,
					scope_extent, uv))
				{
					layer_over(pixel, scope, scope_size, scope_size, uv);
				}
				for (over = 0; over < 2; over++)
				{
					if (over && layers->hud && hud && layer_hit(layers->eye[eye].position, direction, hud_position,
						hud_orientation, layers->hud_cylinder, layers->hud_radius, layers->hud_angle, layers->hud_size,
						uv))
					{
						layer_over(pixel, hud, hud_width, hud_height, uv);
					}
					for (panel = 0; panel < VR_PANEL_COUNT; panel++)
					{
						if (layers->hud && layers->panels[panel].shown && !layers->panels[panel].over_hud == !over &&
							panels[panel] && layer_hit(layers->eye[eye].position, direction, panel_position[panel],
								panel_orientation[panel], 0, 0.0f, 0.0f, layers->panels[panel].size, uv))
						{
							layer_over(pixel, panels[panel], panel_width[panel], panel_height[panel], uv);
						}
					}
				}
			}
		}
	}
}
#endif
