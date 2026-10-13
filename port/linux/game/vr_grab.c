/*
VR_GRAB.C

The VR mode's hands grabbing the world (HALO_VR; vr.grab_world,
port/linux/README.md "VR"): the game's side of what port/linux/src/vr.c sees
the hands do, for player_control.c, players.c and render_objects.c.

Halo's action button (X) does what the game offers the player where they
stand (players.c, player_examine_nearby_objects): a weapon on the ground at
the player's feet is offered to pick up (with one weapon carried) or to swap
for the one in hand (with two), and X held a moment takes it (the swap bit,
player_handle_weapon_swap). In the VR mode the right hand takes it as a hand
would: reached down to the weapon (its grip within 20 cm of it) and gripping
(vr.c, grab_update). The weapon lights up as the hand nears it, as the
holsters' weapons do (vr_holster.c), the controller ticks as the hand gets
there, and the grip presses the game's pick-up, the swap bit, through its
input and the netcode as X held does: as a network game's client, the host
picks it up as it would for X. The weapon taken goes where the game puts it:
in hand, the one held dropped for it, or the one held put in its holster.

The game offers one weapon at a time, the nearest the player's body
(player_set_action_result); with several about the feet it offers the one
the right hand reaches for instead (vr_grab_offer), so that the grip takes
what the hand is at, and the HUD's prompt names it. A network game's client
offers what the host would (the host decides pick-ups, by the body), and its
hand takes only that one. X keeps working as ever. Ammunition is picked up
by walking over it, as ever.

A control the game would let the player use (a device control: a door's
panel, a bridge's, a lift's: "Press X to activate"; in reach of the body,
able to change, facing the eye: player_examine_nearby_device) is pressed by
pushing either hand's fingers into its face (vr.c, press_update), a click
in the hand: the game's action, as X. The game offers a control only along
the player's aim, which in the VR mode is the right controller's, whatever
the hand pressing does; so as the hand presses, the game's aim turns to the
control (vr_grab_facing) until the game offers it, a tick later, and its
action is pressed, as a network game's host decides a client's use of a
control by the client's aim: the host, and the client's copy, both see the
aim on the control as the action comes. The aim is the game's alone (the
reticle, the weapon and the view follow the controller), for a few ticks;
the controls of the campaign are large (a Forerunner holo panel, 2 m
across) and the game mostly offers them at arm's length whatever the aim.
Entering a vehicle stays on X.
*/

#ifdef HALO_VR

#include "cseries.h"
#include "math/real_math.h"
#include "ai/ai_script.h"
#include "cache/cache_files.h"
#include "devices/device_controls.h"
#include "devices/devices.h"
#include "game/game.h"
#include "game/players.h"
#include "items/weapon_definitions.h"
#include "items/weapons.h"
#include "networking/network_game_globals.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "physics/collision_usage.h"
#include "physics/collisions.h"
#include "render/render.h"
#include "tag_files/tag_files.h"
#include "units/units.h"

#include "../src/vr.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* port/linux/src/port_config.c */
char const *config_string(char const *name);
/* port/linux/game/vr_render.c */
boolean vr_render_world_place(real_point3d const *point, float place[3]);
/* port/linux/game/vr_holster.c */
void vr_holster_highlight_object(long unit_index, real distance, real units, struct render_model_effect *model_effect,
	struct render_lighting *lighting);

void platform_log(char const *format, ...);

/* the game offers the weapon the right hand is nearest instead of the one
nearest the body, the hand nearer its reach than this (metres: where it
begins to light up, vr_holster.c's HIGHLIGHT_DISTANCE) */
#define VR_GRAB_PREFER_DISTANCE 0.35f
/* a weapon is the line through its middle along its length, this much of
its bounding sphere's radius either way, and this thick (metres) */
#define VR_GRAB_WEAPON_LENGTH 0.8f
#define VR_GRAB_WEAPON_THICKNESS 0.04f
/* the weapon the grip took is picked up once the game offers it, within
this many ticks (the game offers the hand's weapon a tick after it is the
hand's), the pick-up pressed for this many ticks (the game takes the swap
bit's press, not its hold) */
#define VR_GRAB_WAIT_TICKS 15
#define VR_GRAB_PRESS_TICKS 3
/* a control is pressed within this radius of its face's middle (metres):
half its bounding sphere's, within these */
#define VR_GRAB_CONTROL_SMALLEST 0.06f
#define VR_GRAB_CONTROL_LARGEST 0.5f
/* a control pressed is used once the game offers it, within this many
ticks, the game's aim turned to it meanwhile (the game offers the control
its aim is on), its action pressed for this many ticks and the aim held on
it one more */
#define VR_GRAB_CONTROL_WAIT_TICKS 10
#define VR_GRAB_ACTION_TICKS 2
/* debug.vr_test_grab: placed this many seconds after the player first
aims, this many at most */
#define VR_GRAB_TEST_SECONDS 3
#define VR_GRAB_TEST_THINGS 2

