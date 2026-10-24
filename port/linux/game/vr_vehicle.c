/*
VR_VEHICLE.C

The VR mode's vehicles' seats ridden first-person (HALO_VR;
vr.ghost_steering, vr.warthog_steering, port/linux/README.md "VR"), for
vr_render.c: the Ghost's driver seat, steered by its handlebars; the
Warthog's, by its wheel, its passenger's, and its gunner's, the hands on the
chaingun's handles; a Banshee's pilot's, steered as it was. A vehicle_seats
entry adds another.

Once seated (the eye in the seat, as vr_seat.c shows it after the blink
in), the view turns with the vehicle: its heading is the vehicle's, as each
frame draws it (the root node's, interpolated between the game's ticks),
eased a little (VEHICLE_HEADING_SECONDS), and the player's axes turn with
it (src/vr.c's body_yaw), so that the view looks along the vehicle wherever
it turns, and the head looks about from there; the edges of the view dim a
little as it turns fast, or the vehicle speeds up or slows hard (the
comfort vignette, vulkan/host_vulkan_vr.c). The view stays level, and the eye, held
in the seat (vr_seat.c), is held in the vehicle as it would be level,
turned with it alone: the vehicle's pitching and rolling as it hovers (up to
9 degrees on b30's beach, the eye 1.45 m behind and 0.58 m above the
vehicle's middle, so a fifth of a metre up and down) reach it only slowly
(VEHICLE_TILT_SECONDS), as a slope's, not its quick wobble.

The Warthog pitches and rolls on its suspension far more than a Ghost
hovers (its pitch past 40 degrees, 35 degrees a second, on b30's beach):
its tilt is let in more slowly. Its height is not smoothed: it rises and
falls with the ground, under 2 Hz (on b30's beach nine tenths of it under 2
Hz, none above 3: no quick bounce), which a smoothing would only lag, the
eye sinking into the seat on a climb.

The handles' places, the model's markers for them (a wheel's grips, which
turn with the model's wheel as the game turns it), are given to src/vr.c each
frame in the player's axes, with how a hand holding one is turned and a
wheel's axis (from its node to its grips): it takes them as the grips close
by them, draws each hand holding its handle on it (halo_vr_view; the
first-person model, without its weapon, first_person_weapons.c), and steers
by the bars or the wheel between the hands (halo_vr_aim): the vehicle turns
toward its heading turned as they ask, as the game turns it toward a
driver's facing, which is all the netcode sends of it.

The switch from the player's own heading to the vehicle's is made as the
eye comes into the seat: the play space turned to it, eased, no faster than
CLIMB_TURN_MOST (with vr.seat_motion = "blink", all at once in the blink's
dark); getting out, the player keeps the vehicle's heading as it was.

With the hands off the handlebars or the wheel, either stick pushed sideways
steers (src/vr.c), the left one throttling as ever: the player can drive
without taking hold. The driver of a vehicle the game steers toward the
facing that is not ridden so (the Scorpion's; a Warthog's with
vr.warthog_steering = "look") is told to src/vr.c each frame
(halo_vr_seat_drive): there the left stick's sideways push turns the
player, and the aim, which the vehicle steers toward, with them.
*/

#ifdef HALO_VR

#include "cseries.h"
#include "math/real_math.h"
#include "game/game.h"
#include "objects/objects.h"
#include "tag_files/tag_files.h"
#include "models/models.h"
#include "objects/object_definitions.h"
#include "units/units.h"
#include "units/unit_definitions.h"
#include "units/vehicle_datum.h"
#include "units/vehicle_definitions.h"

#include "../src/vr.h"

#include <math.h>
#include <string.h>

/* the platform layer's */
void platform_log(char const *format, ...);
/* vr_seat.c's */
boolean vr_seat_seated_shown(void);
real vr_seat_darkness(void);
/* vr_climb.c's */
boolean vr_climb_seating(void);
void vr_climb_offset(real_vector3d *offset);

