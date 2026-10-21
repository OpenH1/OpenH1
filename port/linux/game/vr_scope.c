/*
VR_SCOPE.C

The VR mode's scopes (HALO_VR; vr.scopes, vr.scope_view, port/linux/
README.md "VR"): the zoom of the sniper rifle, the pistol and the other
weapons that zoom, for render.c, vr_render.c and player_control.c.

On the flat screen the game zooms by narrowing the whole view. In the
headset the eyes' view is the head's and cannot narrow: the zoom magnifies
a view of its own instead, and the rest of the view stays as it was. Its
view is drawn before the eyes' (render.c's scope pass) into an image of its
own (host_vulkan_vr.c), which the compositor shows on a quad layer (vr.c,
host_vr.c), which the runtime reprojects to the head's latest pose as it
does the eyes'. Two ways (vr.scope_view):

- "smartlink" (the default): as the MJOLNIR armour's smart-link pipes the
  weapon's sight into the helmet's HUD, the zoom shows in the HUD. The
  right stick's click zooms as the game does (its levels, from the last
  back to none), wherever the weapon is held, and while zoomed a window is
  about the reticle, at its depth, facing the head (or on the visor where
  the HUD is: vr.smartlink_place), drawn into both eyes' images over the
  weapon in hand (host_vulkan_vr.c, vr.scope_gun). Its view is the
  weapon's sight's, drawn from the controller along the aim's line, its
  field of view the window's over the magnification, on what the line
  meets. For the sniper rifle and the pistol it is framed: the flat
  screen's zoomed view, the scope's own mask and zoomed HUD drawn over it
  (vr_scope_draw_hud), in a 4:3 window as large as the HUD; for the other
  weapons that zoom (the rocket launcher), a 16:9 window vr.smartlink_size
  wide, with a reticle and readouts of its own. In a vehicle's
  seat that aims the vehicle's weapon (the Scorpion's driver, a turret's
  gunner), seen from the seat (vr.vehicle_view = "first_person"), the
  click zooms the same window about that seat's reticle (vr_scope_seat).

- "eye": a scope magnifies the view within it, as a real one does, when it
  is raised to an eye: drawn from the scope's eyepiece along the aim, shown
  on a quad at the eyepiece, to that eye alone. The sniper rifle's and the
  pistol's (the table below); other weapons zoom as the game does, which
  magnifies nothing in the headset.

The view in the scope is as many times larger as the game's zoom
magnifies (2 and 8 for the sniper rifle, whose HUD says "10x"; 2 for the
pistol), as the lens is seen from the eye (or the smart-link's window from
between the eyes): its field of view is the angle the lens spans from the
eye over the magnification, so the image is that much larger than the
world about it, wherever the eye is. The view shows the zoom as the HUD
labels it; the HUD's zoomed view is not drawn while it shows
(hud_weapon.c), nor, at the eye, the display on the back of the sniper
rifle's scope, about the lens at the eye.

At the eye, a scope is raised when an eye looks through it (behind its
eyepiece, within its eye relief, near its axis) and it points about where
the head looks; then the game zooms (the player's zoom level, which the netcode
sends as ever: the sounds, and what the zoom does to the weapon), and
lowered it does not (the right stick's click alone does not zoom it).
While it is raised the click steps through the weapon's levels, and a
weapon raised again has the level it had (not after a switch). What
unzooms the game (a hit, a reload, a grenade) unzooms the scope too: its
view is then the world's size, until the click. Other weapons that zoom
(the rocket launcher) zoom as the game does, which magnifies nothing in the
headset.
*/

#ifdef HALO_VR

#include "cseries.h"
#include "math/real_math.h"
#include "cache/cache_files.h"
#include "camera/director.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/players.h"
#include "interface/hud.h"
#include "interface/interface.h"
#include "items/weapon_definitions.h"
#include "items/weapons.h"
#include "objects/objects.h"
#include "render/render.h"
#include "render/render_cameras.h"
#include "tag_files/tag_files.h"
#include "units/units.h"

#include "../src/vr.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* raised: an eye behind the eyepiece, within the scope's eye relief and
near its axis (vr_scope_definition), no more than this in front of the lens
(metres), and the scope pointing within this angle of the head's forward
(degrees); lowered again past these, and past the scope's relief and
distance from the axis times LOWER_MARGIN */
#define RAISE_IN_FRONT 0.02f
#define RAISE_ANGLE 20.0f
#define LOWER_ANGLE 25.0f
#define LOWER_MARGIN 1.25f
/* a world unit is 10 feet */
#define METRES_PER_WORLD_UNIT 3.048f
/* the eye's distance from the lens no less than this (metres), for the
angle the lens spans */
#define NEAREST_EYE 0.02f
/* the scope's view, its 480 lines in the middle of the 640x480 screen
(host_vulkan_vr.c copies those into its image) */
#define SCOPE_VIEW_LEFT 80
#define SCOPE_VIEW_RIGHT 560
/* the zoom shown in the lens this tall as the eye sees it (degrees), and
no less nor more of the lens's width than these */
#define ZOOM_DEGREES 1.6f
#define ZOOM_LEAST 0.04f
#define ZOOM_MOST 0.12f
/* the scope's pass drawn once unseen, to make its shaders: this wide
(degrees) */
#define WARM_FIELD_OF_VIEW 100.0f
/* the smart-link's window: 16:9 (its height of its width), its glow about
it (of its half width), no nearer than this (metres: the reticle on a wall
at arm's length), opening and closing over these (seconds) */
#define WINDOW_HEIGHT (9.0f / 16.0f)
#define WINDOW_GLOW 0.03f
#define WINDOW_NEAREST 0.5f
#define WINDOW_OPEN_SECONDS 0.18f
/* its magnification eased from one level to another over this (seconds) */
#define WINDOW_RAMP_SECONDS 0.2f
#define WINDOW_CLOSE_SECONDS 0.1f
/* the smart-link's window drawn as the flat screen's zoomed view (a scope
of vr_scope_definitions): grown from the reticle over this (seconds), and
gone at once as the game unzooms, as the flat screen's (the game's zoomed
view, the scope's mask, is gone with the zoom: a fade would show the bare
magnified view); its magnification eased
from one level to another over this, as the flat screen's zoom (seconds);
the world about it dimmed this much (0 to 1: the scope's mask darkens the
zoomed view about its frame, as on the flat screen); no nearer than this
(metres: within reach the eyes would turn in to it); its corners' radius
(of the image's half width) and its edge fading into the world over this
(degrees). Its size: the zoomed screen in it at the scale that makes the
widest scope's frame (the sniper rifle's) vr.smartlink_size wide, the
others' as large as on the flat screen beside it; it is 4:3, as wide as the
HUD (vr.hud_size) or this many times the widest frame, the wider (the
frame with the zoomed surround about it, as on the flat screen) */
#define FRAMED_OPEN_SECONDS 0.1f
#define FRAMED_RAMP_SECONDS 0.17f
#define FRAMED_DIM 0.12f
#define FRAMED_NEAREST 1.5f
#define FRAMED_CORNER 0.04f
#define FRAMED_FEATHER_DEGREES 1.5f
#define FRAMED_WIDEST (vr_scope_definitions[0].frame)
#define FRAMED_AREA 1.2f
/* the smart-link's window in a feed cockpit (vr_cabin.c): no wider than
this (degrees), to be within its display (40 degrees high: 16:9) */
#define FEED_WINDOW_DEGREES 62.0f
/* a seat's zoom levels (vr.vehicle_zoom): no more than these */
#define MAXIMUM_SEAT_LEVELS 4

