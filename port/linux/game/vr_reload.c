/*
VR_RELOAD.C

The VR mode's reload by hand (HALO_VR; vr.manual_reload, port/linux/README.md
"VR"): the game's side of it, for player_control.c, weapons.c and
first_person_weapons.c.

The hand reloads as it would a real gun: the left hand takes another
magazine from its pouch on the body (the left hip; the rocket launcher's
tubes from behind the left shoulder), the one in the weapon ejecting itself
(or the right stick pushed down ejects it first), and pushes it into the
weapon where it goes (port/linux/src/vr.c, which has the hands). The
shotgun is fed a shell at a time, with no magazine to eject.

Each one pushed home is loaded at once, where this machine runs the weapon
(alone, split screen, a network game's host): the hand took the reload's
time, so the rounds are in the magazine on the tick it is pushed home, the
HUD counts them, and the weapon fires (weapons.c,
weapon_magazine_reload_at_once, in the weapon's own update: the game's
ammunition as its reload would leave it). Without the game's reload, its
state, its time and its animations: the unit's reload animation would show
the body reloading a weapon already loaded, and keep it from firing until it
ends (the unit is busy); others see the count change, not the body reload.
Should the weapon be busy (firing, drawn), it is loaded as soon as it is
not, within PRESS_TICKS. X reloads as ever.

On a network game's client the host runs the weapon (network_objects.c:
its count is the host's), and nothing it is sent says a reload was the
hand's: there each one pushed home presses the game's reload, through the
player's input as the keyboard's reload key does (player_control.c), so
that the host reloads, in the game's time, as it does for X. Not X itself,
which over a weapon on the ground would take it up. A client loading at
once alone would fight the host: its count is taken from the host's a round
trip later, before the host's reload has ended.

The game reloads a magazine whole, and the shotgun a shell at a time: its
magazine reloads one round (rounds_reloaded) and starts over until it is full
(weapon_magazine_finish_reload), unless the trigger is held. Each shell the
hand pushes in is one of those (loaded at once, or pressing the reload); the
game goes on to the next only for a shell the hand has pushed in meanwhile,
else it stops (vr_reload_chain). Nor does an empty magazine reload itself,
as the game would do it at once (weapons.c's idle trigger): the hand
reloads it, or X (vr_reload_automatic). Both are the game's decisions on
the machine that runs the weapon: on a client the host's copy decides, so
there the game reloads as ever.

What the weapon's magazine is: its first-person model's node named for it
(the assault rifle's, the pistol's and the sniper rifle's "frame magazine",
the shotgun's "frame bullet", the shell it loads, the rocket launcher's
"frame tubes"), with the nodes under it. Drawn alone (the model's other
nodes shrunk to nothing), it is the one held in the left hand and the one
in the pouch; where it rests in the weapon (the model's default pose, about
its parent node as the weapon is drawn) is where the one held goes in (the
shotgun's shell rests inside the receiver: just below it, at the port). A
weapon whose model has none (the needler) has its ammunition's pickup
instead, and a place under its grip. While the magazine is out it is not
drawn in the weapon.

On a client the game's reload after the hand's runs underneath (the
ammunition, its time, the unit's animation that others see), but the
first-person weapon plays no reload (with the hands the controllers',
vr.hands): it stays in the right hand, idle, as the hand's grenade throw
leaves it there (vr_grenade.c). Its animation would swing it up out of the
hand and take the magazine out and put it back again. The weapon is the hands' and the
game's ammunition's: the shotgun's shells and the rocket launcher's tubes
the same.

Lit up as the holsters are (vr_holster.c): the pouch's magazine as the left
hand comes to it, while one is to be taken (the weapon has room for one,
and rounds to fill it), and where the magazine goes, in
the weapon, while the hand holds one, the more the nearer it comes.

debug.vr_test_weapon puts a weapon of the map's in the first player's hand,
for automated tests of the reload by hand with each kind of weapon.
*/

#ifdef HALO_VR

#include "cseries.h"
#include "math/real_math.h"
#include "cache/cache_files.h"
#include "game/game.h"
#include "game/players.h"
#include "items/weapons.h"
#include "items/weapon_definitions.h"
#include "models/model_definitions.h"
#include "models/models.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "rasterizer/rasterizer_model_types.h"
#include "render/render.h"
#include "tag_files/tag_files.h"
#include "tag_files/tag_groups.h"
#include "units/units.h"