/* the view's heading eased toward the vehicle's (the time constant,
seconds): the game's ticks, blended, and a hover's wobble about its up */
#define VEHICLE_HEADING_SECONDS 0.05f
/* the comfort vignette (vr.comfort_vignette) asked for: from nothing as the
view turns at VIGNETTE_TURN_FROM radians a second to all of it at
VIGNETTE_TURN_TO, and as the vehicle speeds up or slows (its speed along
the ground's change, metres a second squared: not its turning, which
swings its velocity about as much as a hard start does) from
VIGNETTE_SPEEDING_FROM to VIGNETTE_SPEEDING_TO; the more of the two */
#define VIGNETTE_TURN_FROM 0.70f
#define VIGNETTE_TURN_TO 2.09f
#define VIGNETTE_SPEEDING_FROM 6.0f
#define VIGNETTE_SPEEDING_TO 16.0f
/* got into the seat by the hands (vr_climb.c: no blink), the play space
turned from the player's heading to the vehicle's over this long at least,
eased, no faster than CLIMB_TURN_MOST at its fastest (seconds, radians a
second) */
#define CLIMB_TURN_SECONDS 0.5f
#define CLIMB_TURN_MOST (45.0f * 0.017453292f)

/* the seats ridden first-person: the part of the vehicle's tag name, the
part of the seat's label, its controls (HALO_VR_SEAT_*), the left and right
handles' markers (none: NULL), a wheel's node (which its grips turn about),
the time constant its tilt is let into the eye over (seconds), and whether
it flies: its climbs and dives (its pitch's turn, which the level view does
not follow, the world streaming past it up or down) count in the comfort
vignette as its turns do */
static struct vehicle_seat
{
	char const *vehicle;
	char const *seat;
	short kind;
	char const *handles[2];
	char const *wheel;
	real tilt_seconds;
	boolean flies;
} const vehicle_seats[] =
{
	/* (the Ghost's, and the multiplayer maps' ghost_mp: its handles are the
	Elite driver's hands' markers, where the Chief's hands are too as he
	drives: the model has none of its own) */
	{ "vehicles\\ghost\\ghost", "G-driver", HALO_VR_SEAT_BARS, { "left hand elite", "right hand elite" }, NULL,
		0.3f, FALSE },
	/* (the Warthog's, and the rocket Warthog's: its wheel turns on the model
	as the game turns its wheels, the grips with it) */
	{ "warthog\\", "W-driver", HALO_VR_SEAT_WHEEL, { "left hand wheel grip", "right hand wheel grip" },
		"frame steering wheel", 0.5f, FALSE },
	/* (its passenger: the hands as on foot, the weapon in them) */
	{ "warthog\\", "W-passenger", HALO_VR_SEAT_RIDE, { NULL, NULL }, NULL, 0.5f, FALSE },
	/* (its gunner: the hands on the chaingun's handles, which turn and
	pitch with it, the right controller aiming it) */
	{ "warthog\\", "W-gunner", HALO_VR_SEAT_TURRET, { "gunner left hand", "gunner right hand" }, NULL, 0.5f,
		FALSE },
	/* (the Banshee's pilot: no handles, the right controller steering it
	where it points, as it aims a weapon; the ship banks and pitches about
	the eye, the view level) */
	{ "vehicles\\banshee\\banshee", "B-driver", HALO_VR_SEAT_RIDE, { NULL, NULL }, NULL, 0.4f, TRUE },
};