/* a scope: the weapon whose tag this is, the middle of its eyepiece from
the camera the first-person weapon is posed from (forward, left, up), and
its lens's radius, in metres as the first-person model has them (its world
units, as large at any vr.world_scale as the world is); its eye relief, how
far behind the eyepiece an eye looks through it, and how far from its axis
(metres); the smallest half angle (degrees) its lens spans from the eye;
what the names of the shaders of the display on the back of its scope have
in them (NULL: none), not drawn while it is raised; its zoom levels'
labels, which its lens shows as the game's HUD does (its zoom's sprites:
the sniper rifle's second level magnifies 8 times, and says "10x"; 0: the
magnification); and for the smart-link's window drawn as the flat screen's
zoomed view, the scope's own mask in it (its screen effect), the half
width of the mask's frame, of the middle 480x480 of the 640x480 screen's
half (as the flat screen draws it zoomed: the sniper rifle's rounded
rectangle, the pistol's circle), which sizes it.
The sniper rifle's is a rifle scope, at the eye: its eyepiece the back of
the scope, where its display is. The pistol's (the smart-linked scope on
the front of its slide) is a pistol scope, of long eye relief, looked
through with the arm out, its lens shown no smaller than a window of 12
degrees, as its sight's display. Each was found from the model: its lens
shown at every pose (as if raised) over the weapon held still, seen from
behind and the side (debug.vr_test_hands, view*.bmp: the compositor's
placement; the game's debug lines are not where they should be in the
eyes). The sniper rifle's display (its zoom's green waveform and dial, a
hand's width across) is all about the lens at the eye, which shows the zoom
itself (host_vulkan_vr.c). */
struct vr_scope_definition
{
	char const *name;
	real eyepiece[3];
	real radius;
	real relief;
	real off_axis;
	real smallest_angle;
	char const *screen;
	short labels[2];
	real frame;
};

static struct vr_scope_definition const vr_scope_definitions[] =
{
	{ "weapons\\sniper rifle\\sniper rifle", { 0.26f, -0.13f, -0.058f }, 0.025f, 0.12f, 0.07f, 0.0f, "screen", { 2, 10 }, 0.808f },
	{ "weapons\\pistol\\pistol", { 0.545f, -0.057f, -0.101f }, 0.013f, 0.65f, 0.04f, 6.0f, NULL, { 2, 0 }, 0.5f },
};

void platform_log(char const *format, ...);
char const *config_string(char const *name);
long config_integer(char const *name);
struct player_control *player_control_get(short local_player_index);
/* vr_holster.c's */
void vr_holster_update(long unit_index, boolean aiming);
/* vr_render.c's */
boolean vr_render_scope_view(struct render_camera *weapon_camera, real_point3d eyes[2], real_vector3d *head_forward,
	real *units);
void vr_render_world_to_head(real_point3d const *point, real_vector3d const *forward, real_vector3d const *up,
	float position[3], float head_forward[3], float head_up[3]);
boolean vr_seat_feed(real *grate, real *wash, real *off);
boolean vr_render_scope_seat_view(real_point3d eyes[2], real_vector3d *head_forward, real *units);
boolean vr_render_scope_aim(real_point3d *point, real_point3d *origin, real_vector3d *direction);
real vr_render_scope_range(void);
real vr_render_frame_seconds(void);

static struct
{
	/* the weapon in hand whose scope was placed last frame, its scope (an
	index of vr_scope_definitions; NONE, smart-linked, for a weapon not in
	it), and whether it is raised, to which eye; or whether the smart-link's
	view is shown (vr.scope_view = "smartlink"), and the zoom level it
	shows */
	long weapon_index;
	short scope;
	boolean raised;
	short eye;
	boolean smartlink_shown;
	short smartlink_level;
	/* the smart-link's window: the seconds since it began to open, how far
	it has closed (0 to 1; 0 open), the seconds since its level changed, and
	the magnification and level it shows (as it closes, the last zoomed);
	NONE seconds: not shown */
	real window_seconds;
	real window_closing;
	real window_switched;
	real window_magnification;
	short window_level;
	/* the magnification it draws, easing (in its logarithm) from one to
	another: from, to, and the seconds since it began */
	real window_drawn;
	real ramp_from;
	real ramp_to;
	real ramp_seconds;
	/* its size (of its whole: 0 to 1), as it closed from, and how far it has
	opened (its eased progress: 0 to 1) */
	real window_size;
	real size_from;
	real window_opening;
	/* the zoom: the scope raised as the player's control saw it last, and
	the level the weapon had as it was lowered */
	boolean zoomed;
	long remembered_weapon_index;
	short remembered_level;
	/* in a vehicle's seat: the vehicle's weapon the seat aims as the
	player's control saw it last, and its zoom level (NONE: unzoomed) */
	long seat_weapon_index;
	short seat_level;
	/* the smart-link window's widest in the seat (degrees; 0: as
	vr.smartlink_size): within a feed cockpit's display */
	real window_degrees_most;
	/* the game's tick the scope's pass was first drawn at in this map, to
	make its shaders (NONE: not yet) */
	long warmed_tick;
	/* debug.vr_test_weapons: given, and the game's tick the player was
	first aiming at (NONE: not yet) */
	boolean test_weapons_given;
	long test_weapons_tick;
} vr_scope = { NONE, NONE, FALSE, 0, FALSE, NONE, -1.0f, 0.0f, 0.0f, 0.0f, NONE, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	FALSE, NONE, NONE, NONE, NONE, 0.0f, NONE, FALSE, NONE };