#include "../src/vr.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* the pouch's magazine lit up as the left hand comes nearer than this to
its reach (metres), to POUCH_HIGHLIGHT_NEAR at the reach and full within */
#define POUCH_HIGHLIGHT_DISTANCE 0.30f
#define POUCH_HIGHLIGHT_NEAR 0.7f
/* where the magazine goes lit up while the hand holds one, at least
WELL_HIGHLIGHT_LEAST, full as the one held comes to it from
WELL_HIGHLIGHT_DISTANCE (metres) */
#define WELL_HIGHLIGHT_DISTANCE 0.30f
#define WELL_HIGHLIGHT_LEAST 0.4f
/* the game's reload pressed from the magazine pushed home until the game
reloads, no longer than this (ticks) */
#define PRESS_TICKS 15
/* a weapon whose first-person model has no magazine (the needler): its
ammunition's pickup in the hand and in the pouch, at this size, going in
here from the right controller's grip (metres forward, left, up) */
#define STAND_IN_SCALE 0.6f
static const float stand_in_well[3] = { 0.08f, 0.0f, -0.07f };
/* the shotgun's shell rests in its model inside the receiver, over the
port it is pushed in at: the port this far below (metres, along the
weapon's up), where it is lit up */
#define SHELL_PORT_BELOW 0.10f
/* drawn at their finest detail */
#define DETAIL_PIXELS 1000.0f

enum
{
	_render_model_effect_type_none = 0,
};

void platform_log(char const *format, ...);
const char *config_string(char const *name);
/* vr_render.c's */
boolean vr_render_left_hand(real_matrix4x3 *hand, real *units);
boolean vr_render_right_hand(real_matrix4x3 *hand, real *units);
boolean vr_render_body_place(const float place[3], const float forward[3], const float up[3], real_matrix4x3 *matrix,
	real *units);
/* d3d8_device.c's */
void halo_vr_test_screenshot(void);
/* vr_holster.c's */
void vr_holster_highlight(boolean well, long unit_index, real strength, real units,
	struct render_model_effect *model_effect, struct render_lighting *lighting);

static struct
{
	/* the VR player's unit, the weapon in its hand and its kind
	(HALO_VR_RELOAD_*) */
	long unit_index;
	long weapon_index;
	int kind;
	/* the magazines the hand has pushed home that were seen
	(halo_vr_reload's inserted) */
	unsigned long inserted;
	/* the game's reload pressed (player_control.c) since this tick, until
	the game reloads (a client) */
	boolean pressing;
	long press_time;
	/* the magazines (shells) pushed home to be loaded at once, the first of
	them since this tick (where this machine runs the weapon); the trigger
	and the first shot after one is, to be logged, and the rounds it was
	loaded with */
	short loads;
	long load_time;
	boolean watch_shot;
	boolean watch_trigger;
	short watch_rounds;
	/* debug.screenshot_directory: the frames to take after one is loaded */
	short screenshots;
	/* debug.vr_test_weapon: given (or not to be) */
	boolean test_weapon_done;
	/* the shells pushed into the shotgun that the game has yet to load */
	short shells;
	/* the game reloads after the hand's: the first-person weapon plays no
	reload */
	boolean hand_reload;
	/* the game's own reload of the empty magazine held, logged */
	boolean empty_logged;
	/* the weapon's first-person model, its magazine's node (NONE: none,
	the stand-in's model instead), that node's parent, and each node's
	place about that parent in the model's default pose */
	long model_index;
	short magazine_node;
	short parent_node;
	boolean in_magazine[MAXIMUM_NODES_PER_MODEL];
	real_matrix4x3 rest[MAXIMUM_NODES_PER_MODEL];
	long stand_in_model_index;
	/* where the magazine goes, from the right controller's grip (metres
	forward, left, up: told to vr.c), as the weapon was last drawn, and in
	the world this frame */
	boolean well_known;
	float well[3];
	boolean well_drawn;
	real_matrix4x3 well_matrix;
	/* the tags' generation the weapon and its models were kept in
	(cache_files_tags_generation): another map numbers its models again,
	and its unit's and weapon's indices may be the same as these were */
	unsigned long generation;
} vr_reload = { NONE, NONE, HALO_VR_RELOAD_NONE, 0, FALSE, 0, 0, 0, FALSE, FALSE, 0, 0, FALSE, 0, FALSE, FALSE,
	NONE, NONE, NONE };

/* the weapon's reload by hand (HALO_VR_RELOAD_*), and its magazine's
definition: none without a magazine of rounds (the plasma weapons' is their
charge); the rocket launcher's and the fuel rod gun's tubes; the shotgun's
(by its rounds, one reloaded at a time) shells; any other's a magazine */
static int vr_reload_kind(long weapon_index, struct weapon_magazine_definition **magazine_definition)
{
	struct weapon_definition *definition;
	struct weapon_magazine_definition *magazine;
	char const *name;

	if (weapon_index == NONE)
		return HALO_VR_RELOAD_NONE;
	definition = weapon_definition_get(object_get(weapon_index)->definition_index);
	if (definition->weapon.magazines.count <= 0)
		return HALO_VR_RELOAD_NONE;
	magazine = TAG_BLOCK_GET_ELEMENT(&definition->weapon.magazines, 0, struct weapon_magazine_definition);
	if (magazine->rounds_loaded_maximum <= 0)
		return HALO_VR_RELOAD_NONE;
	*magazine_definition = magazine;
	name = tag_get_name(object_get(weapon_index)->definition_index);
	if (name && (strstr(name, "rocket") || strstr(name, "plasma_cannon") || strstr(name, "fuel rod")))
		return HALO_VR_RELOAD_TUBE;
	if (magazine->rounds_reloaded == 1 && magazine->rounds_loaded_maximum > 1)
		return HALO_VR_RELOAD_SHELL;
	return HALO_VR_RELOAD_MAGAZINE;
}

