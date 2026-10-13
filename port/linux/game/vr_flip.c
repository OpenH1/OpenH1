/*
VR_FLIP.C

The VR mode's flip of an overturned vehicle by a lift of both hands (HALO_VR;
vr.vehicle_flip, port/linux/README.md "VR"): the game's side of the gesture
that port/linux/src/vr.c sees, for player_control.c and hud.c.

The game offers a player on foot the flip of a vehicle that lies on its side
or its roof, with no driver, beside them (players.c,
player_examine_nearby_vehicle): the flip outranks any other action there, and
the action button (X) flips it, its HUD prompt saying so. In the VR mode the
hands flip it instead (vr.vehicle_flip = "lift", the default): both grips
taken low beside it, as of a bumper, and both hands lifted together, fast,
which presses the game's action as X would (vr.c). The action is the game's,
through its input and the netcode as ever: as a network game's client, the
host flips the vehicle as it would for X.

So that X alone does not flip it, X's action is a reload while the game
offers a flip and only the lift flips (the reload X gives where there is
nothing else to do: near an overturned vehicle the flip outranked every
other action X had, the pick-ups, the seats, so X loses nothing else). The
prompt says to lift it, in the HUD's own state message.
*/

#ifdef HALO_VR

#include "cseries.h"
#include "math/real_math.h"
#include "ai/ai_script.h"
#include "cache/cache_files.h"
#include "game/game.h"
#include "game/players.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "physics/collision_usage.h"
#include "physics/collisions.h"
#include "tag_files/tag_files.h"
#include "text/unicode.h"
#include "units/units.h"
#include "units/vehicle_definitions.h"

#include "../src/vr.h"

#include <stdio.h>
#include <string.h>

/* port/linux/src/port_config.c */
char const *config_string(char const *name);
/* port/linux/game/vr_render.c */
boolean vr_render_world_place(real_point3d const *point, float place[3]);

void platform_log(char const *format, ...);

/* debug.vr_test_flip: the vehicle placed this many seconds after the
player first aims */
#define VR_FLIP_TEST_SECONDS 3

static struct
{
	/* the vehicle on offer last logged */
	long logged_vehicle_index;
	/* debug.vr_test_flip: when the player first aimed (NONE: not yet, on
	this map), and the vehicle placed */
	long test_first_aim_tick;
	boolean test_placed;
	long test_vehicle_index;
	/* X's action, made a reload, logged (until let go) */
	boolean button_logged;
} vr_flip = { NONE, NONE, FALSE, NONE, FALSE };

/* debug.vr_test_flip: "<name> [roof|side|upright] [<metres ahead>]", a
vehicle whose tag name has name in it, overturned before the first player
(on its roof, or its side; or upright, to enter) VR_FLIP_TEST_SECONDS after they first aim, once a map, near enough
to flip (its middle as far ahead as the player's and its bounding spheres
reach, by default), the level's AI erased, for automated tests of the lift */
static void vr_flip_test_vehicle(long unit_index, boolean aiming)
{
	char name[64] = "", pose[16] = "roof";
	real ahead = 0.0f;
	long now = game_time_get();
	struct unit_datum *unit;
	struct tag_iterator iterator;
	struct object_placement_data placement;
	struct collision_result collision;
	real_vector3d forward, down = { 0.0f, 0.0f, -6.0f / 3.048f };
	real_point3d above;
	long tag_index, vehicle_index;

	if (sscanf(config_string("debug.vr_test_flip"), "%63s %15s %f", name, pose, &ahead) < 1 || unit_index == NONE)
		return;
	if (vr_flip.test_first_aim_tick != NONE && now < vr_flip.test_first_aim_tick)
	{
		/* (a new map, or a revert) */
		vr_flip.test_first_aim_tick = NONE;
		vr_flip.test_placed = FALSE;
		vr_flip.test_vehicle_index = NONE;
	}
	if (vr_flip.test_first_aim_tick == NONE && aiming)
		vr_flip.test_first_aim_tick = now;
	if (vr_flip.test_first_aim_tick == NONE)
		return;
	/* (how it lies, each second: upright again once flipped; and whether
	the player is in it) */
	if (vr_flip.test_vehicle_index != NONE && (now - vr_flip.test_first_aim_tick) % TICKS_PER_SECOND == 0)
	{
		struct object_datum *vehicle = object_try_and_get(vr_flip.test_vehicle_index);

		if (vehicle)
			platform_log("vr flip: test vehicle's up %.2f %.2f %.2f at tick %ld%s", vehicle->object.up.i,
				vehicle->object.up.j, vehicle->object.up.k, now,
				unit_get(unit_index)->object.parent_object_index == vr_flip.test_vehicle_index ? ", the player in it" : "");
	}
	/* (the level's own AI erased, once a second: nothing shoots the player
	or drives off in the vehicle) */
	if ((now - vr_flip.test_first_aim_tick) % TICKS_PER_SECOND == 0)
		ai_scripting_erase_all();
	if (vr_flip.test_placed || now < vr_flip.test_first_aim_tick + VR_FLIP_TEST_SECONDS * TICKS_PER_SECOND)
		return;
	vr_flip.test_placed = TRUE;
	tag_iterator_new(&iterator, VEHICLE_DEFINITION_TAG);
	for (tag_index = tag_iterator_next(&iterator); tag_index != NONE; tag_index = tag_iterator_next(&iterator))
	{
		char const *tag_name = tag_get_name(tag_index);

		if (tag_name && strstr(tag_name, name))
			break;
	}
	unit = unit_get(unit_index);
	forward = unit->object.forward;
	forward.k = 0.0f;
	if (tag_index == NONE || normalize3d(&forward) <= 0.0f)
	{
		platform_log("vr flip: no test vehicle \"%s\" in this map", name);
		return;
	}
	if (ahead <= 0.0f)
	{
		ahead = (object_definition_get(tag_index)->object.bounding_radius + unit->object.bounding_sphere_radius) *
			0.75f;
	}
	else
	{
		ahead /= 3.048f;
	}
	/* (broadside to the player, on the ground there, a little above it) */
	object_placement_data_new(&placement, tag_index, NONE);
	placement.position.x = unit->object.position.x + forward.i * ahead;
	placement.position.y = unit->object.position.y + forward.j * ahead;
	placement.position.z = unit->object.position.z;
	above = placement.position;
	above.z += 3.0f / 3.048f;
	if (global_current_collision_user_depth < MAXIMUM_COLLISION_USER_STACK_DEPTH)
	{
		global_current_collision_users[global_current_collision_user_depth++] = _collision_user_ui;
		if (collision_test_vector(FLAG(_collision_test_structure_bit), &above, &down, NONE, &collision))
			placement.position.z = collision.point.z;
		--global_current_collision_user_depth;
	}
	placement.position.z += 0.5f / 3.048f;
	placement.forward.i = -forward.j;
	placement.forward.j = forward.i;
	placement.forward.k = 0.0f;
	if (!strcmp(pose, "upright"))
	{
		placement.up = *global_up3d;
	}
	else if (!strcmp(pose, "side"))
	{
		/* (on its left side: its up toward the player) */
		placement.up.i = -forward.i;
		placement.up.j = -forward.j;
		placement.up.k = 0.0f;
	}
	else
	{
		placement.up.i = 0.0f;
		placement.up.j = 0.0f;
		placement.up.k = -1.0f;
	}
	vehicle_index = object_new(&placement);
	vr_flip.test_vehicle_index = vehicle_index;
	platform_log("vr flip: test vehicle %s %s, %.2f m ahead%s", tag_get_name(tag_index),
		!strcmp(pose, "upright") ? "upright" : !strcmp(pose, "side") ? "on its side" : "on its roof", ahead * 3.048f,
		vehicle_index == NONE ? ": not made" : "");
}

