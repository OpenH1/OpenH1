/* The host's sub-step controls of a client's player whose input and
prediction it takes as its tick begins (host_live_apply), in a tick that runs
the client's tick after the input it took (the prediction came before that
tick's input): what came of the tick it runs, or the latest of the input it
took, not that input's tick-start actions after the tick before ran its later
sub-steps' controls. */
#include "harness.h"
enum { MAXIMUM_TRACKED_PLAYERS = 4, PLAYER = 1, COUNT = 4 };
typedef unsigned short word;
typedef struct { real i, j; } real_vector2d;
typedef struct { real yaw, pitch; } real_euler_angles2d;
enum { _game_connection_network_client = 1, _game_connection_network_server = 2 };
#include "config.inc"
struct player_datum { long unit_index; short local_player_index; };
struct unit_datum
{
	struct { long parent_object_index; } object;
	struct { real_vector3d desired_aiming_vector; } unit;
};
static struct player_datum player_data = { 7, NONE };
static struct unit_datum unit_data = { { NONE } };
static long input_key;
static struct live_control const *given_live;
static struct
{
	struct live_control live[MAXIMUM_TRACKED_PLAYERS];
	struct substep_record replaying[MAXIMUM_TRACKED_PLAYERS];
	struct substep_record ran[MAXIMUM_TRACKED_PLAYERS];
	long tick_key[MAXIMUM_TRACKED_PLAYERS];
	struct host_live_tick host_live[MAXIMUM_TRACKED_PLAYERS][HOST_LIVE_TICKS];
	boolean host_fresh[MAXIMUM_TRACKED_PLAYERS];
} network_substeps_globals;
static int game_connection(void) { return _game_connection_network_server; }
static boolean distributed_prediction_at_tick_start(short player_index) { return player_index == PLAYER; }
static long update_queues_distributed_input_key(long player_index) { (void)player_index; return input_key; }
static short network_substeps_count(void) { return COUNT; }
static struct player_datum *distributed_player(short player_index)
{ return player_index == PLAYER ? &player_data : NULL; }
static long distributed_living_unit(struct player_datum const *player) { return player->unit_index; }
static struct unit_datum *unit_get(long unit_index) { (void)unit_index; return &unit_data; }
static void substep_control_from_unit(struct unit_datum const *unit, struct substep_control *control)
{ (void)unit; memset(control, 0, sizeof(*control)); }
static void euler_angles2d_from_vector3d(real_euler_angles2d *facing, real_vector3d const *aim)
{ facing->yaw = aim->i; facing->pitch = aim->j; }
static void live_control_give(short player_index, struct player_datum const *player, struct live_control const *live,
	short substep_index)
{ (void)player_index; (void)player; (void)substep_index; given_live = live; }
#include "under_test.inc"

/* the client's controls of a tick's sub-step, as they came */
static void came(long key, short substep)
{
	struct host_live_tick *tick = &network_substeps_globals.host_live[PLAYER][key & (HOST_LIVE_TICKS - 1)];

	if (tick->key != key) { tick->key = key; tick->given = 0; }
	memset(&tick->controls[substep], 0, sizeof(tick->controls[substep]));
	tick->controls[substep].valid = TRUE;
	tick->controls[substep].key = key;
	tick->controls[substep].substep = substep;
	tick->controls[substep].count = COUNT;
	tick->given |= (word)FLAG(substep);
}

/* the controls given at a sub-step (NONE: as the tick begins): of key's
live_substep, or (key NONE) none */
static void expect(char const *name, short substep, long key, short live_substep)
{
	boolean given;

	given_live = NULL;
	given = host_live_apply(PLAYER, substep);
	if (key == NONE)
	{
		CHECK(!given && !given_live, "%s: sub-step %d gave %ld.%d", name, substep,
			given_live ? given_live->key : (long)NONE, given_live ? given_live->substep : NONE);
		return;
	}
	CHECK(given && given_live, "%s: sub-step %d gave nothing, not %ld.%d", name, substep, key, live_substep);
	CHECK(given_live->key == key && given_live->substep == live_substep, "%s: sub-step %d gave %ld.%d, not %ld.%d",
		name, substep, given_live->key, given_live->substep, key, live_substep);
}

int main(int argc, char **argv) {
	char const *name = argc > 1 ? argv[1] : "";
	short index;

	for (index = 0; index < HOST_LIVE_TICKS; index++)
		network_substeps_globals.host_live[PLAYER][index].key = NONE;
	/* (the tick runs the client's tick 31, its prediction of 30's end come;
	the input taken, 30's, whose sub-steps the tick before ran) */
	network_substeps_globals.tick_key[PLAYER] = 31;
	network_substeps_globals.host_fresh[PLAYER] = TRUE;
	input_key = 30;
	came(30, 0); came(30, 1); came(30, 2); came(30, 3);
	if (!strcmp(name, "late-input"))
	{
		/* (none of 31 yet: 30's last, held, not 30's first) */
		expect(name, NONE, 30, 3);
		expect(name, 1, 30, 3);
		came(31, 1);
		expect(name, 2, 31, 1);
	}
	else if (!strcmp(name, "later-came"))
	{
		/* (a later sub-step of 31 come before its first: 30's last until it) */
		came(31, 2);
		expect(name, NONE, 30, 3);
		expect(name, 1, 30, 3);
		expect(name, 2, 31, 2);
	}
	else if (!strcmp(name, "lost-substeps"))
	{
		/* (30's later sub-steps lost: the latest of it that came) */
		network_substeps_globals.host_live[PLAYER][30 & (HOST_LIVE_TICKS - 1)].given = FLAG(0) | FLAG(1);
		expect(name, NONE, 30, 1);
	}
	else if (!strcmp(name, "fresh"))
	{
		came(31, 0);
		expect(name, NONE, 31, 0);
	}
	else if (!strcmp(name, "taken"))
	{
		/* (31's input taken: its own actions given, nothing earlier) */
		input_key = 31;
		expect(name, NONE, NONE, 0);
		expect(name, 1, NONE, 0);
	}
	else if (!strcmp(name, "older"))
	{
		/* (only controls older than the input taken: its actions stay) */
		for (index = 0; index < HOST_LIVE_TICKS; index++)
			network_substeps_globals.host_live[PLAYER][index].key = NONE;
		came(29, 3);
		expect(name, NONE, NONE, 0);
	}
	else if (!strcmp(name, "no-input"))
	{
		input_key = NONE;
		expect(name, NONE, NONE, 0);
	}
	else if (!strcmp(name, "record-came"))
	{
		network_substeps_globals.replaying[PLAYER].valid = TRUE;
		expect(name, 1, NONE, 0);
	}
	else if (!strcmp(name, "other-count"))
	{
		network_substeps_globals.host_live[PLAYER][30 & (HOST_LIVE_TICKS - 1)].controls[3].count = 2;
		expect(name, NONE, NONE, 0);
	}
	else { fprintf(stderr, "no case %s\n", name); return 2; }
	return 0;
}