/* the weapon's magazine in its first-person model (vr_reload: its node, the
nodes under it, its parent and their default places about that parent), or
its ammunition's pickup's model */
static void vr_reload_model(long weapon_index, int kind)
{
	static char const *const magazine_names[] = { "magazine", "ammo", "clip" };
	static char const *const shell_names[] = { "bullet", "shell" };
	static char const *const tube_names[] = { "tubes", "rocket", "rod" };
	struct weapon_definition *definition = weapon_definition_get(object_get(weapon_index)->definition_index);
	long model_index = definition->weapon.interface_definition.first_person_model.index;
	struct weapon_magazine_definition *magazine;
	char const *const *names = kind == HALO_VR_RELOAD_SHELL ? shell_names : kind == HALO_VR_RELOAD_TUBE ? tube_names :
		magazine_names;
	short name_count = kind == HALO_VR_RELOAD_SHELL ? NUMBEROF(shell_names) : kind == HALO_VR_RELOAD_TUBE ?
		NUMBEROF(tube_names) : NUMBEROF(magazine_names);
	short name_index, node_index;

	vr_reload.model_index = model_index;
	vr_reload.magazine_node = NONE;
	vr_reload.parent_node = NONE;
	vr_reload.stand_in_model_index = NONE;
	if (model_index != NONE)
	{
		struct model *model = model_definition_get(model_index);

		if (model->nodes.count > 0 && model->nodes.count <= MAXIMUM_NODES_PER_MODEL)
		{
			for (name_index = 0; name_index < name_count && vr_reload.magazine_node == NONE; name_index++)
			{
				for (node_index = 1; node_index < model->nodes.count; node_index++)
				{
					struct model_node *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, node_index, struct model_node);

					if (strstr(node->name, names[name_index]) && node->parent_node_index != NONE)
					{
						vr_reload.magazine_node = node_index;
						vr_reload.parent_node = node->parent_node_index;
						break;
					}
				}
			}
		}
		if (vr_reload.magazine_node != NONE)
		{
			real_matrix4x3 parent_inverse;
			real_point3d origin = { 0.0f, 0.0f, 0.0f };
			real_vector3d forward = { 1.0f, 0.0f, 0.0f }, up = { 0.0f, 0.0f, 1.0f };

			/* (the nodes under it: each node's parents come before it) */
			for (node_index = 0; node_index < model->nodes.count; node_index++)
			{
				struct model_node *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, node_index, struct model_node);

				vr_reload.in_magazine[node_index] = node_index == vr_reload.magazine_node ||
					(node->parent_node_index != NONE && node->parent_node_index < node_index &&
					vr_reload.in_magazine[node->parent_node_index]);
			}
			model_get_node_matrices(model, vr_reload.rest, &origin, &forward, &up);
			matrix4x3_inverse(&vr_reload.rest[vr_reload.parent_node], &parent_inverse);
			for (node_index = 0; node_index < model->nodes.count; node_index++)
			{
				if (vr_reload.in_magazine[node_index])
					matrix4x3_multiply(&parent_inverse, &vr_reload.rest[node_index], &vr_reload.rest[node_index]);
			}
		}
	}
	if (vr_reload.magazine_node == NONE && vr_reload_kind(weapon_index, &magazine) != HALO_VR_RELOAD_NONE &&
		magazine->ammunition_objects.count > 0)
	{
		struct weapon_ammunition_object *ammunition = TAG_BLOCK_GET_ELEMENT(&magazine->ammunition_objects, 0,
			struct weapon_ammunition_object);

		if (ammunition->object.index != NONE)
			vr_reload.stand_in_model_index = object_definition_get(ammunition->object.index)->object.model.index;
	}
	platform_log("vr reload: %s: %s", tag_get_name(object_get(weapon_index)->definition_index),
		vr_reload.magazine_node != NONE ? TAG_BLOCK_GET_ELEMENT(&model_definition_get(model_index)->nodes,
		vr_reload.magazine_node, struct model_node)->name : vr_reload.stand_in_model_index != NONE ?
		"no magazine in its model: its ammunition's pickup" : "nothing to draw");
}

