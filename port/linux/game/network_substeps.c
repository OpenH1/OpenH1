/*
NETWORK_SUBSTEPS.C

An OpenH1 game's sub-step inputs (NETCODE.md, "Sub-step inputs"). At 60
and 120 Hz a tick runs 2 or 4 sub-steps (sim_rate.h), and a local player's
unit takes a new reading of the controller in each sub-step after the
tick's first (player_control.c): its held controls, movement, trigger and
facing. Every machine replays the other players from the input it is given,
so each machine needs each player's sub-steps too, not only their ticks:

- Recorded: what a local player's unit was given in each sub-step past the
  first (network_substeps_local_given), as the unit has it after the
  sub-step's reading: its held controls, throttle, trigger, and its facing
  if its aim turned.
- Up: a client sends its own players' sub-steps of the tick just run with
  its input of that tick (_distributed_message_player_substeps, unreliable,
  in the same datagram), keyed by the client's tick, at full precision.
- Replayed: in the next tick that runs the input of a tick, each sub-step
  past the first gives the player's unit that sub-step's controls, before
  the units and bipeds move (network_substeps_replay). A record is taken
  only if it is of the tick whose input the tick took (keys latched as the
  tick begins: player_queues_new.c's update_queues_distributed_input_key),
  and only for a unit no local player controls. Without one (lost, or a
  player whose sub-steps changed nothing), the tick's input holds through
  its sub-steps, as at 30.
- Relayed: the host sends each client, with the players' input it relays
  (_distributed_message_relayed_actions), the sub-steps its tick ran for
  them (_distributed_message_relayed_substeps), keyed by the host's update,
  as compact as the relayed input (angles in 16 bits, the sticks of 127, the
  trigger of 255), and none for a player whose sub-steps changed nothing.

Nothing here runs in a game of one sub-step a tick (a Legacy game, an OpenH1
game at 30), nor sends anything: those games' traffic is as it was.
Anything another machine sends is checked: its player (the sender's own, to
the host), its count of sub-steps (the game's), and every number finite and
within a controller's reach.
*/

#include "cseries.h"
#include "game/game.h"
#include "game/players.h"
#include "game/player_queues_new.h"
#include "units/units.h"
#include "networking/network_game_globals.h"
#include "network_distributed.h"
#include "network_substeps.h"
#include "sim_rate.h"

#include <math.h>
#include <string.h>

/* source/game/player_queues_new.c */
long update_queues_distributed_input_key(long absolute_player_index);

/* ---------- constants */

enum
{
	MAXIMUM_LATER_SUBSTEPS = MAXIMUM_SIM_SUBSTEPS - 1,
	/* a relayed record's bytes on the wire: its player and count, then each
	later sub-step's held controls (2), whether it turned (1), sticks (2),
	trigger (1) and facing (4) */
	RELAYED_SUBSTEP_SIZE = 10,
	RELAYED_RECORD_MAXIMUM_SIZE = 2 + RELAYED_SUBSTEP_SIZE * MAXIMUM_LATER_SUBSTEPS,
	/* a relayed message's entries, at most, in bytes (well within a datagram) */
	RELAYED_MESSAGE_ENTRIES_SIZE = 640,
};

/* the unit controls a sub-step's reading changes (player_control.c's
PLAYER_CONTROL_SUBSTEP_HELD_FLAGS) */
#define SUBSTEP_HELD_FLAGS ( \
	FLAG(_unit_control_crouch_modifier_bit) | \
	FLAG(_unit_control_jump_bit) | \
	FLAG(_unit_control_weapon_primary_trigger_bit) | \
	FLAG(_unit_control_weapon_secondary_trigger_bit) | \
	FLAG(_unit_control_throw_grenade_bit))

/* ---------- structures */

/* what a unit was given in one sub-step past its tick's first */
struct substep_control
{
	unsigned long held_flags;
	real_vector2d throttle;
	real primary_trigger;
	real_euler_angles2d facing;
	boolean turned;
};

/* a player's sub-steps of one tick: of the tick (or host update) key, for
count sub-steps, those past the first */
struct substep_record
{
	boolean valid;
	long key;
	short count;
	long unit_index;
	/* (being recorded: the sub-steps given, a bit each) */
	word given;
	struct substep_control controls[MAXIMUM_LATER_SUBSTEPS];
};

