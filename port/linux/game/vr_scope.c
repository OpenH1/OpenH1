/*
VR_SCOPE.C

The VR mode's scopes (HALO_VR; vr.scopes, port/linux/README.md "VR"): the
sniper rifle's and the pistol's, for render.c, vr_render.c and
player_control.c.

On the flat screen the game zooms by narrowing the whole view. In the
headset the eyes' view is the head's and cannot narrow: a scope magnifies
the view within it instead, as a real one does, when it is raised to an
eye, and the rest of the view stays as it was. Its view is drawn before the
eyes' (render.c's scope pass), from the scope's eyepiece along the aim,
into an image of its own (d3d8_gl.c), which the compositor shows on a quad
at the eyepiece, to that eye alone (vr.c, host_vr.c): a quad layer, which
the runtime reprojects to the head's latest pose as it does the eyes'.

The view in the scope is as many times larger as the game's zoom
magnifies (2 and 8 for the sniper rifle, whose HUD says "10x"; 2 for the
pistol), as the lens is seen from the eye: its field of view is the angle
the lens spans from the eye over the magnification, so the image is that
much larger than the world about it, wherever the eye is. The lens shows
the zoom as the HUD labels it; the HUD's zoomed view is not drawn while a
scope is raised (hud_weapon.c), nor the display on the back of the sniper
rifle's scope, about the lens at the eye.

A scope is raised when an eye looks through it (behind its eyepiece,
within its eye relief, near its axis) and it points about where the head
looks; then the game zooms (the player's zoom level, which the netcode
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
#include "items/weapon_definitions.h"
#include "items/weapons.h"
#include "objects/objects.h"
#include "render/render_cameras.h"
#include "tag_files/tag_files.h"
#include "units/units.h"

#include "../src/vr.h"

#include <math.h>
#include <stdio.h>
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
(d3d8_gl.c copies those into its image) */
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

/* a scope: the weapon whose tag this is, the middle of its eyepiece from
the camera the first-person weapon is posed from (forward, left, up), and
its lens's radius, in metres as the first-person model has them (its world
units, as large at any vr.world_scale as the world is); its eye relief, how
far behind the eyepiece an eye looks through it, and how far from its axis
(metres); the smallest half angle (degrees) its lens spans from the eye;
what the names of the shaders of the display on the back of its scope have
in them (NULL: none), not drawn while it is raised; and its zoom levels'
labels, which its lens shows as the game's HUD does (its zoom's sprites:
the sniper rifle's second level magnifies 8 times, and says "10x"; 0: the
magnification).
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
itself (d3d8_gl.c). */
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
};

static struct vr_scope_definition const vr_scope_definitions[] =
{
	{ "weapons\\sniper rifle\\sniper rifle", { 0.26f, -0.13f, -0.058f }, 0.025f, 0.12f, 0.07f, 0.0f, "screen", { 2, 10 } },
	{ "weapons\\pistol\\pistol", { 0.545f, -0.057f, -0.101f }, 0.013f, 0.65f, 0.04f, 6.0f, NULL, { 2, 0 } },
};

void platform_log(char const *format, ...);
char const *config_string(char const *name);
struct player_control *player_control_get(short local_player_index);
/* vr_holster.c's */
void vr_holster_update(long unit_index, boolean aiming);
/* vr_render.c's */
boolean vr_render_scope_view(struct render_camera *weapon_camera, real_point3d eyes[2], real_vector3d *head_forward,
	real *units);
void vr_render_world_to_head(real_point3d const *point, real_vector3d const *forward, real_vector3d const *up,
	float position[3], float head_forward[3], float head_up[3]);

static struct
{
	/* the weapon in hand whose scope was placed last frame, its scope (an
	index of vr_scope_definitions), and whether it is raised, to which eye */
	long weapon_index;
	short scope;
	boolean raised;
	short eye;
	/* the zoom: the scope raised as the player's control saw it last, and
	the level the weapon had as it was lowered */
	boolean zoomed;
	long remembered_weapon_index;
	short remembered_level;
	/* the game's tick the scope's pass was first drawn at in this map, to
	make its shaders (NONE: not yet) */
	long warmed_tick;
	/* debug.vr_test_weapons: given, and the game's tick the player was
	first aiming at (NONE: not yet) */
	boolean test_weapons_given;
	long test_weapons_tick;
} vr_scope = { NONE, NONE, FALSE, 0, FALSE, NONE, NONE, NONE, FALSE, NONE };

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

/* the scope's camera's view: the middle 480x480 of the screen, which
d3d8_gl.c copies into its image; the rasterizer's camera the same */
static void vr_scope_camera_view(struct render_camera *camera, struct render_camera *rasterizer_camera)
{
	camera->mirrored = FALSE;
	camera->viewport_bounds.x0 = SCOPE_VIEW_LEFT;
	camera->viewport_bounds.x1 = SCOPE_VIEW_RIGHT;
	camera->viewport_bounds.y0 = 0;
	camera->viewport_bounds.y1 = 480;
	camera->window_bounds = camera->viewport_bounds;
	*rasterizer_camera = *camera;
}