/* whether the unit and the weapon kept from the frame's update (or an
earlier one) are still the first player's unit and the weapon in its hand,
found again from the player: the game's ticks since, or the level ending
(the frame drawn as it ends, the map's objects gone), may have deleted them */
static boolean vr_reload_current(void)
{
	long player_index = local_player_get_player_index(0);
	long unit_index;

	if (vr_reload.unit_index == NONE || vr_reload.weapon_index == NONE || !halo_vr_running() ||
		player_index == NONE || vr_reload.generation != cache_files_tags_generation())
	{
		return FALSE;
	}
	unit_index = player_get(player_index)->unit_index;
	return unit_index != NONE && unit_index == vr_reload.unit_index && unit_try_and_get(unit_index) &&
		unit_inventory_get_weapon(unit_index, unit_get(unit_index)->unit.current_weapon_index) ==
		vr_reload.weapon_index;
}

/* this machine decides the weapon (not a network game's client, whose host
does) */
static boolean vr_reload_decides(void)
{
	return game_connection() != _game_connection_network_client;
}

/* debug.vr_test_weapon, "<text> <seconds>": that many seconds into the
level, the unit's weapons are taken and a new one in its hand, the first of
the map's whose tag name has the text in it */
static void vr_reload_test_weapon(long unit_index)
{
	struct tag_iterator iterator;
	struct object_placement_data data;
	char text[64];
	real seconds;
	long tag_index, weapon_index;

	if (vr_reload.test_weapon_done || unit_index == NONE)
		return;
	if (sscanf(config_string("debug.vr_test_weapon"), "%63s %f", text, &seconds) < 2)
	{
		vr_reload.test_weapon_done = TRUE;
		return;
	}
	if (game_time_get() < (long)(seconds * TICKS_PER_SECOND))
		return;
	vr_reload.test_weapon_done = TRUE;
	tag_iterator_new(&iterator, WEAPON_DEFINITION_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		char const *name = tag_get_name(tag_index);

		if (name && strstr(name, text))
			break;
	}
	if (tag_index == NONE)
	{
		platform_log("vr reload test: no weapon in this map named with \"%s\"", text);
		return;
	}
	object_placement_data_new(&data, tag_index, NONE);
	data.position = object_get(unit_index)->object.position;
	weapon_index = object_new(&data);
	if (weapon_index != NONE && unit_add_weapon_to_inventory(unit_index, weapon_index, _unit_add_weapon_replace))
	{
		short slot;

		/* (the player's choice of weapon too, which the unit's follows) */
		for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
		{
			if (unit_inventory_get_weapon(unit_index, slot) == weapon_index)
				player_control_set_desired_weapon(unit_index, slot);
		}
		platform_log("vr reload test: %s in hand", tag_get_name(tag_index));
	}
	else
	{
		platform_log("vr reload test: %s could not be given", tag_get_name(tag_index));
	}
}

