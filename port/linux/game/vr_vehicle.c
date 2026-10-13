/*
VR_VEHICLE.C

The VR mode's vehicles steered by their handlebars (HALO_VR;
vr.ghost_steering, port/linux/README.md "VR"), for vr_render.c: the Ghost's
driver seat now; a vehicle_bars entry adds another (the Warthog's wheel, the
Banshee's).

Once seated (the eye in the seat, as vr_seat.c shows it after the blink
in), the view turns with the vehicle: its heading is the vehicle's, as each
frame draws it (the root node's, interpolated between the game's ticks),
eased a little (VEHICLE_HEADING_SECONDS), and the player's axes turn with
it (src/vr.c's body_yaw), so that the view looks along the vehicle wherever
it turns, and the head looks about from there; the edges of the view dim a
little as it turns fast, or the vehicle speeds up or slows hard (the
comfort vignette, src/d3d8_gl.c). The view stays level, and the eye, held
in the seat (vr_seat.c), is held in the vehicle as it would be level,
turned with it alone: the vehicle's pitching and rolling as it hovers (up to
9 degrees on b30's beach, the eye 1.45 m behind and 0.58 m above the
vehicle's middle, so a fifth of a metre up and down) reach it only slowly
(VEHICLE_TILT_SECONDS), as a slope's, not its quick wobble.

The handles' places, the model's markers for them, are given to src/vr.c
each frame in the player's axes: it takes them as the grips close by them,
draws each hand holding its handle on it (halo_vr_view; the first-person
model, without its weapon, first_person_weapons.c), and steers by the bars
between the hands (halo_vr_aim): the vehicle turns toward its heading
turned as the bars ask, as the game turns it toward a driver's facing,
which is all the netcode sends of it.

The switch from the player's own heading to the vehicle's is made as the
eye moves into the seat, in the blink's dark; getting out, the player keeps
the vehicle's heading as it was.
*/

#ifdef HALO_VR

#include "cseries.h"
#include "math/real_math.h"
#include "game/game.h"
#include "objects/objects.h"
#include "tag_files/tag_files.h"
#include "units/units.h"
#include "units/unit_definitions.h"

#include "../src/vr.h"

#include <math.h>
#include <string.h>

/* the platform layer's */
void platform_log(char const *format, ...);
/* vr_seat.c's */
boolean vr_seat_seated_shown(void);

/* the view's heading eased toward the vehicle's (the time constant,
seconds): the game's ticks, blended, and a hover's wobble about its up */
#define VEHICLE_HEADING_SECONDS 0.05f
/* the eye's shift by the vehicle's tilt (from where it would be in the
vehicle level) eased in over this time constant (seconds) */
#define VEHICLE_TILT_SECONDS 0.3f
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

/* the vehicles steered by handlebars: the part of the vehicle's tag name,
the part of the seat's label, and the left and right handles' markers */
static struct vehicle_bars
{
	char const *vehicle;
	char const *seat;
	char const *handles[2];
} const vehicle_bars[] =
{
	/* (the Ghost's, and the multiplayer maps' ghost_mp: its handles are the
	Elite driver's hands' markers, where the Chief's hands are too as he
	drives: the model has none of its own) */
	{ "vehicles\\ghost\\ghost", "G-driver", { "left hand elite", "right hand elite" } },
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
	/* the vehicle's velocity at its last tick (world units a tick), that
	tick, and its acceleration along the ground then (metres a second
	squared) */
	real_vector3d velocity;
	long velocity_tick;
	real speeding;
} vr_vehicle = { NONE };

/* the handlebars of the seat the unit is in, if it has any */
static struct vehicle_bars const *vehicle_bars_find(long unit_index, long *vehicle_index)
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
	for (index = 0; index < (short)NUMBEROF(vehicle_bars); index++)
	{
		if (name && strstr(name, vehicle_bars[index].vehicle) && strstr(seat->label, vehicle_bars[index].seat))
			return &vehicle_bars[index];
	}
	return NULL;
}

/* vr_render.c, each frame the view is drawn from a seat (seated_unit_index,
else NONE): in a seat steered by handlebars, once seated, the view's heading
replaced by the vehicle's (base_yaw), and the handles placed for src/vr.c
from the eye (eye, in the world, held level in the vehicle: in and out;
units: world units a metre); FALSE leaves the view's heading the player's */
boolean vr_vehicle_view(long seated_unit_index, real seconds, real_point3d *eye, real units, real *base_yaw)
{
	long vehicle_index = NONE;
	struct vehicle_bars const *bars;
	real_matrix4x3 const *root;
	real heading, last_heading, c, s;
	float handles[2][3];
	short hand;

	if (!halo_vr_handlebars_enabled() ||
		(bars = vehicle_bars_find(seated_unit_index, &vehicle_index)) == NULL || !vr_seat_seated_shown())
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
		real ease = seconds > 0.0f ? 1.0f - (real)exp(-seconds / VEHICLE_TILT_SECONDS) : 0.0f;
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
		platform_log("vr vehicle: %s's handlebars: the view turns with it (from %.0f degrees to its %.0f)",
			tag_get_name(object_get(vehicle_index)->definition_index), *base_yaw * 57.29578f, heading * 57.29578f);
		vr_vehicle.vehicle_index = vehicle_index;
		vr_vehicle.heading = heading;
		vr_vehicle.bob_squares = vr_vehicle.bob_largest = 0.0f;
		vr_vehicle.bob_frames = 0;
		vr_vehicle.velocity_tick = NONE;
		vr_vehicle.speeding = 0.0f;
		vr_vehicle.handles_found = TRUE;
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
	*base_yaw = vr_vehicle.heading;

	/* the handles, from the eye in the player's axes (forward, left, up:
	turned back by the view's heading), in metres */
	c = (real)cos(vr_vehicle.heading);
	s = (real)sin(vr_vehicle.heading);
	for (hand = 0; hand < 2; hand++)
	{
		struct object_marker marker;
		real_vector3d d;

		if (object_get_marker_by_name(vehicle_index, bars->handles[hand], &marker, 1) < 1)
		{
			if (vr_vehicle.handles_found)
				platform_log("vr vehicle: no \"%s\" marker on the vehicle", bars->handles[hand]);
			vr_vehicle.handles_found = FALSE;
			return TRUE;
		}
		vector_from_points3d(eye, &marker.matrix.position, &d);
		handles[hand][0] = (float)((d.i * c + d.j * s) / units);
		handles[hand][1] = (float)((-d.i * s + d.j * c) / units);
		handles[hand][2] = (float)(d.k / units);
	}
	/* (and the heading the game steers from: the vehicle's at its last
	tick, not as drawn a tick behind and eased) */
	{
		real_vector3d const *forward = &object_get(vehicle_index)->object.forward;

		halo_vr_handlebars_place(handles[0], handles[1], (float)vr_vehicle.heading,
			(float)atan2(forward->j, forward->i));
	}
	return TRUE;
}

#endif
