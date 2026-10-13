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
*/

#ifdef HALO_VR

#include "cseries.h"
#include "math/real_math.h"
#include "ai/ai_script.h"
#include "cache/cache_files.h"
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
	/* debug.vr_test_grab: when the player first aimed (NONE: not yet, on
	this map), the thing placed, and the weapons the player carried, logged */
	long test_first_aim_tick;
	boolean test_placed;
	long test_weapon_index[VR_GRAB_TEST_THINGS];
	long test_carried[2];
	char test_logged[VR_GRAB_TEST_THINGS][160];
} vr_grab = { NONE, 0, { 0 }, FALSE, NONE, 0.0f, 0.0f, NONE, NONE, NONE, NONE, FALSE, { NONE, NONE }, { NONE, NONE } };

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

	if (strncmp(test, "weapon ", 7) || player->unit_index == NONE)
		return;
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

		/* (the level's own AI erased: nothing shoots the player) */
		ai_scripting_erase_all();
		for (thing = 0; thing < VR_GRAB_TEST_THINGS; thing++)
		{
			long weapon_index = vr_grab.test_weapon_index[thing];
			struct object_datum *weapon = weapon_index != NONE ? object_try_and_get(weapon_index) : NULL;
			float place[3];
			char text[160];

			if (!weapon || !vr_render_world_place(&weapon->object.bounding_sphere_center, place))
				continue;
			snprintf(text, sizeof(text), "at %.1f m ahead, %.1f m right, %.1f m up from the head%s; the game offers "
				"%s; the hand %s", place[0], -place[1], place[2],
				weapon->object.parent_object_index != NONE ? ", carried" : "",
				player->action_object_index == weapon_index ?
					(player->action_result == _player_action_result_swap_for_weapon ? "a swap for it" : "it") :
					player->action_object_index != NONE ? "another" : "nothing",
				vr_grab.nearest != weapon_index ? "not nearest it" : vr_grab.distance <= 0.0f ? "at it" :
					vr_grab.distance < VR_GRAB_PREFER_DISTANCE ? "near it" : "nearest it, far");
			if (strcmp(text, vr_grab.test_logged[thing]))
			{
				platform_log("vr grab: test weapon %d (%s) %s", thing,
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
	vr_grab.test_placed = TRUE;
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

	vr_grab_test(player_index, halo_vr_aiming());
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
	}
	if (!halo_vr_grab(&grab))
	{
		vr_grab.enabled = FALSE;
		vr_grab.nearest = NONE;
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
