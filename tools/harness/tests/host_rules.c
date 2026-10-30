/*
HOST_RULES.C (test)

The gametype's rules a player host changes during a game
(port/linux/game/network_host_rules.c, Host Controls' RULES): a team's
vehicle set stepped, a rule changed on the variant and options, which rules
change a game under way at once (the host's own always; the flags every
machine draws by only where every machine hears the host's message), the
message checked whole and its flags taken alone; and the game's side
(source/game/game_engine.c): a vehicle set changed in a game under way
swaps the vehicles nobody rides for the new set's at the map's placements,
keeps a ridden one until it is empty and places the new vehicle its pad
waits for then; a changed loadout is given at the next spawn, and a living
player keeps theirs. test_host_rules.py takes the code (rules.inc,
engine.inc) and the constants (config.inc).
*/

#include "harness.h"

typedef unsigned short word;
#define csmemset memset
#define csmemcpy memcpy
#define SET_FLAG(flags, bit, value) ((value) ? ((flags) |= FLAG(bit)) : ((flags) &= ~FLAG(bit)))
#define NUMBEROF(array) ((long)(sizeof(array) / sizeof((array)[0])))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define VALID_INDEX(index, count) ((index) >= 0 && (index) < (count))
#define SHORT_MAX 32767
#define TICKS_PER_SECOND 30

#include "config.inc"

/* ---------- the variant (the parts the rules read) */

struct universal_variant
{
	boolean teams;
	unsigned long flags;
	long weapon_set;
	long vehicle_set;
};

struct game_variant
{
	struct universal_variant universal_variant;
	word flags;
};

struct game_variant_options
{
	byte vehicle_set[2];
	byte vehicle_counts[2][NUMBER_OF_VARIANT_VEHICLES];
	byte loadout;
	byte primary_weapon;
	byte secondary_weapon;
};

#include "rules.inc"

/* ---------- the world: objects, the map's vehicle placements */

enum
{
	_object_type_biped = 0,
	_object_type_vehicle = 1,
	_object_type_weapon = 2,
	_object_mask_vehicle = 1 << 1,
	_object_dead_bit = 0,
	_scenario_object_placement_not_automatic_bit = 0,
	_object_header_being_deleted_bit = 2,
	MAXIMUM_TEST_OBJECTS = 64,
	MAXIMUM_INVENTORY = 4,
	TEST_WARTHOG = 100,
	TEST_GHOST = 101,
	TEST_SCORPION = 102,
	TEST_WEAPON_BASE = 200,
};

typedef struct { real yaw, pitch, roll; } real_euler_angles3d;

struct object_body
{
	real_point3d position;
	unsigned long damage_flags;
	long first_child_object_index;
	long next_object_index;
	short type;
	short name_index;
};

struct object_datum
{
	long definition_index;
	struct object_body object;
	struct
	{
		long driver_object_index;
		long gunner_object_index;
	} unit;
	/* (a unit's weapons, its first the one it holds) */
	long inventory[MAXIMUM_INVENTORY];
	short inventory_count;
};
#define unit_datum object_datum

/* (an object deleted stays, marked, until the objects' update takes it
out, as the game's does) */
struct object_header_datum { unsigned long flags; };

static struct
{
	boolean exists[MAXIMUM_TEST_OBJECTS];
	struct object_header_datum headers[MAXIMUM_TEST_OBJECTS];
	struct object_datum objects[MAXIMUM_TEST_OBJECTS];
	short deleted;
	long time;
	boolean distributed_client;
	struct game_variant_options options;
} world;

struct object_iterator { long index; long next; };
struct object_placement_data
{
	long definition_index;
	real_point3d position;
	real_vector3d forward;
	real_vector3d up;
	short variant_number;
};

