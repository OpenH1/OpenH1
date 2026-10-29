/*
PLAYER_ACTION_DELETED.C (test)

A player's action let go with its object (players.c's
players_handle_deleted_object): the action (the prompt the HUD draws) is
chosen once a tick, and its object can be deleted before the next tick
chooses again (an item another unit takes); the deleted object's players'
actions are cleared as player_reset_action_result clears them, the others'
kept, and a deleted unit's player still dies. test_player_action_deleted.py takes the
code (under_test.inc).
*/

#include "harness.h"

#define _object_mask_unit 0x3
#define _player_action_result_reload 0
#define _player_action_result_pickup_powerup 1

struct player_datum
{
	long unit_index;
	short action_result;
	long action_object_index;
};

struct object_datum
{
	struct
	{
		short type;
	} object;
};

#define PLAYERS 4

static struct player_datum players[PLAYERS];
static struct object_datum objects[16];
static long died = NONE;
static struct data_array *player_data;

struct data_iterator
{
	long index;
	long datum_index;
};

static void data_iterator_new(struct data_iterator *iterator, void *data)
{
	(void)data;
	iterator->index = 0;
	iterator->datum_index = NONE;
}

static void *data_iterator_next(struct data_iterator *iterator)
{
	if (iterator->index >= PLAYERS)
		return NULL;
	iterator->datum_index = iterator->index;
	return &players[iterator->index++];
}

static struct object_datum *object_get(long object_index)
{
	return &objects[object_index];
}

static void player_died(long player_index)
{
	died = player_index;
}

/* (the game's code assigns in its loops' conditions) */
#pragma clang diagnostic ignored "-Wparentheses"

#include "under_test.inc"

static void reset(void)
{
	long index;

	for (index = 0; index < PLAYERS; index++)
	{
		players[index].unit_index = 10 + index;
		players[index].action_result = _player_action_result_reload;
		players[index].action_object_index = NONE;
	}
	for (index = 0; index < 16; index++)
		objects[index].object.type = index >= 10 ? 0 : 3;
	died = NONE;
}

/* a health pack two players' prompts name deleted: both let go, the other
player's action kept */
static void case_item(void)
{
	reset();
	players[0].action_result = _player_action_result_pickup_powerup;
	players[0].action_object_index = 5;
	players[2].action_result = _player_action_result_pickup_powerup;
	players[2].action_object_index = 5;
	players[1].action_result = _player_action_result_pickup_powerup;
	players[1].action_object_index = 6;
	players_handle_deleted_object(5);
	CHECK(players[0].action_object_index == NONE && players[0].action_result == _player_action_result_reload,
		"player 0's action on the deleted object kept");
	CHECK(players[2].action_object_index == NONE, "player 2's action on the deleted object kept");
	CHECK(players[1].action_object_index == 6 && players[1].action_result == _player_action_result_pickup_powerup,
		"another object's action let go");
	CHECK(died == NONE, "a player died of an item's deletion");
}

/* a player's unit deleted: the player dies, as before */
static void case_unit(void)
{
	reset();
	players_handle_deleted_object(12);
	CHECK(died == 2, "the deleted unit's player did not die");
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	CASE("item") case_item();
	else CASE("unit") case_unit();
	else
	{
		fprintf(stderr, "unknown case %s\n", case_name);
		return 2;
	}
	return 0;
}