/* player_control.c, each frame, for the first local player's unit (aiming:
the controller aims it, on foot): the weapon in hand, told to vr.c; the
magazines the hand pushed home, a press of the game's reload each (the
shotgun's shells as it has room for them) */
void vr_reload_update(long unit_index, boolean aiming)
{
	struct halo_vr_reload_weapon weapon_state;
	struct halo_vr_reload reload;
	struct weapon_magazine_definition *magazine_definition = NULL;
	struct weapon_magazine *magazine = NULL;
	long weapon_index = NONE;
	long now = game_time_get();
	int kind = HALO_VR_RELOAD_NONE;
	boolean reloading = FALSE;
	unsigned long inserted;

	if (unit_index != NONE && aiming && unit_get(unit_index)->object.parent_object_index == NONE)
	{
		vr_reload_test_weapon(unit_index);
		weapon_index = unit_inventory_get_weapon(unit_index, unit_get(unit_index)->unit.current_weapon_index);
		kind = vr_reload_kind(weapon_index, &magazine_definition);
	}
	if (kind != HALO_VR_RELOAD_NONE)
	{
		magazine = &weapon_get(weapon_index)->weapon.magazines[0];
		reloading = magazine->state == _magazine_reloading;
	}
	if (weapon_index != vr_reload.weapon_index || kind != vr_reload.kind ||
		vr_reload.generation != cache_files_tags_generation())
	{
		vr_reload.pressing = FALSE;
		vr_reload.shells = 0;
		vr_reload.loads = 0;
		vr_reload.watch_shot = FALSE;
		vr_reload.hand_reload = FALSE;
		vr_reload.empty_logged = FALSE;
		vr_reload.well_known = FALSE;
		if (kind != HALO_VR_RELOAD_NONE)
			vr_reload_model(weapon_index, kind);
	}
	vr_reload.unit_index = unit_index;
	vr_reload.weapon_index = weapon_index;
	vr_reload.generation = cache_files_tags_generation();
	vr_reload.kind = kind;

	/* the magazines pushed home since the last frame: loaded at once where
	this machine runs the weapon (weapons.c, vr_reload_by_hand); else the
	game's reload pressed for each, the shells counted for the shotgun to
	load in turn */
	halo_vr_reload(&reload);
	inserted = reload.inserted - vr_reload.inserted;
	vr_reload.inserted = reload.inserted;
	if (inserted && kind != HALO_VR_RELOAD_NONE)
	{
		if (vr_reload_decides())
		{
			if (!vr_reload.loads)
				vr_reload.load_time = now;
			vr_reload.loads += (short)inserted;
		}
		else if (kind == HALO_VR_RELOAD_SHELL)
		{
			vr_reload.shells += (short)inserted;
		}
		else
		{
			vr_reload.pressing = TRUE;
			vr_reload.press_time = now;
		}
		platform_log("vr reload: tick %ld: pushed home (%d rounds loaded, %d more carried)%s", now,
			magazine->rounds_loaded, magazine->rounds_total, vr_reload_decides() ? ": to be loaded at once" :
			kind == HALO_VR_RELOAD_SHELL ? ", a shell to load" : ": the game's reload pressed");
	}
	if (vr_reload.loads && (now - vr_reload.load_time > PRESS_TICKS || now < vr_reload.load_time))
	{
		platform_log("vr reload: tick %ld: the weapon could not be loaded (busy)", now);
		vr_reload.loads = 0;
	}
	/* (the trigger and the first shot after, and what the HUD shows) */
	if (vr_reload.watch_shot && magazine && !vr_reload.watch_trigger &&
		TEST_FLAG(unit_get(unit_index)->unit.control_flags, _unit_control_weapon_primary_trigger_bit))
	{
		platform_log("vr reload: tick %ld: the trigger pulled", now);
		vr_reload.watch_trigger = TRUE;
	}
	if (vr_reload.watch_shot && magazine && magazine->rounds_loaded < vr_reload.watch_rounds)
	{
		platform_log("vr reload: tick %ld: fired, %d rounds loaded", now, magazine->rounds_loaded);
		vr_reload.watch_shot = FALSE;
	}
	if (vr_reload.screenshots > 0)
	{
		vr_reload.screenshots--;
		if (vr_reload.screenshots % 12 == 11 || vr_reload.screenshots == 0)
			halo_vr_test_screenshot();
	}
	if (kind == HALO_VR_RELOAD_SHELL && !vr_reload.pressing && !reloading && vr_reload.shells > 0)
	{
		vr_reload.pressing = TRUE;
		vr_reload.press_time = now;
	}
	if (reloading)
		vr_reload.empty_logged = FALSE;
	if (vr_reload.pressing && reloading)
	{
		vr_reload.pressing = FALSE;
		vr_reload.hand_reload = TRUE;
		if (kind == HALO_VR_RELOAD_SHELL && vr_reload.shells > 0)
			vr_reload.shells--;
		platform_log("vr reload: tick %ld: the game reloads (%d rounds loaded, %d more carried)", now,
			magazine->rounds_loaded, magazine->rounds_total);
	}
	else if (vr_reload.pressing && (kind == HALO_VR_RELOAD_NONE || now - vr_reload.press_time > PRESS_TICKS ||
		now < vr_reload.press_time))
	{
		vr_reload.pressing = FALSE;
		vr_reload.shells = 0;
		platform_log("vr reload: tick %ld: the game did not reload", now);
	}
	if (vr_reload.hand_reload && !reloading && !vr_reload.pressing)
	{
		vr_reload.hand_reload = FALSE;
		if (magazine)
		{
			platform_log("vr reload: tick %ld: reloaded: %d rounds loaded, %d more carried", now, magazine->rounds_loaded,
				magazine->rounds_total);
		}
	}

	/* the weapon, told to vr.c (the shells pushed in and not yet loaded
	counted as loaded, and the one loading) */
	csmemset(&weapon_state, 0, sizeof(weapon_state));
	weapon_state.kind = kind;
	weapon_state.weapon = weapon_index;
	if (magazine)
	{
		short coming = kind == HALO_VR_RELOAD_SHELL ? vr_reload.shells + vr_reload.loads + (reloading ? 1 : 0) : 0;

		weapon_state.reloading = reloading || magazine->state == _magazine_chambering;
		weapon_state.can_reload = magazine->rounds_total > coming &&
			magazine->rounds_loaded + coming < magazine_definition->rounds_loaded_maximum &&
			(kind == HALO_VR_RELOAD_SHELL || (!reloading && !vr_reload.loads));
	}
	weapon_state.well_known = vr_reload.well_known;
	weapon_state.well[0] = vr_reload.well[0];
	weapon_state.well[1] = vr_reload.well[1];
	weapon_state.well[2] = vr_reload.well[2];
	halo_vr_reload_weapon(&weapon_state);
}

/* player_control.c, as the first local player's input is read: the game's
reload pressed, for a magazine the hand pushed home */
boolean vr_reload_pressed(void)
{
	return vr_reload.pressing;
}