static struct object_datum *object_get(long index) { return &world.objects[index]; }
#define object_header_get(index) (&world.headers[index])
static struct unit_datum *unit_try_and_get(long index)
{
	return VALID_INDEX(index, MAXIMUM_TEST_OBJECTS) && world.exists[index] ? &world.objects[index] : NULL;
}
static void object_iterator_new(struct object_iterator *iterator, long mask, long flags)
{
	(void)mask;
	(void)flags;
	iterator->next = 0;
	iterator->index = NONE;
}
static boolean object_iterator_next(struct object_iterator *iterator)
{
	for (; iterator->next < MAXIMUM_TEST_OBJECTS; iterator->next++)
	{
		if (world.exists[iterator->next] && world.objects[iterator->next].object.type == _object_type_vehicle)
		{
			iterator->index = iterator->next++;
			return TRUE;
		}
	}
	return FALSE;
}
static void object_delete(long index)
{
	if (unit_try_and_get(index) && !TEST_FLAG(world.headers[index].flags, _object_header_being_deleted_bit))
	{
		world.headers[index].flags |= FLAG(_object_header_being_deleted_bit);
		world.deleted++;
	}
}
/* (the objects' update) the deleted taken out */
static void objects_collect(void)
{
	long index;

	for (index = 0; index < MAXIMUM_TEST_OBJECTS; index++)
	{
		if (TEST_FLAG(world.headers[index].flags, _object_header_being_deleted_bit))
		{
			world.exists[index] = FALSE;
			world.headers[index].flags = 0;
		}
	}
}
static long test_object_new(long definition_index, real_point3d const *position, short type)
{
	long index;

	for (index = 0; index < MAXIMUM_TEST_OBJECTS; index++)
	{
		if (!world.exists[index])
		{
			memset(&world.objects[index], 0, sizeof(world.objects[index]));
			world.exists[index] = TRUE;
			world.headers[index].flags = 0;
			world.objects[index].definition_index = definition_index;
			world.objects[index].object.position = *position;
			world.objects[index].object.type = type;
			world.objects[index].object.first_child_object_index = NONE;
			world.objects[index].object.next_object_index = NONE;
			world.objects[index].object.name_index = NONE;
			world.objects[index].unit.driver_object_index = NONE;
			world.objects[index].unit.gunner_object_index = NONE;
			return index;
		}
	}
	return NONE;
}
static void object_placement_data_new(struct object_placement_data *data, long definition_index, long owner)
{
	(void)owner;
	memset(data, 0, sizeof(*data));
	data->definition_index = definition_index;
}
static long object_new(struct object_placement_data *data)
{
	return test_object_new(data->definition_index, &data->position, data->definition_index >= TEST_WEAPON_BASE ?
		_object_type_weapon : _object_type_vehicle);
}
static void object_claim_scenario_name(long object_index, short name_index) { (void)object_index; (void)name_index; }
static void vectors3d_from_euler_angles3d(real_vector3d *forward, real_vector3d *up, real_euler_angles3d const *angles)
{
	(void)angles;
	forward->i = 1.0f;
	forward->j = forward->k = 0.0f;
	up->k = 1.0f;
	up->i = up->j = 0.0f;
}
static boolean network_game_distributed_client(void) { return world.distributed_client; }
static long game_time_get(void) { return world.time; }
static struct game_variant_options const *game_variant_options_get(void) { return &world.options; }

/* the map: four placements (a Warthog, two Ghosts, a Scorpion) and a Ghost
the map does not place by itself (not automatic) */
struct tag_block { long count; void *address; };
#define TAG_BLOCK_GET_ELEMENT(block, index, type) (&((type *)(block)->address)[index])
struct scenario_object_datum
{
	short palette_entry_index;
	short name_index;
	unsigned long placement_flags;
	real_point3d position;
	real_euler_angles3d rotation;
};
struct scenario_object_palette_entry { struct { long index; } reference; };
struct scenario { struct tag_block object_names; };

static struct scenario_object_datum placements[5] =
{
	{ 0, NONE, 0, { 0.0f, 0.0f, 0.0f }, { 0 } },
	{ 1, NONE, 0, { 10.0f, 0.0f, 0.0f }, { 0 } },
	{ 1, NONE, 0, { 20.0f, 0.0f, 0.0f }, { 0 } },
	{ 2, NONE, 0, { 30.0f, 0.0f, 0.0f }, { 0 } },
	{ 1, NONE, FLAG(_scenario_object_placement_not_automatic_bit), { 40.0f, 0.0f, 0.0f }, { 0 } },
};
static struct scenario_object_palette_entry palette_entries[3] = { { { TEST_WARTHOG } }, { { TEST_GHOST } },
	{ { TEST_SCORPION } } };
static struct tag_block placement_block = { 5, placements };
static struct tag_block palette_block = { 3, palette_entries };
static struct scenario test_scenario = { { 0, NULL } };

