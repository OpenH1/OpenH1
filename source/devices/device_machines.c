/*
DEVICE_MACHINES.C

symbols in this file:
00084100 0010:
	_machines_initialize (0000)
00084110 0010:
	_machines_dispose (0000)
00084120 0010:
	_machines_initialize_for_new_map (0000)
00084130 0010:
	_machines_dispose_from_old_map (0000)
00084140 0080:
	_machine_place (0000)
000841C0 0080:
	_machine_new (0000)
00084240 0010:
	_machine_delete (0000)
00084250 0030:
	_machine_bumped (0000)
00084280 0050:
	_machine_try_to_open_with_damage (0000)
000842D0 0400:
	_machine_update (0000)
*/

/* ---------- headers */

#include "cseries.h"

#include "device_machines.h"
#include "game/game.h"
#include "game/game_allegiance.h"
#include "memory/data.h"
#include "objects/objects.h"
#include "units/bipeds.h"
#include "units/unit_definitions.h"
#include "sim_rate.h" /* port: port/linux/game/sim_rate.c */

/* ---------- constants */

enum
{
	_machine_type_door = 0,
	_machine_type_platform = 2,
};

enum
{
	_device_position_changed_bit = 2,
};

enum
{
	_object_function2_active_bit = 2,
	_object_runtime_unit_cannot_open_doors_automatically_bit = 14,
};

enum
{
	/* port: the AI's bipeds the lifts carry in one tick, at most
	(machine_carried_riders) */
	MAXIMUM_CARRIED_AI_RIDERS = 64,
};

/* ---------- macros */

/* ---------- structures */

struct device_group_datum
{
	short identifier;
	word pad;
	real actual_value;
};

/* ---------- prototypes */

static void machine_carried_rider_add(long biped_index, real_vector3d const *offset);

/* ---------- globals */

/* port: at game.sim_rate 60 and 120, the AI's bipeds a lift carried this tick
(machine_move_elevator_riders) and by how much. An AI's biped moves its
sub-steps in its own update (bipeds.c), before the lift's update or after
it, and the lift carries it by the tick's whole movement in its own, so
where each of its sub-steps left it is off its share of the carry by the
share still to come; bipeds.c's bipeds_replay_substep takes that off
(machine_elevator_carried). A player's biped is carried in the fast phase
instead, a share each sub-step (objects.c's lanes). Nothing here outlives the
tick: bipeds.c forgets it at the tick's last sub-step */
static struct
{
	long tick; /* (game_time) */
	short count;
	struct
	{
		long biped_index;
		real_vector3d offset;
	} riders[MAXIMUM_CARRIED_AI_RIDERS];
} machine_carried_riders = { NONE, 0 };

/* ---------- public code */

void machines_initialize(
	void)
{
	return;
}

void machines_dispose(
	void)
{
	return;
}

void machines_initialize_for_new_map(
	void)
{
	return;
}

void machines_dispose_from_old_map(
	void)
{
	return;
}

void machine_place(
	long machine_index,
	struct scenario_machine_datum *scenario_machine)
{
	struct machine_datum *machine = machine_get(machine_index);

	device_add_scenario_information(machine_index, &scenario_machine->device);
	if (TEST_FLAG(scenario_machine->flags, _machine_does_not_operate_automatically_bit))
	{
		machine->machine.flags |= FLAG(_machine_does_not_operate_automatically_bit);
	}
	if (TEST_FLAG(scenario_machine->flags, _machine_one_sided_bit))
	{
		machine->machine.flags |= FLAG(_machine_one_sided_bit);
	}
	if (TEST_FLAG(scenario_machine->flags, _machine_never_appears_locked_bit))
	{
		machine->machine.flags |= FLAG(_machine_never_appears_locked_bit);
	}
	if (TEST_FLAG(scenario_machine->flags, _machine_opened_by_melee_attack_bit))
	{
		machine->machine.flags |= FLAG(_machine_opened_by_melee_attack_bit);
	}

	return;
}

