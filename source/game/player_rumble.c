/*
PLAYER_RUMBLE.C

symbols in this file:
000A8EA0 0020:
	_rumble_initialize (0000)
000A8EC0 0010:
	_rumble_dispose (0000)
000A8ED0 0020:
	_rumble_initialize_for_new_map (0000)
000A8EF0 0020:
	_rumble_player_set_scripted_values (0000)
000A8F10 0020:
	_rumble_player_set_scale (0000)
000A8F30 0170:
	_rumble_player_impulse (0000)
000A90A0 0030:
	_rumble_player_clear (0000)
000A90D0 0040:
	_rumble_clear_all_now (0000)
000A9110 0030:
	_rumble_player_continuous (0000)
000A9140 0060:
	_rumble_dispose_from_old_map (0000)
000A91A0 01a0:
	_rumble_calculate (0000)
000A9340 0130:
	_rumble_update (0000)
0025CE94 0007:
	??_C@_06OLEBFMCD@rumble?$AA@ (0000)
0025CE9C 0012:
	??_C@_0BC@NEAPFALG@rumble_definition?$AA@ (0000)
0025CEB0 0024:
	??_C@_0CE@KKOMCMLO@c?3?2halo?2SOURCE?2game?2player_rumbl@ (0000)
00453404 0004:
	_rumble_globals (0000)
*/

/* ---------- headers */

#include "game/player_rumble.h"

#include "game/game.h"
#include "game/players.h"
#include "input/input.h"
#include "interface/player_ui.h"
#include "math/periodic_functions.h"
#include "memory/data.h"
#include "saved games/game_state.h"

/* ---------- constants */

enum
{
	MAXIMUM_RUMBLE_IMPULSES = 8,
	MAXIMUM_RUMBLE_MOTOR_VALUE = 65535
};

/* ---------- macros */

/* ---------- structures */

struct rumble_player
{
	struct rumble_definition impulses[MAXIMUM_RUMBLE_IMPULSES];
	real impulse_time[MAXIMUM_RUMBLE_IMPULSES];
	real continuous_left;
	real continuous_right;
};

struct rumble_globals
{
	struct rumble_player players[MAXIMUM_LOCAL_PLAYERS];
	real scripted_left_motor;
	real scripted_right_motor;
	real scripted_scale;
};

struct rumble_motor_values
{
	word left;
	word right;
};

/* ---------- prototypes */

static struct rumble_motor_values rumble_calculate(
	struct rumble_player *player);
static void rumble_port_write_motors(
	void);

/* ---------- globals */

static struct rumble_globals *rumble_globals;
/* port: the tick whose rumble_update has run, NONE for none
(rumble_player_impulse) */
static long rumble_port_updated_tick = NONE;

/* ---------- public code */

void rumble_initialize(
	void)
{
	rumble_globals = (struct rumble_globals *)game_state_malloc(
		"rumble",
		NULL,
		sizeof(*rumble_globals));

	return;
}

void rumble_dispose(
	void)
{
	return;
}

void rumble_initialize_for_new_map(
	void)
{
	csmemset(rumble_globals, 0, sizeof(*rumble_globals));
	rumble_port_updated_tick = NONE;

	return;
}

void rumble_player_set_scripted_values(
	real left_motor,
	real right_motor)
{
	rumble_globals->scripted_left_motor = left_motor;
	rumble_globals->scripted_right_motor = right_motor;

	return;
}

void rumble_player_set_scale(
	real scale)
{
	rumble_globals->scripted_scale = scale;

	return;
}