/* on the wire, to the host: a client player's sub-steps of its tick */
struct distributed_substep_input_control
{
	word held_flags;
	byte turned;
	byte pad;
	real throttle_i;
	real throttle_j;
	real primary_trigger;
	real yaw;
	real pitch;
};

struct distributed_substep_input
{
	byte player_index;
	byte substep_count;
	word pad;
	long tick;
	struct distributed_substep_input_control controls[MAXIMUM_LATER_SUBSTEPS];
};

typedef char forward_maximum_size_assert[
	1 + RELAYED_SUBSTEP_SIZE * MAXIMUM_LATER_SUBSTEPS == NETWORK_SUBSTEPS_FORWARD_MAXIMUM_SIZE ? 1 : -1];
typedef char distributed_substep_input_size_assert[
	sizeof(struct distributed_substep_input) == 8 + 24 * MAXIMUM_LATER_SUBSTEPS ? 1 : -1];

/* ---------- globals */

static struct
{
	/* the latest heard of each player (by absolute index): the host's of a
	client's players, keyed by the client's tick; a client's of the others,
	by the host's update */
	struct substep_record heard[MAXIMUM_TRACKED_PLAYERS];
	/* ... and a client's of the other clients' players, as the host sent
	them on with their input as they came (network_distributed.c's forwarded
	input), keyed by their client's tick */
	struct substep_record forwarded[MAXIMUM_TRACKED_PLAYERS];
	/* this tick's: the record replayed, and what the tick ran (a local
	player's as given, a remote one's as replayed) */
	struct substep_record replaying[MAXIMUM_TRACKED_PLAYERS];
	struct substep_record ran[MAXIMUM_TRACKED_PLAYERS];
	/* what each player's unit had as the tick's sub-steps began: a tick
	whose sub-steps changed none of it has nothing to send */
	struct substep_control start[MAXIMUM_TRACKED_PLAYERS];
	boolean start_valid[MAXIMUM_TRACKED_PLAYERS];
	/* the controls of a replayed unit that went down in the running sub-step */
	long pressed_unit[MAXIMUM_TRACKED_PLAYERS];
	unsigned long pressed_flags[MAXIMUM_TRACKED_PLAYERS];
	long bytes_sent;
} network_substeps_globals;

/* ---------- private code */

/* the unit's sub-step controls as it has them now */
static void substep_control_from_unit(
	struct unit_datum const *unit,
	struct substep_control *control)
{
	control->held_flags = unit->unit.control_flags & SUBSTEP_HELD_FLAGS;
	control->throttle.i = unit->unit.throttle.i;
	control->throttle.j = unit->unit.throttle.j;
	control->primary_trigger = unit->unit.primary_trigger;
}

/* whether a tick's record changed nothing of what its unit began with */
static boolean substep_record_unchanged(
	struct substep_record const *record,
	struct substep_control const *start)
{
	short index;

	for (index = 0; index < record->count - 1; index++)
	{
		struct substep_control const *control = &record->controls[index];

		if (control->turned || control->held_flags != start->held_flags ||
			control->throttle.i != start->throttle.i || control->throttle.j != start->throttle.j ||
			control->primary_trigger != start->primary_trigger)
		{
			return FALSE;
		}
	}
	return TRUE;
}

/* whether a record being made has every sub-step past the first */
static boolean substep_record_complete(
	struct substep_record const *record)
{
	return record->count > 1 && record->count <= MAXIMUM_SIM_SUBSTEPS &&
		record->given == (word)(((1 << record->count) - 1) & ~1);
}

/* the game's sub-steps a tick, as the agreed rate has them */
static short network_substeps_count(
	void)
{
	return sim_substep_count();
}

/* a heard record kept (of heard, or of forwarded), if newer than the one kept */
static void substep_record_heard(
	struct substep_record *records,
	short player_index,
	struct substep_record const *record)
{
	struct substep_record *kept = &records[player_index];

	if (kept->valid && record->key <= kept->key &&
		/* (a key far behind: the sender's game began again) */
		kept->key - record->key < TICKS_PER_SECOND * 10)
	{
		return;
	}
	*kept = *record;
	kept->valid = TRUE;
}

static real substep_pin(
	real value,
	real minimum,
	real maximum)
{
	if (!valid_real(value))
		return 0.0f;
	return PIN(value, minimum, maximum);
}

