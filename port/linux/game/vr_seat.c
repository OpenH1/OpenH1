/*
VR_SEAT.C

The VR mode's view from a vehicle's seat (HALO_VR; vr.vehicle_view,
vr.seat_motion, port/linux/README.md "VR"), for vr_render.c, and its
automated test.

With vr.vehicle_view = "first_person" the eye is in the seat, and the game
animates the seated body: getting in is most of a second of climbing (the
eye swept 2.7 to 3.4 m, up to half a metre a frame), getting out a third of
a second to a second (1.7 to 2.7 m), and a turret's seat swings about the
turret's pivot as it aims (the Shade's eye some 0.65 m from it), with
flinches when hit. None of
that is the player's own motion. With vr.seat_motion = "still" the eye stays
where it is in the vehicle instead: as the body climbs in, where it was
standing; once seated, where the head then is, until the player is out; and
it moves between those (and from the seat to where the player stands, once
out) in a blink: the view darkens with the eye held, the eye moves in the
dark, and the view lightens. The vehicle's own motion it rides with, and
the view stays level and turned by the head and the stick alone
(vr_render.c). Any other jump of the eye between the game's camera and the
seat's, or between the game's first-person camera and the one following a
vehicle (vr.vehicle_view = "third_person"), is blinked through the same way:
the director cuts between those cameras in the VR mode (director.c) rather
than flying between them for a second.

debug.vr_test_seat seats the first player in the nearest vehicle's seat
whose label has the given text in it ("gunner", "driver", ...), standing at
its entrance (put there two seconds before), as the action button does (unit_enter_seat: the seat's entering animation), and
takes them out as it does (unit_try_and_exit_seat) some seconds later. From
a second before getting in to a few seconds after getting out, each frame's
centre camera is logged against the last's, how far it moved and turned,
with the phase (on foot, entering, seated, exiting) and the blink's
darkness, and each phase's sums as it ends: the view's motion the player
did not make, with the head held still (debug.vr_test_head = "0"), and how
much of it was seen (not in the dark).

Called from the main loop every frame (main.c), with each VR frame's centre
camera (vr_render.c), and in the eye pass (render.c).
*/

#ifdef HALO_VR

#include "cseries.h"
#include "math/real_math.h"
#include "cache/cache_files.h"
#include "game/game.h"
#include "game/players.h"
#include "objects/objects.h"
#include "units/units.h"
#include "units/unit_definitions.h"
#include "units/vehicle_definitions.h"
#include "tag_files/tag_files.h"
#include "camera/director.h"
#include "render/render_cameras.h"
#include "rasterizer/rasterizer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* the platform layer's (port/linux/src/port_config.c) */
const char *config_string(char const *name);
unsigned long config_changes(void);
void platform_log(char const *format, ...);
/* vr_cabin.c's */
short vr_cabin_seat_eye(long seated_unit_index, real_point3d *eye);
/* d3d8_gl.c's, vr.c's */
void halo_vr_eye_tint(float red, float green, float blue);
float halo_vr_vignette(float fov[2][4]);

#define METRES_PER_WORLD_UNIT 3.048f
#define DEGREES_PER_RADIAN (180.0f / 3.14159265f)
/* a blink: for a jump of the eye further than this (metres), the view
darkens over this time (seconds), the eye moves, and it lightens over
this */
#define BLINK_DISTANCE 0.1f
#define BLINK_DARKENING_SECONDS 0.1f
#define BLINK_LIGHTENING_SECONDS 0.15f

/* where the eye is: the game's first-person camera, another of the game's
(following a vehicle, the dead's), the seated head (vr.seat_motion =
"head"), or held in the vehicle as the body climbs in or once seated */
enum
{
	_seat_source_first_person,
	_seat_source_game,
	_seat_source_head,
	_seat_source_getting_in,
	_seat_source_seated
};

enum
{
	_seat_phase_on_foot,
	_seat_phase_entering,
	_seat_phase_seated,
	_seat_phase_exiting,
	NUMBER_OF_SEAT_PHASES
};