static struct scenario *global_scenario_get(void) { return &test_scenario; }
static struct tag_block *scenario_get_object_type_scenario_datums(struct scenario *scenario, short type, long *size)
{
	(void)scenario;
	(void)type;
	*size = sizeof(struct scenario_object_datum);
	return &placement_block;
}
static struct tag_block *scenario_get_object_type_scenario_palette(struct scenario *scenario, short type)
{
	(void)scenario;
	(void)type;
	return &palette_block;
}
static void *tag_block_get_element_with_size(struct tag_block *block, long index, long size)
{
	return (byte *)block->address + index * size;
}
static boolean custom_edition_vehicle_placement_allowed(struct scenario_object_datum const *placement)
{
	(void)placement;
	return TRUE;
}
/* (the game's: a set's placements of its type; default, all; none, none) */
static void game_engine_vehicle_placement_begin(void) {}
static boolean game_engine_vehicle_placement_allowed(struct scenario_object_datum const *placement,
	struct tag_block *palette)
{
	byte set = world.options.vehicle_set[0];

	(void)palette;
	if (set == _game_engine_vehicles_default)
		return TRUE;
	if (set == _game_engine_vehicles_none)
		return FALSE;
	return placement->palette_entry_index == set - _game_engine_vehicles_warthog;
}
static long game_engine_remap_object_definition(long definition_index) { return definition_index; }
static long object_new_from_scenario(struct scenario_object_datum *placement, struct tag_block *palette)
{
	struct object_placement_data data;

	object_placement_data_new(&data, TAG_BLOCK_GET_ELEMENT(palette, placement->palette_entry_index,
		struct scenario_object_palette_entry)->reference.index, NONE);
	data.position = placement->position;
	return object_new(&data);
}

/* a unit's weapons (units.c's), the globals' weapon list */
static void unit_delete_all_weapons(long unit_index) { world.objects[unit_index].inventory_count = 0; }
static boolean unit_has_weapon_definition_index(long unit_index, long definition_index)
{
	short index;

	for (index = 0; index < world.objects[unit_index].inventory_count; index++)
	{
		if (world.objects[world.objects[unit_index].inventory[index]].definition_index == definition_index)
			return TRUE;
	}
	return FALSE;
}
static boolean unit_add_weapon_to_inventory(long unit_index, long weapon_index, short mode)
{
	struct object_datum *unit = &world.objects[unit_index];

	(void)mode;
	if (unit->inventory_count >= MAXIMUM_INVENTORY)
		return FALSE;
	unit->inventory[unit->inventory_count++] = weapon_index;
	return TRUE;
}
static long list_index_to_weapon_definition_index(long list_index) { return TEST_WEAPON_BASE + list_index; }
static long local_random_range(long lower, long upper) { (void)upper; return lower; }

/* the game's statics of the vehicle homes (game_engine.c's) */
enum { MAXIMUM_VEHICLE_HOMES = 1024 };
static struct
{
	long object_index;
	long definition_index;
	real_point3d position;
	real_vector3d forward;
	real_vector3d up;
	long idle_since;
	short name_index;
} game_engine_vehicle_homes[MAXIMUM_VEHICLE_HOMES];
static short game_engine_vehicle_home_count = NONE;
static boolean game_engine_vehicle_homes_waiting[MAXIMUM_VEHICLE_HOMES];
static long game_engine_vehicles_leaving[MAXIMUM_VEHICLE_HOMES];
static short game_engine_vehicles_leaving_count = 0;
static void *game_engine = &world;

#include "engine.inc"

/* ---------- the cases */

static short vehicles_of(long definition_index)
{
	short count = 0;
	long index;

	for (index = 0; index < MAXIMUM_TEST_OBJECTS; index++)
	{
		if (world.exists[index] && world.objects[index].object.type == _object_type_vehicle &&
			!TEST_FLAG(world.headers[index].flags, _object_header_being_deleted_bit) &&
			(definition_index == NONE || world.objects[index].definition_index == definition_index))
		{
			count++;
		}
	}
	return count;
}

static long vehicle_at(real x)
{
	long index;

	for (index = 0; index < MAXIMUM_TEST_OBJECTS; index++)
	{
		if (world.exists[index] && world.objects[index].object.type == _object_type_vehicle &&
			!TEST_FLAG(world.headers[index].flags, _object_header_being_deleted_bit) &&
			world.objects[index].object.position.x > x - 1.0f && world.objects[index].object.position.x < x + 1.0f)
		{
			return index;
		}
	}
	return NONE;
}