boolean machine_new(
	long machine_index)
{
	struct machine_datum *machine = machine_get(machine_index);
	struct machine_definition *definition = machine_definition_get(machine->definition_index);

	machine->object.flags |= FLAG(13);
	if (TEST_FLAG(definition->machine.flags, _machine_is_elevator_bit))
	{
		machine->object.flags |= FLAG(14);
	}
	else
	{
		machine->object.flags &= ~FLAG(14);
	}

	if (TEST_FLAG(definition->machine.flags, _machine_is_elevator_bit))
	{
		machine->object.flags |= FLAG(15);
	}
	else
	{
		machine->object.flags &= ~FLAG(15);
	}

	return TRUE;
}

void machine_delete(
	long machine_index)
{
	return;
}

void machine_bumped(
	long machine_index,
	long biped_index)
{
	struct machine_datum *machine = machine_get(machine_index);

	machine_definition_get(machine->definition_index);

	return;
}

void machine_try_to_open_with_damage(
	long machine_index)
{
	struct machine_datum *machine = machine_get(machine_index);

	machine_definition_get(machine->definition_index);
	if (TEST_FLAG(machine->machine.flags, _machine_opened_by_melee_attack_bit))
	{
		device_set_actual_position(machine_index, 1.0f);
	}

	return;
}

/* port: at game.sim_rate 60 and 120 a machine (a lift, a door, a platform)
still moves once a tick, in the macro lane (objects.h): its position is a
quantity of the tick, which devices.c's device_update moves by its velocity
and acceleration per tick toward its group's value (accelerate_to_position),
a platform's by its speed per tick, and its nodes are its animation's frame at
that position. Between two ticks it has no other path than the one between
the two positions, so a sub-step that saw it "at its time" would see what the
lanes already show it at: objects.c blends its nodes from the tick before's
to this tick's by the sub-step's fraction (the position linearly, a rotation
the shorter way round), which for a lift's animation, a translation a frame,
is its animation at the sub-step's position exactly. Moving it in the
sub-steps would add nothing a rider or a collider could meet, and would move
what is authored on the tick off it: its delay and the door's open ticks,
the door's check every fourth tick, the start and stop effects, the group
values the scripts wait on and the network sends. A rider of the micro lane
stands on the blend: a player's biped in the fast phase, an AI's biped in its
own update (objects.c's object_lane_view_substep). */
boolean machine_update(
	long machine_index)
{
	boolean activate;
	struct machine_definition *definition;
	long *object_index;
	short object_count;
	real_vector3d elevator_offset;
	long object_indices[16];
	struct machine_datum *machine = machine_get(machine_index);

	definition = machine_definition_get(machine->definition_index);
	if (definition->machine.type == _machine_type_platform)
	{
		machine->device.position +=
			(1.0f - machine->device.power) * definition->device.runtime_maximum_depowered_position_velocity +
			machine->device.power * definition->device.runtime_maximum_powered_position_velocity;
		if (machine->device.position >= 1.0f)
		{
			machine->device.position -= 1.0f;
		}
		machine->device.position_velocity = 0.0f;
		machine->device.flags |= FLAG(_device_position_changed_bit);
		if (machine->device.position_group_index != NONE)
		{
			struct device_group_datum *group = (struct device_group_datum *)datum_get(
				device_groups_data,
				machine->device.position_group_index);

			group->actual_value = machine->device.position;
		}
	}

	if (!TEST_FLAG(machine->machine.flags, _machine_does_not_operate_automatically_bit) &&
		definition->machine.type == _machine_type_door &&
		((game_time_get() + machine_index) & 3) == 0)
	{
		real automatic_activation_radius;

		activate = FALSE;
		automatic_activation_radius = definition->device.automatic_activation_radius < _real_epsilon
			? machine->object.bounding_sphere_radius
			: definition->device.automatic_activation_radius;
		object_count = objects_in_sphere(
			_object_mask_biped,
			_object_mask_biped,
			&machine->object.location,
			&machine->object.bounding_sphere_center,
			automatic_activation_radius,
			object_indices,
			NUMBEROF(object_indices));
		if (object_count > 0)
		{
			long remaining_object_count;

			object_index = object_indices;
			remaining_object_count = (unsigned short)object_count;
			do
			{
				struct unit_datum *unit = unit_get(*object_index);
				struct unit_definition *unit_definition = unit_definition_get(unit->definition_index);
				boolean can_open = TRUE;

				if (TEST_FLAG(*((byte *)unit + 0xb6), _object_function2_active_bit) ||
					TEST_FLAG(*(unsigned long *)((byte *)unit_definition + 0x17c), _object_runtime_unit_cannot_open_doors_automatically_bit))
				{
					can_open = FALSE;
				}

				if (!(TEST_FLAG(machine->machine.flags, _machine_one_sided_bit) &&
					machine->device.position == 0.0f &&
					!game_team_is_enemy(_game_team_player, unit->object.owner_team_index) &&
					dot_product3d(
						vector_from_points3d(
							&machine->object.bounding_sphere_center,
							&unit->object.bounding_sphere_center,
							&elevator_offset),
						&machine->object.forward) > 0.0f))
				{
					if (can_open)
					{
						activate = TRUE;
					}
				}

				object_index++;
			}
			while (--remaining_object_count);
		}

		if (activate)
		{
			if (machine->device.position_group_index != NONE)
			{
				device_group_set_desired_value(machine->device.position_group_index, 1.0f);
			}
			machine->machine.door_open_ticks = -3;
		}
	}

	if (definition->machine.type == _machine_type_door)
	{
		if (machine->device.position == 1.0f)
		{
			machine->machine.door_open_ticks++;
			if (machine->machine.door_open_ticks > definition->machine.runtime_door_open_ticks &&
				machine->device.position_group_index != NONE)
			{
				device_group_set_desired_value(machine->device.position_group_index, 0.0f);
			}
		}
		else
		{
			machine->machine.door_open_ticks = 0;
		}
	}

	/* port: the original carried the elevator's riders here, by the elevator
	node's movement since the last tick, before this tick's node matrices
	were computed: one tick late, and the wrong way as an elevator reverses.
	machine_move_elevator_riders (below), which object_update calls after
	computing them, carries them by this tick's movement instead, in single
	player too. */
	if (TEST_FLAG(machine->device.flags, _device_position_changed_bit))
	{
		object_translate(machine_index, &machine->object.position, NULL);
		machine->device.flags &= ~FLAG(_device_position_changed_bit);
	}

	return TRUE;
}