/* weapons.c, as the weapon updates: whether the hand pushed home a
magazine (a shell) for it to load at once (where this machine runs it).
The shotgun's shell pushed in while the game loads one (X) goes on after it
instead (vr_reload_chain). */
boolean vr_reload_by_hand(long weapon_index)
{
	struct halo_vr_reload reload;

	if (weapon_index != vr_reload.weapon_index || vr_reload.loads <= 0 || vr_reload.kind == HALO_VR_RELOAD_NONE ||
		!vr_reload_decides() || !halo_vr_reload(&reload))
	{
		return FALSE;
	}
	if (vr_reload.kind == HALO_VR_RELOAD_SHELL &&
		weapon_get(weapon_index)->weapon.magazines[0].state == _magazine_reloading)
	{
		vr_reload.shells += vr_reload.loads;
		vr_reload.loads = 0;
		return FALSE;
	}
	return TRUE;
}

/* weapons.c, as it has loaded one the hand pushed home */
void vr_reload_loaded(long weapon_index)
{
	struct weapon_magazine *magazine = &weapon_get(weapon_index)->weapon.magazines[0];

	if (vr_reload.loads > 0)
		vr_reload.loads--;
	platform_log("vr reload: tick %ld: loaded at once: %d rounds loaded, %d more carried", game_time_get(),
		magazine->rounds_loaded, magazine->rounds_total);
	vr_reload.watch_shot = TRUE;
	vr_reload.watch_trigger = FALSE;
	vr_reload.watch_rounds = magazine->rounds_loaded;
	if (*config_string("debug.screenshot_directory"))
		vr_reload.screenshots = 37;
}

/* first_person_weapons.c, as the first-person weapon is told to play its
reload: FALSE for the hand's reload with the hands the controllers'
(vr.hands): the magazine went in by hand, and the weapon stays in the
right one, idle, while the game reloads (its animation swings the weapon
up out of the hand, and would take the magazine out and put it back). With
the game's arms they reload as ever. As vr_grenade_first_person_throw does
for the hand's throw. */
boolean vr_reload_first_person_reload(long weapon_index)
{
	struct halo_vr_reload reload;

	if (weapon_index != vr_reload.weapon_index || vr_reload.kind == HALO_VR_RELOAD_NONE ||
		!(vr_reload.pressing || vr_reload.hand_reload) || !halo_vr_reload(&reload) || !reload.hands)
	{
		return TRUE;
	}
	platform_log("vr reload: tick %ld: the first-person weapon stays in the hand", game_time_get());
	return FALSE;
}

/* weapons.c, as the weapon's magazine is empty and idle: whether the game
reloads it by itself, as it does but for the VR player's weapon reloaded by
hand (the hand, or X, reloads it), where this machine decides it */
boolean vr_reload_automatic(long weapon_index)
{
	struct halo_vr_reload reload;

	if (weapon_index != vr_reload.weapon_index || vr_reload.kind == HALO_VR_RELOAD_NONE || !vr_reload_decides() ||
		!halo_vr_reload(&reload))
	{
		return TRUE;
	}
	if (!vr_reload.empty_logged && weapon_get(weapon_index)->weapon.magazines[0].state != _magazine_reloading)
	{
		platform_log("vr reload: tick %ld: empty, not reloaded by itself (%d more carried)", game_time_get(),
			weapon_get(weapon_index)->weapon.magazines[0].rounds_total);
		vr_reload.empty_logged = TRUE;
	}
	return FALSE;
}

/* weapons.c, as the shotgun's magazine has loaded its round and would go on
to the next: whether it does, for the VR player's shotgun only for a shell
the hand pushed in meanwhile (where this machine decides it) */
boolean vr_reload_chain(long weapon_index)
{
	struct halo_vr_reload reload;

	if (weapon_index != vr_reload.weapon_index || vr_reload.kind != HALO_VR_RELOAD_SHELL || !vr_reload_decides() ||
		!halo_vr_reload(&reload))
	{
		return TRUE;
	}
	if (vr_reload.shells > 0)
	{
		vr_reload.shells--;
		return TRUE;
	}
	/* (not for one loaded at once: vr_reload_loaded logs it) */
	if (vr_reload.loads <= 0)
	{
		platform_log("vr reload: tick %ld: a shell loaded (%d), no other pushed in", game_time_get(),
			weapon_get(weapon_index)->weapon.magazines[0].rounds_loaded);
	}
	return FALSE;
}

/* the magazine's nodes placed about a parent's place (frame: world units):
the rest shrunk to nothing there, so that the magazine is drawn alone */
static void vr_reload_magazine_nodes(struct model const *model, real_matrix4x3 const *frame,
	real_matrix4x3 *node_matrices)
{
	short node_index;

	for (node_index = 0; node_index < model->nodes.count; node_index++)
	{
		if (vr_reload.in_magazine[node_index])
		{
			matrix4x3_multiply(frame, &vr_reload.rest[node_index], &node_matrices[node_index]);
		}
		else
		{
			node_matrices[node_index] = *frame;
			node_matrices[node_index].scale = 0.0f;
		}
	}
}

