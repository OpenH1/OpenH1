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
that is the player's own motion. With vr.seat_motion = "still" (the
default) the player stays in the first person all the way in and out, with
no cut: as the body climbs in, the eye rides its head's path smoothed (the
glide: critically damped over RIDE_SECONDS, no faster than RIDE_FASTEST,
the climb's bumps and its half-metre steps gone), and once seated it stays
where the head then is in the vehicle, until the body climbs out, when it
rides the head's path out again; whatever is left between where the eye was
and where it belongs (the seat's eye, a cabin's, the first person's once
out) closes over GLIDE_SECONDS, no faster than RIDE_FASTEST. With
vr.seat_motion = "blink" (the older way) the eye is held where the player
stood as the body climbs in, and in the seat as it climbs out, and moves
between those in a blink: the view darkens with the eye held, the eye moves
in the dark, and the view lightens. Either way the vehicle's own motion it
rides with, and the view stays level and turned by the head and the stick
alone (vr_render.c). A jump of the eye between the game's camera and the
seat's, or between the game's first-person camera and the one following a
vehicle (vr.vehicle_view = "third_person"), is blinked through: the
director cuts between those cameras in the VR mode (director.c) rather
than flying between them for a second.

A feed cockpit (vr_cabin.c: the Scorpion's, vr.scorpion_view = "feed"),
with vr.seat_motion = "still", is got into and out of in the first person,
with no break: the eye rides the body through the game's own animations,
climbing onto the tank and down into the driver's place, and the cockpit is
there about it as it comes in. The eye follows the animated head's path
smoothed (critically damped over RIDE_SECONDS: the climb's bumps gone),
drawn toward the cockpit's eye as the animation goes on (at its end, the
cockpit's eye), never the head's turn: the player's own head keeps its
pitch and roll. As the body climbs in, the play space turns with it (eased
over the climb, no faster than RIDE_TURN_MOST) so that the player faces the
display as they sit; the comfort vignette is light while the eye moves
faster than walking (RIDE_WALKING). Seated, the display wakes: the grate
closes over its opening and the feed washes over it. Getting out, the
display collapses dark as the body starts to climb, and the eye rides the
body up and out and down beside the tank, on to where the player stands.

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
boolean vr_cabin_feed(short index);
/* vr.c's */
void halo_vr_body_turn(float radians);
/* vr_climb.c's */
boolean vr_climb_entering(real_point3d *eye);
boolean vr_climb_exiting(void);
boolean vr_climb_active(void);
/* vr_render.c's */
real vr_render_view_yaw(void);
/* d3d8_device.c's, xgpu_stream_encode.c's, vr.c's */
void halo_vr_eye_tint(float red, float green, float blue);
float halo_vr_vignette(float fov[2][4]);
void halo_vr_comfort_vignette(float amount);
void halo_vr_test_face(float yaw);
void halo_vr_test_screenshot(void);

#define METRES_PER_WORLD_UNIT 3.048f
#define DEGREES_PER_RADIAN (180.0f / 3.14159265f)
/* a blink: for a jump of the eye further than this (metres), the view
darkens over this time (seconds), the eye moves, and it lightens over
this */
#define BLINK_DISTANCE 0.1f
#define BLINK_DARKENING_SECONDS 0.1f
#define BLINK_LIGHTENING_SECONDS 0.15f
/* a feed cockpit's ride in and out: the eye's path smoothed over this
(seconds: a critically damped spring's time) and no faster than this
(metres a second: the game slides the body to the climb's start, 2 m in a
tenth of a second), done once within this of
where it goes (world units: 1 cm); the play space turned no faster than this
(radians a second); the comfort vignette from this speed of the eye (metres
a second: walking), as strong as this at its most */
#define RIDE_SECONDS 0.12f
#define RIDE_FASTEST 3.0f
#define RIDE_CLOSE (0.01f / METRES_PER_WORLD_UNIT)
#define RIDE_TURN_MOST (45.0f / DEGREES_PER_RADIAN)
#define RIDE_WALKING 1.4f
#define RIDE_VIGNETTE 0.45f
/* the glide (vr.seat_motion = "still"): a jump of the eye between its
places closed over this time constant (seconds), no faster than
RIDE_FASTEST, done within this (world units: 2 mm) */
#define GLIDE_SECONDS 0.12f
#define GLIDE_DONE (0.002f / METRES_PER_WORLD_UNIT)

/* vr.seat_motion */
enum
{
	_seat_motion_still,
	_seat_motion_blink,
	_seat_motion_head
};

/* where the eye is: the game's first-person camera, another of the game's
(following a vehicle, the dead's), the seated head (vr.seat_motion =
"head"), the body's head as it climbs in or out (vr.seat_motion = "still";
"blink": held where it was as it climbs in), or held in the vehicle once
seated */
enum
{
	_seat_source_first_person,
	_seat_source_game,
	_seat_source_head,
	_seat_source_getting_in,
	_seat_source_seated,
	_seat_source_getting_out
};

/* the glide: none, the eye following the body's head's path as it climbs
(in the vehicle), or what is left of a jump closing */
enum
{
	_glide_none,
	_glide_following,
	_glide_settling
};

/* a feed cockpit: the eye riding in, the display's stages as it wakes, in,
the display collapsing as the body climbs out, the eye riding out */
enum
{
	_feed_none,
	_feed_riding_in,
	_feed_grate,
	_feed_bars,
	_feed_wash,
	_feed_in,
	_feed_off,
	_feed_riding_out,
	NUMBER_OF_FEED_STAGES
};

static char const *const feed_stage_names[NUMBER_OF_FEED_STAGES] =
{
	"none", "riding in", "the grate closing", "the world through its bars", "the feed washing over", "in",
	"the display going dark", "riding out",
};

static real const feed_stage_seconds[NUMBER_OF_FEED_STAGES] =
{
	0.0f, 0.0f, 0.12f, 0.08f, 0.2f, 0.0f, 0.25f, 0.0f,
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
	/* the glide (vr.seat_motion = "still"): what it does, the vehicle its
	places are in (else NONE: the world), the eye following the head's path
	and its speed (world units, world units a second), what is left of a
	jump (world units, in that vehicle's axes), and how many so far */
	short glide;
	long glide_vehicle_index;
	real_point3d glide_local;
	real_vector3d glide_velocity;
	real_vector3d glide_offset;
	long glides;
	/* a feed cockpit: the stage, the seconds in it, the seat (its count)
	it is for, its vehicle and cabin; the eye riding in or out, in the
	vehicle, and its speed (world units a second); the body's animation's
	frames as it began, the play space's turn to make and made so far, and
	the seconds it is made over (the climb's, or longer: RIDE_TURN_MOST) */
	short feed_stage;
	real feed_seconds;
	long feed_serial;
	long feed_vehicle_index;
	short feed_cabin;
	real_point3d ride_local;
	real_vector3d ride_velocity;
	short ride_frames;
	real ride_turn;
	real ride_turned;
	real ride_turn_seconds;
	real ride_seconds;
	/* got in by the hands (vr_climb.c): the ride straight to the cockpit's
	eye from where the hands had the eye */
	boolean ride_direct;

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
	/* "climb [<metres>]": brought and stood this much nearer it than its
	entrance (0.25 m), facing it, the hands to get in and out (vr_climb.c),
	not the test */
	boolean climb;
	real climb_nearer;
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

/* vr.seat_motion: the eye stays put in the vehicle once seated, riding the
body's head in and out ("still") or blinking there ("blink"), else it is
the seated player's head as the game animates it ("head") */
static short seat_motion(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static short motion;

	if (read_at != config_changes())
	{
		char const *setting = config_string("vr.seat_motion");

		read_at = config_changes();
		motion = !strcmp(setting, "head") ? _seat_motion_head : !strcmp(setting, "blink") ? _seat_motion_blink :
			_seat_motion_still;
	}
	return motion;
}

static boolean seat_still(void)
{
	return seat_motion() != _seat_motion_head;
}

/* the eye's jumps blinked through, not glided */
static boolean seat_blinks(void)
{
	return seat_motion() == _seat_motion_blink;
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

static void feed_stage_set(short stage, real seconds)
{
	if (stage != vr_seat.feed_stage)
	{
		platform_log("vr seat: feed: %s (frame %ld)", feed_stage_names[stage], vr_seat.frame);
		vr_seat.feed_stage = stage;
	}
	vr_seat.feed_seconds = seconds;
}

/* how far a stage has gone (0 to 1) */
static real feed_progress(void)
{
	real length = feed_stage_seconds[vr_seat.feed_stage];

	return length > 0.0f ? PIN(vr_seat.feed_seconds / length, 0.0f, 1.0f) : 1.0f;
}

/* the display's timed stages moved on */
static void feed_stages(real seconds)
{
	vr_seat.feed_seconds += seconds;
	while ((vr_seat.feed_stage == _feed_grate || vr_seat.feed_stage == _feed_bars || vr_seat.feed_stage == _feed_wash) &&
		vr_seat.feed_seconds >= feed_stage_seconds[vr_seat.feed_stage])
	{
		feed_stage_set(vr_seat.feed_stage + 1, vr_seat.feed_seconds - feed_stage_seconds[vr_seat.feed_stage]);
	}
}

/* the eye riding toward a place in the vehicle (the head's path, the
cockpit's eye, where the player stands), from where it is (place, and its
velocity: in the vehicle, world units, a second): critically damped over
RIDE_SECONDS (no overshoot, no step); the comfort vignette while it moves
faster than walking */
static void ride_spring(real_point3d *place, real_vector3d *place_velocity, real_point3d const *target, real seconds)
{
	real omega = 2.0f / RIDE_SECONDS, x = omega * seconds;
	real decay = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
	real *position = &place->x, *velocity = &place_velocity->i;
	real const *goal = &target->x;
	real_point3d was = *place;
	real_vector3d step;
	real speed, moved;
	short axis;

	for (axis = 0; axis < 3; axis++)
	{
		real change = position[axis] - goal[axis];
		real temp = (velocity[axis] + omega * change) * seconds;

		velocity[axis] = (velocity[axis] - omega * temp) * decay;
		position[axis] = goal[axis] + (change + temp) * decay;
	}
	/* (no faster than RIDE_FASTEST: a jump of the body's becomes a glide) */
	vector_from_points3d(&was, place, &step);
	moved = (real)sqrt(magnitude_squared3d(&step));
	if (seconds > 0.0f && moved > RIDE_FASTEST / METRES_PER_WORLD_UNIT * seconds)
	{
		real scale = RIDE_FASTEST / METRES_PER_WORLD_UNIT * seconds / moved;

		place->x = was.x + step.i * scale;
		place->y = was.y + step.j * scale;
		place->z = was.z + step.k * scale;
		place_velocity->i = step.i * scale / seconds;
		place_velocity->j = step.j * scale / seconds;
		place_velocity->k = step.k * scale / seconds;
	}
	speed = (real)sqrt(magnitude_squared3d(place_velocity)) * METRES_PER_WORLD_UNIT;
	if (speed > RIDE_WALKING)
		halo_vr_comfort_vignette(MIN(RIDE_VIGNETTE, (speed - RIDE_WALKING) * 0.25f));
}

/* a feed cockpit's ride toward a place in the vehicle */
static void ride_toward(real_point3d const *target, real seconds)
{
	ride_spring(&vr_seat.ride_local, &vr_seat.ride_velocity, target, seconds);
}

/* what is left of a jump of the eye (its length, world units) after this
many seconds more of the glide: closed over GLIDE_SECONDS, no faster than
RIDE_FASTEST, none once within GLIDE_DONE */
static real glide_left(real length, real seconds)
{
	real left = MAX(length * (real)exp(-seconds / GLIDE_SECONDS), length - RIDE_FASTEST / METRES_PER_WORLD_UNIT * seconds);

	return left < GLIDE_DONE ? 0.0f : left;
}

/* a vector in a vehicle's axes (else NONE: the world's) and back */
static void glide_to_frame(long vehicle_index, real_vector3d const *world, real_vector3d *local)
{
	if (vehicle_index != NONE)
		matrix4x3_inverse_transform_vector(object_get_node_matrix(vehicle_index, 0), world, local);
	else
		*local = *world;
}

static void glide_from_frame(long vehicle_index, real_vector3d const *local, real_vector3d *world)
{
	if (vehicle_index != NONE)
		matrix4x3_transform_vector(object_get_node_matrix(vehicle_index, 0), local, world);
	else
		*world = *local;
}

/* the play space turned on toward the turn to make (eased over its
seconds) */
static void ride_turn(real seconds)
{
	real t, wanted;

	if (vr_seat.ride_turn == 0.0f)
		return;
	vr_seat.ride_seconds += seconds;
	t = vr_seat.ride_turn_seconds > 0.0f ? PIN(vr_seat.ride_seconds / vr_seat.ride_turn_seconds, 0.0f, 1.0f) : 1.0f;
	wanted = vr_seat.ride_turn * t * t * (3.0f - 2.0f * t);
	halo_vr_body_turn(wanted - vr_seat.ride_turned);
	vr_seat.ride_turned = wanted;
}

/* the turn (radians) from a yaw to another, the shorter way */
static real yaw_difference(real from, real to)
{
	real d = (real)fmod(to - from, 2.0 * 3.14159265);

	if (d > 3.14159265f)
		d -= 2.0f * 3.14159265f;
	if (d < -3.14159265f)
		d += 2.0f * 3.14159265f;
	return d;
}

/* each frame in a feed cockpit's seat (or riding out of it once the body is
out: unit_index NONE), from where the eye was, the head's place and the
cockpit's eye (in the world), the vehicle, and the seconds since the last
frame: the eye's place (eye) */
static void feed_ride(long unit_index, long vehicle_index, real_point3d const *was, real_point3d const *head,
	real_point3d const *cockpit_eye, real_point3d *eye, real seconds)
{
	real_point3d target, head_local, cockpit_local;
	short animation_state = NONE, remaining = 0;
	real w = 1.0f;

	/* (the frame's time, as the body's animation goes on: the path
	follows it at any rate of frames) */
	seconds = PIN(seconds, 0.0f, 0.1f);
	if (unit_index != NONE)
		remaining = unit_get_animation_frames_remaining(unit_index, &animation_state);
	if (vr_seat.feed_stage == _feed_none || vr_seat.feed_serial != vr_seat.seat_serial)
	{
		/* (getting in: the ride from where the eye was, and the play space's
		turn to face the hull's front as the body sits: over the climb, no
		faster than RIDE_TURN_MOST at the eased turn's fastest) */
		real_matrix4x3 const *hull = object_get_node_matrix(vehicle_index, 0);
		real climb = animation_state == _unit_state_entering_seat ? remaining / 30.0f : 0.0f;

		real_point3d climbed;

		vr_seat.feed_serial = vr_seat.seat_serial;
		vr_seat.feed_vehicle_index = vehicle_index;
		vr_seat.ride_direct = vr_climb_entering(&climbed);
		vehicle_to_local(vehicle_index, vr_seat.ride_direct ? &climbed : was, &vr_seat.ride_local);
		vr_seat.ride_velocity.i = vr_seat.ride_velocity.j = vr_seat.ride_velocity.k = 0.0f;
		vr_seat.ride_frames = MAX(remaining, 1);
		vr_seat.ride_turn = yaw_difference(vr_render_view_yaw(), (real)atan2(hull->forward.j, hull->forward.i));
		vr_seat.ride_turned = vr_seat.ride_seconds = 0.0f;
		vr_seat.ride_turn_seconds = MAX(climb, 1.5f * (real)fabs(vr_seat.ride_turn) / RIDE_TURN_MOST);
		feed_stage_set(_feed_riding_in, 0.0f);
		platform_log("vr seat: feed: the play space to turn %.0f degrees over %.2f s", vr_seat.ride_turn * DEGREES_PER_RADIAN,
			vr_seat.ride_turn_seconds);
	}
	vehicle_to_local(vehicle_index, cockpit_eye, &cockpit_local);
	if (unit_index != NONE && animation_state == _unit_state_entering_seat)
	{
		/* (in: the head's path, drawn to the cockpit's eye as the climb goes
		on) */
		real t = 1.0f - PIN((real)remaining / (real)vr_seat.ride_frames, 0.0f, 1.0f);

		w = vr_seat.ride_direct ? 1.0f : t * t * (3.0f - 2.0f * t);
	}
	else if (unit_index != NONE && animation_state == _unit_state_exiting_seat)
	{
		/* (out: from the cockpit's eye to the head's path as the climb goes
		on; the display collapsing) */
		real t;

		if (vr_seat.feed_stage < _feed_off)
		{
			vr_seat.ride_frames = MAX(remaining, 1);
			feed_stage_set(_feed_off, 0.0f);
		}
		t = 1.0f - PIN((real)remaining / (real)vr_seat.ride_frames, 0.0f, 1.0f);
		/* (out by the hands: the eye where they hold it, from the seat's) */
		w = vr_climb_exiting() ? 1.0f : 1.0f - t * t * (3.0f - 2.0f * t);
	}
	else if (unit_index == NONE)
	{
		w = 0.0f;
		if (vr_seat.feed_stage < _feed_riding_out)
			feed_stage_set(_feed_riding_out, 0.0f);
	}
	else if (vr_seat.feed_stage == _feed_riding_in)
	{
		/* (seated: the display wakes once the eye is in) */
		feed_stage_set(_feed_grate, 0.0f);
	}
	vehicle_to_local(vehicle_index, head, &head_local);
	target.x = head_local.x + (cockpit_local.x - head_local.x) * w;
	target.y = head_local.y + (cockpit_local.y - head_local.y) * w;
	target.z = head_local.z + (cockpit_local.z - head_local.z) * w;
	ride_toward(&target, seconds);
	ride_turn(seconds);
	feed_stages(seconds);
	vehicle_from_local(vehicle_index, &vr_seat.ride_local, eye);
	if (vr_seat.testing)
	{
		platform_log("vr seat: ride: frame %ld %s: head %.3f %.3f %.3f eye %.3f %.3f %.3f m (in the vehicle), %.2f m/s, "
			"turned %.1f of %.1f degrees", vr_seat.frame, feed_stage_names[vr_seat.feed_stage],
			head_local.x * METRES_PER_WORLD_UNIT, head_local.y * METRES_PER_WORLD_UNIT, head_local.z * METRES_PER_WORLD_UNIT,
			vr_seat.ride_local.x * METRES_PER_WORLD_UNIT, vr_seat.ride_local.y * METRES_PER_WORLD_UNIT,
			vr_seat.ride_local.z * METRES_PER_WORLD_UNIT,
			(real)sqrt(magnitude_squared3d(&vr_seat.ride_velocity)) * METRES_PER_WORLD_UNIT,
			vr_seat.ride_turned * DEGREES_PER_RADIAN, vr_seat.ride_turn * DEGREES_PER_RADIAN);
		/* (with debug.screenshot_directory: the eyes every third frame as
		it rides and the display wakes, for strips of them) */
		if (vr_seat.feed_stage != _feed_in && vr_seat.frame % 3 == 0)
			halo_vr_test_screenshot();
	}
}

/* whether the eye has ridden out to where the player stands (in the
vehicle: the place's) */
static boolean ride_arrived(long vehicle_index, real_point3d const *place)
{
	real_point3d local;

	vehicle_to_local(vehicle_index, place, &local);
	return distance3d(&local, &vr_seat.ride_local) < RIDE_CLOSE &&
		magnitude_squared3d(&vr_seat.ride_velocity) * METRES_PER_WORLD_UNIT * METRES_PER_WORLD_UNIT < 0.01f;
}

/* each VR frame (vr_render.c), the eye's place before the head's offset:
the game's camera, else in a vehicle's seat (seated_unit_index, with
vr.vehicle_view = "first_person") the seat's, and a glide (or a blink)
through any jump between the two. In: the game's camera; out: the eye.
aiming: the view is
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
		vr_seat.glide = _glide_none;
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
		else if (vr_cabin_feed(vr_cabin_seat_eye(seated_unit_index, &desired)))
		{
			/* (a feed cockpit: the eye rides the body in and out, the cockpit
			about it: feed_ride) */
			real_point3d head, cockpit_eye = desired, was;

			if (vr_seat.shown_valid)
				shown_position(&was);
			else
				was = *position;
			unit_get_head_position(seated_unit_index, &head);
			source = _seat_source_seated;
			vr_seat.cabin = vr_cabin_seat_eye(seated_unit_index, &cockpit_eye);
			feed_ride(seated_unit_index, vehicle_index, &was, &head, &cockpit_eye, &desired, seconds);
			vr_seat.feed_cabin = vr_seat.cabin;
			vr_seat.anchored = TRUE;
			vr_seat.shown_serial = vr_seat.seat_serial;
		}
		else if (!vr_seat.anchored && unit->unit.animation.state == _unit_state_entering_seat && !seat_blinks() &&
			!vr_climb_active())
		{
			/* getting in: the eye rides the body's head as it climbs in (the
			body not drawn: vr_render_hides_object), smoothed (the glide,
			below) */
			source = _seat_source_getting_in;
			unit_get_head_position(seated_unit_index, &desired);
		}
		else if (vr_seat.anchored && unit->unit.animation.state == _unit_state_exiting_seat && !seat_blinks() &&
			!vr_climb_active())
		{
			/* getting out (not by the hands, which hold the eye: vr_climb.c):
			the eye rides the body's head out, smoothed */
			source = _seat_source_getting_out;
			unit_get_head_position(seated_unit_index, &desired);
		}
		else if (!vr_seat.anchored && unit->unit.animation.state == _unit_state_entering_seat)
		{
			/* getting in (vr.seat_motion = "blink", or by the hands, which
			carry the eye from there: vr_climb.c): the eye stays where it was
			as the body climbs in, with the vehicle */
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
		glided or blinked to) */
		if (source != _seat_source_getting_in && source != _seat_source_getting_out &&
			vr_seat.feed_stage == _feed_none)
		{
			short cabin = vr_cabin_seat_eye(seated_unit_index, &desired);

			if (cabin != vr_seat.cabin)
			{
				vr_seat.cabin = cabin;
				vr_seat.anchored = FALSE;
				vr_seat.seat_serial++;
			}
		}
		if (source != _seat_source_seated || !seat_still() || !vr_cabin_feed(vr_seat.cabin))
			vr_seat.feed_stage = _feed_none;
	}
	else if (vr_seat.feed_stage != _feed_none && seat_still() && vr_seat.feed_vehicle_index != NONE &&
		object_try_and_get(vr_seat.feed_vehicle_index) && !ride_arrived(vr_seat.feed_vehicle_index, position) &&
		!vr_climb_exiting() && !vr_climb_active())
	{
		/* (out of a feed cockpit's seat: the eye rides on to where the
		player stands, from the body's head's path, the cockpit about it
		until it is out; then the first person's, with no jump) */
		real_point3d stand = *position;

		vehicle_index = vr_seat.feed_vehicle_index;
		feed_ride(NONE, vehicle_index, &stand, &stand, &stand, &desired, seconds);
		source = _seat_source_seated;
	}
	else
	{
		vr_seat.seat_vehicle_index = NONE;
		vr_seat.seat_index = NONE;
		vr_seat.anchored = FALSE;
		vr_seat.frozen = FALSE;
		vr_seat.cabin = NONE;
		vr_seat.feed_stage = _feed_none;
		vr_seat.feed_vehicle_index = NONE;
		source = director_get_perspective(0) == _director_perspective_first_person ?
			_seat_source_first_person : _seat_source_game;
	}

	/* the glide (vr.seat_motion = "still", "head"): as the body climbs in
	or out, the eye follows its head's path smoothed; where the eye would
	jump between places of the seat's and the first person's, it closes
	the jump from where it was, with no cut. Not a jump to or from the
	game's own camera (the third person's, the dead's), which is blinked;
	not in a feed cockpit, which rides itself (feed_ride); nor while the
	hands climb, which hold the eye themselves (vr_climb.c) */
	{
		boolean jumped = vr_seat.shown_valid && (source != vr_seat.source || vr_seat.seat_serial != vr_seat.shown_serial);
		boolean following = (source == _seat_source_getting_in || source == _seat_source_getting_out) &&
			vehicle_index != NONE;
		real_point3d from;

		if (seat_blinks() || !vr_seat.shown_valid || vr_seat.blinking || vr_climb_active() ||
			vr_seat.feed_stage != _feed_none || source == _seat_source_game || vr_seat.source == _seat_source_game ||
			(vr_seat.glide != _glide_none && vr_seat.glide_vehicle_index != NONE &&
			!object_try_and_get(vr_seat.glide_vehicle_index)))
		{
			vr_seat.glide = _glide_none;
		}
		else if (following)
		{
			if (vr_seat.glide != _glide_following || vr_seat.glide_vehicle_index != vehicle_index)
			{
				shown_position(&from);
				vr_seat.glide = _glide_following;
				vr_seat.glide_vehicle_index = vehicle_index;
				vehicle_to_local(vehicle_index, &from, &vr_seat.glide_local);
				vr_seat.glide_velocity.i = vr_seat.glide_velocity.j = vr_seat.glide_velocity.k = 0.0f;
				vr_seat.glides++;
				if (vr_seat.testing)
				{
					platform_log("vr seat test: frame %ld: glide %ld: the eye follows the body's head (source %d to %d)",
						vr_seat.frame, vr_seat.glides, vr_seat.source, source);
				}
			}
		}
		else if (vr_seat.glide == _glide_following || jumped)
		{
			real_vector3d left;

			shown_position(&from);
			vector_from_points3d(&desired, &from, &left);
			if (vr_seat.glide == _glide_following || vr_seat.glide == _glide_settling ||
				(real)sqrt(magnitude_squared3d(&left)) > GLIDE_DONE)
			{
				if (vr_seat.testing && vr_seat.glide == _glide_none)
				{
					platform_log("vr seat test: frame %ld: glide %ld, the eye from source %d to %d, %.3f m",
						vr_seat.frame, vr_seat.glides + 1, vr_seat.source, source,
						(real)sqrt(magnitude_squared3d(&left)) * METRES_PER_WORLD_UNIT);
				}
				if (vr_seat.glide == _glide_none)
					vr_seat.glides++;
				vr_seat.glide = _glide_settling;
				vr_seat.glide_vehicle_index = vehicle_index;
				glide_to_frame(vehicle_index, &left, &vr_seat.glide_offset);
			}
		}
	}

	/* a blink where the eye would jump (and does not glide): it darkens
	with the eye held where it was, moves in the dark, and lightens */
	if (vr_seat.shown_valid && !vr_seat.blinking && !vr_climb_active() && vr_seat.glide == _glide_none &&
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
	if (vr_seat.glide == _glide_following)
	{
		/* (along the head's path, in the vehicle) */
		real_point3d target_local;

		vehicle_to_local(vehicle_index, &desired, &target_local);
		ride_spring(&vr_seat.glide_local, &vr_seat.glide_velocity, &target_local, PIN(seconds, 0.0f, 0.1f));
		vehicle_from_local(vehicle_index, &vr_seat.glide_local, &desired);
	}
	else if (vr_seat.glide == _glide_settling)
	{
		/* (what is left of the jump closing, in the vehicle or the world) */
		real length = (real)sqrt(magnitude_squared3d(&vr_seat.glide_offset));
		real left = glide_left(length, PIN(seconds, 0.0f, 0.1f));

		if (left <= 0.0f || length <= 0.0f)
		{
			vr_seat.glide = _glide_none;
			if (vr_seat.testing)
				platform_log("vr seat test: frame %ld: glide %ld done", vr_seat.frame, vr_seat.glides);
		}
		else
		{
			real_vector3d offset;

			scale_vector3d(&vr_seat.glide_offset, left / length, &vr_seat.glide_offset);
			glide_from_frame(vr_seat.glide_vehicle_index, &vr_seat.glide_offset, &offset);
			point_from_line3d(&desired, &offset, 1.0f, &desired);
		}
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

/* whether the eye is in the seat now, once moved there (the body seated,
the eye gliding the rest of the way in; the blink in at its darkest, or
since): vr_vehicle.c turns the view with the vehicle from then */
boolean vr_seat_seated_shown(void)
{
	return vr_seat.shown_valid && vr_seat.source == _seat_source_seated && vr_seat.anchored;
}

/* vr_climb.c: the eye in the seat (in a feed cockpit, the display woken) */
boolean vr_seat_in(void)
{
	return vr_seat_seated_shown() && (vr_seat.feed_stage == _feed_none || vr_seat.feed_stage == _feed_in);
}

/* vr_climb.c: a feed cockpit's ride carries the eye (in, seated or out) */
boolean vr_seat_feed_riding(void)
{
	return vr_seat.feed_stage != _feed_none;
}

/* vr_cabin.c: the feed cockpit the eye rides out of once the body is out
of its seat (its cabin's index, and the vehicle), else NONE */
short vr_seat_feed_cabin(long *vehicle_index)
{
	if (vr_seat.feed_stage == _feed_none || vr_seat.feed_vehicle_index == NONE || !vr_seat_seated_shown())
		return NONE;
	*vehicle_index = vr_seat.feed_vehicle_index;
	return vr_seat.feed_cabin;
}

/* vr_cabin.c: in a feed cockpit (the eye riding in, seated, or riding
out), how far its grate has closed over the display's opening (0 open, 1
closed), how far the feed has washed over it (0 the world, 1 the feed), and
how far it has gone dark (0 on, 1 dark: as the body climbs out; as the eye
rides in, the world is seen through its opening as it is); FALSE out of
one */
boolean vr_seat_feed(real *grate, real *wash, real *off)
{
	short stage = vr_seat.feed_stage;
	real t;

	*grate = *wash = 0.0f;
	*off = 1.0f;
	if (stage == _feed_none || !vr_seat_seated_shown())
		return FALSE;
	t = feed_progress();
	if (stage == _feed_grate)
		*grate = t * t * (3.0f - 2.0f * t);
	else if (stage >= _feed_bars && stage <= _feed_in)
		*grate = 1.0f;
	if (stage == _feed_wash)
		*wash = t;
	else if (stage >= _feed_in)
		*wash = 1.0f;
	if (stage >= _feed_riding_in && stage <= _feed_in)
		*off = 0.0f;
	else if (stage == _feed_off)
		*off = t;
	return TRUE;
}

/* the player back from a death (vr_death.c): the next eye is the new
one's, at once, not blinked to from the death's (its cam's vantage, or
where the player died), which the blink held in view as it darkened while
the helmet's view switched on, out of the body; the death's own dark and
switching bring the new eye up */
void vr_seat_eye_cut(void)
{
	vr_seat.shown_valid = FALSE;
	vr_seat.blinking = FALSE;
	vr_seat.darkness = 0.0f;
	vr_seat.glide = _glide_none;
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

/* how dark the blink makes the view (0 to 1: always 0 with
vr.seat_motion = "still", but for a jump to or from the game's own
camera) */
real vr_seat_darkness(void)
{
	return vr_seat.darkness;
}

/* the eye pass, after the game's screen flash (render.c): the blink's
darkness over both eyes, all of each (the VR host's tint, as they are
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
	vr_seat.climb_nearer = 0.25f;
	if (sscanf(setting, "%31s %f %f %15s %f", vr_seat.seat_label, &vr_seat.enter_seconds, &vr_seat.seated_seconds,
		drive, &vr_seat.climb_nearer) >= 2)
	{
		vr_seat.testing = TRUE;
		vr_seat.drive = !strcmp(drive, "drive");
		vr_seat.climb = !strcmp(drive, "climb");
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
		/* (to climb: nearer it than the entrance, 25 cm (toward the Scorpion's
		track guard's edge) unless told, facing the vehicle's middle) */
		if (vr_seat.climb)
		{
			real_point3d const *middle = &object_get(vehicle_index)->object.position;
			real_vector3d toward;

			vector_from_points3d(&entrance, middle, &toward);
			toward.k = 0.0f;
			if (normalize3d(&toward) > 0.0f)
			{
				entrance.x += toward.i * vr_seat.climb_nearer / METRES_PER_WORLD_UNIT;
				entrance.y += toward.j * vr_seat.climb_nearer / METRES_PER_WORLD_UNIT;
			}
			halo_vr_test_face((float)atan2(middle->y - entrance.y, middle->x - entrance.x));
		}
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
	if (!vr_seat.entered && !vr_seat.climb && time >= (long)(vr_seat.enter_seconds * TICKS_PER_SECOND))
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