static struct
{
	/* the weapons the game offered the first player at its last tick
	(players.c), and that tick */
	long offered_tick;
	short offered_count;
	long offered[HALO_VR_GRAB_THINGS];
	/* the grab is on (vr.c, last frame), the weapon the right hand is
	nearest (NONE: none) and how far from its reach (metres), the world's
	units a metre */
	boolean enabled;
	long nearest;
	real distance;
	real units;
	/* the weapon the grip took, until the game offers it (NONE: none) or
	the tick it is given up at; the pick-up pressed until this tick */
	long taken;
	long taken_until;
	long press_until;
	/* the control a hand pressed, until the game offers it (NONE: none) or
	the tick it is given up at; its action pressed until this tick, and the
	aim on it until that one */
	long control;
	long control_until;
	long action_until;
	long facing_until;
	/* debug.vr_test_grab: when the player first aimed (NONE: not yet, on
	this map), the thing placed, and the weapons the player carried, logged */
	long test_first_aim_tick;
	boolean test_placed;
	long test_weapon_index[VR_GRAB_TEST_THINGS];
	long test_carried[2];
	char test_logged[VR_GRAB_TEST_THINGS][160];
} vr_grab = { NONE, 0, { 0 }, FALSE, NONE, 0.0f, 0.0f, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, FALSE,
	{ NONE, NONE }, { NONE, NONE } };

/* the pick-up of a weapon, as the game offers it */
static boolean vr_grab_weapon_result(short action_result)
{
	return action_result == _player_action_result_swap_for_weapon ||
		action_result == _player_action_result_add_weapon_to_inventory;
}

/* the first local player's unit on foot and alive (NULL: not) */
static struct unit_datum *vr_grab_unit(long player_index)
{
	struct player_datum *player = player_get(player_index);
	struct unit_datum *unit;