static struct
{
	/* the view turns with this vehicle (else NONE), its heading as shown
	(radians), and whether the handles were found last frame */
	long vehicle_index;
	real heading;
	boolean handles_found;
	/* the eye's shift by the vehicle's tilt as it is let in (world units) */
	real_vector3d tilt;
	/* what holding the eye level took out (the tilt's shift less its part
	let in), its squares' sum and its largest (metres), and the frames,
	logged as the view stops turning with the vehicle */
	real bob_squares;
	real bob_largest;
	long bob_frames;
	/* its forward as drawn last frame (a flier's turn, its pitch's too) */
	real_vector3d forward;
	/* the vehicle's velocity at its last tick (world units a tick), that
	tick, and its acceleration along the ground then (metres a second
	squared) */
	real_vector3d velocity;
	long velocity_tick;
	real speeding;
	/* got in by the hands, the play space's turn from the player's heading
	to the vehicle's (radians: the view's heading from the vehicle's at
	first), over these seconds, so far */
	real turn;
	real turn_seconds;
	real turned_seconds;
} vr_vehicle = { NONE };

/* the seat the unit is in, if it is ridden first-person */
static struct vehicle_seat const *vehicle_seat_find(long unit_index, long *vehicle_index)
{
	struct unit_datum *unit;
	struct unit_definition *definition;
	struct unit_seat *seat;
	char const *name;
	short index;

	if (unit_index == NONE || (unit = unit_get(unit_index))->object.parent_object_index == NONE ||
		unit->unit.parent_seat_index == NONE)
	{
		return NULL;
	}
	*vehicle_index = unit->object.parent_object_index;
	definition = unit_definition_get(object_get(*vehicle_index)->definition_index);
	if (unit->unit.parent_seat_index >= definition->unit.seats.count)
		return NULL;
	seat = TAG_BLOCK_GET_ELEMENT(&definition->unit.seats, unit->unit.parent_seat_index, struct unit_seat);
	name = tag_get_name(object_get(*vehicle_index)->definition_index);
	for (index = 0; index < (short)NUMBEROF(vehicle_seats); index++)
	{
		if (name && strstr(name, vehicle_seats[index].vehicle) && strstr(seat->label, vehicle_seats[index].seat))
			return halo_vr_seat_enabled(vehicle_seats[index].kind) ? &vehicle_seats[index] : NULL;
	}
	return NULL;
}

/* the driver's seat of a vehicle the game steers toward the driver's
facing whose sideways throttle moves nothing (the tank's and the jeep's
physics, vehicles.c: their slide is unused), seated: told to src/vr.c
(halo_vr_seat_drive) for one not ridden by vehicle_seats */
static void vehicle_drive(long unit_index)
{
	struct unit_datum *unit;
	struct object_datum *vehicle;
	struct unit_definition *definition;
	short type;

	if (unit_index == NONE || (unit = unit_get(unit_index))->object.parent_object_index == NONE ||
		unit->unit.parent_seat_index == NONE || unit->unit.animation.state == _unit_state_entering_seat ||
		unit->unit.animation.state == _unit_state_exiting_seat)
	{
		return;
	}
	vehicle = object_get(unit->object.parent_object_index);
	if (vehicle->object.type != _object_type_vehicle)
		return;
	definition = unit_definition_get(vehicle->definition_index);
	if (unit->unit.parent_seat_index >= definition->unit.seats.count ||
		!TEST_FLAG(TAG_BLOCK_GET_ELEMENT(&definition->unit.seats, unit->unit.parent_seat_index,
		struct unit_seat)->flags, _unit_seat_driver_bit))
	{
		return;
	}
	/* (the tag's vehicle type, vehicles.c's struct vehicle_definition's
	vehicle_type: 0 the human tank, 1 the human jeep) */
	type = *(short const *)((char const *)tag_get(VEHICLE_DEFINITION_TAG, vehicle->definition_index) + 0x2f4);
	if (type == 0 || type == 1)
		halo_vr_seat_drive();
}

/* a point (from the eye) or a direction in the world in the player's axes
(forward, left, up: turned back by the view's heading; metres) */
static void player_axes(real_vector3d const *v, real heading, real scale, float out[3])
{
	real c = (real)cos(heading), s = (real)sin(heading);

	out[0] = (float)((v->i * c + v->j * s) * scale);
	out[1] = (float)((-v->i * s + v->j * c) * scale);
	out[2] = (float)(v->k * scale);
}

