/*
DEVICE_MACHINES.H
*/

#ifndef __DEVICE_MACHINES_H
#define __DEVICE_MACHINES_H
#pragma once

/* ---------- headers */

#include "device_definitions.h"
#include "devices.h"
#include "scenario/scenario_definitions.h"

/* ---------- macros */

#define machine_get(index) ((struct machine_datum *)object_get_and_verify_type((index), _object_mask_machine))
#define machine_try_and_get(index) ((struct machine_datum *)object_try_and_get_and_verify_type((index), _object_mask_machine))

/* ---------- structures */

struct scenario_machine_datum
{
	struct scenario_object_datum object;
	struct scenario_device_datum device;
	byte flags;
	byte pad[3];
};

/* ---------- prototypes/DEVICE_MACHINES.C */

void machines_initialize(
	void);
void machines_dispose(
	void);
void machines_initialize_for_new_map(
	void);
void machines_dispose_from_old_map(
	void);
void machine_place(
	long machine_index,
	struct scenario_machine_datum *scenario_machine);
boolean machine_new(
	long machine_index);
void machine_delete(
	long machine_index);
void machine_bumped(
	long machine_index,
	long biped_index);
void machine_try_to_open_with_damage(
	long machine_index);
boolean machine_update(
	long machine_index);

/* port: carry elevator riders with the current tick's collision transform. */
boolean machine_get_elevator_position(long machine_index, real_point3d *position);
void machine_move_elevator_riders(long machine_index, real_point3d const *previous_position);
/* port: whether the lift carries the biped by its movement (it stands on it,
and has a collision model); an AI's biped's carry this tick at game.sim_rate
60 and 120, for its sub-steps (bipeds.c), forgotten at the tick's last */
boolean machine_carries_rider(long machine_index, long biped_index);
boolean machine_elevator_carried(long biped_index, real_vector3d *offset);
void machine_elevator_carried_forget(void);

#endif // __DEVICE_MACHINES_H