/* a set changed in a game under way: a Warthog nobody rides, a Ghost
someone rides on its pad, a Ghost left empty away from its pad, a wrecked
Warthog; Ghosts asked for */
static void case_vehicles(void)
{
	real_point3d point;
	long rider, ridden, moved, empty, wreck;
	short removed, placed, waiting;

	memset(&world, 0, sizeof(world));
	world.options.vehicle_set[0] = world.options.vehicle_set[1] = _game_engine_vehicles_default;
	point = placements[0].position;
	empty = test_object_new(TEST_WARTHOG, &point, _object_type_vehicle);
	point = placements[1].position;
	ridden = test_object_new(TEST_GHOST, &point, _object_type_vehicle);
	rider = test_object_new(1, &point, _object_type_biped);
	world.objects[ridden].unit.driver_object_index = rider;
	point.x = 55.0f;
	moved = test_object_new(TEST_GHOST, &point, _object_type_vehicle);
	point.x = 70.0f;
	wreck = test_object_new(TEST_WARTHOG, &point, _object_type_vehicle);
	world.objects[wreck].object.damage_flags = FLAG(_object_dead_bit);
	world.options.vehicle_set[0] = world.options.vehicle_set[1] = _game_engine_vehicles_ghost;
	game_engine_vehicle_set_changed(&removed, &placed, &waiting);
	/* (their slots another's now: where they were) */
	(void)empty;
	(void)moved;
	(void)wreck;
	CHECK(vehicle_at(0.0f) == NONE && vehicle_at(55.0f) == NONE && vehicle_at(70.0f) == NONE,
		"the vehicles nobody rides kept");
	CHECK(world.exists[ridden] && world.objects[ridden].unit.driver_object_index == rider, "the ridden Ghost gone");
	CHECK(removed == 3 && placed == 1 && waiting == 1, "%d gone, %d placed, %d waiting: not 3, 1 and 1", removed,
		placed, waiting);
	CHECK(vehicles_of(TEST_WARTHOG) == 0 && vehicles_of(TEST_SCORPION) == 0, "another set's vehicle placed");
	CHECK(vehicle_at(20.0f) != NONE && world.objects[vehicle_at(20.0f)].definition_index == TEST_GHOST,
		"no Ghost on the clear pad");
	CHECK(vehicle_at(40.0f) == NONE, "a placement not automatic placed");
	CHECK(vehicles_of(TEST_GHOST) == 2, "%d Ghosts, not the ridden one and the new one", vehicles_of(TEST_GHOST));
	CHECK(game_engine_vehicle_home_count == 2, "%d homes, not 2", game_engine_vehicle_home_count);
	/* (changed again in the same frame, its deleted vehicles not yet out:
	they are gone, and no pad is held by them) */
	game_engine_vehicle_set_changed(&removed, &placed, &waiting);
	CHECK(removed == 1 && placed == 1 && waiting == 1, "again in the frame: %d gone, %d placed, %d waiting", removed,
		placed, waiting);
	CHECK(vehicles_of(TEST_GHOST) == 2, "%d Ghosts after the second change", vehicles_of(TEST_GHOST));
	objects_collect();
	/* (still ridden: it stays, and the pad's Ghost waits) */
	world.time = TICKS_PER_SECOND;
	game_engine_update_vehicle_set_change();
	CHECK(world.exists[ridden] && vehicles_of(TEST_GHOST) == 2, "the ridden Ghost gone, or another on its pad");
	/* (the rider gets out: the old Ghost goes, the pad's comes) */
	world.objects[ridden].unit.driver_object_index = NONE;
	world.time = 2 * TICKS_PER_SECOND;
	removed = world.deleted;
	game_engine_update_vehicle_set_change();
	CHECK(world.deleted == removed + 1, "the Ghost left empty not gone");
	objects_collect();
	CHECK(vehicle_at(10.0f) != NONE && world.objects[vehicle_at(10.0f)].definition_index == TEST_GHOST &&
		vehicles_of(TEST_GHOST) == 2, "the waiting Ghost not placed once its pad was clear");
	CHECK(!game_engine_vehicle_homes_waiting[0] && !game_engine_vehicle_homes_waiting[1], "a home still waits");
	/* (none: every vehicle goes) */
	world.options.vehicle_set[0] = world.options.vehicle_set[1] = _game_engine_vehicles_none;
	game_engine_vehicle_set_changed(&removed, &placed, &waiting);
	CHECK(vehicles_of(NONE) == 0 && removed == 2 && placed == 0, "vehicles left with none asked for");
	objects_collect();
	/* (a client changes nothing: the host's objects are its) */
	point = placements[0].position;
	empty = test_object_new(TEST_WARTHOG, &point, _object_type_vehicle);
	world.distributed_client = TRUE;
	world.options.vehicle_set[0] = world.options.vehicle_set[1] = _game_engine_vehicles_ghost;
	game_engine_vehicle_set_changed(&removed, &placed, &waiting);
	CHECK(world.exists[empty] && vehicles_of(NONE) == 1, "a client swapped vehicles");
}