/* player_control.c, as the first local player's input is read (the
player's unit, and the flags the input has set): the flip of an overturned
vehicle as vr.vehicle_flip says. While the game offers a flip, X's action
reloads instead where only the lift flips, and the lift presses the action */
void vr_flip_input(long player_index, unsigned long *unit_control_flags)
{
	struct player_datum *player = player_get(player_index);
	struct unit_datum *unit = player->unit_index != NONE ? unit_get(player->unit_index) : NULL;
	struct halo_vr_flip flip;
	struct object_datum *vehicle = NULL;

	memset(&flip, 0, sizeof(flip));
	vr_flip_test_vehicle(player->unit_index, halo_vr_aiming());
	if (unit && unit->object.parent_object_index == NONE && !TEST_FLAG(unit->object.damage_flags, _object_dead_bit) &&
		player->action_result == _player_action_result_flip_vehicle)
	{
		vehicle = object_try_and_get(player->action_object_index);
	}
	flip.offered = vehicle != NULL;
	if (vehicle)
		flip.vehicle_known = vr_render_world_place(&vehicle->object.bounding_sphere_center, flip.vehicle);
	if (!halo_vr_flip(&flip))
		return;
	if (flip.offered && player->action_object_index != vr_flip.logged_vehicle_index)
	{
		platform_log("vr flip: %s on offer to flip (%s)", tag_get_name(vehicle->definition_index),
			flip.flips == HALO_VR_FLIP_LIFT ? "by a lift" : flip.flips == HALO_VR_FLIP_BUTTON ? "by X" :
			"by a lift or X");
	}
	vr_flip.logged_vehicle_index = flip.offered ? player->action_object_index : NONE;
	if (!flip.offered)
		return;
	if (!(flip.flips & HALO_VR_FLIP_BUTTON) && TEST_FLAG(*unit_control_flags, _unit_control_action_bit))
	{
		if (!vr_flip.button_logged)
			platform_log("vr flip: X reloads: the lift flips it");
		vr_flip.button_logged = TRUE;
		SET_FLAG(*unit_control_flags, _unit_control_action_bit, FALSE);
		SET_FLAG(*unit_control_flags, _unit_control_weapon_reload_bit, TRUE);
	}
	else
	{
		vr_flip.button_logged = FALSE;
	}
	if (flip.pressed)
		SET_FLAG(*unit_control_flags, _unit_control_action_bit, TRUE);
}

/* hud.c, with the game offering the first player a flip: the prompt to
show instead of the game's (TRUE), when only the lift flips: as the game's
names the vehicle (name: the HUD's item string, empty when it has none) */
boolean vr_flip_prompt(wchar_t const *name, wchar_t *text, long length)
{
	if (halo_vr_flips() != HALO_VR_FLIP_LIFT || length < 2)
		return FALSE;
	ustrncpy(text, L"Grip low with both hands and lift to flip ", length - 1);
	text[length - 1] = 0;
	ustrncat(text, name && *name ? name : L"it", length - 1 - ustrlen(text));
	return TRUE;
}

#endif