/* (the host) a client's players' sub-steps */
static void network_substeps_handle_inputs(
	long machine_index,
	struct distributed_substep_input const *inputs,
	short count)
{
	short index;

	for (index = 0; index < count; index++)
	{
		struct distributed_substep_input input;
		struct substep_record record;
		short substep;

		csmemcpy(&input, &inputs[index], sizeof(input));
		if (input.player_index >= MAXIMUM_TRACKED_PLAYERS ||
			!distributed_machine_has_player(machine_index, input.player_index) ||
			input.substep_count != network_substeps_count() || input.substep_count < 2)
		{
			continue;
		}
		csmemset(&record, 0, sizeof(record));
		record.key = input.tick;
		record.count = input.substep_count;
		record.unit_index = NONE;
		for (substep = 0; substep < record.count - 1; substep++)
		{
			struct distributed_substep_input_control const *wire = &input.controls[substep];
			struct substep_control *control = &record.controls[substep];

			control->held_flags = wire->held_flags & SUBSTEP_HELD_FLAGS;
			control->turned = wire->turned != 0;
			control->throttle.i = substep_pin(wire->throttle_i, -1.0f, 1.0f);
			control->throttle.j = substep_pin(wire->throttle_j, -1.0f, 1.0f);
			control->primary_trigger = substep_pin(wire->primary_trigger, 0.0f, 1.0f);
			control->facing.yaw = substep_pin(wire->yaw, -4.0f * _pi, 4.0f * _pi);
			control->facing.pitch = substep_pin(wire->pitch, -_pi, _pi);
		}
		substep_record_heard(network_substeps_globals.heard, input.player_index, &record);
	}
}

/* a record's later sub-steps' bytes, as compact as the relayed input
(RELAYED_SUBSTEP_SIZE each) */
static byte *substep_controls_write(
	struct substep_record const *record,
	byte *cursor)
{
	short index;

	for (index = 0; index < record->count - 1; index++)
	{
		struct substep_control const *control = &record->controls[index];
		word held = (word)control->held_flags;
		real throttle_i = PIN(control->throttle.i, -1.0f, 1.0f);
		real throttle_j = PIN(control->throttle.j, -1.0f, 1.0f);
		real trigger = PIN(control->primary_trigger, 0.0f, 1.0f);
		short yaw = distributed_angle_pack(control->facing.yaw);
		short pitch = distributed_angle_pack(control->facing.pitch);

		csmemcpy(cursor, &held, sizeof(held));
		cursor += sizeof(held);
		*cursor++ = (byte)control->turned;
		*cursor++ = (byte)(signed char)(long)floor(throttle_i * 127.0f + 0.5f);
		*cursor++ = (byte)(signed char)(long)floor(throttle_j * 127.0f + 0.5f);
		*cursor++ = (byte)(long)floor(trigger * 255.0f + 0.5f);
		csmemcpy(cursor, &yaw, sizeof(yaw));
		cursor += sizeof(yaw);
		csmemcpy(cursor, &pitch, sizeof(pitch));
		cursor += sizeof(pitch);
	}
	return cursor;
}

/* ... and read (record->count set), no further than end: the bytes past
them, NULL for too few */
static byte const *substep_controls_read(
	struct substep_record *record,
	byte const *data,
	byte const *end)
{
	short substep;

	if (record->count < 2 || record->count > MAXIMUM_SIM_SUBSTEPS ||
		end - data < (long)(RELAYED_SUBSTEP_SIZE * (record->count - 1)))
	{
		return NULL;
	}
	for (substep = 0; substep < record->count - 1; substep++)
	{
		struct substep_control *control = &record->controls[substep];
		word held;
		short yaw, pitch;

		csmemcpy(&held, data, sizeof(held));
		control->held_flags = held & SUBSTEP_HELD_FLAGS;
		control->turned = data[2] != 0;
		control->throttle.i = PIN((real)(signed char)data[3] / 127.0f, -1.0f, 1.0f);
		control->throttle.j = PIN((real)(signed char)data[4] / 127.0f, -1.0f, 1.0f);
		control->primary_trigger = (real)data[5] / 255.0f;
		csmemcpy(&yaw, data + 6, sizeof(yaw));
		csmemcpy(&pitch, data + 8, sizeof(pitch));
		control->facing.yaw = distributed_angle_unpack(yaw, FALSE);
		control->facing.pitch = distributed_angle_unpack(pitch, TRUE);
		data += RELAYED_SUBSTEP_SIZE;
	}
	return data;
}