static char const *const seat_phase_names[NUMBER_OF_SEAT_PHASES] =
{
	"on foot",
	"entering",
	"seated",
	"exiting",
};

static struct
{
	/* the eye last frame: where it was, in the world and in the vehicle
	it rode (else NONE), and from which source */
	boolean shown_valid;
	real_point3d shown;
	long shown_vehicle_index;
	real_point3d shown_local;
	short source;
	/* the seat the player is in (else NONE), counted each time it changes,
	and the count the eye was shown for */
	long seat_vehicle_index;
	short seat_index;
	long seat_serial;
	long shown_serial;
	/* vr.seat_motion = "still": the eye held as the body climbs in, and
	once seated, in the vehicle's frame */
	boolean frozen;
	real_point3d frozen_local;
	boolean anchored;
	real_point3d anchor_local;
	/* the cabin of the seat (vr_cabin.c: the eye its, not the head's), else
	NONE */
	short cabin;
	/* the blink: darkening toward the jump, and how dark (0 to 1) */
	boolean blinking;
	real darkness;
	long blinks;

	boolean checked;
	/* debug.vr_test_seat: the seat's label's text, when to get in and for
	how long (seconds) */
	boolean testing;
	char seat_label[32];
	real enter_seconds;
	real seated_seconds;
	/* and whether someone drives the vehicle the player is in (for a seat
	that does not: "drive"), the driver made for it, and the vehicle */
	boolean drive;
	long driver_index;
	long driven_index;
	/* the game's time (ticks) the test got in, and whether it got out */
	long entered_time;
	boolean brought;
	boolean approached;
	boolean entered;
	boolean exited;
	/* the log: the last frame's camera, its phase, and the phase's sums */
	boolean logging;
	boolean last_valid;
	real_point3d last_position;
	real_vector3d last_forward;
	real_vector3d last_up;
	short phase;
	long phase_frames;
	real phase_distance;
	real phase_turn;
	real phase_largest_move;
	real phase_largest_turn;
	real phase_seen_distance;
	real phase_largest_seen_move;
	long frame;
} vr_seat;

/* ---------- the view */

/* the first player's unit, else NONE */
static long first_player_unit(void)
{
	long player_index = local_player_get_player_index(0);

	return player_index == NONE ? NONE : player_get(player_index)->unit_index;
}

/* vr.seat_motion: the eye stays put in the vehicle ("still"), else it is
the seated player's head as the game animates it ("head") */
static boolean seat_still(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static boolean still;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		still = strcmp(config_string("vr.seat_motion"), "head") != 0;
	}
	return still;
}

/* a point in a vehicle's frame (its root node's, posed between ticks as it
is drawn) and back */
static void vehicle_to_local(long vehicle_index, real_point3d const *point, real_point3d *local)
{
	matrix4x3_inverse_transform_point(object_get_node_matrix(vehicle_index, 0), point, local);
}

static void vehicle_from_local(long vehicle_index, real_point3d const *local, real_point3d *point)
{
	matrix4x3_transform_point(object_get_node_matrix(vehicle_index, 0), local, point);
}

/* where the eye was last frame: in the world, and in the vehicle it rode
then (the place held through a blink's darkening) */
static void shown_position(real_point3d *position)
{
	if (vr_seat.shown_vehicle_index != NONE && object_try_and_get(vr_seat.shown_vehicle_index))
		vehicle_from_local(vr_seat.shown_vehicle_index, &vr_seat.shown_local, position);
	else
		*position = vr_seat.shown;
}