/* first_person_weapons.c, as the first-person weapon is posed for its draw
(its model's nodes in the world): where its magazine goes, for the hand;
while it is out, not drawn in the weapon */
void vr_reload_weapon_nodes(long model_index, long weapon_index, real_matrix4x3 *node_matrices)
{
	struct halo_vr_reload reload;
	struct model *model;
	real_matrix4x3 hand;
	real_point3d well;
	real units;
	short node_index;

	vr_reload.well_drawn = FALSE;
	if (weapon_index != vr_reload.weapon_index || vr_reload.kind == HALO_VR_RELOAD_NONE ||
		model_index != vr_reload.model_index || !vr_reload_current() || !halo_vr_reload(&reload) ||
		!vr_render_right_hand(&hand, &units))
	{
		return;
	}
	model = model_definition_get(model_index);
	if (vr_reload.magazine_node != NONE)
	{
		real_matrix4x3 const *parent = &node_matrices[vr_reload.parent_node];

		matrix4x3_multiply(parent, &vr_reload.rest[vr_reload.magazine_node], &vr_reload.well_matrix);
		if (vr_reload.kind == HALO_VR_RELOAD_SHELL)
		{
			vr_reload.well_matrix.position.x -= parent->up.i * SHELL_PORT_BELOW * units;
			vr_reload.well_matrix.position.y -= parent->up.j * SHELL_PORT_BELOW * units;
			vr_reload.well_matrix.position.z -= parent->up.k * SHELL_PORT_BELOW * units;
		}
		for (node_index = 0; node_index < model->nodes.count; node_index++)
		{
			if (!vr_reload.in_magazine[node_index])
				continue;
			if (reload.ejected)
				node_matrices[node_index].scale = 0.0f;
		}
	}
	else
	{
		/* (the stand-in's place under the grip) */
		vr_reload.well_matrix = hand;
		vr_reload.well_matrix.position.x += (hand.forward.i * stand_in_well[0] + hand.left.i * stand_in_well[1] +
			hand.up.i * stand_in_well[2]) * units;
		vr_reload.well_matrix.position.y += (hand.forward.j * stand_in_well[0] + hand.left.j * stand_in_well[1] +
			hand.up.j * stand_in_well[2]) * units;
		vr_reload.well_matrix.position.z += (hand.forward.k * stand_in_well[0] + hand.left.k * stand_in_well[1] +
			hand.up.k * stand_in_well[2]) * units;
	}
	vr_reload.well_drawn = TRUE;
	matrix4x3_inverse_transform_point(&hand, &vr_reload.well_matrix.position, &well);
	vr_reload.well[0] = well.x / units;
	vr_reload.well[1] = well.y / units;
	vr_reload.well[2] = well.z / units;
	vr_reload.well_known = TRUE;
}

/* the magazine drawn with its frame (the place of its node, the
stand-in's), lit up as strong as this (well: where it goes) */
static void vr_reload_draw_magazine(real_matrix4x3 const *magazine_frame, real highlight, boolean well, real units,
	unsigned long flags)
{
	struct render_model_effect model_effect;
	struct render_lighting lighting;
	real_matrix4x3 node_matrices[MAXIMUM_NODES_PER_MODEL];
	struct model *model;
	long model_index;

	csmemset(&model_effect, 0, sizeof(model_effect));
	model_effect.type = _render_model_effect_type_none;
	if (highlight > 0.0f)
		vr_holster_highlight(well, vr_reload.unit_index, highlight, units, &model_effect, &lighting);
	else
		lighting = *object_get_cached_render_lighting(vr_reload.unit_index, FLT_MAX);
	if (vr_reload.magazine_node != NONE)
	{
		real_matrix4x3 rest_inverse, frame;

		model_index = vr_reload.model_index;
		model = model_definition_get(model_index);
		/* (the parent's frame that puts the magazine's node there) */
		matrix4x3_inverse(&vr_reload.rest[vr_reload.magazine_node], &rest_inverse);
		matrix4x3_multiply(magazine_frame, &rest_inverse, &frame);
		vr_reload_magazine_nodes(model, &frame, node_matrices);
	}
	else
	{
		short node_index;

		model_index = vr_reload.stand_in_model_index;
		if (model_index == NONE)
			return;
		model = model_definition_get(model_index);
		if (model->nodes.count <= 0 || model->nodes.count > MAXIMUM_NODES_PER_MODEL)
			return;
		model_get_node_matrices(model, node_matrices, &magazine_frame->position, &magazine_frame->forward,
			&magazine_frame->up);
		for (node_index = 0; node_index < model->nodes.count; node_index++)
		{
			real_point3d *position = &node_matrices[node_index].position;

			node_matrices[node_index].scale *= STAND_IN_SCALE;
			position->x = magazine_frame->position.x + (position->x - magazine_frame->position.x) * STAND_IN_SCALE;
			position->y = magazine_frame->position.y + (position->y - magazine_frame->position.y) * STAND_IN_SCALE;
			position->z = magazine_frame->position.z + (position->z - magazine_frame->position.z) * STAND_IN_SCALE;
		}
	}
	render_model(model_index, DETAIL_PIXELS, node_matrices, NULL, NULL, NULL, &lighting, &magazine_frame->position,
		0.0f, &model_effect, vr_reload.weapon_index, 0, flags);
}

