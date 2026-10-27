#include "harness.h"

/* The controller's rumble (source/game/player_rumble.c) starts at the end of
the simulation step that made it (a shot's impulse, from its weapon's firing
effect, in the objects' update), not at the next tick's rumble_update, and
lasts the impulse's duration from then: an impulse made after the tick's
update is a tick on at the next update, one made before it (the effects'
update) starts at that update. A vehicle's continuous rumble, which the
camera's effects set in a frame, is written in that frame. The world: one
local player on controller 1, ticks of one step (game.sim_rate 30), each
tick's update at its start (game_tick_macro_step) and each step's end
(game_tick_substep) */

typedef unsigned short word;

#define MAXIMUM_LOCAL_PLAYERS 4
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define PIN(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#define csmemset memset
#define fast_ftol(x) ((long)(x))
#define LATENCY_LOG(call) ((void)0)

#include "config.inc"

/* the fake world: the game's time, controller 1's motors as written */
static long game_time;
static word motors[2];
static int writes;

static long game_time_get(void) { return game_time; }

static void *game_state_malloc(char const *name, void *unused, long size)
{
	(void)name;
	(void)unused;
	return calloc(1, (size_t)size);
}

static boolean input_has_gamepad(short gamepad_index) { return gamepad_index == 0; }

static void input_set_gamepad_rumbler_state(short gamepad_index, word left, word right)
{
	if (gamepad_index != 0)
		return;
	motors[0] = left;
	motors[1] = right;
	writes++;
}

struct player_datum
{
	short local_player_index;
};

static struct player_datum player = { 0 };

static long local_player_get_player_index(short local_player_index) { return local_player_index == 0 ? 0 : NONE; }
static struct player_datum *player_get(long player_index) { (void)player_index; return &player; }
static boolean player_ui_rumble_disabled(short local_player_index) { (void)local_player_index; return FALSE; }
static short player_ui_get_single_player_local_player_from_controller(short controller_index)
{
	return controller_index;
}
/* (a straight fall from full to nothing over the duration) */
static real transition_function_evaluate(short function, real value) { (void)function; return value; }

#include "under_test.inc"

/* a shot's: the left motor at full for 12 ticks (0.4 seconds), falling */
static struct rumble_definition shot;

enum
{
	_impulse_none,
	_impulse_before_update,
	_impulse_in_step
};

/* a tick of one step: its update, its step (an impulse made in it), the
step's end; whether the motors run at its end */
static boolean tick(int impulse)
{
	if (impulse == _impulse_before_update)
		rumble_player_impulse(0, &shot, 1.0f, 1.0f);
	rumble_update();
	if (impulse == _impulse_in_step)
		rumble_player_impulse(0, &shot, 1.0f, 1.0f);
	rumble_port_step_end();
	game_time++;
	return motors[0] != 0;
}

static void begin(void)
{
	long index;

	memset(&shot, 0, sizeof(shot));
	shot.motors[0].scale = 1.0f;
	shot.motors[0].duration = 12.0f / 30.0f;
	shot.scale_floor = 1.0f;
	rumble_initialize();
	rumble_initialize_for_new_map();
	game_time = 0;
	motors[0] = motors[1] = 0;
	writes = 0;
	for (index = 0; index < 5; index++)
		tick(_impulse_none);
}

/* the ticks, from the one the impulse is made in, at whose ends the motors
run, and the motors' first value */
static int ticks_running(int impulse, word *first)
{
	int count = 0;
	int index;

	*first = 0;
	if (tick(impulse))
	{
		*first = motors[0];
		count++;
	}
	for (index = 0; index < 40; index++)
	{
		if (!tick(_impulse_none))
			break;
		if (!*first)
			*first = motors[0];
		count++;
	}
	return count;
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	word first;
	int count;

	CASE("starts_in_its_step")
	{
		begin();
		tick(_impulse_in_step);
		CHECK(motors[0] == 65535, "the step that made the impulse ended with the motor at %u, not full", motors[0]);
		return 0;
	}
	CASE("lasts_its_duration")
	{
		begin();
		count = ticks_running(_impulse_in_step, &first);
		CHECK(count == 12, "an impulse of 12 ticks made in a step ran for %d", count);
		CHECK(first == 65535, "it started at %u, not full", first);
		CHECK(motors[0] == 0 && motors[1] == 0, "the motors were left at %u, %u", motors[0], motors[1]);
		return 0;
	}
	CASE("before_the_update")
	{
		begin();
		count = ticks_running(_impulse_before_update, &first);
		CHECK(count == 12, "an impulse of 12 ticks made before the tick's update ran for %d", count);
		CHECK(first == 65535, "it started at %u, not full", first);
		return 0;
	}
	CASE("vehicle_in_its_frame")
	{
		int before;

		begin();
		before = writes;
		rumble_player_continuous(0, 0.5f, 0.25f);
		CHECK(writes > before && motors[0] == 32767 && motors[1] == 16383,
			"a vehicle's rumble set in a frame was not written then (%u, %u)", motors[0], motors[1]);
		rumble_player_continuous(0, 0.0f, 0.0f);
		CHECK(motors[0] == 0 && motors[1] == 0, "a vehicle's rumble that ended left the motors at %u, %u",
			motors[0], motors[1]);
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