/* each VR frame (vr_render.c), the eye's place before the head's offset:
the game's camera, else in a vehicle's seat (seated_unit_index, with
vr.vehicle_view = "first_person") the seat's, and a blink through any jump
between the two. In: the game's camera; out: the eye. aiming: the view is
the player's (not a cinematic's or the menus') */
void vr_seat_eye(long seated_unit_index, boolean aiming, real seconds, real_point3d *position)
{
	real_point3d desired = *position;
	long vehicle_index = NONE;
	short source;

	if (!aiming)
	{
		vr_seat.shown_valid = FALSE;
		vr_seat.blinking = FALSE;
		vr_seat.darkness = 0.0f;
		vr_seat.anchored = FALSE;
		vr_seat.frozen = FALSE;
		return;
	}
	if (seated_unit_index != NONE)
	{
		struct unit_datum *unit = unit_get(seated_unit_index);
		short seat_index = unit->unit.parent_seat_index;

		vehicle_index = unit->object.parent_object_index;
		if (vehicle_index != vr_seat.seat_vehicle_index || seat_index != vr_seat.seat_index)
		{
			/* (another seat: what was held belongs to the last) */
			vr_seat.seat_vehicle_index = vehicle_index;
			vr_seat.seat_index = seat_index;
			vr_seat.anchored = FALSE;
			vr_seat.frozen = FALSE;
			vr_seat.seat_serial++;
		}
		if (!seat_still())
		{
			source = _seat_source_head;
			unit_get_head_position(seated_unit_index, &desired);
		}
		else if (!vr_seat.anchored && unit->unit.animation.state == _unit_state_entering_seat)
		{
			/* getting in: the eye stays where it was as the body climbs in
			(not drawn: vr_render_hides_object), with the vehicle */
			source = _seat_source_getting_in;
			if (!vr_seat.frozen)
			{
				real_point3d from;

				if (vr_seat.shown_valid)
					shown_position(&from);
				else
					from = *position;
				vehicle_to_local(vehicle_index, &from, &vr_seat.frozen_local);
				vr_seat.frozen = TRUE;
			}
			vehicle_from_local(vehicle_index, &vr_seat.frozen_local, &desired);
		}
		else
		{
			/* seated, and getting out: the eye stays where the head was
			once seated, in the vehicle: it rides with the vehicle, but not
			with the seat's animations or a turret's turning (which swings
			the seat about the turret's pivot, and the gun about the eye
			instead) */
			source = _seat_source_seated;
			if (vr_seat.anchored)
				vehicle_from_local(vehicle_index, &vr_seat.anchor_local, &desired);
			else
				unit_get_head_position(seated_unit_index, &desired);
		}
		/* (once seated, where the seat has a cabin, the eye is its: where the
		cabin is drawn about it; another cabin's, as its view changes, is
		blinked to) */
		if (source != _seat_source_getting_in)
		{
			short cabin = vr_cabin_seat_eye(seated_unit_index, &desired);

			if (cabin != vr_seat.cabin)
			{
				vr_seat.cabin = cabin;
				vr_seat.anchored = FALSE;
				vr_seat.seat_serial++;
			}
		}
	}
	else
	{
		vr_seat.seat_vehicle_index = NONE;
		vr_seat.seat_index = NONE;
		vr_seat.anchored = FALSE;
		vr_seat.frozen = FALSE;
		vr_seat.cabin = NONE;
		source = director_get_perspective(0) == _director_perspective_first_person ?
			_seat_source_first_person : _seat_source_game;
	}

	/* a blink where the eye would jump: it darkens with the eye held where
	it was, moves in the dark, and lightens */
	if (vr_seat.shown_valid && !vr_seat.blinking &&
		(source != vr_seat.source || vr_seat.seat_serial != vr_seat.shown_serial))
	{
		real_point3d from;

		shown_position(&from);
		if (distance3d(&from, &desired) * METRES_PER_WORLD_UNIT > BLINK_DISTANCE)
		{
			vr_seat.blinking = TRUE;
			vr_seat.blinks++;
			if (vr_seat.testing)
			{
				platform_log("vr seat test: frame %ld: blink %ld, the eye from source %d to %d, %.3f m", vr_seat.frame,
					vr_seat.blinks, vr_seat.source, source, distance3d(&from, &desired) * METRES_PER_WORLD_UNIT);
			}
		}
	}
	if (vr_seat.blinking)
	{
		vr_seat.darkness += seconds / BLINK_DARKENING_SECONDS;
		if (vr_seat.darkness < 1.0f)
		{
			shown_position(&desired);
			source = vr_seat.source;
			vehicle_index = vr_seat.shown_vehicle_index;
			if (vehicle_index != NONE && !object_try_and_get(vehicle_index))
				vehicle_index = NONE;
		}
		else
		{
			vr_seat.darkness = 1.0f;
			vr_seat.blinking = FALSE;
		}
	}
	else
	{
		vr_seat.darkness = MAX(0.0f, vr_seat.darkness - seconds / BLINK_LIGHTENING_SECONDS);
	}
	if (!vr_seat.blinking && source == _seat_source_seated && !vr_seat.anchored)
	{
		/* (the seat's place, taken as the eye moves there) */
		vehicle_to_local(vehicle_index, &desired, &vr_seat.anchor_local);
		vr_seat.anchored = TRUE;
	}

	*position = desired;
	vr_seat.shown_valid = TRUE;
	vr_seat.shown = desired;
	vr_seat.source = source;
	if (!vr_seat.blinking)
		vr_seat.shown_serial = vr_seat.seat_serial;
	vr_seat.shown_vehicle_index = vehicle_index;
	if (vehicle_index != NONE)
		vehicle_to_local(vehicle_index, &desired, &vr_seat.shown_local);
}