/* vr_render.c, each frame the view is drawn from a seat (seated_unit_index,
else NONE): in a seat steered by handlebars, once seated, the view's heading
replaced by the vehicle's (base_yaw), and the handles placed for src/vr.c
from the eye (eye, in the world, held level in the vehicle: in and out;
units: world units a metre); FALSE leaves the view's heading the player's */
boolean vr_vehicle_view(long seated_unit_index, real seconds, real_point3d *eye, real units, real *base_yaw)
{
	long vehicle_index = NONE;
	struct vehicle_seat const *seat;
	real_matrix4x3 const *root;
	real heading, last_heading, shown_heading;
	struct halo_vr_seat placed;
	struct object_marker markers[2];
	real_point3d handle_eye;
	short hand;

	if ((seat = vehicle_seat_find(seated_unit_index, &vehicle_index)) == NULL)
		vehicle_drive(seated_unit_index);
	if (seat == NULL || !vr_seat_seated_shown())
	{
		if (vr_vehicle.vehicle_index != NONE)
		{
			platform_log("vr vehicle: the view turns as the player's again; the vehicle's tilt kept from the eye "
				"over %ld frames: %.1f cm (RMS), %.1f cm at most", vr_vehicle.bob_frames,
				vr_vehicle.bob_frames ? (real)sqrt(vr_vehicle.bob_squares / vr_vehicle.bob_frames) * 100.0f : 0.0f,
				vr_vehicle.bob_largest * 100.0f);
		}
		vr_vehicle.vehicle_index = NONE;
		return FALSE;
	}
	root = object_get_node_matrix(vehicle_index, 0);
	heading = (real)atan2(root->forward.j, root->forward.i);
	last_heading = vr_vehicle.vehicle_index == vehicle_index ? vr_vehicle.heading : heading;
	{
		/* the eye where it would be with the vehicle level (its place in the
		vehicle, turned by the vehicle's heading alone), and its shift by
		the tilt let in slowly */
		real_vector3d offset, local, tilt;
		real hc = (real)cos(heading), hs = (real)sin(heading);
		real ease = seconds > 0.0f ? 1.0f - (real)exp(-seconds / seat->tilt_seconds) : 0.0f;
		real_point3d level;

		vector_from_points3d(&root->position, eye, &offset);
		local.i = dot_product3d(&offset, &root->forward);
		local.j = dot_product3d(&offset, &root->left);
		local.k = dot_product3d(&offset, &root->up);
		level.x = root->position.x + local.i * hc - local.j * hs;
		level.y = root->position.y + local.i * hs + local.j * hc;
		level.z = root->position.z + local.k;
		vector_from_points3d(&level, eye, &tilt);
		if (vr_vehicle.vehicle_index != vehicle_index)
			vr_vehicle.tilt = tilt;
		else
		{
			vr_vehicle.tilt.i += (tilt.i - vr_vehicle.tilt.i) * ease;
			vr_vehicle.tilt.j += (tilt.j - vr_vehicle.tilt.j) * ease;
			vr_vehicle.tilt.k += (tilt.k - vr_vehicle.tilt.k) * ease;
		}
		{
			/* (what that keeps from the eye) */
			real_vector3d kept;
			real metres;

			kept.i = tilt.i - vr_vehicle.tilt.i;
			kept.j = tilt.j - vr_vehicle.tilt.j;
			kept.k = tilt.k - vr_vehicle.tilt.k;
			metres = (real)sqrt(magnitude_squared3d(&kept)) / units;
			vr_vehicle.bob_squares += metres * metres;
			vr_vehicle.bob_largest = MAX(vr_vehicle.bob_largest, metres);
			vr_vehicle.bob_frames++;
		}
		eye->x = level.x + vr_vehicle.tilt.i;
		eye->y = level.y + vr_vehicle.tilt.j;
		eye->z = level.z + vr_vehicle.tilt.k;
	}
	if (vr_vehicle.vehicle_index != vehicle_index)
	{
		platform_log("vr vehicle: %s's %s seat: the view turns with it (from %.0f degrees to its %.0f)",
			tag_get_name(object_get(vehicle_index)->definition_index), seat->seat, *base_yaw * 57.29578f,
			heading * 57.29578f);
		vr_vehicle.vehicle_index = vehicle_index;
		vr_vehicle.heading = heading;
		vr_vehicle.bob_squares = vr_vehicle.bob_largest = 0.0f;
		vr_vehicle.bob_frames = 0;
		vr_vehicle.velocity_tick = NONE;
		vr_vehicle.speeding = 0.0f;
		vr_vehicle.handles_found = TRUE;
		/* (got in by the hands, or by the action with the eye riding the
		body in (vr.seat_motion = "still": no blink): the play space turned
		to the vehicle's heading as the eye comes into the seat; only in a
		blink's dark, all at once) */
		vr_vehicle.turn = vr_climb_seating() || vr_seat_darkness() < 0.5f ?
			(real)remainder(*base_yaw - heading, 2.0 * M_PI) : 0.0f;
		vr_vehicle.turn_seconds = MAX(CLIMB_TURN_SECONDS, 1.5f * (real)fabs(vr_vehicle.turn) / CLIMB_TURN_MOST);
		vr_vehicle.turned_seconds = 0.0f;
		if (vr_vehicle.turn != 0.0f)
		{
			platform_log("vr vehicle: got in by the hands: the play space to turn %.0f degrees over %.2f s",
				vr_vehicle.turn * 57.29578f, vr_vehicle.turn_seconds);
		}
	}
	else if (seconds > 0.0f)
	{
		vr_vehicle.heading += (real)remainder(heading - vr_vehicle.heading, 2.0 * M_PI) *
			(1.0f - (real)exp(-seconds / VEHICLE_HEADING_SECONDS));
		vr_vehicle.heading = (real)remainder(vr_vehicle.heading, 2.0 * M_PI);
	}
	{
		/* the comfort vignette: the view's turn, the vehicle's speeding up
		or slowing (each tick: the game's velocity) */
		real_vector3d const *velocity = &object_get(vehicle_index)->object.translational_velocity;
		long tick = game_time_get();
		real turn = seconds > 0.0f ? (real)fabs(remainder(vr_vehicle.heading - last_heading, 2.0 * M_PI)) / seconds :
			0.0f;

		/* (a flier's: how fast its forward turns, up and down too) */
		if (seat->flies && seconds > 0.0f && vr_vehicle.velocity_tick != NONE)
		{
			real along = PIN(dot_product3d(&root->forward, &vr_vehicle.forward), -1.0f, 1.0f);

			turn = MAX(turn, (real)acos(along) / seconds);
		}
		vr_vehicle.forward = root->forward;
		real amount;

		if (vr_vehicle.velocity_tick != NONE && tick != vr_vehicle.velocity_tick)
		{
			real now = (real)sqrt(velocity->i * velocity->i + velocity->j * velocity->j);
			real then = (real)sqrt(vr_vehicle.velocity.i * vr_vehicle.velocity.i +
				vr_vehicle.velocity.j * vr_vehicle.velocity.j);

			vr_vehicle.speeding = (real)fabs(now - then) / (real)(tick - vr_vehicle.velocity_tick) *
				TICKS_PER_SECOND * TICKS_PER_SECOND / units;
		}
		if (tick != vr_vehicle.velocity_tick)
		{
			vr_vehicle.velocity = *velocity;
			vr_vehicle.velocity_tick = tick;
		}
		amount = MAX(PIN((turn - VIGNETTE_TURN_FROM) / (VIGNETTE_TURN_TO - VIGNETTE_TURN_FROM), 0.0f, 1.0f),
			PIN((vr_vehicle.speeding - VIGNETTE_SPEEDING_FROM) / (VIGNETTE_SPEEDING_TO - VIGNETTE_SPEEDING_FROM),
			0.0f, 1.0f));
		halo_vr_comfort_vignette((float)amount);
	}
	shown_heading = vr_vehicle.heading;
	if (vr_vehicle.turn != 0.0f)
	{
		real t;

		vr_vehicle.turned_seconds += seconds;
		t = PIN(vr_vehicle.turned_seconds / vr_vehicle.turn_seconds, 0.0f, 1.0f);
		shown_heading += vr_vehicle.turn * (1.0f - t * t * (3.0f - 2.0f * t));
		if (t >= 1.0f)
			vr_vehicle.turn = 0.0f;
	}
	*base_yaw = shown_heading;

	/* the seat for src/vr.c: its handles, from the eye in the player's axes
	(forward, left, up: turned back by the view's heading), in metres, how a
	hand holding one is turned, a wheel's axis; the heading the game steers
	from (the vehicle's at its last tick, not as drawn a tick behind and
	eased), which way it goes, and its wheels' lock */
	memset(&placed, 0, sizeof(placed));
	placed.kind = seat->kind;
	placed.heading = (float)shown_heading;
	{
		struct vehicle_datum *vehicle = (struct vehicle_datum *)object_get(vehicle_index);
		/* (the tag's turn block, vehicles.c's struct vehicle_definition's
		unknown308 and unknown30c: its wheels' lock left and right, degrees) */
		real const *turn = (real const *)((char const *)tag_get(VEHICLE_DEFINITION_TAG,
			object_get(vehicle_index)->definition_index) + 0x308);

		placed.steer_heading = (float)atan2(vehicle->object.forward.j, vehicle->object.forward.i);
		placed.steer_sign = vehicle->vehicle.speed < 0.0f ? -1.0f : 1.0f;
		placed.lock[0] = (float)fabs(turn[0]) * 0.017453292f;
		placed.lock[1] = (float)fabs(turn[1]) * 0.017453292f;
	}
	placed.handles_valid = seat->handles[0] != NULL;
	/* (from the eye as the hands climbing put it: vr_climb.c's offset,
	last frame's) */
	{
		real_vector3d offset;

		vr_climb_offset(&offset);
		point_from_line3d(eye, &offset, 1.0f, &handle_eye);
	}
	for (hand = 0; hand < 2 && placed.handles_valid; hand++)
	{
		real_vector3d d;

		if (object_get_marker_by_name(vehicle_index, seat->handles[hand], &markers[hand], 1) < 1)
		{
			if (vr_vehicle.handles_found)
				platform_log("vr vehicle: no \"%s\" marker on the vehicle", seat->handles[hand]);
			vr_vehicle.handles_found = FALSE;
			placed.handles_valid = FALSE;
			break;
		}
		vector_from_points3d(&handle_eye, &markers[hand].matrix.position, &d);
		player_axes(&d, shown_heading, 1.0f / units, placed.handles[hand]);
	}
	/* (held along the vehicle, upright; a turret's, as its handles are turned;
	on a wheel, into it, along its rim) */
	placed.handle_forward[0] = 1.0f;
	placed.handle_up[2] = 1.0f;
	if (seat->kind == HALO_VR_SEAT_TURRET && placed.handles_valid)
	{
		player_axes(&markers[1].matrix.forward, shown_heading, 1.0f, placed.handle_forward);
		player_axes(&markers[1].matrix.up, shown_heading, 1.0f, placed.handle_up);
	}
	if (seat->wheel && placed.handles_valid)
	{
		short node_index = model_find_node(object_definition_get(object_get(vehicle_index)->definition_index)->object.model.index,
			seat->wheel);
		real_point3d const *centre;
		real_vector3d to_left, to_right, axis, across, up;

		if (node_index == NONE)
		{
			if (vr_vehicle.handles_found)
				platform_log("vr vehicle: no \"%s\" node on the vehicle", seat->wheel);
			vr_vehicle.handles_found = FALSE;
			placed.handles_valid = FALSE;
		}
		else
		{
			/* (its axis: across its grips about its middle, toward the driver) */
			centre = &object_get_node_matrix(vehicle_index, node_index)->position;
			vector_from_points3d(centre, &markers[0].matrix.position, &to_left);
			vector_from_points3d(centre, &markers[1].matrix.position, &to_right);
			cross_product3d(&to_left, &to_right, &axis);
			if (normalize3d(&axis) <= 0.0f)
				axis = root->forward;
			{
				real_vector3d to_eye;

				vector_from_points3d(centre, eye, &to_eye);
				if (dot_product3d(&axis, &to_eye) < 0.0f)
					axis.i = -axis.i, axis.j = -axis.j, axis.k = -axis.k;
			}
			vector_from_points3d(&markers[0].matrix.position, &markers[1].matrix.position, &across);
			cross_product3d(&axis, &across, &up);
			normalize3d(&up);
			player_axes(&axis, shown_heading, 1.0f, placed.wheel_axis);
			axis.i = -axis.i, axis.j = -axis.j, axis.k = -axis.k;
			player_axes(&axis, shown_heading, 1.0f, placed.handle_forward);
			player_axes(&up, shown_heading, 1.0f, placed.handle_up);
		}
	}
	halo_vr_seat_place(&placed);
	return TRUE;
}