void rumble_player_impulse(
	short local_player_index,
	struct rumble_definition *rumble_definition,
	real scale,
	real duration_scale)
{
	struct rumble_player *player = &rumble_globals->players[local_player_index];
	struct rumble_definition *impulse = player->impulses;
	real longest = player->impulse_time[0];
	real motor_scale;
	long impulse_index;

	match_assert(
		"c:\\halo\\SOURCE\\game\\player_rumble.c",
		0xa4,
		rumble_definition);

	if (longest < player->impulse_time[1])
	{
		impulse = &player->impulses[1];
		longest = player->impulse_time[1];
	}
	if (longest < player->impulse_time[2])
	{
		impulse = &player->impulses[2];
		longest = player->impulse_time[2];
	}
	if (longest < player->impulse_time[3])
	{
		impulse = &player->impulses[3];
		longest = player->impulse_time[3];
	}
	if (longest < player->impulse_time[4])
	{
		impulse = &player->impulses[4];
		longest = player->impulse_time[4];
	}
	if (longest < player->impulse_time[5])
	{
		impulse = &player->impulses[5];
		longest = player->impulse_time[5];
	}
	if (longest < player->impulse_time[6])
	{
		impulse = &player->impulses[6];
		longest = player->impulse_time[6];
	}
	if (longest < player->impulse_time[7])
		impulse = &player->impulses[7];

	*impulse = *rumble_definition;

	motor_scale = (1.0f - rumble_definition->scale_floor) * scale +
		rumble_definition->scale_floor;

	impulse->motors[0].scale *= motor_scale;
	impulse->motors[1].scale *= motor_scale;
	impulse->motors[0].duration *= duration_scale;
	impulse->motors[1].duration *= duration_scale;
	/* port: debug.latency_log, the impulse's step to its motors' write */
	LATENCY_LOG(latency_log_rumble_caused(local_player_index, MAX(
		impulse->motors[0].scale > 0.0f ? impulse->motors[0].duration : 0.0f,
		impulse->motors[1].scale > 0.0f ? impulse->motors[1].duration : 0.0f)));

	impulse_index = impulse - player->impulses;
	/* port: the motors are written at the end of the step that made the
	impulse (rumble_port_step_end), at its start; rumble_update ages it a
	tick before it writes them, so one made after this tick's update is
	a tick on at the next, and one made before it (the effects' update)
	starts at the update, as they did: the impulse lasts its duration from
	the step, not from the next tick's update a tick later */
	player->impulse_time[impulse_index] = rumble_port_updated_tick == game_time_get() ? 0.0f : -1.0f / 30.0f;

	return;
}

void rumble_player_clear(
	short local_player_index)
{
	csmemset(
		&rumble_globals->players[local_player_index],
		0,
		sizeof(struct rumble_player));

	return;
}

void rumble_clear_all_now(
	void)
{
	long gamepad_index;

	csmemset(rumble_globals, 0, sizeof(*rumble_globals));

	for (gamepad_index = 0;
		gamepad_index < MAXIMUM_LOCAL_PLAYERS;
		gamepad_index++)
	{
		if (input_has_gamepad((short)gamepad_index))
		{
			input_set_gamepad_rumbler_state(
				(short)gamepad_index,
				0,
				0);
		}
	}

	return;
}

void rumble_player_continuous(
	short local_player_index,
	real left_motor,
	real right_motor)
{
	struct rumble_player *player = &rumble_globals->players[local_player_index];
	/* port: a change written in the frame that makes it (the camera's
	effects, from the looping sounds' continuous effects: a vehicle's
	engine), not at the next tick's update */
	boolean changed = player->continuous_left != left_motor || player->continuous_right != right_motor;

	player->continuous_left = left_motor;
	player->continuous_right = right_motor;
	if (changed)
		rumble_port_write_motors();

	return;
}

void rumble_dispose_from_old_map(
	void)
{
	short gamepad_index;
	long index;

	for (gamepad_index = 0;
		gamepad_index < MAXIMUM_LOCAL_PLAYERS;
		gamepad_index++)
	{
		input_set_gamepad_rumbler_state(gamepad_index, 0, 0);
	}

	csmemset(rumble_globals, 0, sizeof(*rumble_globals));

	for (index = 0; index < MAXIMUM_LOCAL_PLAYERS; index++)
	{
		if (input_has_gamepad((short)index))
		{
			input_set_gamepad_rumbler_state((short)index, 0, 0);
		}
	}

	return;
}