/* whether the eye is in the seat now, once moved there (the blink in at
its darkest, or since): vr_vehicle.c turns the view with the vehicle from
then */
boolean vr_seat_seated_shown(void)
{
	return vr_seat.shown_valid && vr_seat.source == _seat_source_seated && vr_seat.anchored;
}

/* where the eye was last frame (vr_death.c: the eye held as the player
dies), FALSE when none was */
boolean vr_seat_shown(real_point3d *position)
{
	if (!vr_seat.shown_valid)
		return FALSE;
	shown_position(position);
	return TRUE;
}

/* how dark the blink makes the view (0 to 1) */
real vr_seat_darkness(void)
{
	return vr_seat.darkness;
}

/* the eye pass, after the game's screen flash (render.c): the blink's
darkness over both eyes, all of each (d3d8_gl.c's tint, as they are
presented; the HUD's layer stays lit) */
void vr_seat_blink_draw(void)
{
	real t = vr_seat.darkness;

	if (t <= 0.0f)
		return;
	t = 1.0f - t * t * (3.0f - 2.0f * t);
	halo_vr_eye_tint(t, t, t);
}

/* ---------- the test */

static void vr_seat_read_settings(void)
{
	char const *setting = config_string("debug.vr_test_seat");

	vr_seat.checked = TRUE;
	char drive[16] = "";

	vr_seat.seated_seconds = 10.0f;
	vr_seat.driver_index = NONE;
	if (sscanf(setting, "%31s %f %f %15s", vr_seat.seat_label, &vr_seat.enter_seconds, &vr_seat.seated_seconds,
		drive) >= 2)
	{
		vr_seat.testing = TRUE;
		vr_seat.drive = !strcmp(drive, "drive");
		platform_log("vr seat test: the \"%s\" seat %.1f s in, for %.1f s%s", vr_seat.seat_label,
			vr_seat.enter_seconds, vr_seat.seated_seconds, vr_seat.drive ? ", someone else driving" : "");
	}
}

/* the nearest vehicle with a free seat whose label has the test's text in
it, and the seat, else NONE */
static long vr_seat_test_vehicle(long unit_index, short *seat_found)
{
	real_point3d const *position = &object_get(unit_index)->object.position;
	struct object_iterator vehicles;
	long nearest_index = NONE;
	real nearest_distance = 0.0f;

	object_iterator_new(&vehicles, _object_mask_vehicle, 0);
	while (object_iterator_next(&vehicles))
	{
		struct unit_definition *definition = unit_definition_get(object_get(vehicles.index)->definition_index);
		real distance = distance_squared3d(position, &object_get(vehicles.index)->object.position);
		short seat_index;

		if (nearest_index != NONE && distance >= nearest_distance)
			continue;
		for (seat_index = 0; seat_index < definition->unit.seats.count; seat_index++)
		{
			struct unit_seat *seat = TAG_BLOCK_GET_ELEMENT(&definition->unit.seats, seat_index, struct unit_seat);

			if (strstr(seat->label, vr_seat.seat_label) && unit_can_enter_seat(unit_index, vehicles.index, seat_index, NULL))
			{
				nearest_index = vehicles.index;
				*seat_found = seat_index;
				nearest_distance = distance;
				break;
			}
		}
	}
	return nearest_index;
}