/* a loadout changed during the game: a living player keeps theirs, the next
spawn takes the new one */
static void case_loadout(void)
{
	struct game_variant variant;
	real_point3d point = { 0.0f, 0.0f, 0.0f };
	long living, weapon, spawned;
	short tries;

	memset(&world, 0, sizeof(world));
	memset(&variant, 0, sizeof(variant));
	world.options.loadout = _loadout_custom;
	world.options.primary_weapon = _loadout_weapon_assault_rifle;
	world.options.secondary_weapon = _loadout_weapon_pistol;
	living = test_object_new(1, &point, _object_type_biped);
	game_engine_give_loadout(living);
	weapon = world.objects[living].inventory[0];
	CHECK(world.objects[living].inventory_count == 2 &&
		world.objects[weapon].definition_index == TEST_WEAPON_BASE + _weapon_list_assault_rifle, "the first spawn's");
	for (tries = 0; tries < NUMBER_OF_LOADOUT_WEAPONS && world.options.primary_weapon != _loadout_weapon_sniper_rifle;
		tries++)
	{
		network_host_rules_change(&variant, &world.options, _host_rule_primary, 1, FALSE);
	}
	CHECK(world.options.primary_weapon == _loadout_weapon_sniper_rifle, "the primary not stepped to the sniper rifle");
	CHECK(world.objects[living].inventory_count == 2 && world.objects[living].inventory[0] == weapon &&
		world.objects[weapon].definition_index == TEST_WEAPON_BASE + _weapon_list_assault_rifle,
		"the living player's weapons changed");
	spawned = test_object_new(1, &point, _object_type_biped);
	game_engine_give_loadout(spawned);
	CHECK(world.objects[spawned].inventory_count == 2 &&
		world.objects[world.objects[spawned].inventory[0]].definition_index ==
			TEST_WEAPON_BASE + _weapon_list_sniper_rifle &&
		world.objects[world.objects[spawned].inventory[1]].definition_index == TEST_WEAPON_BASE + _weapon_list_pistol,
		"the next spawn not given the new primary");
	/* (STARTING WEAPONS back to the map's: the variant's own) */
	CHECK(network_host_rules_change(&variant, &world.options, _host_rule_weapons, 1, FALSE) &&
		world.options.loadout == _loadout_category, "STARTING WEAPONS not the map's");
	CHECK(network_host_rules_change(&variant, &world.options, _host_rule_weapon_set, -1, FALSE) &&
		variant.universal_variant.weapon_set == HOST_RULES_WEAPON_SETS - 1, "the weapon set stepped left from the first");
}

