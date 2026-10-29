/*
KING_MULTI_HILL.C (test)

King of the Hill's Multi-Hill (source/game/game_engine_king.c, OpenCE's
#467): the variant's multi_hill gives the hills in play at once (2 to 5,
else one: king_hills_wanted), which king_pick_extra_hills finds among the
map's hills, each once and each with an area; the host sends the hills
after the first behind King of the Hill's January state
(game_engine_king_write_network_state), as OpenCE's host does, and a client
takes them (game_engine_king_read_network_state), refusing a count out of
range. The map's hills are ids with or without an area (find_hill's stub
finds three points for one with an area). test_king_multi_hill.py takes the
types (king_types.inc) and the code (under_test.inc).
*/

#include "harness.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define NUMBEROF(array) ((long)(sizeof(array) / sizeof((array)[0])))
#define VALID_INDEX(index, count) ((index) >= 0 && (index) < (count))
#define csmemset memset
#define csmemcpy memcpy

typedef struct { real x, y; } real_point2d;

#include "king_types.inc"

/* the variant: what the code reads of it */
static struct
{
	struct { long score_to_win; } universal_variant;
	struct { struct { boolean moving_hill; byte multi_hill; } king; } game_engine_variant;
} variant;
#define game_engine_get_variant() (&variant)

/* the map's hills: their ids, and which have an area */
static short king_engine_num_hills;
static short king_engine_hills[MAXIMUM_HILLS];
static boolean hill_has_area[MAXIMUM_HILLS];
static struct king_globals king_globals;
static struct king_hill king_hills[MAXIMUM_HILLS_AT_ONCE];
static short king_hill_count;

static short random_start;
#define random_range(low, high) (random_start % (high))
static int sounds;
#define game_engine_play_multiplayer_sound(sound) ((void)(sound), sounds++)
#define king_client_score_sounds(team, before, after) ((void)0)

static void find_hill(struct king_hill *hill)
{
	hill->hill_point_count = VALID_INDEX(hill->hill_id, MAXIMUM_HILLS) && hill_has_area[hill->hill_id] ? 3 : 0;
	hill->hill_center.x = (real)hill->hill_id;
}

#include "under_test.inc"

/* a map of hills 0 up to count - 1, all with an area but those listed */
static void map_of(short count, long without_area)
{
	short index;

	memset(hill_has_area, 0, sizeof(hill_has_area));
	king_engine_num_hills = count;
	for (index = 0; index < count; index++)
	{
		king_engine_hills[index] = index;
		hill_has_area[index] = index != without_area;
	}
}

/* a new game's hills, as king_engine_initialize_for_new_map finds them */
static void new_game(byte multi_hill)
{
	variant.game_engine_variant.king.multi_hill = multi_hill;
	memset(king_hills, 0, sizeof(king_hills));
	king_hills[0].hill_id = 0;
	king_hills[0].hill_previous_controller = NONE;
	find_hill(&king_hills[0]);
	king_hill_count = 1;
	king_pick_extra_hills(FALSE);
	king_hills_to_globals();
}