/* a vehicle's tag in the map with such a seat (none of its kind placed
yet: the level's Ghosts come later), else NONE */
static long vr_seat_test_vehicle_tag(void)
{
	struct tag_iterator iterator;
	long tag_index;

	tag_iterator_new(&iterator, VEHICLE_DEFINITION_TAG);
	for (tag_index = tag_iterator_next(&iterator); tag_index != NONE; tag_index = tag_iterator_next(&iterator))
	{
		struct unit_definition *definition = vehicle_definition_get(tag_index);
		short seat_index;

		for (seat_index = 0; seat_index < definition->unit.seats.count; seat_index++)
		{
			if (strstr(TAG_BLOCK_GET_ELEMENT(&definition->unit.seats, seat_index, struct unit_seat)->label,
				vr_seat.seat_label))
			{
				return tag_index;
			}
		}
	}
	return NONE;
}

/* three seconds before getting in, a vehicle of the kind found is made
before the first player (a level's are far apart, or carried by dropships),
or of a kind the map has a tag for: two world units ahead, upright, a
little above the ground it falls to */
static void vr_seat_test_bring(long unit_index)
{
	short seat_index = NONE;
	long vehicle_index = vr_seat_test_vehicle(unit_index, &seat_index);
	long definition_index = vehicle_index != NONE ? object_get(vehicle_index)->definition_index :
		vr_seat_test_vehicle_tag();
	struct object_datum *unit = object_get(unit_index);
	struct object_placement_data data;
	long new_index;

	if (definition_index == NONE)
	{
		platform_log("vr seat test: no vehicle with a free \"%s\" seat", vr_seat.seat_label);
		return;
	}
	object_placement_data_new(&data, definition_index, NONE);
	data.forward.i = unit->object.forward.i;
	data.forward.j = unit->object.forward.j;
	data.forward.k = 0.0f;
	normalize3d(&data.forward);
	data.up = *global_up3d;
	data.position.x = unit->object.position.x + data.forward.i * 2.0f;
	data.position.y = unit->object.position.y + data.forward.j * 2.0f;
	data.position.z = unit->object.position.z + 0.2f;
	new_index = object_new(&data);
	platform_log("vr seat test: %s %s before the player", tag_get_name(definition_index),
		new_index == NONE ? "could not be made" : "made");
}

/* two seconds before getting in (before the log starts), the first player
is put at the nearest such seat's entrance, where they stand to press the
action button */
static void vr_seat_test_approach(long unit_index)
{
	short seat_index = NONE;
	long vehicle_index = vr_seat_test_vehicle(unit_index, &seat_index);
	real_point3d entrance, seat_point;

	if (vehicle_index != NONE &&
		unit_get_seat_entrance_point(unit_index, vehicle_index, seat_index, &entrance, &seat_point, NULL))
	{
		object_set_position(unit_index, &entrance, NULL, NULL);
	}
}

/* the first player gets into the nearest such seat, as the action button
does */
static void vr_seat_test_enter(long unit_index)
{
	short seat_index = NONE;
	long vehicle_index = vr_seat_test_vehicle(unit_index, &seat_index);
	struct unit_seat *seat;

	if (vehicle_index == NONE)
	{
		platform_log("vr seat test: no vehicle with a free \"%s\" seat", vr_seat.seat_label);
		return;
	}
	seat = TAG_BLOCK_GET_ELEMENT(&unit_definition_get(object_get(vehicle_index)->definition_index)->unit.seats,
		seat_index, struct unit_seat);
	if (unit_enter_seat(unit_index, vehicle_index, seat_index))
	{
		platform_log("vr seat test: frame %ld: into %s's seat %d (%s)", vr_seat.frame,
			tag_get_name(object_get(vehicle_index)->definition_index), seat_index, seat->label);
	}
	else
	{
		platform_log("vr seat test: could not get into %s's seat %d (%s)",
			tag_get_name(object_get(vehicle_index)->definition_index), seat_index, seat->label);
	}
}