/* port: sample the floor around its node recomputation, rather than carrying
riders by the previous tick's movement (wrong when an elevator reverses). */
boolean machine_get_elevator_position(
	long machine_index,
	real_point3d *position)
{
	struct machine_datum *machine = machine_try_and_get(machine_index);
	struct machine_definition *definition;
	short node;

	if (!machine)
		return FALSE;
	definition = machine_definition_get(machine->definition_index);
	node = definition->machine.elevator_node_index;
	if (!TEST_FLAG(definition->machine.flags, _machine_is_elevator_bit) ||
		!VALID_INDEX(node, machine->object.node_matrices.size / sizeof(real_matrix4x3)))
		return FALSE;
	*position = object_get_node_matrix(machine_index, node)->position;
	return TRUE;
}

/* port: objects.c calls this after computing the new collision nodes. The
same displacement carries riders and moves their floor, including phase snaps. */
void machine_move_elevator_riders(
	long machine_index,
	real_point3d const *previous_position)
{
	struct machine_datum *machine = machine_get(machine_index);
	struct machine_definition *definition = machine_definition_get(machine->definition_index);
	real_point3d *node_position;
	long *elevator_object_index;
	short object_count;
	real_vector3d elevator_offset;
	real_point3d new_position;
	long elevator_object_indices[2048];

	if (TEST_FLAG(definition->machine.flags, _machine_is_elevator_bit) &&
		definition->machine.elevator_node_index != NONE)
	{
		real_matrix4x3 *node_matrix = object_get_node_matrix(
			machine_index,
			(unsigned short)definition->machine.elevator_node_index);

		elevator_offset.i = node_matrix->position.x - previous_position->x;
		node_position = &node_matrix->position;
		elevator_offset.j = node_position->y - previous_position->y;
		elevator_offset.k = node_position->z - previous_position->z;
		if (elevator_offset.i != 0.0f || elevator_offset.j != 0.0f || elevator_offset.k != 0.0f)
		{
			object_count = objects_in_sphere(
				_object_mask_biped,
				_object_mask_biped,
				&machine->object.location,
				&machine->object.bounding_sphere_center,
				machine->object.bounding_sphere_radius,
				elevator_object_indices,
				NUMBEROF(elevator_object_indices));
			if (object_count > 0)
			{
				long remaining_object_count;

				elevator_object_index = elevator_object_indices;
				remaining_object_count = (unsigned short)object_count;
				do
				{
					long biped_index = *elevator_object_index;
					struct biped_datum *biped = biped_get(biped_index);

					if (biped->biped.elevator_object_index == machine_index)
					{
						add_vectors3d(&elevator_offset, (real_vector3d const *)&biped->object.position, (real_vector3d *)&new_position);
						object_translate(biped_index, &new_position, NULL);
						machine_carried_rider_add(biped_index, &elevator_offset);
					}

					elevator_object_index++;
				}
				while (--remaining_object_count);
			}
		}
		machine->machine.elevator_position = *node_position;
	}
}