static void check_distinct(void)
{
	short a, b;

	for (a = 0; a < king_hill_count; a++)
	{
		CHECK(king_hills[a].hill_point_count > 0, "hill %d (#%ld) has no area", a, king_hills[a].hill_id);
		for (b = 0; b < a; b++)
			CHECK(king_hills[a].hill_id != king_hills[b].hill_id, "hill #%ld is in play twice", king_hills[a].hill_id);
	}
}

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";
	byte buffer[sizeof(struct king_globals) + sizeof(struct king_extra_hills_network_state) + 16];

	CASE("count-from-option")
	{
		/* (0 and 1 one hill, as January; 2 to 5 that many; more, 5) */
		static byte const options[] = { 0, 1, 2, 3, 4, 5, 6, 200 };
		static short const counts[] = { 1, 1, 2, 3, 4, 5, 5, 5 };
		int index;

		map_of(8, NONE);
		for (index = 0; index < (int)NUMBEROF(options); index++)
		{
			new_game(options[index]);
			CHECK(king_hill_count == counts[index], "multi_hill %d gave %d hills, not %d", options[index],
				king_hill_count, counts[index]);
			CHECK(king_hills[0].hill_id == 0 && king_globals.hill_id == 0, "the first hill is #%ld",
				king_hills[0].hill_id);
			check_distinct();
		}
		return 0;
	}
	CASE("map-has-fewer")
	{
		map_of(3, NONE);
		new_game(5);
		CHECK(king_hill_count == 3, "a map of 3 hills gave %d", king_hill_count);
		check_distinct();
		map_of(1, NONE);
		new_game(4);
		CHECK(king_hill_count == 1, "a map of 1 hill gave %d", king_hill_count);
		return 0;
	}
	CASE("no-area-left-out")
	{
		/* hill #2 has no area (its flags in a line): never in play */
		for (random_start = 0; random_start < 5; random_start++)
		{
			map_of(5, 2);
			new_game(5);
			CHECK(king_hill_count == 4, "4 hills with an area gave %d", king_hill_count);
			check_distinct();
		}
		random_start = 0;
		return 0;
	}
	CASE("moving")
	{
		/* the hills' minute up: where the map has others, not the ones they were */
		short index;

		map_of(6, NONE);
		new_game(3);
		CHECK(king_hill_count == 3, "%d hills", king_hill_count);
		random_start = 1;
		king_pick_extra_hills(TRUE);
		CHECK(king_hill_count == 3, "%d hills after the move", king_hill_count);
		check_distinct();
		for (index = 1; index < king_hill_count; index++)
		{
			CHECK(king_hills[index].hill_id >= 3, "hill %d stayed at #%ld (of 0, 1 and 2 before)", index,
				king_hills[index].hill_id);
		}
		random_start = 0;
		return 0;
	}
	CASE("one-hill-state")
	{
		/* a game of one hill sends January's state alone, as OpenCE's and older builds read it */
		map_of(6, NONE);
		new_game(0);
		CHECK(game_engine_king_write_network_state(buffer, sizeof(buffer)) == (long)sizeof(struct king_globals),
			"a game of one hill sends more than its state");
		return 0;
	}
	CASE("state-round-trip")
	{
		struct king_extra_hills_network_state sent;
		long size;
		long ids[MAXIMUM_HILLS_AT_ONCE];
		short index;

		map_of(6, NONE);
		new_game(4);
		CHECK(king_hill_count == 4, "%d hills", king_hill_count);
		size = game_engine_king_write_network_state(buffer, sizeof(buffer));
		CHECK(size == (long)(sizeof(struct king_globals) + sizeof(struct king_extra_hills_network_state)),
			"Multi-Hill's state is %ld bytes", size);
		/* (the layout OpenCE's host sends: the count after the first, then their ids) */
		memcpy(&sent, buffer + sizeof(struct king_globals), sizeof(sent));
		CHECK(sent.hill_count == 3, "the state names %ld other hills", sent.hill_count);
		for (index = 0; index < king_hill_count; index++)
			ids[index] = king_hills[index].hill_id;
		for (index = 1; index < king_hill_count; index++)
			CHECK(sent.hill_ids[index - 1] == ids[index], "hill %d sent as #%ld", index, sent.hill_ids[index - 1]);
		/* a client's: one hill, then the host's */
		new_game(0);
		CHECK(game_engine_king_read_network_state(buffer, size, TRUE), "the client refused the host's state");
		CHECK(king_hill_count == 4, "the client has %d hills", king_hill_count);
		for (index = 0; index < king_hill_count; index++)
		{
			CHECK(king_hills[index].hill_id == ids[index], "the client's hill %d is #%ld, not #%ld", index,
				king_hills[index].hill_id, ids[index]);
			CHECK(king_hills[index].hill_point_count > 0, "the client found no area of hill %d", index);
		}
		/* and back to one hill with January's state alone */
		CHECK(game_engine_king_read_network_state(buffer, sizeof(struct king_globals), FALSE),
			"the client refused a state of one hill");
		CHECK(king_hill_count == 1, "the client kept %d hills", king_hill_count);
		return 0;
	}
	CASE("state-refused")
	{
		struct king_extra_hills_network_state sent;
		long size;

		map_of(6, NONE);
		new_game(3);
		size = game_engine_king_write_network_state(buffer, sizeof(buffer));
		CHECK(!game_engine_king_read_network_state(buffer, size - 1, TRUE), "a state a byte short was taken");
		CHECK(!game_engine_king_read_network_state(buffer, size + 1, TRUE), "a state a byte long was taken");
		memcpy(&sent, buffer + sizeof(struct king_globals), sizeof(sent));
		sent.hill_count = -1;
		memcpy(buffer + sizeof(struct king_globals), &sent, sizeof(sent));
		CHECK(!game_engine_king_read_network_state(buffer, size, TRUE), "a count of -1 other hills was taken");
		sent.hill_count = MAXIMUM_HILLS_AT_ONCE;
		memcpy(buffer + sizeof(struct king_globals), &sent, sizeof(sent));
		CHECK(!game_engine_king_read_network_state(buffer, size, TRUE), "a count of %d other hills was taken",
			MAXIMUM_HILLS_AT_ONCE);
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
