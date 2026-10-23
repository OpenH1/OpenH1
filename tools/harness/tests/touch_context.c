/*
TOUCH_CONTEXT.C (test)

What the Android touch controls' buttons show of player 1
(port/linux/game/touch_game.c, touch_game_context and touch_game_reload_due):
an action the HUD prompts, a magazine to reload, no grenades of the current
type, a vehicle's seat. test_touch_context.py takes the enums and structure
(config.inc) and the code (under_test.inc).
*/

#include "harness.h"

typedef unsigned short word;

#include "config.inc"

/* the fake world: one player, one unit, one weapon */
struct player_datum
{
	long unit_index;
	short action_result;
};

struct unit_datum
{
	struct
	{
		long parent_object_index;
	} object;
	struct
	{
		short parent_seat_index;
		short current_weapon_index;
		char current_grenade_index;
		char grenade_counts[NUMBER_OF_UNIT_GRENADE_TYPES];
	} unit;
};

struct weapon_interface_state
{
	real heat;
	real age;
	boolean overheated;
	short magazine_count;
	struct weapon_interface_magazine_state magazines[2];
};

static long the_player_index;
static struct player_datum the_player;
static struct unit_datum the_unit;
static long the_weapon_index;
static struct weapon_interface_state the_weapon;

#define local_player_get_player_index(local) (the_player_index)
#define player_get(index) (&the_player)
#define unit_get(index) (&the_unit)
#define unit_inventory_get_weapon(unit_index, index) (the_weapon_index)
#define weapon_build_weapon_interface_state(weapon_index, state) (*(state) = the_weapon)

#include "under_test.inc"

/* a player on foot with a rifle half full, plenty in reserve, and grenades */
static void on_foot(void)
{
	the_player_index = 0x10000;
	the_player.unit_index = 0x20000;
	the_player.action_result = _player_action_result_reload;
	memset(&the_unit, 0, sizeof(the_unit));
	the_unit.object.parent_object_index = NONE;
	the_unit.unit.parent_seat_index = NONE;
	the_unit.unit.current_weapon_index = 0;
	the_unit.unit.current_grenade_index = 0;
	the_unit.unit.grenade_counts[0] = 2;
	the_weapon_index = 0x30000;
	memset(&the_weapon, 0, sizeof(the_weapon));
	the_weapon.magazine_count = 1;
	the_weapon.magazines[0].rounds_loaded = 30;
	the_weapon.magazines[0].rounds_loaded_maximum = 60;
	the_weapon.magazines[0].rounds_remaining = 120;
	the_weapon.magazines[0].rounds_remaining_maximum = 600;
}

static void case_on_foot(void)
{
	on_foot();
	CHECK(touch_game_context() == 0, "nothing to show: %d", touch_game_context());
	the_player_index = NONE;
	CHECK(touch_game_context() == 0, "no player");
	on_foot();
	the_player.unit_index = NONE;
	CHECK(touch_game_context() == 0, "a dead player");
}

static void case_action(void)
{
	on_foot();
	the_player.action_result = _player_action_result_pickup_weapon;
	CHECK(touch_game_context() == _touch_context_action, "a weapon to pick up glows");
	the_player.action_result = _player_action_result_enter_vehicle;
	CHECK(touch_game_context() == _touch_context_action, "a vehicle to enter glows");
	the_player.action_result = _player_action_result_exit_vehicle;
	CHECK(!(touch_game_context() & _touch_context_action), "exiting is the vehicle's, not a glow");
}

static void case_reload(void)
{
	struct weapon_interface_magazine_state *magazine;

	on_foot();
	magazine = &the_weapon.magazines[0];
	magazine->rounds_loaded = 14;
	CHECK(touch_game_context() == _touch_context_reload, "under a quarter loaded");
	magazine->rounds_loaded = 15;
	CHECK(touch_game_context() == 0, "a quarter loaded is not low");
	magazine->rounds_loaded = 0;
	CHECK(touch_game_context() == _touch_context_reload, "empty with a reserve");
	magazine->rounds_remaining = 0;
	CHECK(touch_game_context() == 0, "empty with no reserve has nothing to load");
	magazine->rounds_remaining = 120;
	magazine->reloading = TRUE;
	CHECK(touch_game_context() == 0, "not while reloading");
}

static void case_no_magazine(void)
{
	on_foot();
	/* a plasma weapon: its charge, no magazine (whatever the state holds past its count) */
	the_weapon.magazine_count = 0;
	the_weapon.magazines[0].rounds_loaded = 0;
	CHECK(touch_game_context() == 0, "a plasma weapon never pulses");
	the_weapon.magazine_count = 1;
	the_weapon.magazines[0].rounds_loaded_maximum = 0;
	the_weapon.magazines[0].rounds_loaded = 0;
	CHECK(touch_game_context() == 0, "a magazine of no size never pulses");
	on_foot();
	the_weapon_index = NONE;
	CHECK(touch_game_context() == 0, "no weapon");
}

static void case_grenades(void)
{
	on_foot();
	the_unit.unit.grenade_counts[0] = 0;
	the_unit.unit.grenade_counts[1] = 4;
	CHECK(touch_game_context() == _touch_context_no_grenades, "none of the current type dims");
	the_unit.unit.current_grenade_index = 1;
	CHECK(touch_game_context() == 0, "the other type's count is not the current one's");
	the_unit.unit.current_grenade_index = NUMBER_OF_UNIT_GRENADE_TYPES;
	CHECK(touch_game_context() == 0, "an index past the counts is not read");
}

static void case_vehicle(void)
{
	on_foot();
	the_unit.object.parent_object_index = 0x40000;
	the_unit.unit.parent_seat_index = 0;
	the_player.action_result = _player_action_result_exit_vehicle;
	the_weapon_index = NONE;
	CHECK(touch_game_context() == _touch_context_vehicle, "a driver's seat");
	the_unit.unit.parent_seat_index = NONE;
	CHECK(!(touch_game_context() & _touch_context_vehicle), "carried, not seated");
}

int main(
	int argc,
	char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	CASE("on-foot")
	{
		case_on_foot();
		return 0;
	}
	CASE("action")
	{
		case_action();
		return 0;
	}
	CASE("reload")
	{
		case_reload();
		return 0;
	}
	CASE("no-magazine")
	{
		case_no_magazine();
		return 0;
	}
	CASE("grenades")
	{
		case_grenades();
		return 0;
	}
	CASE("vehicle")
	{
		case_vehicle();
		return 0;
	}
	fprintf(stderr, "unknown case: %s\n", case_name);
	return 2;
}