void rumble_update(
	void)
{
	short local_player_index;
	long impulse_index;

	/* port: the impulses aged a tick, then the motors written
	(rumble_port_write_motors), where the motors were written at each
	impulse's age before it was aged (rumble_player_impulse) */
	for (local_player_index = 0;
		local_player_index < MAXIMUM_LOCAL_PLAYERS;
		local_player_index++)
	{
		struct rumble_player *player =
			&rumble_globals->players[local_player_index];

		for (impulse_index = 0;
			impulse_index < MAXIMUM_RUMBLE_IMPULSES;
			impulse_index++)
		{
			player->impulse_time[impulse_index] += 1.0f / 30.0f;
		}
	}
	rumble_port_updated_tick = game_time_get();
	rumble_port_write_motors();

	return;
}

/* port: the end of a simulation step (game_tick_substep: a tick at
game.sim_rate 30, a sub-step at 60 and 120): the motors written for the
impulses, scripted rumble and clears it made, in the step itself, not at the
next tick's update (rumble_update) */
void rumble_port_step_end(
	void)
{
	rumble_port_write_motors();

	return;
}

/* ---------- private code */

/* port: each local player's motors at their impulses' ages, written to
their controller (rumble_update's, apart from its aging the impulses) */
static void rumble_port_write_motors(
	void)
{
	short local_player_index;

	for (local_player_index = 0;
		local_player_index < MAXIMUM_LOCAL_PLAYERS;
		local_player_index++)
	{
		struct rumble_player *player =
			&rumble_globals->players[local_player_index];
		struct rumble_motor_values motors = rumble_calculate(player);
		long player_index;

		player_index = local_player_get_player_index(local_player_index);
		if (player_index != NONE)
		{
			long controller_index = player_get(player_index)->local_player_index;

			if (controller_index != NONE)
			{
				if (!player_ui_rumble_disabled(
					player_ui_get_single_player_local_player_from_controller(
						(short)controller_index)))
				{
					input_set_gamepad_rumbler_state(
						(short)controller_index,
						motors.left,
						motors.right);
				}
				else
				{
					input_set_gamepad_rumbler_state(
						(short)controller_index,
						0,
						0);
				}
			}
		}
		else
		{
			input_set_gamepad_rumbler_state(local_player_index, 0, 0);
		}
	}

	return;
}

static struct rumble_motor_values rumble_calculate(
	struct rumble_player *player)
{
	struct rumble_motor_values values;
	real motors[NUMBER_OF_RUMBLE_MOTORS];
	real value;
	long impulse_index;
	long motor_index;

	motors[0] = player->continuous_left;
	motors[1] = player->continuous_right;

	for (impulse_index = 0;
		impulse_index < MAXIMUM_RUMBLE_IMPULSES;
		impulse_index++)
	{
		real time = player->impulse_time[impulse_index];

		for (motor_index = 0;
			motor_index < NUMBER_OF_RUMBLE_MOTORS;
			motor_index++)
		{
			struct rumble_motor *motor =
				&player->impulses[impulse_index].motors[motor_index];

			if (motor->duration > time)
			{
				value = PIN(1.0f - time / motor->duration, 0.0f, 1.0f);
				motors[motor_index] +=
					transition_function_evaluate(
						motor->transition_function,
						value) * motor->scale;
			}
		}
	}

	if (rumble_globals->scripted_scale != 0.0f)
	{
		motors[0] += rumble_globals->scripted_left_motor *
			rumble_globals->scripted_scale;
		motors[1] += rumble_globals->scripted_right_motor *
			rumble_globals->scripted_scale;
	}

	value = PIN(
		motors[0] * (real)MAXIMUM_RUMBLE_MOTOR_VALUE,
		0.0f,
		(real)MAXIMUM_RUMBLE_MOTOR_VALUE);
	values.left = (word)fast_ftol(value);

	value = PIN(
		motors[1] * (real)MAXIMUM_RUMBLE_MOTOR_VALUE,
		0.0f,
		(real)MAXIMUM_RUMBLE_MOTOR_VALUE);
	values.right = (word)fast_ftol(value);

	return values;
}