/* the vehicle sets in their order, and a team's set changed */
static void case_sets(void)
{
	struct game_variant variant;
	struct game_variant_options options;
	byte set = _game_engine_vehicles_default;
	short steps;

	for (steps = 0; steps < 8; steps++)
		set = network_host_rules_vehicle_step(set, 1, FALSE);
	CHECK(set == VARIANT_VEHICLE_SET_PC, "after Gun Turrets: %d, not all the map's", set);
	CHECK(network_host_rules_vehicle_step(set, 1, FALSE) == _game_engine_vehicles_default, "no wrapping round");
	CHECK(network_host_rules_vehicle_step(set, 1, TRUE) == VARIANT_VEHICLE_SET_CUSTOM, "custom not offered");
	CHECK(network_host_rules_vehicle_step(_game_engine_vehicles_default, -1, FALSE) == VARIANT_VEHICLE_SET_PC,
		"left from the first");
	CHECK(network_host_rules_vehicle_step(0x77, 1, FALSE) == _game_engine_vehicles_default, "an unknown set");
	memset(&variant, 0, sizeof(variant));
	memset(&options, 0, sizeof(options));
	/* (free for all: both sides') */
	CHECK(network_host_rules_change(&variant, &options, _host_rule_vehicles, 1, FALSE) &&
		options.vehicle_set[0] == _game_engine_vehicles_none && options.vehicle_set[1] == _game_engine_vehicles_none &&
		variant.universal_variant.vehicle_set == _game_engine_vehicles_none, "free for all's vehicles: %d %d",
		options.vehicle_set[0], options.vehicle_set[1]);
	/* (a team game's: blue's alone; the PC version's types are no Xbox set) */
	variant.universal_variant.teams = TRUE;
	options.vehicle_set[1] = 4;
	CHECK(network_host_rules_change(&variant, &options, _host_rule_vehicles_blue, 1, FALSE) &&
		options.vehicle_set[1] == 5 && options.vehicle_set[0] == _game_engine_vehicles_none, "blue's vehicles: %d %d",
		options.vehicle_set[0], options.vehicle_set[1]);
	options.vehicle_set[0] = 4;
	network_host_rules_change(&variant, &options, _host_rule_vehicles, 1, FALSE);
	CHECK(options.vehicle_set[0] == 5 && variant.universal_variant.vehicle_set == 0,
		"Rocket Warthogs as the Xbox's set %ld", variant.universal_variant.vehicle_set);
	/* (the flags: shields off is the variant's no shields) */
	CHECK(network_host_rules_change(&variant, &options, _host_rule_shields, 1, FALSE) &&
		TEST_FLAG(variant.universal_variant.flags, _game_variant_no_shields_bit), "SHIELDS not off");
	CHECK(!network_host_rules_change(&variant, &options, _host_rule_save, 1, FALSE), "SAVE changed a rule");
}

/* which rules change a game under way at once */
static void case_live(void)
{
	short rule;

	for (rule = 0; rule < NUMBER_OF_HOST_RULES; rule++)
	{
		boolean flag = rule == _host_rule_shields || rule == _host_rule_motion_tracker ||
			rule == _host_rule_infinite_grenades || rule == _host_rule_invisible;

		CHECK(network_host_rules_client_side(rule) == flag, "rule %d every machine's: %d", rule, !flag);
		CHECK(network_host_rules_live_for(rule, TRUE, FALSE, 3), "rule %d waits in a lobby", rule);
		CHECK(network_host_rules_live_for(rule, FALSE, TRUE, 0), "rule %d waits in an OpenH1 game", rule);
		CHECK(network_host_rules_live_for(rule, FALSE, FALSE, 0) == !flag, "rule %d in a Legacy game", rule);
		CHECK(network_host_rules_live_for(rule, FALSE, TRUE, 1) == !flag, "rule %d in a game of OpenCE's", rule);
	}
}

/* the message whole, and a client's flags as it has them */
static void case_message(void)
{
	struct host_rules_message message;
	unsigned long teams_bit = FLAG(_game_variant_allow_friendly_navpoints_bit);
	unsigned long flags;

	memset(&message, 0, sizeof(message));
	message.format = HOST_RULES_FORMAT;
	message.flags = FLAG(_game_variant_no_shields_bit) | FLAG(_game_variant_infinite_grenades_bit);
	CHECK(network_host_rules_message_valid(&message), "a whole message refused");
	message.format = HOST_RULES_FORMAT + 1;
	CHECK(!network_host_rules_message_valid(&message), "another format taken");
	message.format = HOST_RULES_FORMAT;
	message.flags |= FLAG(_game_variant_generic_starting_equipment_bit);
	CHECK(!network_host_rules_message_valid(&message), "a flag the message does not carry taken");
	message.flags &= ~FLAG(_game_variant_generic_starting_equipment_bit);
	message.pad[1] = 1;
	CHECK(!network_host_rules_message_valid(&message), "padding not zero taken");
	flags = network_host_rules_client_flags(teams_bit | FLAG(_game_variant_draw_object_in_motion_sensor_bit),
		FLAG(_game_variant_no_shields_bit) | FLAG(_game_variant_generic_starting_equipment_bit));
	CHECK(flags == (teams_bit | FLAG(_game_variant_no_shields_bit)), "the client's flags %lx", flags);
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	CASE("vehicles") case_vehicles();
	else CASE("loadout") case_loadout();
	else CASE("sets") case_sets();
	else CASE("live") case_live();
	else CASE("message") case_message();
	else
	{
		fprintf(stderr, "no case %s\n", case_name);
		return 2;
	}
	printf("%s: passed\n", case_name);
	return 0;
}