/* a relayed record's bytes (at most RELAYED_RECORD_MAXIMUM_SIZE) */
static word relayed_record_write(
	short player_index,
	struct substep_record const *record,
	byte *buffer)
{
	byte *cursor = buffer;

	*cursor++ = (byte)player_index;
	*cursor++ = (byte)record->count;
	cursor = substep_controls_write(record, cursor);
	return (word)(cursor - buffer);
}

/* (a client) the host's relayed records, of its update */
static void network_substeps_handle_relayed(
	byte const *data,
	byte const *end,
	short count)
{
	long update_number;
	short index;

	if (end - data < (long)sizeof(update_number))
		return;
	csmemcpy(&update_number, data, sizeof(update_number));
	data += sizeof(update_number);
	for (index = 0; index < count; index++)
	{
		struct substep_record record;
		short player_index;

		if (end - data < 2)
			return;
		player_index = data[0];
		csmemset(&record, 0, sizeof(record));
		record.key = update_number;
		record.count = data[1];
		record.unit_index = NONE;
		data = substep_controls_read(&record, data + 2, end);
		if (!data)
			return;
		/* (a count other than the game's, or a player not tracked: passed
		over, its bytes read) */
		if (player_index < MAXIMUM_TRACKED_PLAYERS && record.count == network_substeps_count())
			substep_record_heard(network_substeps_globals.heard, player_index, &record);
	}
}

/* ---------- public code */

boolean network_substeps_active(
	void)
{
	short connection = game_connection();

	return (connection == _game_connection_network_client || connection == _game_connection_network_server) &&
		!sim_rate_follows_setting() && sim_substep_count() > 1;
}

void network_substeps_new_game(
	void)
{
	short player_index;

	csmemset(&network_substeps_globals, 0, sizeof(network_substeps_globals));
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
		network_substeps_globals.pressed_unit[player_index] = NONE;
}

void network_substeps_tick_begun(
	short substep_count)
{
	boolean active = network_substeps_active();
	short player_index;

	/* (the inputs this tick took, as the tests' reports have them: a tick's
	sub-steps may be spread over frames, between which newer inputs come) */
	network_distributed_tick_begun();
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		struct substep_record *replaying = &network_substeps_globals.replaying[player_index];
		struct substep_record *ran = &network_substeps_globals.ran[player_index];
		struct substep_record const *heard = update_client_input_forwarded(player_index) ?
			&network_substeps_globals.forwarded[player_index] : &network_substeps_globals.heard[player_index];
		struct player_datum *player = active ? distributed_player(player_index) : NULL;
		long unit_index = player ? player->unit_index : NONE;

		replaying->valid = FALSE;
		ran->valid = FALSE;
		ran->given = 0;
		ran->count = substep_count;
		ran->unit_index = unit_index;
		network_substeps_globals.start_valid[player_index] = FALSE;
		network_substeps_globals.pressed_unit[player_index] = NONE;
		if (unit_index == NONE)
			continue;
		substep_control_from_unit(unit_get(unit_index), &network_substeps_globals.start[player_index]);
		network_substeps_globals.start_valid[player_index] = TRUE;
		/* (a remote player's: the record of the input this tick took) */
		if (player->local_player_index == NONE && heard->valid && heard->count == substep_count &&
			heard->key == update_queues_distributed_input_key(player_index))
		{
			*replaying = *heard;
			replaying->unit_index = unit_index;
		}
	}
}

void network_substeps_replay(
	short substep_index,
	short substep_count)
{
	short player_index;

	if (!network_substeps_active() || substep_index < 1 || substep_index >= substep_count)
		return;
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		struct substep_record const *replaying = &network_substeps_globals.replaying[player_index];
		struct substep_record *ran = &network_substeps_globals.ran[player_index];
		struct substep_control const *control;
		struct player_datum *player;
		struct unit_datum *unit;
		unsigned long before;

		network_substeps_globals.pressed_unit[player_index] = NONE;
		if (!replaying->valid || replaying->count != substep_count)
			continue;
		player = distributed_player(player_index);
		if (!player || player->unit_index != replaying->unit_index || player->local_player_index != NONE)
			continue;
		unit = unit_get(player->unit_index);
		control = &replaying->controls[substep_index - 1];
		before = unit->unit.control_flags;
		unit->unit.control_flags = (before & ~SUBSTEP_HELD_FLAGS) | (control->held_flags & SUBSTEP_HELD_FLAGS);
		unit->unit.throttle.i = control->throttle.i;
		unit->unit.throttle.j = control->throttle.j;
		unit->unit.throttle.k = 0.0f;
		unit->unit.primary_trigger = control->primary_trigger;
		if (control->turned)
		{
			real_vector3d aiming_vector;

			player_aiming_vector_from_facing(DATUM_INDEX_NEW(player_index, player->identifier), &aiming_vector,
				&control->facing);
			unit->unit.desired_aiming_vector = aiming_vector;
			unit->unit.desired_facing_vector = aiming_vector;
			unit->unit.desired_looking_vector = aiming_vector;
		}
		network_substeps_globals.pressed_unit[player_index] = player->unit_index;
		network_substeps_globals.pressed_flags[player_index] = control->held_flags & ~before & SUBSTEP_HELD_FLAGS;
		/* (what the tick ran, for the host to relay) */
		ran->controls[substep_index - 1] = *control;
		ran->given |= (word)FLAG(substep_index);
	}
}