/* first_person_weapons.c, as the first-person weapon is drawn (flags: its
model's): the magazine the left hand holds, the one in the pouch while one
is to be taken (lit up as the hand comes), and where it goes in the weapon
while the hand holds one (lit up, the more the nearer it comes) */
void vr_reload_draw(unsigned long flags)
{
	struct halo_vr_reload reload;
	real_matrix4x3 frame;
	real units;

	/* (the weapon and the unit as the frame's update saw them, while they
	are still the player's) */
	if (vr_reload.kind == HALO_VR_RELOAD_NONE || !vr_reload_current() || !halo_vr_reload(&reload) ||
		reload.kind == HALO_VR_RELOAD_NONE)
	{
		vr_reload.well_drawn = FALSE;
		return;
	}
	/* in the left hand, its top (where it goes in) held_offset from the
	grip, as it would be in a weapon pointing where the hand points (a
	left-hander's, in the right hand, mirrored about it, as the weapon it
	goes into is: vr_hands.c) */
	if (reload.held && vr_render_left_hand(&frame, &units))
	{
		real_matrix4x3 magazine_frame;
		real_point3d held;
		float const *o = reload.held_offset;

		if (halo_vr_left_handed())
			negate_vector3d(&frame.left, &frame.left);

		held.x = frame.position.x + (frame.forward.i * o[0] + frame.left.i * o[1] + frame.up.i * o[2]) * units;
		held.y = frame.position.y + (frame.forward.j * o[0] + frame.left.j * o[1] + frame.up.j * o[2]) * units;
		held.z = frame.position.z + (frame.forward.k * o[0] + frame.left.k * o[1] + frame.up.k * o[2]) * units;
		frame.position = held;
		if (vr_reload.magazine_node != NONE)
			matrix4x3_multiply(&frame, &vr_reload.rest[vr_reload.magazine_node], &magazine_frame);
		else
			magazine_frame = frame;
		magazine_frame.position = held;
		vr_reload_draw_magazine(&magazine_frame, 0.0f, FALSE, units, flags);
	}
	/* where it goes, while the hand holds one */
	if (reload.held && vr_reload.well_drawn)
	{
		real highlight = 1.0f - reload.well_distance / WELL_HIGHLIGHT_DISTANCE;

		if (highlight < 0.0f)
			highlight = 0.0f;
		highlight = WELL_HIGHLIGHT_LEAST + (1.0f - WELL_HIGHLIGHT_LEAST) * highlight;
		vr_reload_draw_magazine(&vr_reload.well_matrix, highlight, TRUE, units, flags);
	}
	vr_reload.well_drawn = FALSE;
	/* the pouch's, upright on the body (the tubes across the back, as the
	shoulder's holster carries a long gun), while one is to be taken */
	if (reload.wanted)
	{
		float forward[3], up[3];
		real c = (real)cos(reload.body_yaw), s = (real)sin(reload.body_yaw), highlight;

		if (reload.kind == HALO_VR_RELOAD_TUBE)
		{
			/* (forward: down 0.8, right 0.6; up: backward) */
			forward[0] = 0.6f * s;
			forward[1] = -0.6f * c;
			forward[2] = -0.8f;
			up[0] = -c;
			up[1] = -s;
			up[2] = 0.0f;
		}
		else
		{
			forward[0] = c;
			forward[1] = s;
			forward[2] = 0.0f;
			up[0] = 0.0f;
			up[1] = 0.0f;
			up[2] = 1.0f;
		}
		if (vr_render_body_place(reload.pouch, forward, up, &frame, &units))
		{
			real_matrix4x3 magazine_frame;

			if (reload.pouch_distance <= 0.0f)
				highlight = 1.0f;
			else if (reload.pouch_distance < POUCH_HIGHLIGHT_DISTANCE)
				highlight = (1.0f - reload.pouch_distance / POUCH_HIGHLIGHT_DISTANCE) * POUCH_HIGHLIGHT_NEAR;
			else
				highlight = 0.0f;
			if (vr_reload.magazine_node != NONE)
				matrix4x3_multiply(&frame, &vr_reload.rest[vr_reload.magazine_node], &magazine_frame);
			else
				magazine_frame = frame;
			magazine_frame.position = frame.position;
			vr_reload_draw_magazine(&magazine_frame, highlight, FALSE, units, flags);
		}
	}
}

#endif