/* the weapon's scope, or NONE */
static short vr_scope_find(long weapon_index)
{
	char const *name;
	short index;

	if (weapon_index == NONE)
		return NONE;
	if (weapon_definition_get(object_get(weapon_index)->definition_index)->weapon.zoom_level_count <= 0)
		return NONE;
	name = tag_get_name(object_get(weapon_index)->definition_index);
	if (!name)
		return NONE;
	for (index = 0; index < (short)NUMBEROF(vr_scope_definitions); index++)
	{
		if (!strcmp(name, vr_scope_definitions[index].name))
			return index;
	}
	return NONE;
}

/* the first player's weapon in hand on foot, first person, alive, with the
scopes on; else NONE */
static long vr_scope_weapon(void)
{
	long player_index = local_player_get_player_index(0);
	long unit_index;
	struct unit_datum *unit;

	if (player_index == NONE || !halo_vr_scopes() || cinematic_in_progress() ||
		director_get_perspective(0) != _director_perspective_first_person)
	{
		return NONE;
	}
	unit_index = player_get(player_index)->unit_index;
	if (unit_index == NONE)
		return NONE;
	unit = unit_get(unit_index);
	if (unit->object.parent_object_index != NONE || TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
		return NONE;
	return unit_inventory_get_weapon(unit_index, unit->unit.current_weapon_index);
}

/* in a vehicle's seat, the first player alive, with the scopes smart-linked:
the vehicle's weapon if the seat aims it (the player is the vehicle's
gunner: the Scorpion's driver, a turret's or the Warthog's gunner); else
NONE. (The seated player holds no weapon of their own there, so the game's
zoom does nothing: none of the vehicles' weapons has zoom levels.) */
static long vr_scope_seat_weapon(void)
{
	long player_index = local_player_get_player_index(0);
	long unit_index, vehicle_index;
	struct unit_datum *unit, *vehicle;

	if (player_index == NONE || halo_vr_scopes() != HALO_VR_SCOPES_SMARTLINK || cinematic_in_progress())
		return NONE;
	unit_index = player_get(player_index)->unit_index;
	if (unit_index == NONE)
		return NONE;
	unit = unit_get(unit_index);
	vehicle_index = unit->object.parent_object_index;
	if (vehicle_index == NONE || TEST_FLAG(unit->object.damage_flags, _object_dead_bit) ||
		!TEST_FLAG(_object_mask_unit, object_get(vehicle_index)->object.type))
	{
		return NONE;
	}
	vehicle = unit_get(vehicle_index);
	if (vehicle->unit.gunner_object_index != unit_index)
		return NONE;
	return unit_inventory_get_weapon(vehicle_index, vehicle->unit.current_weapon_index);
}

/* a seat's zoom levels' magnifications, from the first: the vehicle's
weapon's own, else vr.vehicle_zoom's ("2 4"; "" none); how many */
static short vr_scope_seat_levels(long weapon_index, real magnifications[MAXIMUM_SEAT_LEVELS])
{
	struct weapon_definition const *definition = weapon_definition_get(object_get(weapon_index)->definition_index);
	char const *text;
	short count = 0;

	if (definition->weapon.zoom_level_count > 0)
	{
		for (count = 0; count < definition->weapon.zoom_level_count && count < MAXIMUM_SEAT_LEVELS; count++)
			magnifications[count] = weapon_get_zoom_magnification(weapon_index, count);
		return count;
	}
	for (text = config_string("vr.vehicle_zoom"); *text && count < MAXIMUM_SEAT_LEVELS; )
	{
		char *end;
		real magnification = (real)strtod(text, &end);

		if (end == text)
			break;
		text = end;
		if (magnification > 1.0f)
			magnifications[count++] = MIN(magnification, 20.0f);
	}
	return count;
}

/* the scope's camera's view: the middle 480x480 of the screen, which
host_vulkan_vr.c copies into its image (or all the 640x480 screen, framed:
the flat screen's zoomed view, its mask and HUD laid out on it as there,
vr_scope_draw_hud, in the window the game gave the camera: the HUD's safe
area within the screen); the rasterizer's camera the same */
static void vr_scope_camera_view(struct render_camera *camera, struct render_camera *rasterizer_camera,
	boolean framed)
{
	camera->mirrored = FALSE;
	camera->viewport_bounds.x0 = framed ? 0 : SCOPE_VIEW_LEFT;
	camera->viewport_bounds.x1 = framed ? 640 : SCOPE_VIEW_RIGHT;
	camera->viewport_bounds.y0 = 0;
	camera->viewport_bounds.y1 = 480;
	if (!framed)
		camera->window_bounds = camera->viewport_bounds;
	*rasterizer_camera = *camera;
}

/* render.c: prepare the mono scene at map load, even with no scoped weapon
in hand. The shared enumeration draws every material through this camera. */
void vr_scope_enumerate_camera(struct render_camera *camera, struct render_camera *rasterizer_camera)
{
	camera->vertical_field_of_view = WARM_FIELD_OF_VIEW * _pi / 180.0f;
	vr_scope_camera_view(camera, rasterizer_camera, FALSE);
	vr_scope.warmed_tick = game_time_get();
}

/* the camera's up: the world's, square to its forward (else the weapon's,
looking straight up or down) */
static void vr_scope_camera_up(struct render_camera *camera, real_vector3d const *fallback)
{
	real along_up;

	camera->up = *global_up3d;
	along_up = dot_product3d(&camera->up, &camera->forward);
	camera->up.i -= camera->forward.i * along_up;
	camera->up.j -= camera->forward.j * along_up;
	camera->up.k -= camera->forward.k * along_up;
	if (normalize3d(&camera->up) <= 0.0f)
		camera->up = *fallback;
}

/* the scope's shaders made the first time a scope is in hand in a map, not
the first time it is raised or zoomed (its pass draws without the eyes'
multiview, so in programs of its own, which a driver makes as they are first
drawn with: half a second on spark the first time, longer where a driver
makes them slowly): its pass drawn once, a wide view where the head looks,
and not shown. TRUE as it is to be drawn. */
static boolean vr_scope_warm(struct render_camera *camera, struct render_camera *rasterizer_camera,
	real_point3d const eyes[2], real_vector3d const *head_forward, real_vector3d const *fallback_up)
{
	if (game_time_get() < vr_scope.warmed_tick)
		vr_scope.warmed_tick = NONE;
	if (vr_scope.warmed_tick != NONE)
		return FALSE;
	vr_scope.warmed_tick = game_time_get();
	camera->position.x = (eyes[0].x + eyes[1].x) * 0.5f;
	camera->position.y = (eyes[0].y + eyes[1].y) * 0.5f;
	camera->position.z = (eyes[0].z + eyes[1].z) * 0.5f;
	camera->forward = *head_forward;
	vr_scope_camera_up(camera, fallback_up);
	camera->vertical_field_of_view = WARM_FIELD_OF_VIEW * _pi / 180.0f;
	vr_scope_camera_view(camera, rasterizer_camera, FALSE);
	platform_log("vr scope: its shaders made ready (its view drawn once, unseen)");
	return TRUE;
}

/* the zoom as the lens shows it: the label the game's HUD gives the level
(the scope's own), else its magnification; 0 unzoomed */
static real vr_scope_label(short scope, short zoom_level, real magnification)
{
	if (zoom_level == NONE)
		return 0.0f;
	if (scope != NONE && zoom_level < (short)NUMBEROF(vr_scope_definitions[scope].labels) &&
		vr_scope_definitions[scope].labels[zoom_level])
	{
		return vr_scope_definitions[scope].labels[zoom_level];
	}
	return magnification;
}

/* the time from the last frame to this one for the window's animation
(seconds, no more than 0.1); with debug.screenshot_every = 1, a 72 Hz
headset's frame, as the frames written each frame slow the game, so that
each step of it shows in them */
static real vr_scope_window_seconds(void)
{
	return config_integer("debug.screenshot_every") == 1 ? 1.0f / 72.0f : PIN(vr_render_frame_seconds(), 0.0f, 0.1f);
}

/* the smart-link's window (vr.scope_view = "smartlink"), while the game is
zoomed (framed, a scope of vr_scope_definitions: the flat screen's zoomed
view in a 4:3 window, FRAMED_*): a flat 16:9 window vr.smartlink_size wide,
its middle on the reticle
(the point the aim meets, at the reticle's smoothed distance), facing the
eyes (square to the line from between them to that point), up as the
world's: each eye sees it where it sees the reticle, at its depth, and the
head's turn and tilt do not move it nor turn it in the world (only the
head's place does, a little), and it is level with the horizon, as the view
in it is. host_vulkan_vr.c draws it into the eyes'
images, over the weapon in hand (vr.scope_gun), under the HUD. Its view is
the weapon's sight's, from the controller along the aim's line, as wide as
the quad is from between the eyes over the magnification, its own reticle
in its middle on what the line meets. As the game zooms it opens out from
the reticle over WINDOW_OPEN_SECONDS, the view in it pushing in from the
world's size to the zoom's as it grows (its feed connecting: host_vulkan_vr.c);
as it unzooms it shrinks back as its feed switches off, over
WINDOW_CLOSE_SECONDS; from one level to the next its magnification eases,
with a flicker. Unzoomed and closed, not shown (its
shaders made ready once a map). */
static boolean vr_scope_smartlink(struct render_camera *camera, struct render_camera *rasterizer_camera,
	long weapon_index, short scope, short zoom_level, real magnification, struct render_camera const *weapon_camera,
	real_point3d const eyes[2], real_vector3d const *head_forward, real units)
{
	struct halo_vr_scope layer;
	struct render_camera layer_camera;
	real_point3d centre, aim, line_origin;
	real_vector3d aim_direction, line_direction;
	real distance, tangent, half_width, glow;
	float view_degrees, view_distance, hud_degrees;
	boolean zoomed, aimed, on_visor = halo_vr_smartlink_hud(&hud_degrees);
	/* (drawn as the flat screen's zoomed view: a scope of
	vr_scope_definitions, on foot) */
	boolean framed = scope != NONE;

	vr_scope.raised = FALSE;
	centre.x = (eyes[0].x + eyes[1].x) * 0.5f;
	centre.y = (eyes[0].y + eyes[1].y) * 0.5f;
	centre.z = (eyes[0].z + eyes[1].z) * 0.5f;
	zoomed = zoom_level != NONE && magnification > 0.0f;
	aimed = vr_render_scope_aim(&aim, &line_origin, &line_direction);

	/* opening as the game zooms (its feed connecting: host_vulkan_vr.c), closing
	over WINDOW_CLOSE_SECONDS as it unzooms (at the level it showed), a
	flicker as the level changes; always at its whole size and
	magnification */
	{
		real seconds = vr_scope_window_seconds();

		if (zoomed && aimed)
		{
			if (vr_scope.window_seconds < 0.0f || vr_scope.window_closing > 0.0f)
			{
				vr_scope.window_seconds = 0.0f;
				vr_scope.window_switched = WINDOW_OPEN_SECONDS;
			}
			else
			{
				vr_scope.window_seconds += seconds;
				vr_scope.window_switched += seconds;
				if (zoom_level != vr_scope.window_level)
					vr_scope.window_switched = 0.0f;
			}
			vr_scope.window_closing = 0.0f;
			vr_scope.window_magnification = magnification;
			vr_scope.window_level = zoom_level;
		}
		else if (vr_scope.window_seconds >= 0.0f && aimed && !framed)
		{
			vr_scope.window_closing += MAX(seconds, 0.001f) / WINDOW_CLOSE_SECONDS;
			if (vr_scope.window_closing >= 1.0f)
				vr_scope.window_seconds = -1.0f;
		}
		else
		{
			vr_scope.window_seconds = -1.0f;
		}
	}
	if (vr_scope.window_seconds < 0.0f || vr_scope.window_magnification <= 0.0f)
	{
		if (vr_scope.smartlink_shown)
			platform_log("vr scope: smart-link unzoomed");
		vr_scope.smartlink_shown = FALSE;
		vr_scope.smartlink_level = NONE;
		vr_scope.window_seconds = -1.0f;
		vr_scope.window_closing = 0.0f;
		vr_scope.window_magnification = 0.0f;
		vr_scope.window_level = NONE;
		return vr_scope_warm(camera, rasterizer_camera, eyes, head_forward, &weapon_camera->up);
	}
	zoom_level = vr_scope.window_level;

	/* one motion: as it opens, its size (from the reticle out) and the
	magnification drawn (from 1, the world about it, to the zoom's, in its
	logarithm: as fast to the eye all the way) both follow one eased
	progress over WINDOW_OPEN_SECONDS; as it closes, both shrink back
	together over WINDOW_CLOSE_SECONDS; from level to level, at its whole
	size, the magnification eases over WINDOW_RAMP_SECONDS. The game's zoom
	(the shot, the network's) is the game's. */
	{
		real target = vr_scope.window_magnification;
		real seconds = vr_scope_window_seconds();
		real eased;

		if (vr_scope.window_closing > 0.0f)
		{
			if (vr_scope.ramp_to != 1.0f)
			{
				vr_scope.ramp_from = vr_scope.window_drawn;
				vr_scope.ramp_to = 1.0f;
				vr_scope.size_from = vr_scope.window_size;
			}
			eased = PIN(vr_scope.window_closing, 0.0f, 1.0f);
			eased = eased * eased * (3.0f - 2.0f * eased);
			vr_scope.window_size = vr_scope.size_from * (1.0f - eased);
			vr_scope.window_drawn = (real)exp(log(MAX(vr_scope.ramp_from, 1.0f)) * (1.0f - eased));
			vr_scope.window_opening = 1.0f;
		}
		else if (framed && vr_scope.window_seconds < FRAMED_RAMP_SECONDS)
		{
			/* framed: its frame grows from the reticle, and the view in it
			pushes in to the zoom's over the flat screen's ramp */
			eased = PIN(vr_scope.window_seconds / FRAMED_OPEN_SECONDS, 0.0f, 1.0f);
			vr_scope.window_size = 1.0f - (1.0f - eased) * (1.0f - eased) * (1.0f - eased);
			eased = vr_scope.window_seconds / FRAMED_RAMP_SECONDS;
			eased = 1.0f - (1.0f - eased) * (1.0f - eased) * (1.0f - eased);
			vr_scope.window_drawn = (real)exp(log(MAX(target, 1.0f)) * eased);
			vr_scope.window_opening = vr_scope.window_size;
			vr_scope.ramp_from = vr_scope.ramp_to = target;
			vr_scope.ramp_seconds = FRAMED_RAMP_SECONDS;
		}
		else if (!framed && vr_scope.window_seconds < WINDOW_OPEN_SECONDS)
		{
			eased = vr_scope.window_seconds / WINDOW_OPEN_SECONDS;
			eased = 1.0f - (1.0f - eased) * (1.0f - eased) * (1.0f - eased);
			vr_scope.window_size = eased;
			vr_scope.window_drawn = (real)exp(log(MAX(target, 1.0f)) * eased);
			vr_scope.window_opening = eased;
			vr_scope.ramp_from = vr_scope.ramp_to = target;
			vr_scope.ramp_seconds = WINDOW_RAMP_SECONDS;
		}
		else
		{
			if (target != vr_scope.ramp_to)
			{
				vr_scope.ramp_from = vr_scope.window_drawn;
				vr_scope.ramp_to = target;
				vr_scope.ramp_seconds = 0.0f;
			}
			else
			{
				vr_scope.ramp_seconds += seconds;
			}
			eased = PIN(vr_scope.ramp_seconds / (framed ? FRAMED_RAMP_SECONDS : WINDOW_RAMP_SECONDS), 0.0f, 1.0f);
			eased = 1.0f - (1.0f - eased) * (1.0f - eased) * (1.0f - eased);
			vr_scope.window_size = 1.0f;
			vr_scope.window_drawn = (real)exp(log(MAX(vr_scope.ramp_from, 1.0f)) +
				(log(MAX(vr_scope.ramp_to, 1.0f)) - log(MAX(vr_scope.ramp_from, 1.0f))) * eased);
			vr_scope.window_opening = 1.0f;
		}
		magnification = vr_scope.window_drawn;
	}
	/* where it is: about the reticle, at its distance (no nearer than its
	nearest), or on the visor, where the HUD is (vr.smartlink_place) */
	halo_vr_smartlink_view(&view_degrees, &view_distance);
	aim_direction.i = aim.x - centre.x;
	aim_direction.j = aim.y - centre.y;
	aim_direction.k = aim.z - centre.z;
	distance = normalize3d(&aim_direction) / units;
	if (distance <= 0.0f)
		aim_direction = weapon_camera->forward;
	if (distance < (framed ? FRAMED_NEAREST : WINDOW_NEAREST))
		distance = framed ? FRAMED_NEAREST : WINDOW_NEAREST;
	if (on_visor)
	{
		aim_direction = *head_forward;
		distance = view_distance;
	}
	aim.x = centre.x + aim_direction.i * distance * units;
	aim.y = centre.y + aim_direction.j * distance * units;
	aim.z = centre.z + aim_direction.k * distance * units;

	/* its size: the window vr.smartlink_size wide at its distance (as it
	opens or closes, that much of it), 16:9, and its glow about it */
	if (vr_scope.window_degrees_most > 0.0f && view_degrees > vr_scope.window_degrees_most)
		view_degrees = vr_scope.window_degrees_most;
	tangent = (real)tan(view_degrees * 0.5f * _pi / 180.0f) * MAX(vr_scope.window_size, 0.005f);
	half_width = distance * tangent;
	glow = WINDOW_GLOW;
	/* (framed: the quad the image's square, the scope's frame within it,
	the widest frame vr.smartlink_size wide, the others to its scale; the
	window within it the HUD's size, or more about the widest frame) */
	if (framed)
	{
		tangent *= vr_scope_definitions[scope].frame / FRAMED_WIDEST;
		half_width = distance * tangent;
		glow = 1.0f / vr_scope_definitions[scope].frame - 1.0f;
	}

	/* its view: the weapon's sight's, from the controller along the aim's
	line, its reticle in its middle on what the line meets, however far: not
	from between the eyes toward that point, which jumps along the line as it
	crosses an edge (from a turret 12 m away to the hills behind it), and the
	view with it, by the eyes' parallax (2.4 degrees there, 19 in the view
	at 8x, over a few frames); as wide as the quad over the magnification,
	up as the world's */
	camera->position = line_origin;
	camera->forward = line_direction;
	vr_scope_camera_up(camera, &weapon_camera->up);
	camera->vertical_field_of_view = 2.0f * (real)atan(tangent * (1.0f + glow) / magnification);
	vr_scope_camera_view(camera, rasterizer_camera, framed);

	/* its quad, on the reticle, facing back along the line from between the
	eyes to it, up as the world's: the head's turn and tilt leave it as it
	is in the world, level with the horizon, however the weapon is held */
	layer_camera.forward = aim_direction;
	vr_scope_camera_up(&layer_camera, &weapon_camera->up);
	vr_render_world_to_head(&aim, &layer_camera.forward, &layer_camera.up, layer.position, layer.forward, layer.up);
	layer.eye = -1;
	layer.size = 2.0f * half_width * (1.0f + glow);
	layer.height = (WINDOW_HEIGHT + glow) / (1.0f + glow);
	layer.window[0] = 1.0f / (1.0f + glow);
	layer.window[1] = WINDOW_HEIGHT / (1.0f + glow);
	layer.corner = -1.0f;
	layer.dim = 0.0f;
	layer.feather = 0.0f;
	if (framed)
	{
		real image_tangent = tangent * (1.0f + glow);
		real hud_tangent = MAX((real)tan(hud_degrees * 0.5f * _pi / 180.0f),
			(real)tan(view_degrees * 0.5f * _pi / 180.0f) * FRAMED_AREA) * MAX(vr_scope.window_size, 0.005f);

		layer.height = 1.0f;
		layer.window[0] = MIN(hud_tangent / image_tangent, 1.0f);
		layer.window[1] = MIN(hud_tangent * 0.75f / image_tangent, 1.0f);
		layer.corner = FRAMED_CORNER;
		layer.dim = FRAMED_DIM * vr_scope.window_opening;
		layer.feather = (real)tan(FRAMED_FEATHER_DEGREES * _pi / 180.0f) * MAX(vr_scope.window_size, 0.005f) /
			image_tangent;
	}
	layer.reticle[0] = layer.reticle[1] = 0.0f;
	layer.zoom = vr_scope_label(scope, zoom_level, vr_scope.window_magnification);
	layer.zoom_height = 0.0f;
	layer.opened_seconds = vr_scope.window_opening;
	layer.closing = vr_scope.window_closing;
	layer.switched_seconds = vr_scope.window_switched;

	/* the readouts: the range to what the aim meets now (from the weapon),
	and the rounds */
	layer.range = vr_render_scope_range();
	layer.rounds_loaded = layer.rounds_carried = -1;
	if (weapon_definition_get(object_get(weapon_index)->definition_index)->weapon.magazines.count > 0)
	{
		struct weapon_magazine const *magazine = &weapon_get(weapon_index)->weapon.magazines[0];

		layer.rounds_loaded = magazine->rounds_loaded;
		layer.rounds_carried = magazine->rounds_total;
	}
	halo_vr_scope(&layer);
	if (!vr_scope.smartlink_shown || zoom_level != vr_scope.smartlink_level)
	{
		platform_log("vr scope: smart-link %s at %gx (level %d, magnified %.1f times), a window %.0f degrees wide at "
			"%.1f m", tag_get_name(object_get(weapon_index)->definition_index), layer.zoom, zoom_level,
			vr_scope.window_magnification,
			view_degrees, distance);
	}
	vr_scope.smartlink_shown = TRUE;
	vr_scope.smartlink_level = zoom_level;
	return TRUE;
}

/* the smart-link's window in a vehicle's seat that aims the vehicle's
weapon (vr_scope_seat_weapon), seen from the seat (vr.vehicle_view =
"first_person"): as on foot, about the reticle VR draws for the seat, the
right controller's line meeting the world (which the seat's weapon turns
to: the Scorpion's turret, a gun turret), at the seat's zoom level
(vr_scope_zoom steps it), its readouts the vehicle weapon's rounds (the
Scorpion's shells; none for a plasma weapon's charge). The game's zoom is
not changed (the seated player holds no weapon that zooms): the view alone
is magnified, as the netcode sees. TRUE as its view is drawn (or its
shaders made: vr_scope_warm). */
static boolean vr_scope_seat(struct render_camera *camera, struct render_camera *rasterizer_camera)
{
	struct render_camera seat_camera;
	real_point3d eyes[2];
	real_vector3d head_forward;
	real units, magnifications[MAXIMUM_SEAT_LEVELS], magnification = 1.0f;
	long weapon_index = vr_scope_seat_weapon();
	short count, zoom_level = vr_scope.seat_level;

	if (weapon_index == NONE || weapon_index != vr_scope.seat_weapon_index ||
		!vr_render_scope_seat_view(eyes, &head_forward, &units))
	{
		vr_scope.seat_level = NONE;
		return FALSE;
	}
	count = vr_scope_seat_levels(weapon_index, magnifications);
	if (zoom_level != NONE && zoom_level < count)
		magnification = magnifications[zoom_level];
	else
		zoom_level = NONE;
	vr_scope.weapon_index = weapon_index;
	vr_scope.scope = NONE;
	/* (no weapon in hand: up as the world's, and the head's forward where
	the aim has no direction) */
	csmemset(&seat_camera, 0, sizeof(seat_camera));
	seat_camera.forward = head_forward;
	seat_camera.up = *global_up3d;
	/* (in a feed cockpit, the window within its display: 85 degrees across
	and 40 high) */
	{
		real grate, wash, off;

		vr_scope.window_degrees_most = vr_seat_feed(&grate, &wash, &off) ? FEED_WINDOW_DEGREES : 0.0f;
	}
	return vr_scope_smartlink(camera, rasterizer_camera, weapon_index, NONE, zoom_level, magnification, &seat_camera,
		eyes, &head_forward, units);
}

/* render.c, before the eye pass (after vr_render_camera): whether the
scope's view is drawn, and if so, its camera (both the render's and the
rasterizer's, from the window's): its view the middle 480x480 of the screen.
Smart-linked, vr_scope_smartlink's. At the eye, while a scope is raised to
an eye: from its eyepiece along the weapon, as wide as the lens is from the
eye over the zoom's magnification. Tells vr.c where its lens is. */
boolean vr_scope_camera(struct render_camera *camera, struct render_camera *rasterizer_camera)
{
	struct render_camera weapon_camera;
	struct vr_scope_definition const *definition;
	struct halo_vr_scope layer;
	real_point3d eyes[2], eyepiece;
	real_vector3d head_forward, left;
	real units, along[2], off_axis[2], tangent, magnification = 1.0f, angle, radius, lens_degrees;
	long weapon_index = vr_scope_weapon();
	int mode = halo_vr_scopes();
	short scope = vr_scope_find(weapon_index), eye, zoom_level;
	boolean smartlink = mode == HALO_VR_SCOPES_SMARTLINK && weapon_index != NONE &&
		weapon_definition_get(object_get(weapon_index)->definition_index)->weapon.zoom_level_count > 0;

	halo_vr_scope(NULL);
	vr_scope.window_degrees_most = 0.0f;
	if (weapon_index == NONE && vr_scope_seat(camera, rasterizer_camera))
		return TRUE;
	if ((scope == NONE && !smartlink) || !vr_render_scope_view(&weapon_camera, eyes, &head_forward, &units))
	{
		if (vr_scope.raised)
			platform_log("vr scope: lowered (no scope in hand)");
		if (vr_scope.smartlink_shown)
			platform_log("vr scope: smart-link unzoomed (no weapon that zooms in hand)");
		vr_scope.weapon_index = NONE;
		vr_scope.scope = NONE;
		vr_scope.raised = FALSE;
		vr_scope.smartlink_shown = FALSE;
		return FALSE;
	}
	if (weapon_index != vr_scope.weapon_index)
	{
		vr_scope.raised = FALSE;
		vr_scope.weapon_index = weapon_index;
		vr_scope.scope = scope;
	}
	if (smartlink)
	{
		zoom_level = player_control_get_zoom_level(0);
		magnification = zoom_level != NONE ? weapon_get_zoom_magnification(weapon_index, zoom_level) : 1.0f;
		return vr_scope_smartlink(camera, rasterizer_camera, weapon_index, scope, zoom_level, magnification,
			&weapon_camera, eyes, &head_forward, units);
	}
	vr_scope.smartlink_shown = FALSE;
	definition = &vr_scope_definitions[scope];
	cross_product3d(&weapon_camera.up, &weapon_camera.forward, &left);
	normalize3d(&left);
	/* (a left-hander's weapon is drawn mirrored about the plane of the
	camera's forward and up: its scope on the camera's other side,
	vr_render_weapon_mirrored) */
	if (halo_vr_left_handed())
		negate_vector3d(&left, &left);
	radius = definition->radius / METRES_PER_WORLD_UNIT;
	{
		real f = definition->eyepiece[0] / METRES_PER_WORLD_UNIT, l = definition->eyepiece[1] / METRES_PER_WORLD_UNIT,
			u = definition->eyepiece[2] / METRES_PER_WORLD_UNIT;

		eyepiece.x = weapon_camera.position.x + weapon_camera.forward.i * f + left.i * l + weapon_camera.up.i * u;
		eyepiece.y = weapon_camera.position.y + weapon_camera.forward.j * f + left.j * l + weapon_camera.up.j * u;
		eyepiece.z = weapon_camera.position.z + weapon_camera.forward.k * f + left.k * l + weapon_camera.up.k * u;
	}

	/* raised: an eye behind the eyepiece, within its relief, near its axis
	(the nearer), the scope along about where the head looks */
	for (eye = 0; eye < 2; eye++)
	{
		real_vector3d from_eye;
		real across;

		from_eye.i = eyepiece.x - eyes[eye].x;
		from_eye.j = eyepiece.y - eyes[eye].y;
		from_eye.k = eyepiece.z - eyes[eye].z;
		along[eye] = dot_product3d(&from_eye, &weapon_camera.forward) / units;
		across = magnitude_squared3d(&from_eye) / (units * units) - along[eye] * along[eye];
		off_axis[eye] = across > 0.0f ? (real)sqrt(across) : 0.0f;
	}
	eye = off_axis[1] < off_axis[0] ? 1 : 0;
	angle = (real)acos(PIN(dot_product3d(&weapon_camera.forward, &head_forward), -1.0f, 1.0f)) * 180.0f / _pi;
	{
		real margin = vr_scope.raised ? LOWER_MARGIN : 1.0f;
		boolean raised = along[eye] > -RAISE_IN_FRONT && along[eye] < definition->relief * margin &&
			off_axis[eye] < definition->off_axis * margin && angle < (vr_scope.raised ? LOWER_ANGLE : RAISE_ANGLE);

		if (raised != vr_scope.raised || (raised && eye != vr_scope.eye))
		{
			platform_log("vr scope: %s %s (the %s eye %.1f cm behind the eyepiece, %.1f cm from its axis, %.0f degrees "
				"from the head's forward)", raised ? "raised" : "lowered",
				tag_get_name(object_get(weapon_index)->definition_index), eye ? "right" : "left", along[eye] * 100.0f,
				off_axis[eye] * 100.0f, angle);
		}
		vr_scope.raised = raised;
		vr_scope.eye = eye;
	}
	if (!vr_scope.raised)
		return vr_scope_warm(camera, rasterizer_camera, eyes, &head_forward, &weapon_camera.up);

	/* the lens's half angle from the eye (its tangent: its radius over the
	eye's distance from its plane, no less than the scope's smallest), over
	the magnification */
	{
		real distance = MAX(along[eye], NEAREST_EYE);
		real smallest = (real)tan(definition->smallest_angle * _pi / 180.0f) * distance * units;

		if (radius < smallest)
			radius = smallest;
		tangent = radius / (distance * units);
	}
	lens_degrees = 2.0f * (real)atan(tangent) * 180.0f / _pi;
	zoom_level = player_control_get_zoom_level(0);
	if (zoom_level != NONE)
		magnification = weapon_get_zoom_magnification(weapon_index, zoom_level);
	tangent /= magnification;

	camera->position = eyepiece;
	camera->forward = weapon_camera.forward;
	camera->up = weapon_camera.up;
	camera->vertical_field_of_view = 2.0f * (real)atan(tangent);
	vr_scope_camera_view(camera, rasterizer_camera, FALSE);

	/* the lens, for vr.c's layer */
	vr_render_world_to_head(&eyepiece, &weapon_camera.forward, &weapon_camera.up, layer.position, layer.forward,
		layer.up);
	layer.eye = eye;
	layer.size = 2.0f * radius / units;
	layer.zoom = vr_scope_label(scope, zoom_level, magnification);
	layer.zoom_height = PIN(ZOOM_DEGREES / MAX(lens_degrees, 1.0f), ZOOM_LEAST, ZOOM_MOST);
	layer.reticle[0] = layer.reticle[1] = 0.0f;
	layer.height = 1.0f;
	layer.window[0] = layer.window[1] = 0.0f;
	layer.range = 0.0f;
	layer.rounds_loaded = layer.rounds_carried = -1;
	layer.opened_seconds = layer.closing = layer.switched_seconds = 0.0f;
	layer.corner = -1.0f;
	layer.dim = layer.feather = 0.0f;
	halo_vr_scope(&layer);
	return TRUE;
}

/* render.c's scope pass, over its view: the framed smart-link window's
weapon's zoomed view as the flat screen draws it (its screen effect: the
scope's mask; hud_draw_vr_zoomed: its ticks and zoom label), laid out on
the 640x480 screen its view is (vr_scope_camera_view), as there. Not for a
seat's window (no weapon of the player's zooms) */
void vr_scope_draw_hud(void)
{
	/* (its crosshairs at the screen's middle, which is the aim: not moved
to where the controller points, nor hidden, as the HUD's layer has them,
vr_render.c) */
	extern short vr_render_crosshair_offset[2];
	short crosshair_offset[2];

	if (vr_scope.smartlink_shown && vr_scope.scope != NONE && vr_scope_seat_weapon() == NONE &&
		player_control_get_zoom_level(0) != NONE)
	{
		crosshair_offset[0] = vr_render_crosshair_offset[0];
		crosshair_offset[1] = vr_render_crosshair_offset[1];
		vr_render_crosshair_offset[0] = vr_render_crosshair_offset[1] = 0;
		interface_draw_vr_scope_screen_effect();
		hud_draw_vr_zoomed();
		vr_render_crosshair_offset[0] = crosshair_offset[0];
		vr_render_crosshair_offset[1] = crosshair_offset[1];
	}
}

/* vr_render.c: a scope is raised to an eye (its reticle is the aim) */
boolean vr_scope_raised(void)
{
	return vr_scope.raised && vr_scope.weapon_index != NONE;
}

/* hud_weapon.c: a scope's view is shown, raised to an eye or smart-linked
on the visor, which shows the zoom (the HUD's zoomed view is not drawn) */
boolean vr_scope_shown(void)
{
	return vr_scope.weapon_index != NONE && (vr_scope.raised || vr_scope.smartlink_shown);
}

/* models.c, as each part of an object's model is drawn: whether this part
(its shader's tag) is the display on the back of the raised scope, not
drawn (the weapon's own, in its first-person model, which only the player
holding it draws) */
boolean vr_scope_hides_part(long object_index, long shader_index)
{
	char const *screen, *name;

	if (!vr_scope.raised || object_index == NONE || object_index != vr_scope.weapon_index || vr_scope.scope == NONE ||
		shader_index == NONE)
	{
		return FALSE;
	}
	screen = vr_scope_definitions[vr_scope.scope].screen;
	name = screen ? tag_get_name(shader_index) : NULL;
	return name && strstr(name, screen);
}

/* debug.vr_test_weapons: "<name>[, <name>]", the weapons whose tag names
have these in them, the first in hand, in place of the unit's own, once a
map, two seconds after the player first aims (a level's script gives its
own as the player takes control), for automated tests of the scopes; TRUE
as they are given (the unit's weapons before are gone) */
static boolean vr_scope_test_weapons(long unit_index, boolean aiming)
{
	char text[256], *name, *next;
	boolean first = TRUE;
	short slot;

	if (game_time_get() < 2)
	{
		vr_scope.test_weapons_given = FALSE;
		vr_scope.test_weapons_tick = NONE;
	}
	if (vr_scope.test_weapons_given || unit_index == NONE || !*config_string("debug.vr_test_weapons"))
		return FALSE;
	if (vr_scope.test_weapons_tick == NONE && aiming)
		vr_scope.test_weapons_tick = game_time_get();
	if (vr_scope.test_weapons_tick == NONE || game_time_get() < vr_scope.test_weapons_tick + 2 * TICKS_PER_SECOND)
		return FALSE;
	vr_scope.test_weapons_given = TRUE;
	snprintf(text, sizeof(text), "%s", config_string("debug.vr_test_weapons"));
	unit_delete_all_weapons(unit_index);
	for (name = text; name; name = next)
	{
		struct tag_iterator iterator;
		long tag_index;

		next = strchr(name, ',');
		if (next)
			*next++ = 0;
		while (*name == ' ')
			name++;
		tag_iterator_new(&iterator, WEAPON_DEFINITION_TAG);
		for (tag_index = tag_iterator_next(&iterator); tag_index != NONE; tag_index = tag_iterator_next(&iterator))
		{
			char const *tag_name = tag_get_name(tag_index);

			if (tag_name && *name && strstr(tag_name, name))
				break;
		}
		if (tag_index != NONE)
		{
			struct object_placement_data placement_data;
			long weapon_index;

			object_placement_data_new(&placement_data, tag_index, NONE);
			weapon_index = object_new(&placement_data);
			if (weapon_index != NONE && unit_add_weapon_to_inventory(unit_index, weapon_index, first ? 2 : 0))
			{
				platform_log("vr scope: test weapon %s", tag_get_name(tag_index));
				/* (the first in hand: the player's control asks for its slot) */
				for (slot = 0; first && slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
				{
					if (unit_inventory_get_weapon(unit_index, slot) == weapon_index)
						player_control_get(0)->desired_weapon_index = slot;
				}
				first = FALSE;
			}
			else if (weapon_index != NONE)
			{
				object_delete(weapon_index);
			}
		}
		else
		{
			platform_log("vr scope: no weapon \"%s\" in this map", name);
		}
	}
	/* (the holster's weapon, told before, may be gone) */
	vr_holster_update(unit_index, aiming);
	return TRUE;
}

/* player_control.c, each update of the first local player (aiming: the
VR mode aims, on foot): the zoom level the player's control has from now
on. Smart-linked, the game's own: the click steps through the levels and
then unzooms, wherever the weapon is, as on the flat screen. At the eye,
with a scope in hand and the controller aiming, the scope's: none while it
is lowered, the weapon's last (or its first) as it is raised, and while
raised the click steps through the levels, from the last back to the first
(the game's step from the last unzooms); what unzooms the game (a hit, a
reload) leaves it unzoomed. Else the game's own. */
short vr_scope_zoom(long unit_index, long weapon_index, short zoom_level, boolean pressed, boolean aiming)
{
	boolean raised;

	if (vr_scope_test_weapons(unit_index, aiming))
		return NONE;
	/* in a vehicle's seat that aims its weapon: the seat's level, which the
	click steps through, from the last to none (the game's is left as it
	is: no weapon of the player's zooms there) */
	{
		long seat_weapon_index = vr_scope_seat_weapon();

		if (seat_weapon_index != vr_scope.seat_weapon_index)
			vr_scope.seat_level = NONE;
		vr_scope.seat_weapon_index = seat_weapon_index;
		if (seat_weapon_index != NONE && aiming && pressed)
		{
			real magnifications[MAXIMUM_SEAT_LEVELS];
			short count = vr_scope_seat_levels(seat_weapon_index, magnifications);

			vr_scope.seat_level = vr_scope.seat_level == NONE ? (count > 0 ? 0 : NONE) :
				vr_scope.seat_level + 1 < count ? vr_scope.seat_level + 1 : NONE;
			platform_log("vr scope: %s's seat zoom level %d of %d",
				tag_get_name(object_get(seat_weapon_index)->definition_index), vr_scope.seat_level, count);
		}
		else if (!aiming)
		{
			vr_scope.seat_level = NONE;
		}
	}
	if (!aiming || weapon_index == NONE || unit_index == NONE || weapon_index != vr_scope_weapon() ||
		vr_scope_find(weapon_index) == NONE || halo_vr_scopes() != HALO_VR_SCOPES_EYE)
	{
		vr_scope.zoomed = FALSE;
		return zoom_level;
	}
	raised = vr_scope.raised && vr_scope.weapon_index == weapon_index;
	if (!raised)
	{
		if (vr_scope.zoomed)
		{
			vr_scope.remembered_weapon_index = weapon_index;
			vr_scope.remembered_level = zoom_level;
		}
		vr_scope.zoomed = FALSE;
		return NONE;
	}
	if (!vr_scope.zoomed)
	{
		vr_scope.zoomed = TRUE;
		if (vr_scope.remembered_weapon_index == weapon_index && vr_scope.remembered_level != NONE)
			return vr_scope.remembered_level;
		return 0;
	}
	if (pressed && zoom_level == NONE)
		return 0;
	return zoom_level;
}

#endif