void network_substeps_local_given(
	long player_index,
	long unit_index,
	short substep_index,
	short substep_count,
	real_euler_angles2d const *facing,
	boolean turned)
{
	long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	struct substep_record *ran;
	struct substep_control *control;

	if (!network_substeps_active() || absolute_index < 0 || absolute_index >= MAXIMUM_TRACKED_PLAYERS ||
		substep_index < 1 || substep_index >= substep_count || substep_count > MAXIMUM_SIM_SUBSTEPS)
	{
		return;
	}
	ran = &network_substeps_globals.ran[absolute_index];
	if (ran->unit_index != unit_index || ran->count != substep_count)
		return;
	control = &ran->controls[substep_index - 1];
	substep_control_from_unit(unit_get(unit_index), control);
	control->facing = *facing;
	control->turned = turned;
	ran->given |= (word)FLAG(substep_index);
}

unsigned long network_substeps_pressed_flags(
	long unit_index)
{
	short player_index;

	if (unit_index == NONE)
		return 0;
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		if (network_substeps_globals.pressed_unit[player_index] == unit_index)
			return network_substeps_globals.pressed_flags[player_index];
	}
	return 0;
}

void network_substeps_client_send(
	void)
{
	struct
	{
		struct distributed_message_header header;
		struct distributed_substep_input inputs[MAXIMUM_LOCAL_PLAYERS];
	} message;
	short count = 0;
	short local_player_index;

	if (!network_substeps_active())
		return;
	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
	{
		long player_index = local_player_get_player_index(local_player_index);
		long absolute_index = player_index != NONE ? DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index) : NONE;
		struct distributed_substep_input *input = &message.inputs[count];
		struct substep_record const *ran;
		struct player_action action;
		unsigned short control_flags[DISTRIBUTED_INPUT_HISTORY];
		long tick;
		short substep;

		if (absolute_index < 0 || absolute_index >= MAXIMUM_TRACKED_PLAYERS)
			continue;
		ran = &network_substeps_globals.ran[absolute_index];
		/* (with the input of the same tick, which goes with it) */
		if (!substep_record_complete(ran) || !network_substeps_globals.start_valid[absolute_index] ||
			substep_record_unchanged(ran, &network_substeps_globals.start[absolute_index]) ||
			!update_client_distributed_input(local_player_index, &tick, &action, control_flags))
		{
			continue;
		}
		csmemset(input, 0, sizeof(*input));
		input->player_index = distributed_player_to_byte(player_index);
		input->substep_count = (byte)ran->count;
		input->tick = tick;
		for (substep = 0; substep < ran->count - 1; substep++)
		{
			struct substep_control const *control = &ran->controls[substep];

			input->controls[substep].held_flags = (word)control->held_flags;
			input->controls[substep].turned = (byte)control->turned;
			input->controls[substep].throttle_i = control->throttle.i;
			input->controls[substep].throttle_j = control->throttle.j;
			input->controls[substep].primary_trigger = control->primary_trigger;
			input->controls[substep].yaw = control->facing.yaw;
			input->controls[substep].pitch = control->facing.pitch;
		}
		count++;
	}
	if (count)
	{
		word size = (word)(sizeof(message.header) + count * sizeof(struct distributed_substep_input));

		distributed_send(&message, _distributed_message_player_substeps, count, size, _distributed_to_host);
		network_substeps_globals.bytes_sent += size;
	}
}