/* "drive": someone of the player's kind in the driver's seat of the
vehicle the player rode in (a seat that does not drive), driving it as a
player would (unit_control, as players.c gives a player's input): full
throttle, straight for TEST_DRIVE_STRAIGHT seconds, then by turns left,
straight, right and straight, each TEST_DRIVE_TURN_SECONDS, its facing
TEST_DRIVE_TURN off the vehicle's heading as it turns; and out with the
player */
#define TEST_DRIVE_STRAIGHT 3.0f
#define TEST_DRIVE_TURN_SECONDS 2.0f
#define TEST_DRIVE_TURN 0.12f
struct unit_control_data
{
	char animation_state;
	char aiming_speed;
	word control_flags;
	short weapon_index;
	short grenade_index;
	short zoom_level;
	short pad;
	real_vector3d throttle;
	real primary_trigger;
	real_vector3d facing_vector;
	real_vector3d aiming_vector;
	real_vector3d looking_vector;
};

static short vr_seat_phase(long unit_index);

static void vr_seat_test_drive(long unit_index, long time)
{
	struct object_datum *unit = object_get(unit_index);
	long vehicle_index = unit->object.parent_object_index;

	if (!vr_seat.drive)
		return;
	if (vr_seat.driver_index != NONE && (!object_try_and_get(vr_seat.driver_index) || vr_seat.exited ||
		vehicle_index != vr_seat.driven_index))
	{
		/* (out with the player: the driver made for the test goes) */
		if (object_try_and_get(vr_seat.driver_index))
			object_delete(vr_seat.driver_index);
		platform_log("vr seat test: frame %ld: the driver gone", vr_seat.frame);
		vr_seat.driver_index = NONE;
		vr_seat.drive = FALSE;
		return;
	}
	if (vehicle_index == NONE || vr_seat_phase(unit_index) != _seat_phase_seated)
		return;
	if (vr_seat.driver_index == NONE)
	{
		struct unit_definition *definition = unit_definition_get(object_get(vehicle_index)->definition_index);
		struct object_placement_data data;
		short seat_index;

		for (seat_index = 0; seat_index < definition->unit.seats.count; seat_index++)
		{
			if (unit_seat_is_driver(vehicle_index, seat_index))
				break;
		}
		object_placement_data_new(&data, unit->definition_index, NONE);
		data.position = object_get(vehicle_index)->object.position;
		data.position.z += 1.0f;
		vr_seat.driver_index = seat_index < definition->unit.seats.count ? object_new(&data) : NONE;
		if (vr_seat.driver_index == NONE || !unit_enter_seat(vr_seat.driver_index, vehicle_index, seat_index))
		{
			platform_log("vr seat test: no one could drive");
			if (vr_seat.driver_index != NONE)
				object_delete(vr_seat.driver_index);
			vr_seat.driver_index = NONE;
			vr_seat.drive = FALSE;
			return;
		}
		vr_seat.driven_index = vehicle_index;
		platform_log("vr seat test: frame %ld: someone drives, in seat %d", vr_seat.frame, seat_index);
	}
	{
		struct unit_control_data control;
		real_vector3d const *forward = &object_get(vehicle_index)->object.forward;
		real seconds = (real)(time - vr_seat.entered_time) / TICKS_PER_SECOND - 2.0f;
		long part = seconds < TEST_DRIVE_STRAIGHT ? 1 : (long)((seconds - TEST_DRIVE_STRAIGHT) / TEST_DRIVE_TURN_SECONDS) & 3;
		real turn = part == 0 ? TEST_DRIVE_TURN : part == 2 ? -TEST_DRIVE_TURN : 0.0f;
		real heading = (real)atan2(forward->j, forward->i) + turn;

		memset(&control, 0, sizeof(control));
		control.animation_state = _unit_animation_state_in_combat;
		control.weapon_index = NONE;
		control.grenade_index = NONE;
		control.zoom_level = NONE;
		control.throttle.i = seconds < 0.0f ? 0.0f : 1.0f;
		control.facing_vector.i = (real)cos(heading);
		control.facing_vector.j = (real)sin(heading);
		control.aiming_vector = control.looking_vector = control.facing_vector;
		unit_control(vr_seat.driver_index, &control);
	}
}