/* port: whether the lift carries the biped by its movement
(machine_move_elevator_riders): it stands on it, and the lift finds it among
the objects that collide (objects_in_sphere's: those with a collision model,
as the game's bipeds all have) */
boolean machine_carries_rider(
	long machine_index,
	long biped_index)
{
	struct machine_datum *machine = machine_try_and_get(machine_index);
	struct biped_datum *biped = (struct biped_datum *)object_try_and_get_and_verify_type(biped_index,
		_object_mask_biped);
	struct machine_definition *definition;

	if (!machine || !biped || biped->biped.elevator_object_index != machine_index ||
		!TEST_FLAG(biped->object.flags, _object_has_collision_model_bit))
	{
		return FALSE;
	}
	definition = machine_definition_get(machine->definition_index);
	return TEST_FLAG(definition->machine.flags, _machine_is_elevator_bit) &&
		definition->machine.elevator_node_index != NONE;
}

/* port: how far a lift carried an AI's biped this tick (above), if it did */
boolean machine_elevator_carried(
	long biped_index,
	real_vector3d *offset)
{
	short index;

	if (machine_carried_riders.tick != game_time_get())
		return FALSE;
	for (index = 0; index < machine_carried_riders.count; index++)
	{
		if (machine_carried_riders.riders[index].biped_index == biped_index)
		{
			*offset = machine_carried_riders.riders[index].offset;
			return TRUE;
		}
	}
	return FALSE;
}

/* port: (the tick's last sub-step, bipeds.c) the tick's carries forgotten */
void machine_elevator_carried_forget(
	void)
{
	machine_carried_riders.tick = NONE;
	machine_carried_riders.count = 0;
}

/* ---------- private code */

/* port: a biped carried by a lift this tick kept, if it is an AI's, which
moves its sub-steps in its own update (machine_carried_riders), at 60 and
120 */
static void machine_carried_rider_add(
	long biped_index,
	real_vector3d const *offset)
{
	short index;

	if (sim_substep_count() <= 1 || biped_get(biped_index)->unit.player_index != NONE ||
		!object_lane_moves_in_substeps(biped_index))
	{
		return;
	}
	if (machine_carried_riders.tick != game_time_get())
	{
		machine_carried_riders.tick = game_time_get();
		machine_carried_riders.count = 0;
	}
	for (index = 0; index < machine_carried_riders.count; index++)
	{
		if (machine_carried_riders.riders[index].biped_index == biped_index)
		{
			add_vectors3d(&machine_carried_riders.riders[index].offset, offset,
				&machine_carried_riders.riders[index].offset);
			return;
		}
	}
	if (machine_carried_riders.count < MAXIMUM_CARRIED_AI_RIDERS)
	{
		machine_carried_riders.riders[machine_carried_riders.count].biped_index = biped_index;
		machine_carried_riders.riders[machine_carried_riders.count].offset = *offset;
		machine_carried_riders.count++;
	}
}