/* player_control.c, each update of the first local player's facing as the
VR mode aims (yaw, pitch: in the world): in a vehicle's seat whose camera
does not follow the vehicle (the Warthog's passenger's), the game takes the
facing as the vehicle tilts (players.c, player_aiming_vector_from_facing:
turned from the world's up to the vehicle's, as the flat screen's camera
tilts with it), so it is turned back first: the shot goes where the reticle
is, the vehicle tilted or not */
void vr_vehicle_facing(long unit_index, real *yaw, real *pitch)
{
	struct unit_datum *unit;
	struct object_datum *vehicle;
	struct unit_definition *definition;
	real_matrix4x3 rotation;
	real_vector3d facing, turned;

	if (unit_index == NONE || (unit = unit_get(unit_index))->object.parent_object_index == NONE ||
		unit->unit.parent_seat_index == NONE)
	{
		return;
	}
	vehicle = object_get(unit->object.parent_object_index);
	if (vehicle->object.type != _object_type_vehicle)
		return;
	definition = unit_definition_get(vehicle->definition_index);
	if (unit->unit.parent_seat_index >= definition->unit.seats.count ||
		TEST_FLAG(TAG_BLOCK_GET_ELEMENT(&definition->unit.seats, unit->unit.parent_seat_index,
		struct unit_seat)->flags, _unit_seat_third_person_camera_bit))
	{
		return;
	}
	/* (the game's turn, as players.c makes it: forward across the vehicle's
	up and the world's right, up the vehicle's) */
	cross_product3d(&vehicle->object.up, global_right3d, &rotation.forward);
	if (normalize3d(&rotation.forward) == 0.0f)
	{
		cross_product3d(&vehicle->object.up, global_down3d, &rotation.forward);
		normalize3d(&rotation.forward);
	}
	matrix4x3_rotation_from_vectors(&rotation, &rotation.forward, &vehicle->object.up);
	facing.i = (real)(cos(*pitch) * cos(*yaw));
	facing.j = (real)(cos(*pitch) * sin(*yaw));
	facing.k = (real)sin(*pitch);
	/* (its inverse: the transpose) */
	turned.i = dot_product3d(&facing, &rotation.forward);
	turned.j = dot_product3d(&facing, &rotation.left);
	turned.k = dot_product3d(&facing, &rotation.up);
	*yaw = (real)atan2(turned.j, turned.i);
	*pitch = (real)asin(PIN(turned.k, -1.0f, 1.0f));
}

#endif