/* each frame: gets in and out on time */
void vr_seat_test_update(void)
{
	long unit_index;
	long time;

	if (!vr_seat.checked)
		vr_seat_read_settings();
	if (!vr_seat.testing || !game_in_progress() || (unit_index = first_player_unit()) == NONE)
		return;
	time = game_time_get();
	vr_seat.logging = time >= (long)((vr_seat.enter_seconds - 1.0f) * TICKS_PER_SECOND) &&
		(!vr_seat.exited || time < vr_seat.entered_time + (long)((vr_seat.seated_seconds + 5.0f) * TICKS_PER_SECOND));
	if (!vr_seat.brought && time >= (long)((vr_seat.enter_seconds - 3.0f) * TICKS_PER_SECOND))
	{
		vr_seat.brought = TRUE;
		vr_seat_test_bring(unit_index);
	}
	if (!vr_seat.approached && time >= (long)((vr_seat.enter_seconds - 2.0f) * TICKS_PER_SECOND))
	{
		vr_seat.approached = TRUE;
		vr_seat_test_approach(unit_index);
	}
	if (!vr_seat.entered && time >= (long)(vr_seat.enter_seconds * TICKS_PER_SECOND))
	{
		vr_seat.entered = TRUE;
		vr_seat.entered_time = time;
		vr_seat_test_enter(unit_index);
	}
	if (vr_seat.entered && !vr_seat.exited &&
		time >= vr_seat.entered_time + (long)(vr_seat.seated_seconds * TICKS_PER_SECOND))
	{
		vr_seat.exited = TRUE;
		if (unit_try_and_exit_seat(unit_index))
			platform_log("vr seat test: frame %ld: out of the seat", vr_seat.frame);
		else
			platform_log("vr seat test: could not get out of the seat");
	}
	if (vr_seat.entered)
		vr_seat_test_drive(unit_index, time);
}

/* the first player's phase: on foot, getting in, seated or getting out */
static short vr_seat_phase(long unit_index)
{
	struct unit_datum *unit;

	if (unit_index == NONE || object_get(unit_index)->object.parent_object_index == NONE)
		return _seat_phase_on_foot;
	unit = unit_get(unit_index);
	if (unit->unit.animation.state == _unit_state_entering_seat)
		return _seat_phase_entering;
	if (unit->unit.animation.state == _unit_state_exiting_seat)
		return _seat_phase_exiting;
	return _seat_phase_seated;
}

static real angle_between(real_vector3d const *a, real_vector3d const *b)
{
	real cosine = dot_product3d(a, b);

	return (real)acos(cosine > 1.0f ? 1.0f : cosine < -1.0f ? -1.0f : cosine) * DEGREES_PER_RADIAN;
}

static void vr_seat_log_phase_end(void)
{
	if (vr_seat.phase_frames == 0)
		return;
	platform_log("vr seat log: %s: %ld frames, moved %.3f m (at most %.1f mm a frame), %.3f m of it seen (at most "
		"%.1f mm a frame), turned %.1f degrees (at most %.2f a frame); %ld blinks so far",
		seat_phase_names[vr_seat.phase], vr_seat.phase_frames, vr_seat.phase_distance,
		vr_seat.phase_largest_move * 1000.0f, vr_seat.phase_seen_distance, vr_seat.phase_largest_seen_move * 1000.0f,
		vr_seat.phase_turn, vr_seat.phase_largest_turn, vr_seat.blinks);
}