/* render.c, before the eye pass (after vr_render_camera): whether a scope
is raised to an eye, and if so, its camera (both the render's and the
rasterizer's, from the window's): from its eyepiece along the weapon, its
view the middle 480x480 of the screen, as wide as the lens is from the eye
over the zoom's magnification. Tells vr.c where its lens is. */
boolean vr_scope_camera(struct render_camera *camera, struct render_camera *rasterizer_camera)
{
	struct render_camera weapon_camera;
	struct vr_scope_definition const *definition;
	struct halo_vr_scope layer;
	real_point3d eyes[2], eyepiece;
	real_vector3d head_forward, left;
	real units, along[2], off_axis[2], tangent, magnification = 1.0f, angle, radius, lens_degrees;
	long weapon_index = vr_scope_weapon();
	short scope = vr_scope_find(weapon_index), eye, zoom_level;

	halo_vr_scope(NULL);
	if (scope == NONE || !vr_render_scope_view(&weapon_camera, eyes, &head_forward, &units))
	{
		if (vr_scope.raised)
			platform_log("vr scope: lowered (no scope in hand)");
		vr_scope.weapon_index = NONE;
		vr_scope.scope = NONE;
		vr_scope.raised = FALSE;
		return FALSE;
	}
	if (weapon_index != vr_scope.weapon_index)
	{
		vr_scope.raised = FALSE;
		vr_scope.weapon_index = weapon_index;
		vr_scope.scope = scope;
	}
	definition = &vr_scope_definitions[scope];
	cross_product3d(&weapon_camera.up, &weapon_camera.forward, &left);
	normalize3d(&left);
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
	{
		/* the scope's shaders made the first time a scope is in hand in a
		map, not the first time it is raised (its pass draws without the
		eyes' multiview, so in programs of its own, which a driver makes as
		they are first drawn with: half a second on spark the first time,
		longer where a driver makes them slowly): its pass drawn once, a
		wide view where the head looks, and not shown */
		if (game_time_get() < vr_scope.warmed_tick)
			vr_scope.warmed_tick = NONE;
		if (vr_scope.warmed_tick != NONE)
			return FALSE;
		vr_scope.warmed_tick = game_time_get();
		camera->position.x = (eyes[0].x + eyes[1].x) * 0.5f;
		camera->position.y = (eyes[0].y + eyes[1].y) * 0.5f;
		camera->position.z = (eyes[0].z + eyes[1].z) * 0.5f;
		camera->forward = head_forward;
		camera->up = *global_up3d;
		{
			real along_up = dot_product3d(&camera->up, &camera->forward);

			camera->up.i -= camera->forward.i * along_up;
			camera->up.j -= camera->forward.j * along_up;
			camera->up.k -= camera->forward.k * along_up;
			if (normalize3d(&camera->up) <= 0.0f)
				camera->up = weapon_camera.up;
		}
		camera->vertical_field_of_view = WARM_FIELD_OF_VIEW * _pi / 180.0f;
		vr_scope_camera_view(camera, rasterizer_camera);
		platform_log("vr scope: its shaders made ready (its view drawn once, unseen)");
		return TRUE;
	}

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
	vr_scope_camera_view(camera, rasterizer_camera);

	/* the lens, for vr.c's layer */
	vr_render_world_to_head(&eyepiece, &weapon_camera.forward, &weapon_camera.up, layer.position, layer.forward,
		layer.up);
	layer.eye = eye;
	layer.size = 2.0f * radius / units;
	layer.zoom = zoom_level == NONE ? 0.0f :
		zoom_level < (short)NUMBEROF(definition->labels) && definition->labels[zoom_level] ? definition->labels[zoom_level] :
		magnification;
	layer.zoom_height = PIN(ZOOM_DEGREES / MAX(lens_degrees, 1.0f), ZOOM_LEAST, ZOOM_MOST);
	halo_vr_scope(&layer);
	return TRUE;
}

/* vr_render.c: a scope is raised to an eye (its reticle is the aim) */
boolean vr_scope_raised(void)
{
	return vr_scope.raised && vr_scope.weapon_index != NONE;
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
on. With a scope in hand and the controller aiming, the scope's: none while
it is lowered, the weapon's last (or its first) as it is raised, and while
raised the click steps through the levels, from the last back to the first
(the game's step from the last unzooms); what unzooms the game (a hit, a
reload) leaves it unzoomed. Else the game's own. */
short vr_scope_zoom(long unit_index, long weapon_index, short zoom_level, boolean pressed, boolean aiming)
{
	boolean raised;

	if (vr_scope_test_weapons(unit_index, aiming))
		return NONE;
	if (!aiming || weapon_index == NONE || unit_index == NONE || weapon_index != vr_scope_weapon() ||
		vr_scope_find(weapon_index) == NONE)
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