	if (player->unit_index == NONE)
		return NULL;
	unit = (struct unit_datum *)object_try_and_get_and_verify_type(player->unit_index, _object_mask_unit);
	if (!unit || unit->object.parent_object_index != NONE || TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
		return NULL;
	return unit;
}

/* debug.vr_test_grab: a weapon laid on the ground before the unit, its tag
name having name in it, this far ahead and right of it (metres), across its
way (its muzzle to the left); NONE: none made */
static long vr_grab_test_weapon(struct unit_datum *unit, char const *name, real ahead, real right)
{
	struct tag_iterator iterator;
	struct object_placement_data placement;
	struct collision_result collision;
	real_vector3d forward, down = { 0.0f, 0.0f, -6.0f / 3.048f };
	real_point3d above;
	long tag_index, weapon_index;

	tag_iterator_new(&iterator, WEAPON_DEFINITION_TAG);
	for (tag_index = tag_iterator_next(&iterator); tag_index != NONE; tag_index = tag_iterator_next(&iterator))
	{
		char const *tag_name = tag_get_name(tag_index);

		if (tag_name && strstr(tag_name, name))
			break;
	}
	forward = unit->object.forward;
	forward.k = 0.0f;
	if (tag_index == NONE || normalize3d(&forward) <= 0.0f)
	{
		platform_log("vr grab: no test weapon \"%s\" in this map", name);
		return NONE;
	}
	/* (on the ground there) */
	object_placement_data_new(&placement, tag_index, NONE);
	placement.position.x = unit->object.position.x + (forward.i * ahead + forward.j * right) / 3.048f;
	placement.position.y = unit->object.position.y + (forward.j * ahead - forward.i * right) / 3.048f;
	placement.position.z = unit->object.position.z;
	above = placement.position;
	above.z += 1.0f / 3.048f;
	if (global_current_collision_user_depth < MAXIMUM_COLLISION_USER_STACK_DEPTH)
	{
		global_current_collision_users[global_current_collision_user_depth++] = _collision_user_ui;
		if (collision_test_vector(FLAG(_collision_test_structure_bit), &above, &down, NONE, &collision))
			placement.position.z = collision.point.z;
		--global_current_collision_user_depth;
	}
	placement.position.z += 0.05f / 3.048f;
	placement.forward.i = -forward.j;
	placement.forward.j = forward.i;
	placement.forward.k = 0.0f;
	placement.up = *global_up3d;
	weapon_index = object_new(&placement);
	platform_log("vr grab: test weapon %s, %.2f m ahead, %.2f m right%s", tag_get_name(tag_index), ahead, right,
		weapon_index == NONE ? ": not made" : "");
	return weapon_index;
}

/* debug.vr_test_grab: "control [<n> [<metres>]]": the controls the unit
could use (able to change) logged, and the unit stood before the nth
nearest (the nearest by default), so far from its face (0.6 m by default),
and (turn) the first player turned to it */
static long vr_grab_test_control(long unit_index, boolean turn, char const *test)
{
	short wanted = 0;
	real metres = 0.6f;
	struct unit_datum *unit = unit_get(unit_index);
	struct object_iterator iterator;
	long usable[32];
	real distances[32];
	short count = 0, index, rank;
	struct object_datum *control;
	struct object_marker marker;
	real_point3d face, place, above;
	real_vector3d front, down = { 0.0f, 0.0f, -3.0f / 3.048f };
	struct collision_result collision;
	real yaw;

	sscanf(test, "%hd %f", &wanted, &metres);
	object_iterator_new(&iterator, _object_mask_control, 0);
	while ((control = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
	{
		real distance = distance3d(&unit->object.position, &control->object.bounding_sphere_center);
		boolean marked = object_get_marker_by_name(iterator.index, "front", &marker, 1) == 1;
		platform_log("vr grab: control %lx %s, %.1f m away, %.2f m across%s%s%s", iterator.index,
			tag_name_strip_path(tag_get_name(control->definition_index)), distance * 3.048f,
			control->object.bounding_sphere_radius * 2.0f * 3.048f,
			device_can_change_position(iterator.index) ? ", usable" : "", marked ? ", its front marked" : "",
			TEST_FLAG(control->object.flags, _object_outside_of_map_bit) ? ", outside the map" : "");
		if (device_can_change_position(iterator.index) && count < NUMBEROF(usable) &&
			!TEST_FLAG(control->object.flags, _object_outside_of_map_bit))
		{
			usable[count] = iterator.index;
			distances[count++] = distance;
		}
	}
	/* (the wanted'th nearest: as many nearer) */
	for (index = 0; index < count; index++)
	{
		short other;

		for (rank = 0, other = 0; other < count; other++)
			rank += distances[other] < distances[index] || (distances[other] == distances[index] && other < index);
		if (rank == wanted)
			break;
	}
	if (index >= count)
	{
		platform_log("vr grab: no usable control %d in this map (%d)", wanted, count);
		return NONE;
	}
	control = object_get(usable[index]);
	face = control->object.bounding_sphere_center;
	vector_from_points3d(&face, &unit->object.position, &front);
	if (object_get_marker_by_name(usable[index], "front", &marker, 1) == 1)
	{
		face = marker.matrix.position;
		front = marker.matrix.forward;
	}
	front.k = 0.0f;
	if (normalize3d(&front) <= 0.0f)
		return NONE;
	/* (on the ground before it) */
	place.x = face.x + front.i * metres / 3.048f;
	place.y = face.y + front.j * metres / 3.048f;
	place.z = face.z;
	above = place;
	if (global_current_collision_user_depth < MAXIMUM_COLLISION_USER_STACK_DEPTH)
	{
		global_current_collision_users[global_current_collision_user_depth++] = _collision_user_ui;
		if (collision_test_vector(FLAG(_collision_test_structure_bit), &above, &down, NONE, &collision))
			place.z = collision.point.z;
		--global_current_collision_user_depth;
	}
	place.z += 0.05f / 3.048f;
	object_set_position(unit_index, &place, NULL, NULL);
	yaw = (real)atan2(-front.j, -front.i);
	if (turn)
		halo_vr_test_face(yaw);
	platform_log("vr grab: unit %lx stood %.2f m before control %lx %s, its face %.2f m above the feet", unit_index,
		metres, usable[index], tag_get_name(control->definition_index), (face.z - place.z) * 3.048f);
	return usable[index];
}

/* debug.vr_test_grab = "control" as a network game's client: the first
player turned, once, to the usable control whose face is within 1.5 m of
the unit across the floor (the host stands them there: vr_grab_test); TRUE
once turned */
static boolean vr_grab_test_control_turn(struct unit_datum *unit)
{
	struct object_iterator iterator;
	struct object_datum *control;
	struct object_marker marker;

	object_iterator_new(&iterator, _object_mask_control, 0);
	while ((control = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
	{
		real_vector3d front, across;

		if (!device_can_change_position(iterator.index) ||
			object_get_marker_by_name(iterator.index, "front", &marker, 1) != 1)
		{
			continue;
		}
		vector_from_points3d(&unit->object.position, &marker.matrix.position, &across);
		if (across.i * across.i + across.j * across.j > (1.5f / 3.048f) * (1.5f / 3.048f) || fabs(across.k) > 1.0f)
			continue;
		front = marker.matrix.forward;
		halo_vr_test_face((real)atan2(-front.j, -front.i));
		platform_log("vr grab: turned to control %lx", iterator.index);
		vr_grab.test_weapon_index[0] = iterator.index;
		return TRUE;
	}
	return FALSE;
}

/* debug.vr_test_grab: "weapon <name>[ @ <metres ahead> <metres right>][;
weapon ...]", up to VR_GRAB_TEST_THINGS weapons whose tag names have name in
them laid on the ground before the first player (0.5 m ahead and 0.15 m to
the right by default) VR_GRAB_TEST_SECONDS after they first aim, once a map,
the level's AI erased (once a second), for automated tests of the grab; and
each second, as they change, where each is from the head, what the game
offers, how near the right hand is, and what the player carries */
static void vr_grab_test(long player_index, boolean aiming)
{
	char const *test = config_string("debug.vr_test_grab");
	long now = game_time_get();
	struct player_datum *player = player_get(player_index);
	struct unit_datum *unit;
	short thing;

	boolean host = game_connection() == _game_connection_network_server && !halo_vr_running();
	boolean client = network_game_distributed_client();

	if ((strncmp(test, "weapon ", 7) && strncmp(test, "control", 7)) || player->unit_index == NONE)
		return;
	/* (a network game's host, flat, stands the last player, on another
	machine, before a control; that machine's player turns to it) */
	if (host)
	{
		struct data_iterator players;
		struct player_datum *other, *last = NULL;

		data_iterator_new(&players, player_data);
		while ((other = (struct player_datum *)data_iterator_next(&players)) != NULL)
			last = other;
		if (!last || last == player || last->unit_index == NONE || strncmp(test, "control", 7))
			return;
		aiming = TRUE;
	}
	if (vr_grab.test_first_aim_tick != NONE && now < vr_grab.test_first_aim_tick)
	{
		/* (a new map, or a revert) */
		vr_grab.test_first_aim_tick = NONE;
		vr_grab.test_placed = FALSE;
		for (thing = 0; thing < VR_GRAB_TEST_THINGS; thing++)
			vr_grab.test_weapon_index[thing] = NONE;
	}
	if (vr_grab.test_first_aim_tick == NONE && aiming)
		vr_grab.test_first_aim_tick = now;
	if (vr_grab.test_first_aim_tick == NONE)
		return;
	unit = unit_get(player->unit_index);
	if ((now - vr_grab.test_first_aim_tick) % TICKS_PER_SECOND == 0)
	{
		long carried[2];
		short slot;

		/* (the level's own AI erased: nothing shoots the player; a
		client's are the host's) */
		if (!client)
			ai_scripting_erase_all();
		for (thing = 0; thing < VR_GRAB_TEST_THINGS; thing++)
		{
			long weapon_index = vr_grab.test_weapon_index[thing];
			struct object_datum *weapon = weapon_index != NONE ? object_try_and_get(weapon_index) : NULL;
			float place[3] = { 0.0f, 0.0f, 0.0f };
			char text[160];

			/* (a flat host's: from nowhere) */
			if (!weapon || (!vr_render_world_place(&weapon->object.bounding_sphere_center, place) && !host))
				continue;
			snprintf(text, sizeof(text), "at %.1f m ahead, %.1f m right, %.1f m up from the head%s; the game offers "
				"%s; the hand %s", place[0], -place[1], place[2],
				weapon->object.parent_object_index != NONE ? ", carried" :
				TEST_FLAG(_object_mask_control, weapon->object.type) ?
				(((struct device_datum *)weapon)->device.position > 0.5f ? ", on" : ", off") : "",
				player->action_object_index == weapon_index ?
					(player->action_result == _player_action_result_swap_for_weapon ? "a swap for it" : "it") :
					player->action_object_index != NONE ? "another" : "nothing",
				vr_grab.nearest != weapon_index ? "not nearest it" : vr_grab.distance <= 0.0f ? "at it" :
					vr_grab.distance < VR_GRAB_PREFER_DISTANCE ? "near it" : "nearest it, far");
			if (strcmp(text, vr_grab.test_logged[thing]))
			{
				platform_log("vr grab: test %s %d (%s) %s", TEST_FLAG(_object_mask_control, weapon->object.type) ?
					"control" : "weapon", thing,
					tag_name_strip_path(tag_get_name(weapon->definition_index)), text);
			}
			strncpy(vr_grab.test_logged[thing], text, sizeof(vr_grab.test_logged[thing]) - 1);
		}
		for (slot = 0; slot < 2; slot++)
			carried[slot] = unit_inventory_get_weapon(player->unit_index, slot);
		if (carried[0] != vr_grab.test_carried[0] || carried[1] != vr_grab.test_carried[1])
		{
			platform_log("vr grab: the player carries %s (in hand) and %s",
				carried[unit->unit.current_weapon_index & 1] != NONE ?
					tag_get_name(object_get(carried[unit->unit.current_weapon_index & 1])->definition_index) : "nothing",
				carried[(unit->unit.current_weapon_index + 1) & 1] != NONE ?
					tag_get_name(object_get(carried[(unit->unit.current_weapon_index + 1) & 1])->definition_index) :
					"nothing");
			vr_grab.test_carried[0] = carried[0];
			vr_grab.test_carried[1] = carried[1];
		}
	}
	if (vr_grab.test_placed || now < vr_grab.test_first_aim_tick + VR_GRAB_TEST_SECONDS * TICKS_PER_SECOND)
		return;
	if (client)
	{
		vr_grab.test_placed = !strncmp(test, "control", 7) && vr_grab_test_control_turn(unit);
		return;
	}
	vr_grab.test_placed = TRUE;
	if (!strncmp(test, "control", 7) && host)
	{
		struct data_iterator players;
		struct player_datum *other, *last = NULL;

		data_iterator_new(&players, player_data);
		while ((other = (struct player_datum *)data_iterator_next(&players)) != NULL)
			last = other;
		vr_grab.test_weapon_index[0] = vr_grab_test_control(last->unit_index, FALSE, test + 7);
		return;
	}
	if (!strncmp(test, "control", 7))
	{
		vr_grab.test_weapon_index[0] = vr_grab_test_control(player->unit_index, TRUE, test + 7);
		return;
	}
	for (thing = 0; thing < VR_GRAB_TEST_THINGS && test && !strncmp(test, "weapon ", 7); thing++)
	{
		char spec[96], *at, *end;
		real ahead = 0.5f, right = 0.15f;
		char const *next = strchr(test, ';');
		size_t length = next ? (size_t)(next - test) : strlen(test);

		if (length >= sizeof(spec))
			length = sizeof(spec) - 1;
		memcpy(spec, test + 7, length - 7);
		spec[length - 7] = 0;
		if ((at = strchr(spec, '@')) != NULL)
		{
			sscanf(at + 1, "%f %f", &ahead, &right);
			*at = 0;
		}
		for (end = spec + strlen(spec); end > spec && end[-1] == ' '; end--)
			end[-1] = 0;
		vr_grab.test_weapon_index[thing] = vr_grab_test_weapon(unit, spec, ahead, right);
		test = next ? next + 1 : NULL;
		while (test && *test == ' ')
			test++;
	}
}

/* players.c, as the game offers a player something to do with the action
button (player_set_action_result), and would offer it (set_action): the
weapons offered to the first local player kept, and among them the one its
right hand reaches for offered first (TRUE), whatever is nearer the body.
Not as a network game's client, whose host decides pick-ups by the body. */
boolean vr_grab_offer(long player_index, short action_result, long object_index, boolean set_action)
{
	struct player_datum *player = player_get(player_index);
	long now = game_time_get();

	if (!vr_grab_weapon_result(action_result) || player_index != local_player_get_player_index(0) ||
		network_game_distributed_client())
	{
		return set_action;
	}
	if (vr_grab.offered_tick != now)
	{
		vr_grab.offered_tick = now;
		vr_grab.offered_count = 0;
	}
	if (vr_grab.offered_count < HALO_VR_GRAB_THINGS)
		vr_grab.offered[vr_grab.offered_count++] = object_index;
	if (!vr_grab.enabled || vr_grab.nearest == NONE || vr_grab.distance >= VR_GRAB_PREFER_DISTANCE ||
		action_result != player->action_result || player->action_object_index == NONE)
	{
		return set_action;
	}
	if (object_index == vr_grab.nearest)
		return TRUE;
	if (player->action_object_index == vr_grab.nearest)
		return FALSE;
	return set_action;
}

/* a weapon on the ground as a thing to grab: the line along it, through
its middle */
static boolean vr_grab_weapon_thing(long weapon_index, struct halo_vr_grab_thing *thing)
{
	struct object_datum *weapon = object_try_and_get(weapon_index);
	real_point3d ends[2];
	real half;
	short end;

	if (!weapon || weapon->object.parent_object_index != NONE)
		return FALSE;
	half = weapon->object.bounding_sphere_radius * VR_GRAB_WEAPON_LENGTH;
	for (end = 0; end < 2; end++)
	{
		real along = end ? half : -half;

		ends[end].x = weapon->object.bounding_sphere_center.x + weapon->object.forward.i * along;
		ends[end].y = weapon->object.bounding_sphere_center.y + weapon->object.forward.j * along;
		ends[end].z = weapon->object.bounding_sphere_center.z + weapon->object.forward.k * along;
		if (!vr_render_world_place(&ends[end], thing->ends[end]))
			return FALSE;
	}
	thing->kind = HALO_VR_GRAB_WEAPON;
	thing->object = weapon_index;
	thing->radius = VR_GRAB_WEAPON_THICKNESS * 0.5f;
	return TRUE;
}

/* a control the game would let the player use (in reach of the body, able
to change, facing the eye) as a thing to press: its face (its "front"
marker, and which way it faces: toward the eye, for one used from either
side), else its middle */
static boolean vr_grab_control_thing(long unit_index, long control_index, struct halo_vr_grab_thing *thing)
{
	struct control_datum *control = control_try_and_get(control_index);
	real units = vr_grab.units > 0.0f ? vr_grab.units : 1.0f / 3.048f;
	struct object_marker marker;
	real_point3d camera, face;
	real_vector3d toward;
	real radius;
	short axis;

	if (!control || !device_can_change_position(control_index))
		return FALSE;
	unit_get_camera_position(unit_index, &camera);
	vector_from_points3d(&camera, &control->object.bounding_sphere_center, &toward);
	if (normalize3d(&toward) <= 0.0f || !device_frontfacing(control_index, &camera, &toward))
		return FALSE;
	face = control->object.bounding_sphere_center;
	thing->front[0] = thing->front[1] = thing->front[2] = 0.0f;
	if (object_get_marker_by_name(control_index, "front", &marker, 1) == 1)
	{
		real_vector3d front = marker.matrix.forward;
		real_point3d ahead;
		float place[3];

		face = marker.matrix.position;
		if (dot_product3d(&front, &toward) > 0.0f)
			scale_vector3d(&front, -1.0f, &front);
		ahead.x = face.x + front.i * units;
		ahead.y = face.y + front.j * units;
		ahead.z = face.z + front.k * units;
		if (!vr_render_world_place(&ahead, place))
			return FALSE;
		for (axis = 0; axis < 3; axis++)
			thing->front[axis] = place[axis];
	}
	if (!vr_render_world_place(&face, thing->ends[0]))
		return FALSE;
	for (axis = 0; axis < 3; axis++)
	{
		thing->ends[1][axis] = thing->ends[0][axis];
		if (thing->front[0] != 0.0f || thing->front[1] != 0.0f || thing->front[2] != 0.0f)
			thing->front[axis] -= thing->ends[0][axis];
	}
	radius = control->object.bounding_sphere_radius / units * 0.5f;
	thing->kind = HALO_VR_GRAB_CONTROL;
	thing->object = control_index;
	thing->radius = PIN(radius, VR_GRAB_CONTROL_SMALLEST, VR_GRAB_CONTROL_LARGEST);
	return TRUE;
}

/* player_control.c, each frame, for the first local player's unit: while a
control a hand pressed waits for the game to offer it, and as its action is
pressed, the facing (TRUE, yaw and pitch) at it from the eye, as X's aim
would be, so that the game offers it, as a network game's host does for a
client (the host decides the controls a player uses, by the player's aim) */
boolean vr_grab_facing(long unit_index, real *yaw, real *pitch)
{
	long now = game_time_get();
	struct object_datum *control;
	real_point3d camera;
	real_vector3d toward;

	if (vr_grab.facing_until == NONE || now >= vr_grab.facing_until ||
		now < vr_grab.facing_until - VR_GRAB_CONTROL_WAIT_TICKS - VR_GRAB_ACTION_TICKS - 1 || unit_index == NONE ||
		vr_grab.control == NONE)
	{
		return FALSE;
	}
	control = (struct object_datum *)object_try_and_get_and_verify_type(vr_grab.control, _object_mask_control);
	if (!control || !object_try_and_get_and_verify_type(unit_index, _object_mask_unit))
		return FALSE;
	unit_get_camera_position(unit_index, &camera);
	vector_from_points3d(&camera, &control->object.bounding_sphere_center, &toward);
	if (normalize3d(&toward) <= 0.0f)
		return FALSE;
	*yaw = (real)atan2(toward.j, toward.i);
	*pitch = (real)asin(PIN(toward.k, -1.0f, 1.0f));
	return TRUE;
}

/* player_control.c, as the first local player's input is read (the
player, and the flags the input has set): the things to grab told to vr.c,
and the grip's weapon picked up, the game's pick-up pressed for it (the swap
bit, as X held) once the game offers it */
void vr_grab_input(long player_index, unsigned long *unit_control_flags)
{
	struct player_datum *player = player_get(player_index);
	struct unit_datum *unit = vr_grab_unit(player_index);
	boolean weapon_offered = unit && vr_grab_weapon_result(player->action_result) &&
		player->action_object_index != NONE;
	long now = game_time_get();
	struct halo_vr_grab grab;
	short index;

	vr_grab_test(player_index, halo_vr_aiming() || vr_grab.enabled);
	memset(&grab, 0, sizeof(grab));
	if (unit)
	{
		/* (the weapons the game offered at its last tick, else the one it
		offers: a network game's client's, the host's) */
		if (!network_game_distributed_client() && vr_grab.offered_tick != NONE && now - vr_grab.offered_tick <= 1 &&
			now >= vr_grab.offered_tick)
		{
			for (index = 0; index < vr_grab.offered_count && grab.count < HALO_VR_GRAB_THINGS; index++)
			{
				if (vr_grab_weapon_thing(vr_grab.offered[index], &grab.things[grab.count]))
					grab.count++;
			}
		}
		else if (weapon_offered && vr_grab_weapon_thing(player->action_object_index, &grab.things[0]))
		{
			grab.count = 1;
		}
		/* (the controls in reach of the body, as the game finds them:
		player_examine_nearby_objects) */
		{
			long controls[HALO_VR_GRAB_THINGS];
			short count = objects_in_sphere(0, _object_mask_control, &unit->object.location,
				&unit->object.bounding_sphere_center, unit->object.bounding_sphere_radius, controls, NUMBEROF(controls));

			for (index = 0; index < count && grab.count < HALO_VR_GRAB_THINGS; index++)
			{
				if (vr_grab_control_thing(player->unit_index, controls[index], &grab.things[grab.count]))
					grab.count++;
			}
		}
	}
	if (!halo_vr_grab(&grab))
	{
		vr_grab.enabled = FALSE;
		vr_grab.nearest = NONE;
		vr_grab.control = NONE;
		return;
	}
	vr_grab.enabled = grab.enabled && unit;
	vr_grab.nearest = vr_grab.enabled && grab.nearest != -1 ? grab.nearest : NONE;
	vr_grab.distance = grab.distance;
	vr_grab.units = grab.world_units_per_metre;
	if (grab.taken != -1 && vr_grab.enabled)
	{
		vr_grab.taken = grab.taken;
		vr_grab.taken_until = now + VR_GRAB_WAIT_TICKS;
	}
	/* (the grip's weapon, once the game offers it: picked up) */
	if (vr_grab.taken != NONE)
	{
		if (!vr_grab.enabled || now > vr_grab.taken_until || now < vr_grab.taken_until - VR_GRAB_WAIT_TICKS)
		{
			platform_log("vr grab: the weapon gripped was not offered: nothing taken");
			vr_grab.taken = NONE;
		}
		else if (weapon_offered && player->action_object_index == vr_grab.taken)
		{
			platform_log("vr grab: %s %s", player->action_result == _player_action_result_swap_for_weapon ?
				"swapped for" : "picked up", tag_get_name(object_get(vr_grab.taken)->definition_index));
			vr_grab.press_until = now + VR_GRAB_PRESS_TICKS;
			vr_grab.taken = NONE;
		}
	}
	if (vr_grab.press_until != NONE && (now >= vr_grab.press_until || now < vr_grab.press_until - VR_GRAB_PRESS_TICKS))
		vr_grab.press_until = NONE;
	if (vr_grab.press_until != NONE)
		SET_FLAG(*unit_control_flags, _unit_control_swap_weapons_bit, TRUE);

	/* (the control a hand pressed: the aim turned to it, vr_grab_facing,
	until the game offers it, then its action) */
	if (grab.pressed != -1 && vr_grab.enabled && vr_grab.action_until == NONE)
	{
		if (player->action_result != _player_action_result_touch_device || player->action_object_index != grab.pressed)
			platform_log("vr grab: the control pressed is not on offer: the aim turned to it");
		vr_grab.control = grab.pressed;
		vr_grab.control_until = now + VR_GRAB_CONTROL_WAIT_TICKS;
		vr_grab.facing_until = vr_grab.control_until + VR_GRAB_ACTION_TICKS + 1;
	}
	if (vr_grab.action_until != NONE && (now >= vr_grab.action_until ||
		now < vr_grab.action_until - VR_GRAB_ACTION_TICKS))
	{
		vr_grab.action_until = NONE;
		vr_grab.control = NONE;
	}
	if (vr_grab.control != NONE && vr_grab.action_until == NONE)
	{
		if (!vr_grab.enabled || now > vr_grab.control_until || now < vr_grab.control_until - VR_GRAB_CONTROL_WAIT_TICKS)
		{
			platform_log("vr grab: the control pressed was not offered: nothing done");
			vr_grab.control = NONE;
			vr_grab.facing_until = NONE;
		}
		else if (unit && player->action_result == _player_action_result_touch_device &&
			player->action_object_index == vr_grab.control)
		{
			platform_log("vr grab: pressed %s", tag_get_name(object_get(vr_grab.control)->definition_index));
			vr_grab.action_until = now + VR_GRAB_ACTION_TICKS;
			vr_grab.facing_until = vr_grab.action_until + 1;
		}
	}
	if (vr_grab.action_until != NONE && player->action_result == _player_action_result_touch_device &&
		player->action_object_index == vr_grab.control)
	{
		SET_FLAG(*unit_control_flags, _unit_control_action_bit, TRUE);
	}
}

/* render_objects.c, as an object is drawn in the world (its light given):
the weapon the right hand reaches for lit up as the holsters' weapons are
(lighting set to the light to draw it with; FALSE, as it is) */
boolean vr_grab_highlight(long object_index, struct render_lighting const *given, struct render_lighting *lighting,
	struct render_model_effect *model_effect)
{
	long player_index = local_player_get_player_index(0);
	long unit_index;

	if (object_index == NONE || object_index != vr_grab.nearest || !vr_grab.enabled || !given || player_index == NONE ||
		!halo_vr_running())
	{
		return FALSE;
	}
	unit_index = player_get(player_index)->unit_index;
	if (unit_index == NONE || !object_try_and_get_and_verify_type(unit_index, _object_mask_unit))
		return FALSE;
	*lighting = *given;
	vr_holster_highlight_object(unit_index, vr_grab.distance, vr_grab.units, model_effect, lighting);
	return TRUE;
}

#endif