void network_substeps_host_send(
	long machine_index,
	long update_number,
	short const *player_indices,
	short count)
{
	struct
	{
		struct distributed_message_header header;
		long update_number;
		byte entries[RELAYED_MESSAGE_ENTRIES_SIZE];
	} message;
	word used = 0;
	short entries = 0;
	short index;

	if (!network_substeps_active() || update_number == NONE)
		return;
	for (index = 0; index <= count; index++)
	{
		short player_index = index < count ? player_indices[index] : NONE;
		struct substep_record const *ran = player_index != NONE ? &network_substeps_globals.ran[player_index] : NULL;

		/* (a full message, or the last, sent) */
		if (entries && (!ran || used + RELAYED_RECORD_MAXIMUM_SIZE > RELAYED_MESSAGE_ENTRIES_SIZE))
		{
			word size = (word)(sizeof(message.header) + sizeof(message.update_number) + used);

			message.update_number = update_number;
			distributed_send_to_machine(machine_index, &message, _distributed_message_relayed_substeps, entries, size);
			network_substeps_globals.bytes_sent += size;
			used = 0;
			entries = 0;
		}
		if (!ran || player_index < 0 || player_index >= MAXIMUM_TRACKED_PLAYERS || !substep_record_complete(ran) ||
			!network_substeps_globals.start_valid[player_index] ||
			substep_record_unchanged(ran, &network_substeps_globals.start[player_index]))
		{
			continue;
		}
		used = (word)(used + relayed_record_write(player_index, ran, message.entries + used));
		entries++;
	}
}

word network_substeps_forward_write(
	short player_index,
	long key,
	byte *buffer)
{
	struct substep_record const *heard;

	buffer[0] = 0;
	if (!network_substeps_active() || player_index < 0 || player_index >= MAXIMUM_TRACKED_PLAYERS)
		return 1;
	heard = &network_substeps_globals.heard[player_index];
	if (!heard->valid || heard->key != key || heard->count != network_substeps_count() || heard->count < 2)
		return 1;
	buffer[0] = (byte)heard->count;
	return (word)(substep_controls_write(heard, buffer + 1) - buffer);
}

word network_substeps_forward_read(
	short player_index,
	long key,
	byte const *data,
	byte const *end)
{
	struct substep_record record;
	byte const *after;

	if (end - data < 1)
		return 0;
	if (!data[0])
		return 1;
	csmemset(&record, 0, sizeof(record));
	record.key = key;
	record.count = data[0];
	record.unit_index = NONE;
	after = substep_controls_read(&record, data + 1, end);
	if (!after)
		return 0;
	if (network_substeps_active() && player_index >= 0 && player_index < MAXIMUM_TRACKED_PLAYERS &&
		record.count == network_substeps_count())
	{
		substep_record_heard(network_substeps_globals.forwarded, player_index, &record);
	}
	return (word)(after - data);
}

boolean network_substeps_handles_message(
	word const *message,
	word size)
{
	struct distributed_message_header header;

	if (size < sizeof(header))
		return FALSE;
	csmemcpy(&header, message, sizeof(header));
	return header.type == _distributed_message_player_substeps || header.type == _distributed_message_relayed_substeps;
}

void network_substeps_handle_message(
	long machine_index,
	word const *message,
	word size)
{
	struct distributed_message_header header;
	byte const *entries = (byte const *)message + sizeof(header);
	byte const *end = (byte const *)message + size;

	if (size < sizeof(header) || !game_in_progress() || !network_substeps_active())
		return;
	csmemcpy(&header, message, sizeof(header));
	if (header.type == _distributed_message_player_substeps)
	{
		/* (the host's, from a client machine) */
		if (game_connection() != _game_connection_network_server || machine_index == NONE ||
			size < sizeof(header) + header.count * sizeof(struct distributed_substep_input))
		{
			return;
		}
		network_substeps_handle_inputs(machine_index, (struct distributed_substep_input const *)entries,
			header.count);
	}
	else if (header.type == _distributed_message_relayed_substeps)
	{
		/* (a client's, from the host) */
		if (game_connection() != _game_connection_network_client || machine_index != NONE)
			return;
		network_substeps_handle_relayed(entries, end, header.count);
	}
}

long network_substeps_bytes_sent(
	void)
{
	long bytes = network_substeps_globals.bytes_sent;

	network_substeps_globals.bytes_sent = 0;
	return bytes;
}