/* each VR frame's centre camera (vr_render.c), and the time since the last
frame's display (seconds): logs its motion while the test logs */
void vr_seat_log_camera(real_point3d const *position, real_vector3d const *forward, real_vector3d const *up,
	real seconds)
{
	long unit_index;
	short phase;

	vr_seat.frame++;
	if (!vr_seat.logging)
	{
		if (vr_seat.last_valid)
			vr_seat_log_phase_end();
		vr_seat.last_valid = FALSE;
		return;
	}
	unit_index = first_player_unit();
	phase = vr_seat_phase(unit_index);
	if (vr_seat.last_valid)
	{
		real move = distance3d(position, &vr_seat.last_position) * METRES_PER_WORLD_UNIT;
		real turn = MAX(angle_between(forward, &vr_seat.last_forward), angle_between(up, &vr_seat.last_up));
		real_point3d local;
		real_vector3d right;
		real vehicle_yaw = 0.0f, vehicle_pitch = 0.0f, vehicle_roll = 0.0f;
		float fov[2][4];
		long parent_index = unit_index == NONE ? NONE : object_get(unit_index)->object.parent_object_index;

		if (phase != vr_seat.phase)
		{
			vr_seat_log_phase_end();
			vr_seat.phase_frames = 0;
			vr_seat.phase_distance = 0.0f;
			vr_seat.phase_turn = 0.0f;
			vr_seat.phase_largest_move = 0.0f;
			vr_seat.phase_largest_turn = 0.0f;
			vr_seat.phase_seen_distance = 0.0f;
			vr_seat.phase_largest_seen_move = 0.0f;
		}
		vr_seat.phase_frames++;
		vr_seat.phase_distance += move;
		vr_seat.phase_turn += turn;
		vr_seat.phase_largest_move = MAX(vr_seat.phase_largest_move, move);
		vr_seat.phase_largest_turn = MAX(vr_seat.phase_largest_turn, turn);
		if (vr_seat.darkness < 1.0f)
		{
			/* (seen: not in a blink's full darkness) */
			vr_seat.phase_seen_distance += move;
			vr_seat.phase_largest_seen_move = MAX(vr_seat.phase_largest_seen_move, move);
		}
		/* (and the eye in the vehicle's frame, its root node's: whether the
		seat swings about the vehicle) */
		local.x = local.y = local.z = 0.0f;
		if (parent_index != NONE)
		{
			real_matrix4x3 const *root = object_get_node_matrix(parent_index, 0);

			matrix4x3_inverse_transform_point(root, position, &local);
			vehicle_yaw = (real)atan2(root->forward.j, root->forward.i) * DEGREES_PER_RADIAN;
			vehicle_pitch = (real)asin(PIN(root->forward.k, -1.0f, 1.0f)) * DEGREES_PER_RADIAN;
			vehicle_roll = (real)asin(PIN(root->left.k, -1.0f, 1.0f)) * DEGREES_PER_RADIAN;
		}
		/* (the view's heading, its roll, the vehicle's heading, degrees, and the
		frame's time: its turning and its acceleration) */
		cross_product3d(forward, up, &right);
		platform_log("vr seat log: frame %ld %s: moved %.1f mm, turned %.2f degrees; pitch %.1f; dark %.2f; "
			"height %.3f m; in the vehicle %.3f %.3f %.3f m; yaw %.3f roll %.3f vehicle %.3f pitch %.3f roll %.3f; "
			"vignette %.2f; %.4f s",
			vr_seat.frame, seat_phase_names[phase], move * 1000.0f, turn,
			(real)asin(forward->k > 1.0f ? 1.0f : forward->k < -1.0f ? -1.0f : forward->k) * DEGREES_PER_RADIAN,
			vr_seat.darkness, position->z * METRES_PER_WORLD_UNIT, local.x * METRES_PER_WORLD_UNIT,
			local.y * METRES_PER_WORLD_UNIT, local.z * METRES_PER_WORLD_UNIT,
			(real)atan2(forward->j, forward->i) * DEGREES_PER_RADIAN,
			(real)asin(right.k > 1.0f ? 1.0f : right.k < -1.0f ? -1.0f : right.k) * DEGREES_PER_RADIAN, vehicle_yaw,
			vehicle_pitch, vehicle_roll, halo_vr_vignette(fov), seconds);
	}
	vr_seat.phase = phase;
	vr_seat.last_valid = TRUE;
	vr_seat.last_position = *position;
	vr_seat.last_forward = *forward;
	vr_seat.last_up = *up;
}

#endif
