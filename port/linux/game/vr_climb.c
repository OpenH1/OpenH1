/*
VR_CLIMB.C

The VR mode's climbing into and out of a vehicle with the hands (HALO_VR;
vr.vehicle_climb, port/linux/README.md "VR"), for vr_render.c, vr_seat.c,
vr_vehicle.c, render_objects.c and first_person_weapons.c.

In VR the motion that is comfortable is the hands': the hands pull and the
world follows them, felt as one's own. So a vehicle with holds (the table
below, a model's holds and its seats') may be climbed: a hand comes to a
hold (the vehicle lights up as the holsters' weapons do, and the controller
ticks), its grip takes it (a firmer tick), and while it holds, the eye moves
against the hand's motion, 1:1 in all three axes (vr.c's halo_vr_climb: both
hands, their mean; hand over hand), the weapon in hand not drawn. Let go of
both, the eye comes back down to the body gently, at walking pace. The eye's
move is the view's alone, as the head's in the room is: the player's unit
stays where it stood beside the vehicle (no farther than CLIMB_FARTHEST from
it), and only what X would do reaches the game. A seat is got into by one of
two rules:

- By its hatch (the Scorpion's): lowered by the rim (the eye drawn across
  toward the opening's middle as it comes down to its plane,
  CLIMB_FUNNEL_SPEED, so that a hand anywhere on the rim brings it in), the
  eye in the opening this far from the rim's plane (the seat's enter), the
  game's seat entry happens, as X's (unit_enter_seat). Out: from the seat,
  the rim glows; holding it, the eye rises as the hands pull down, and past
  the seat's exit above the rim the game's exit happens, as X's
  (unit_try_and_exit_seat).
- By a pull toward it (the Warthog's seats, the Ghost's saddle, a gun's):
  holding a hold of the seat's (the frame by a Warthog's seat, the Ghost's
  handlebars), within CLIMB_PULL_REACH of where the seated eye is, the eye
  is drawn toward it (CLIMB_FUNNEL_SPEED), and within the seat's enter of
  it, the game's seat entry happens. A gun's seat asks for both hands on its
  handles. Out: from the seat, holding a hold the seat offers (the Warthog's
  frame, its bed's sides), the eye moved past the seat's exit from where it
  sat, the game's exit happens. A seat whose handles steer it (the Ghost's
  bars) is got out of by pushing off them: both hands on them, the eye moves
  against them beyond CLIMB_PUSH_SLACK of their move, and the hands go on
  holding them, out.

Got in, the eye glides from where the hands had it to the seat's
(CLIMB_SEAT_SPEED; a feed cockpit's ride carries it instead: vr_seat.c),
and the play space turns to the vehicle's heading as it does (vr_vehicle.c,
eased, no faster than 45 degrees a second). Through each jump of the eye's
place (into a seat, out of one) while the hands climb, the eye is held
where it is (the offset taken again), so that there is no jump and no blink.
X keeps the hands-free ride for all. A vehicle with no holds in the table is
got into with X alone.
*/

#ifdef HALO_VR

#include "cseries.h"
#include "math/real_math.h"
#include "game/game.h"
#include "game/players.h"
#include "models/model_definitions.h"
#include "models/models.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "rasterizer/rasterizer_model_types.h"
#include "render/render.h"
#include "tag_files/tag_files.h"
#include "units/units.h"
#include "units/unit_definitions.h"

#include "../src/vr.h"

#include <math.h>
#include <string.h>

/* the platform layer's (port/linux/src/port_config.c) */
int config_boolean(char const *name);
char const *config_string(char const *name);
void platform_log(char const *format, ...);
/* vr_seat.c's */
boolean vr_seat_seated_shown(void);
boolean vr_seat_in(void);
boolean vr_seat_feed_riding(void);
/* vr_holster.c's */
void vr_holster_highlight_object(long unit_index, real distance, real units, struct render_model_effect *model_effect,
	struct render_lighting *lighting);

#define METRES_PER_WORLD_UNIT 3.048f
/* a vehicle climbed within this of the player's unit (world units: 6 m);
the eye no farther than this from the body (4 m); coming back down to the
body at this speed (world units a second: walking); a hand this near a hold
lights the vehicle up (metres) */
#define CLIMB_RANGE (6.0f / METRES_PER_WORLD_UNIT)
#define CLIMB_FARTHEST (4.0f / METRES_PER_WORLD_UNIT)
#define CLIMB_RETURN_SPEED (1.4f / METRES_PER_WORLD_UNIT)
#define CLIMB_GLOW 0.2f
/* let go this high above the body (world units: 50 cm), the eye comes back
across first and then down */
#define CLIMB_HIGH (0.5f / METRES_PER_WORLD_UNIT)
/* lowered by a hatch's rim at or below its plane, or pulled to a seat within
CLIMB_PULL_REACH of its eye, the eye drawn toward it no faster than this
(world units a second: 1 m/s) */
#define CLIMB_FUNNEL_SPEED (1.0f / METRES_PER_WORLD_UNIT)
#define CLIMB_PULL_REACH (0.6f / METRES_PER_WORLD_UNIT)
/* a jump of the eye's place (world units: 10 cm), held through */
#define CLIMB_JUMP (0.10f / METRES_PER_WORLD_UNIT)
/* got into a seat, the eye's glide to the seat's: no faster than this, slower
as it comes within this time of it, and no slower than the last (world units
a second, seconds) */
#define CLIMB_SEAT_SPEED (0.8f / METRES_PER_WORLD_UNIT)
#define CLIMB_SEAT_EASE 0.25f
#define CLIMB_SEAT_SLOWEST (0.1f / METRES_PER_WORLD_UNIT)
/* pushing off handles that steer: the hands' move that is theirs alone, before
the eye moves against it (metres) */
#define CLIMB_PUSH_SLACK 0.10f

/* a hold: a line on the vehicle (its ends, in its model's space, or in a
marker's axes from the marker: world units forward, left, up), how far about
it a hand takes it (world units), when it is offered (CLIMB_HOLD_*), and the
seat of the vehicle's (its table's) it is for */
#define CLIMB_HOLD_FOOT 1 /* on foot */
#define CLIMB_HOLD_SEATED 2 /* from its seat: out by it */
#define CLIMB_HOLD_ENTRY 4 /* on foot, held, its seat drawn to and got into */
#define CLIMB_HOLD_HANDLE 8 /* one of its seat's handles */

struct vr_climb_hold
{
	real ends[2][3];
	real radius;
	short flags;
	short seat;
	char const *marker;
};

/* a seat: its label's text, how it is got into (_climb_enter_*), where the
seated eye is (in the model's space, or in a marker's axes from the marker),
a hatch's opening (forward and left, from and to) and its rim's height, how
near the seat it is got into (a hatch: the eye this far above the rim's plane,
below it less than 0; a pull: this near the seated eye) and how far it is got
out (a hatch: this far above the rim; else this far from where it sat; world
units), whether both hands on its handles get it, and whether it is got out
of by pushing off its handles (they steer, seated) */
enum
{
	_climb_enter_hatch,
	_climb_enter_pull,
};

struct vr_climb_seat
{
	char const *label;
	short rule;
	char const *marker;
	real eye[3];
	real opening[4];
	real rim;
	real enter;
	real exit;
	boolean handles;
	boolean push;
};

/* a vehicle's: its model's tag name, its seats and its holds */
struct vr_climb_vehicle
{
	char const *model_name;
	struct vr_climb_seat const *seats;
	short seat_count;
	struct vr_climb_hold const *holds;
	short hold_count;
};

/* the Scorpion (6.8 m across its track guards): each side's track guard's
outer edge (1.6 m up), a tie-down rail along the middle of its top, a rail
at the foot of the hull's side and the hull's top edge (each a long reach
from the last, 80 to 90 cm), and the rim of the hatch over the driver's feed
cockpit (vr_cabin.c: its eye at forward 0.465, left 0.2), a square 75 cm
across; in 30 cm below the rim, out 30 cm above it */
static struct vr_climb_seat const vr_climb_scorpion_seats[] =
{
	{ "scorpion-driver", _climb_enter_hatch, NULL, { 0.465f, 0.200f, 0.750f }, { 0.345f, 0.080f, 0.585f, 0.320f },
		0.750f, -0.30f / METRES_PER_WORLD_UNIT, 0.30f / METRES_PER_WORLD_UNIT, FALSE, FALSE },
};

static struct vr_climb_hold const vr_climb_scorpion_holds[] =
{
	{ { { -0.80f, 1.12f, 0.52f }, { 1.20f, 1.03f, 0.52f } }, 0.05f, CLIMB_HOLD_FOOT, NONE, NULL },
	{ { { -0.80f, -1.12f, 0.52f }, { 1.20f, -1.03f, 0.52f } }, 0.05f, CLIMB_HOLD_FOOT, NONE, NULL },
	{ { { -0.60f, 0.86f, 0.53f }, { 1.10f, 0.82f, 0.53f } }, 0.05f, CLIMB_HOLD_FOOT, NONE, NULL },
	{ { { -0.60f, -0.86f, 0.53f }, { 1.10f, -0.82f, 0.53f } }, 0.05f, CLIMB_HOLD_FOOT, NONE, NULL },
	{ { { -0.40f, 0.60f, 0.57f }, { 1.00f, 0.60f, 0.57f } }, 0.05f, CLIMB_HOLD_FOOT, NONE, NULL },
	{ { { -0.40f, -0.60f, 0.57f }, { 1.00f, -0.60f, 0.57f } }, 0.05f, CLIMB_HOLD_FOOT, NONE, NULL },
	{ { { -0.40f, 0.56f, 0.74f }, { 1.00f, 0.56f, 0.70f } }, 0.05f, CLIMB_HOLD_FOOT, NONE, NULL },
	{ { { -0.40f, -0.56f, 0.74f }, { 1.00f, -0.56f, 0.70f } }, 0.05f, CLIMB_HOLD_FOOT, NONE, NULL },
	{ { { 0.345f, 0.080f, 0.750f }, { 0.585f, 0.080f, 0.750f } }, 0.04f,
		CLIMB_HOLD_FOOT | CLIMB_HOLD_ENTRY | CLIMB_HOLD_SEATED, 0, NULL },
	{ { { 0.585f, 0.080f, 0.750f }, { 0.585f, 0.320f, 0.750f } }, 0.04f,
		CLIMB_HOLD_FOOT | CLIMB_HOLD_ENTRY | CLIMB_HOLD_SEATED, 0, NULL },
	{ { { 0.585f, 0.320f, 0.750f }, { 0.345f, 0.320f, 0.750f } }, 0.04f,
		CLIMB_HOLD_FOOT | CLIMB_HOLD_ENTRY | CLIMB_HOLD_SEATED, 0, NULL },
	{ { { 0.345f, 0.320f, 0.750f }, { 0.345f, 0.080f, 0.750f } }, 0.04f,
		CLIMB_HOLD_FOOT | CLIMB_HOLD_ENTRY | CLIMB_HOLD_SEATED, 0, NULL },
};

/* the Warthog (and the multiplayer maps' warthog, the same model; 1.4 m from
the ground to the top of its sides): by the driver's seat and the
passenger's, the windscreen's corner post, the side's top edge and the seat's
back, pulled in by; the bed's sides and its tailgate, climbed into it by
(the gunner's eye 3.2 m up), and the chaingun's two handles (its markers,
which turn and pitch with it), both held the gunner's seat. Out: the post
and the side's edge, the bed's sides and tailgate, pushed down on */
static struct vr_climb_seat const vr_climb_warthog_seats[] =
{
	{ "W-driver", _climb_enter_pull, NULL, { -0.071f, 0.154f, 0.628f }, { 0.0f }, 0.0f, 0.25f / METRES_PER_WORLD_UNIT,
		0.35f / METRES_PER_WORLD_UNIT, FALSE, FALSE },
	{ "W-passenger", _climb_enter_pull, NULL, { -0.002f, -0.160f, 0.784f }, { 0.0f }, 0.0f,
		0.25f / METRES_PER_WORLD_UNIT, 0.35f / METRES_PER_WORLD_UNIT, FALSE, FALSE },
	{ "W-gunner", _climb_enter_pull, "gunner", { 0.080f, -0.030f, 0.291f }, { 0.0f }, 0.0f,
		0.30f / METRES_PER_WORLD_UNIT, 0.35f / METRES_PER_WORLD_UNIT, TRUE, FALSE },
};

#define CLIMB_HOLD_BY_SEAT (CLIMB_HOLD_FOOT | CLIMB_HOLD_ENTRY | CLIMB_HOLD_SEATED)
static struct vr_climb_hold const vr_climb_warthog_holds[] =
{
	{ { { 0.235f, 0.350f, 0.530f }, { 0.220f, 0.260f, 0.710f } }, 0.04f, CLIMB_HOLD_BY_SEAT, 0, NULL },
	{ { { 0.150f, 0.410f, 0.470f }, { -0.220f, 0.410f, 0.520f } }, 0.04f, CLIMB_HOLD_BY_SEAT, 0, NULL },
	{ { { -0.180f, 0.280f, 0.720f }, { -0.180f, 0.170f, 0.720f } }, 0.04f, CLIMB_HOLD_FOOT | CLIMB_HOLD_ENTRY, 0, NULL },
	{ { { 0.235f, -0.350f, 0.530f }, { 0.220f, -0.260f, 0.710f } }, 0.04f, CLIMB_HOLD_BY_SEAT, 1, NULL },
	{ { { 0.150f, -0.410f, 0.470f }, { -0.220f, -0.410f, 0.520f } }, 0.04f, CLIMB_HOLD_BY_SEAT, 1, NULL },
	{ { { -0.180f, -0.280f, 0.720f }, { -0.180f, -0.170f, 0.720f } }, 0.04f, CLIMB_HOLD_FOOT | CLIMB_HOLD_ENTRY, 1,
		NULL },
	{ { { -0.900f, 0.220f, 0.585f }, { -0.900f, -0.220f, 0.585f } }, 0.05f, CLIMB_HOLD_FOOT | CLIMB_HOLD_SEATED, 2,
		NULL },
	{ { { -0.450f, 0.290f, 0.620f }, { -0.800f, 0.290f, 0.570f } }, 0.05f, CLIMB_HOLD_FOOT | CLIMB_HOLD_SEATED, 2,
		NULL },
	{ { { -0.450f, -0.290f, 0.620f }, { -0.800f, -0.290f, 0.570f } }, 0.05f, CLIMB_HOLD_FOOT | CLIMB_HOLD_SEATED, 2,
		NULL },
	{ { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } }, 0.05f, CLIMB_HOLD_FOOT | CLIMB_HOLD_HANDLE, 2,
		"gunner left hand" },
	{ { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } }, 0.05f, CLIMB_HOLD_FOOT | CLIMB_HOLD_HANDLE, 2,
		"gunner right hand" },
};

/* the Ghost: its handlebars (the Elite driver's hands' markers, where the
Chief's hands are too as he drives) and the cowl's back and side edges above
them (the bars are low: 85 cm from the ground), pulled to into the saddle;
out, pushed off the bars (they steer, seated) */
static struct vr_climb_seat const vr_climb_ghost_seats[] =
{
	{ "G-driver", _climb_enter_pull, NULL, { -0.086f, 0.0f, 0.362f }, { 0.0f }, 0.0f, 0.25f / METRES_PER_WORLD_UNIT,
		0.25f / METRES_PER_WORLD_UNIT, FALSE, TRUE },
};

static struct vr_climb_hold const vr_climb_ghost_holds[] =
{
	{ { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } }, 0.05f, CLIMB_HOLD_FOOT | CLIMB_HOLD_ENTRY | CLIMB_HOLD_HANDLE, 0,
		"left hand elite" },
	{ { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } }, 0.05f, CLIMB_HOLD_FOOT | CLIMB_HOLD_ENTRY | CLIMB_HOLD_HANDLE, 0,
		"right hand elite" },
	{ { { 0.070f, 0.120f, 0.405f }, { 0.070f, -0.120f, 0.405f } }, 0.04f, CLIMB_HOLD_FOOT | CLIMB_HOLD_ENTRY, 0, NULL },
	{ { { 0.120f, 0.170f, 0.370f }, { 0.450f, 0.180f, 0.360f } }, 0.04f, CLIMB_HOLD_FOOT | CLIMB_HOLD_ENTRY, 0, NULL },
	{ { { 0.120f, -0.170f, 0.370f }, { 0.450f, -0.180f, 0.360f } }, 0.04f, CLIMB_HOLD_FOOT | CLIMB_HOLD_ENTRY, 0, NULL },
};

static struct vr_climb_vehicle const vr_climb_vehicles[] =
{
	{ "vehicles\\scorpion\\scorpion", vr_climb_scorpion_seats, NUMBEROF(vr_climb_scorpion_seats),
		vr_climb_scorpion_holds, NUMBEROF(vr_climb_scorpion_holds) },
	{ "vehicles\\warthog\\warthog", vr_climb_warthog_seats, NUMBEROF(vr_climb_warthog_seats),
		vr_climb_warthog_holds, NUMBEROF(vr_climb_warthog_holds) },
	{ "vehicles\\ghost\\ghost", vr_climb_ghost_seats, NUMBEROF(vr_climb_ghost_seats),
		vr_climb_ghost_holds, NUMBEROF(vr_climb_ghost_holds) },
};

static struct
{
	/* the vehicle climbed (else NONE) and its table's entry; the eye's
	offset from its place (the world's), the eye's place and where it was
	shown last frame; how many hands hold, the nearest hand's distance from
	a hold (metres beyond its reach); the seat got into or out of by the
	hands (the eye's place there given to vr_seat.c), and out of one, the
	hands not yet let go (none got into again until they are) */
	long vehicle_index;
	short entry;
	boolean valid;
	real_vector3d offset;
	real_point3d last_base;
	real_point3d last_eye;
	short holding;
	real nearest;
	boolean entering;
	real_point3d entering_eye;
	boolean exiting;
	boolean left;
	/* got into a seat, the eye gliding to the seat's: where it is, in the
	vehicle (its root node's frame) */
	boolean seating;
	real_point3d seat_local;
	/* pushing off a seat's handles: where the hands' middle was as they
	took them (metres, the game's axes from the recentred head) */
	boolean pushing;
	float push_from[3];
	/* a hand whose grip is held takes a hold within reach (the handles
	pushed off, held on as the seat is left) */
	boolean grab;
} vr_climb = { NONE, NONE };

static boolean vr_climb_enabled(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static boolean enabled;
	unsigned long config_changes(void);

	if (read_at != config_changes())
	{
		read_at = config_changes();
		enabled = config_boolean("vr.vehicle_climb");
	}
	return enabled;
}

/* the table's entry for this vehicle, else NONE */
static short vr_climb_entry(long vehicle_index)
{
	long model_index;
	char const *name;
	short index;

	if (vehicle_index == NONE)
		return NONE;
	model_index = object_definition_get(object_get(vehicle_index)->definition_index)->object.model.index;
	name = model_index != NONE ? tag_get_name(model_index) : NULL;
	for (index = 0; name && index < (short)NUMBEROF(vr_climb_vehicles); index++)
	{
		if (!strcmp(name, vr_climb_vehicles[index].model_name))
			return index;
	}
	return NONE;
}

/* the nearest vehicle with holds within CLIMB_RANGE of the unit, else NONE */
static long vr_climb_nearest_vehicle(long unit_index, short *entry)
{
	real_point3d const *position = &object_get(unit_index)->object.position;
	struct object_iterator vehicles;
	long index, nearest = NONE;
	real best = CLIMB_RANGE * CLIMB_RANGE;

	object_iterator_new(&vehicles, _object_mask_vehicle, 0);
	while (object_iterator_next(&vehicles))
	{
		short found = vr_climb_entry(index = vehicles.index);
		real d;

		if (found == NONE)
			continue;
		d = distance_squared3d(position, &object_get(index)->object.position);
		if (d < best)
		{
			best = d;
			nearest = index;
			*entry = found;
		}
	}
	return nearest;
}

/* the vehicle's model's space as its root node places it (as render_model
skins its vertices) */
static void vr_climb_model_space(long vehicle_index, real_matrix4x3 *skinning)
{
	long model_index = object_definition_get(object_get(vehicle_index)->definition_index)->object.model.index;
	struct model *model = model_definition_get(model_index);

	matrix4x3_multiply(object_get_node_matrix(vehicle_index, 0),
		&TAG_BLOCK_GET_ELEMENT(&model->nodes, 0, struct model_node)->runtime_default_inverse_matrix, skinning);
}

/* a point of the vehicle's (in its model's space, or in a marker's axes from
the marker) in the world; FALSE, the vehicle has no such marker */
static boolean vr_climb_point(long vehicle_index, real_matrix4x3 const *skinning, char const *marker,
	real const point[3], real_point3d *world)
{
	real_point3d local;

	local.x = point[0];
	local.y = point[1];
	local.z = point[2];
	if (marker)
	{
		struct object_marker found;

		if (object_get_marker_by_name(vehicle_index, marker, &found, 1) < 1)
			return FALSE;
		matrix4x3_transform_point(&found.matrix, &local, world);
	}
	else
	{
		matrix4x3_transform_point(skinning, &local, world);
	}
	return TRUE;
}

/* the vehicle's seat whose label has the text, else NONE */
static short vr_climb_seat(long vehicle_index, char const *label)
{
	struct unit_definition *definition = unit_definition_get(object_get(vehicle_index)->definition_index);
	short index;

	for (index = 0; index < definition->unit.seats.count; index++)
	{
		if (strstr(TAG_BLOCK_GET_ELEMENT(&definition->unit.seats, index, struct unit_seat)->label, label))
			return index;
	}
	return NONE;
}

/* the table's seat (of the vehicle's entry) that is the vehicle's seat,
else NONE */
static short vr_climb_table_seat(long vehicle_index, short entry, short seat_index)
{
	short index;

	for (index = 0; entry != NONE && seat_index != NONE && index < vr_climb_vehicles[entry].seat_count; index++)
	{
		if (vr_climb_seat(vehicle_index, vr_climb_vehicles[entry].seats[index].label) == seat_index)
			return index;
	}
	return NONE;
}

/* a direction turned by a yaw */
static void yaw_turn(real yaw, real x, real y, real *out_x, real *out_y)
{
	real c = (real)cos(yaw), s = (real)sin(yaw);

	*out_x = x * c - y * s;
	*out_y = x * s + y * c;
}

static void vr_climb_reset(void)
{
	vr_climb.valid = FALSE;
	vr_climb.offset.i = vr_climb.offset.j = vr_climb.offset.k = 0.0f;
	vr_climb.holding = 0;
	vr_climb.nearest = 1e9f;
	vr_climb.vehicle_index = NONE;
	vr_climb.entering = vr_climb.exiting = vr_climb.left = vr_climb.seating = vr_climb.pushing = FALSE;
	vr_climb.grab = FALSE;
}

/* the hold the hand holds (the told holds' index, else less than 0) is the
seat's entry's: a handle of its, else one with CLIMB_HOLD_ENTRY */
static boolean vr_climb_holds_entry(struct vr_climb_vehicle const *table, short const *told, int held, short seat,
	boolean handle)
{
	struct vr_climb_hold const *hold;

	if (held < 0)
		return FALSE;
	hold = &table->holds[told[held]];
	return hold->seat == seat && (hold->flags & (handle ? CLIMB_HOLD_HANDLE : CLIMB_HOLD_ENTRY));
}

/* vr_render.c, each VR frame, after the seat's eye (vr_seat.c): the eye
(its place before the head's offset, in the world: in, the seat's or the
game's; out, the climbing eye's), the first player's unit seated (else
NONE), the view's yaw (base_yaw) and the world's units a metre; aiming: the
view is the player's */
void vr_climb_eye(long seated_unit_index, boolean aiming, real base_yaw, real units, real seconds, real_point3d *eye)
{
	struct halo_vr_climb climb;
	long player_index = local_player_get_player_index(0);
	long unit_index, vehicle_index = NONE;
	struct vr_climb_vehicle const *table;
	real_matrix4x3 skinning;
	real_point3d base = *eye, origin, local;
	short entry = NONE, seat = NONE, index, told[HALO_VR_CLIMB_HOLDS];
	boolean seated_in = FALSE, testing = strstr(config_string("debug.vr_test_seat"), "climb") != NULL;
	real moved_x, moved_y, was_farthest;

	if (!vr_climb_enabled() || !aiming || player_index == NONE || units <= 0.0f ||
		(unit_index = player_get(player_index)->unit_index) == NONE ||
		TEST_FLAG(object_get(unit_index)->object.damage_flags, _object_dead_bit))
	{
		vr_climb_reset();
		return;
	}
	seconds = PIN(seconds, 0.0f, 0.1f);
	/* the vehicle: the one the unit is in (seated, the seat's holds from
	inside), else the nearest with holds */
	if (seated_unit_index != NONE)
	{
		struct unit_datum *unit = unit_get(seated_unit_index);

		vehicle_index = unit->object.parent_object_index;
		entry = vr_climb_entry(vehicle_index);
		seat = vr_climb_table_seat(vehicle_index, entry, unit->unit.parent_seat_index);
		seated_in = seat != NONE && unit->unit.animation.state != _unit_state_entering_seat && !vr_climb.seating &&
			(vr_seat_in() || vr_climb.exiting);
		if (entry != NONE && unit->unit.animation.state == _unit_state_entering_seat && vr_seat_feed_riding())
		{
			/* (getting into a feed cockpit: its ride carries the eye on
			from where it was: vr_seat.c) */
			vr_climb.offset.i = vr_climb.offset.j = vr_climb.offset.k = 0.0f;
			vr_climb.holding = 0;
			vr_climb.seating = FALSE;
		}
	}
	else if (object_get(unit_index)->object.parent_object_index == NONE)
	{
		vehicle_index = vr_climb_nearest_vehicle(unit_index, &entry);
	}
	if (vehicle_index != vr_climb.vehicle_index)
	{
		vr_climb.vehicle_index = vehicle_index;
		vr_climb.entry = entry;
	}
	table = entry != NONE ? &vr_climb_vehicles[entry] : NULL;
	if (vr_climb.seating && (seated_unit_index == NONE || seat == NONE))
		vr_climb.seating = FALSE;
	/* (a jump of the eye's place, into a seat or out of one, held through
	while the offset is in use: no jump, no blink) */
	if (vr_climb.valid && !vr_climb.seating && !vr_climb.entering &&
		(vr_climb.holding || magnitude_squared3d(&vr_climb.offset) > 0.0f || vr_climb.exiting) &&
		distance3d(&base, &vr_climb.last_base) > CLIMB_JUMP)
	{
		vector_from_points3d(&base, &vr_climb.last_eye, &vr_climb.offset);
		platform_log("vr: climb: the eye's place jumped %.2f m; held where it was",
			distance3d(&base, &vr_climb.last_base) / units);
	}
	if (vr_climb.entering && seated_unit_index != NONE)
		vr_climb.entering = FALSE;
	if (vr_climb.exiting && seated_unit_index == NONE)
		vr_climb.exiting = FALSE;
	if (vehicle_index != NONE && table)
		vr_climb_model_space(vehicle_index, &skinning);

	/* the holds told to vr.c, as the eye is placed now (the recentred
	head's place in the world: the eye with its offset): on foot, those
	offered on foot; seated, the seat's (and pushed off its handles, as the
	seat is left, those); none as the eye glides into a seat */
	csmemset(&climb, 0, sizeof(climb));
	origin.x = base.x + vr_climb.offset.i;
	origin.y = base.y + vr_climb.offset.j;
	origin.z = base.z + vr_climb.offset.k;
	if (table && vehicle_index != NONE && !vr_climb.seating && (seated_unit_index == NONE || seated_in))
	{
		for (index = 0; index < table->hold_count && climb.count < HALO_VR_CLIMB_HOLDS; index++)
		{
			struct vr_climb_hold const *hold = &table->holds[index];
			short end;

			if (seated_in ? hold->seat != seat || !(hold->flags & CLIMB_HOLD_SEATED) &&
				!(vr_climb.exiting && table->seats[seat].push && (hold->flags & CLIMB_HOLD_HANDLE)) :
				!(hold->flags & CLIMB_HOLD_FOOT))
			{
				continue;
			}
			for (end = 0; end < 2; end++)
			{
				real_point3d world;
				real x, y;

				if (!vr_climb_point(vehicle_index, &skinning, hold->marker, hold->ends[end], &world))
					break;
				yaw_turn(-base_yaw, (world.x - origin.x) / units, (world.y - origin.y) / units, &x, &y);
				climb.ends[climb.count][end][0] = (float)x;
				climb.ends[climb.count][end][1] = (float)y;
				climb.ends[climb.count][end][2] = (float)((world.z - origin.z) / units);
			}
			if (end < 2)
				continue;
			climb.radius[climb.count] = (float)(hold->radius * METRES_PER_WORLD_UNIT);
			told[climb.count] = index;
			climb.count++;
		}
	}
	climb.grab = vr_climb.grab;
	vr_climb.holding = (short)halo_vr_climb(&climb);
	if (vr_climb.holding)
		vr_climb.grab = FALSE;
	if (!vr_climb.holding && !vr_climb.exiting)
		vr_climb.left = FALSE;
	/* (debug.vr_test_seat "... climb": the holds where they are from the
	recentred head, and the vehicle's model's space (for the test's hands
	to be planned), once a second, and the eye's offset, each frame; with
	debug.screenshot_directory, the eyes every twelfth frame a hand holds
	or the eye glides into a seat) */
	if (testing)
	{
		static long logged_tick = -1000;

		if (game_time_get() >= logged_tick + TICKS_PER_SECOND || game_time_get() < logged_tick)
		{
			logged_tick = game_time_get();
			for (index = 0; index < climb.count; index++)
			{
				platform_log("vr: climb: hold %d: %.3f %.3f %.3f to %.3f %.3f %.3f (forward, left, up from the "
					"recentred head, m), reach %.2f", index, climb.ends[index][0][0], climb.ends[index][0][1],
					climb.ends[index][0][2], climb.ends[index][1][0], climb.ends[index][1][1], climb.ends[index][1][2],
					climb.radius[index]);
			}
			if (table && vehicle_index != NONE)
			{
				real_point3d const *body = &object_get(unit_index)->object.position;

				platform_log("vr: climb: frame: seated %d (table seat %d), yaw %.5f, base %.5f %.5f %.5f, offset %.5f "
					"%.5f %.5f, body %.5f %.5f %.5f, model at %.5f %.5f %.5f forward %.5f %.5f %.5f left %.5f %.5f %.5f "
					"up %.5f %.5f %.5f scale %.5f (world units)", seated_unit_index != NONE, seat, base_yaw, base.x,
					base.y, base.z, vr_climb.offset.i, vr_climb.offset.j, vr_climb.offset.k, body->x, body->y, body->z,
					skinning.position.x, skinning.position.y, skinning.position.z, skinning.forward.i,
					skinning.forward.j, skinning.forward.k, skinning.left.i, skinning.left.j, skinning.left.k,
					skinning.up.i, skinning.up.j, skinning.up.k, skinning.scale);
				if (seat != NONE)
				{
					real_matrix4x3 inverse;
					real_point3d in_model, in_marker = { 0.0f, 0.0f, 0.0f };
					struct object_marker found;
					char const *marker = table->seats[seat].marker;

					matrix4x3_inverse(&skinning, &inverse);
					matrix4x3_transform_point(&inverse, &base, &in_model);
					if (marker && object_get_marker_by_name(vehicle_index, marker, &found, 1) == 1)
						matrix4x3_inverse_transform_point(&found.matrix, &base, &in_marker);
					platform_log("vr: climb: frame: the eye's place in the model %.4f %.4f %.4f, from its seat's marker "
						"%.4f %.4f %.4f (world units)", in_model.x, in_model.y, in_model.z, in_marker.x, in_marker.y,
						in_marker.z);
				}
			}
		}
		{
			void halo_vr_test_screenshot(void);
			static long shot;

			if ((vr_climb.holding || vr_climb.seating) && (++shot % 12) == 0)
				halo_vr_test_screenshot();
		}
		platform_log("vr: climb: tick %ld (frame %lu): %d hands hold (%d %d), nearest %.2f %.2f m, eye offset %.3f %.3f %.3f m, "
			"hands %.2f %.2f %.2f and %.2f %.2f %.2f%s%s%s",
			game_time_get(), climb.frame, vr_climb.holding, climb.held[0], climb.held[1], climb.distance[0], climb.distance[1],
			vr_climb.offset.i / units, vr_climb.offset.j / units, vr_climb.offset.k / units, climb.hand[0][0],
			climb.hand[0][1], climb.hand[0][2], climb.hand[1][0], climb.hand[1][1], climb.hand[1][2],
			vr_climb.seating ? ", seating" : "", climb.seat_held ? ", on the seat's handles" : "",
			vr_climb.pushing ? ", pushing" : "");
	}
	vr_climb.nearest = MIN(climb.distance[0], climb.distance[1]);

	was_farthest = MAX(CLIMB_FARTHEST, (real)sqrt(magnitude_squared3d(&vr_climb.offset)));
	if (vr_climb.seating)
	{
		/* (got into a seat by the hands: the eye glides from where they had
		it to the seat's, as the game has it once the eye is in the seat,
		in the vehicle) */
		real_matrix4x3 const *root = object_get_node_matrix(vehicle_index, 0);
		struct vr_climb_seat const *seated = &table->seats[seat];
		real_point3d at, target;
		real d, speed, step;

		matrix4x3_transform_point(root, &vr_climb.seat_local, &at);
		if (vr_seat_seated_shown() || !vr_climb_point(vehicle_index, &skinning, seated->marker, seated->eye, &target))
			target = base;
		d = distance3d(&at, &target);
		speed = MAX(MIN(CLIMB_SEAT_SPEED, d / CLIMB_SEAT_EASE), CLIMB_SEAT_SLOWEST);
		step = MIN(speed * seconds, d);
		if (d > 0.0f)
		{
			at.x += (target.x - at.x) * step / d;
			at.y += (target.y - at.y) * step / d;
			at.z += (target.z - at.z) * step / d;
		}
		matrix4x3_inverse_transform_point(root, &at, &vr_climb.seat_local);
		vector_from_points3d(&base, &at, &vr_climb.offset);
		if (vr_seat_seated_shown() && step >= d)
		{
			vr_climb.seating = FALSE;
			vr_climb.offset.i = vr_climb.offset.j = vr_climb.offset.k = 0.0f;
			platform_log("vr: climb: the eye in %s's seat", tag_get_name(object_get(vehicle_index)->definition_index));
		}
	}
	else if (seated_in && table->seats[seat].push && climb.seat_held && !vr_climb.holding && !vr_climb.exiting)
	{
		/* (on handles that steer: pushed off them, beyond their slack, the eye
		moves against them, as it does against holds) */
		float mean[3], d[3], length;
		real beyond, x, y;
		short axis;

		for (axis = 0; axis < 3; axis++)
			mean[axis] = (climb.hand[0][axis] + climb.hand[1][axis]) * 0.5f;
		if (!vr_climb.pushing)
		{
			vr_climb.pushing = TRUE;
			memcpy(vr_climb.push_from, mean, sizeof(mean));
		}
		for (axis = 0; axis < 3; axis++)
			d[axis] = mean[axis] - vr_climb.push_from[axis];
		length = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		beyond = length > CLIMB_PUSH_SLACK ? (length - CLIMB_PUSH_SLACK) / length : 0.0f;
		yaw_turn(base_yaw, -d[0] * beyond, -d[1] * beyond, &x, &y);
		vr_climb.offset.i = x * units;
		vr_climb.offset.j = y * units;
		vr_climb.offset.k = -d[2] * beyond * units;
	}
	else
	{
		/* the eye moved by the hands (against them), or back down to the
		body at walking pace as they let go (seated, straight back to the
		seat's place) */
		vr_climb.pushing = FALSE;
		yaw_turn(base_yaw, climb.moved[0], climb.moved[1], &moved_x, &moved_y);
		vr_climb.offset.i += moved_x * units;
		vr_climb.offset.j += moved_y * units;
		vr_climb.offset.k += climb.moved[2] * units;
		if (!vr_climb.holding && !(vr_climb.exiting && seated_unit_index != NONE))
		{
			/* (high, across first and then down: off the vehicle beside it,
			not through it) */
			real across = (real)sqrt(vr_climb.offset.i * vr_climb.offset.i + vr_climb.offset.j * vr_climb.offset.j);
			real step = CLIMB_RETURN_SPEED * seconds;
			boolean high = vr_climb.offset.k > CLIMB_HIGH && seated_unit_index == NONE;
			real length = (real)sqrt(magnitude_squared3d(&vr_climb.offset));

			if (seated_unit_index != NONE)
			{
				if (length <= step)
					vr_climb.offset.i = vr_climb.offset.j = vr_climb.offset.k = 0.0f;
				else
					scale_vector3d(&vr_climb.offset, 1.0f - step / length, &vr_climb.offset);
				step = 0.0f;
			}
			if (step > 0.0f && across > 0.0f && (high || vr_climb.offset.k <= 0.0f || across > step))
			{
				real moved = MIN(across, step);

				vr_climb.offset.i -= vr_climb.offset.i / across * moved;
				vr_climb.offset.j -= vr_climb.offset.j / across * moved;
				step -= moved;
			}
			if (step > 0.0f && (!high || across <= CLIMB_RETURN_SPEED * 0.1f))
			{
				if (fabs(vr_climb.offset.k) <= step)
					vr_climb.offset.k = 0.0f;
				else
					vr_climb.offset.k -= vr_climb.offset.k > 0.0f ? step : -step;
			}
		}
	}
	{
		/* (the hands pull the eye no farther than CLIMB_FARTHEST from the
		body; farther, held through a jump of its place, it is not moved,
		and comes back as they let go) */
		real length = (real)sqrt(magnitude_squared3d(&vr_climb.offset));

		if (length > was_farthest)
			scale_vector3d(&vr_climb.offset, was_farthest / length, &vr_climb.offset);
	}
	/* (drawn to a seat the hands get into: lowered by a hatch's rim, across
	toward the opening's middle as the eye comes down to and past the rim's
	plane; pulled to one by its holds, within CLIMB_PULL_REACH, toward its
	eye) */
	if (table && vehicle_index != NONE && seated_unit_index == NONE && vr_climb.holding && !vr_climb.left)
	{
		real step = CLIMB_FUNNEL_SPEED * seconds;
		real_point3d place;
		short which;

		place.x = base.x + vr_climb.offset.i;
		place.y = base.y + vr_climb.offset.j;
		place.z = base.z + vr_climb.offset.k;
		for (which = 0; which < table->seat_count; which++)
		{
			struct vr_climb_seat const *to = &table->seats[which];
			short seat_index = vr_climb_seat(vehicle_index, to->label);
			real_vector3d toward;
			real length;

			if (seat_index == NONE || !unit_can_enter_seat(unit_index, vehicle_index, seat_index, NULL))
				continue;
			if (to->rule == _climb_enter_hatch)
			{
				real_matrix4x3 inverse;
				real dx, dy;

				if (!vr_climb_holds_entry(table, told, climb.held[0], which, FALSE) &&
					!vr_climb_holds_entry(table, told, climb.held[1], which, FALSE))
				{
					continue;
				}
				matrix4x3_inverse(&skinning, &inverse);
				matrix4x3_transform_point(&inverse, &place, &local);
				if (local.z >= to->rim + to->exit)
					continue;
				dx = (to->opening[0] + to->opening[2]) * 0.5f - local.x;
				dy = (to->opening[1] + to->opening[3]) * 0.5f - local.y;
				toward.i = (skinning.forward.i * dx + skinning.left.i * dy) * skinning.scale;
				toward.j = (skinning.forward.j * dx + skinning.left.j * dy) * skinning.scale;
				toward.k = (skinning.forward.k * dx + skinning.left.k * dy) * skinning.scale;
			}
			else
			{
				real_point3d seat_eye;

				if (to->handles ? !vr_climb_holds_entry(table, told, climb.held[0], which, TRUE) ||
					!vr_climb_holds_entry(table, told, climb.held[1], which, TRUE) :
					!vr_climb_holds_entry(table, told, climb.held[0], which, FALSE) &&
					!vr_climb_holds_entry(table, told, climb.held[1], which, FALSE))
				{
					continue;
				}
				if (!vr_climb_point(vehicle_index, &skinning, to->marker, to->eye, &seat_eye) ||
					distance3d(&place, &seat_eye) > CLIMB_PULL_REACH)
				{
					continue;
				}
				vector_from_points3d(&place, &seat_eye, &toward);
			}
			length = (real)sqrt(magnitude_squared3d(&toward));
			if (length > step && length > 0.0f)
				scale_vector3d(&toward, step / length, &toward);
			vr_climb.offset.i += toward.i;
			vr_climb.offset.j += toward.j;
			vr_climb.offset.k += toward.k;
			break;
		}
	}
	eye->x = base.x + vr_climb.offset.i;
	eye->y = base.y + vr_climb.offset.j;
	eye->z = base.z + vr_climb.offset.k;

	/* in: lowered into a hatch past its plane, pulled within reach of a
	seat's eye, the game's seat entry (X's); out: risen out of a hatch,
	moved off a seat by its holds, pushed off its handles, the game's exit
	(X's) */
	if (table && vehicle_index != NONE)
	{
		real_matrix4x3 inverse;

		matrix4x3_inverse(&skinning, &inverse);
		matrix4x3_transform_point(&inverse, eye, &local);
		if (seated_unit_index == NONE && vr_climb.holding && !vr_climb.left)
		{
			short which;

			for (which = 0; which < table->seat_count; which++)
			{
				struct vr_climb_seat const *to = &table->seats[which];
				short seat_index = vr_climb_seat(vehicle_index, to->label);
				boolean in;

				if (seat_index == NONE)
					continue;
				if (to->rule == _climb_enter_hatch)
				{
					in = local.x > to->opening[0] && local.x < to->opening[2] && local.y > to->opening[1] &&
						local.y < to->opening[3] && local.z < to->rim + to->enter;
				}
				else
				{
					real_point3d seat_eye;

					in = (to->handles ? vr_climb_holds_entry(table, told, climb.held[0], which, TRUE) &&
						vr_climb_holds_entry(table, told, climb.held[1], which, TRUE) :
						vr_climb_holds_entry(table, told, climb.held[0], which, FALSE) ||
						vr_climb_holds_entry(table, told, climb.held[1], which, FALSE)) &&
						vr_climb_point(vehicle_index, &skinning, to->marker, to->eye, &seat_eye) &&
						distance3d(eye, &seat_eye) < to->enter;
				}
				if (in && unit_enter_seat(unit_index, vehicle_index, seat_index))
				{
					platform_log("vr: climb: %s %s's seat %d (%s)", to->rule == _climb_enter_hatch ? "lowered into" :
						to->handles ? "both hands on the handles of" : "pulled into", tag_get_name(
						object_get(vehicle_index)->definition_index), seat_index, to->label);
					vr_climb.entering = TRUE;
					vr_climb.entering_eye = *eye;
					vr_climb.seating = TRUE;
					matrix4x3_inverse_transform_point(object_get_node_matrix(vehicle_index, 0), eye,
						&vr_climb.seat_local);
					break;
				}
			}
		}
		else if (seated_in && !vr_climb.exiting)
		{
			struct vr_climb_seat const *from = &table->seats[seat];
			boolean out;

			if (from->push)
				out = vr_climb.pushing && magnitude_squared3d(&vr_climb.offset) > from->exit * from->exit;
			else if (from->rule == _climb_enter_hatch)
				out = vr_climb.holding && local.z > from->rim + from->exit;
			else
				out = vr_climb.holding && magnitude_squared3d(&vr_climb.offset) > from->exit * from->exit;
			if (out && unit_try_and_exit_seat(unit_index))
			{
				platform_log("vr: climb: %s %s's seat (%s)", from->push ? "pushed off the handles, out of" :
					from->rule == _climb_enter_hatch ? "risen out of the hatch, out of" : "climbed out of",
					tag_get_name(object_get(vehicle_index)->definition_index), from->label);
				vr_climb.exiting = TRUE;
				vr_climb.left = TRUE;
				vr_climb.grab = from->push;
				vr_climb.pushing = FALSE;
			}
		}
	}
	if (testing)
	{
		platform_log("vr: climb: eye %.3f %.3f %.3f m (the world's), %d hands hold", eye->x * METRES_PER_WORLD_UNIT,
			eye->y * METRES_PER_WORLD_UNIT, eye->z * METRES_PER_WORLD_UNIT, vr_climb.holding);
	}
	vr_climb.valid = TRUE;
	vr_climb.last_base = base;
	vr_climb.last_eye = *eye;
}

/* vr_seat.c: getting into a seat by the hands this frame: where the eye
is, which the ride into the seat starts from */
boolean vr_climb_entering(real_point3d *eye)
{
	if (!vr_climb.entering)
		return FALSE;
	*eye = vr_climb.entering_eye;
	return TRUE;
}

/* vr_seat.c: out of the seat by the hands (the eye where they hold it,
not riding the body out) */
boolean vr_climb_exiting(void)
{
	return vr_climb.exiting;
}

/* vr_seat.c: the hands climb, or the eye is off the body's place: no blink
(the eye is held through any jump: vr_climb_eye) */
boolean vr_climb_active(void)
{
	return vr_climb.valid && (vr_climb.holding || vr_climb.exiting || vr_climb.entering || vr_climb.seating ||
		magnitude_squared3d(&vr_climb.offset) > 0.0f);
}

/* vr_vehicle.c: got into the seat by the hands, the eye gliding to it (the
play space turned to the vehicle's heading as it does, not in a blink) */
boolean vr_climb_seating(void)
{
	return vr_climb.valid && vr_climb.seating;
}

/* vr_vehicle.c: the eye's offset from its place last frame (world units),
which a seat's handles are placed from */
void vr_climb_offset(real_vector3d *offset)
{
	if (vr_climb.valid)
		*offset = vr_climb.offset;
	else
		offset->i = offset->j = offset->k = 0.0f;
}

/* first_person_weapons.c: a hand holds a hold: the weapon in hand not drawn
(the hands' alone, gripping) */
boolean vr_climb_hands_hold(void)
{
	return vr_climb.valid && vr_climb.holding > 0;
}

/* render_objects.c (vr_grab.c's highlight), as an object is drawn: the
vehicle climbed lit up as a hand comes to a hold (the holsters' light: the
shields' plasma and its light raised); FALSE, as it is */
boolean vr_climb_highlight(long object_index, struct render_lighting const *given, struct render_lighting *lighting,
	struct render_model_effect *model_effect)
{
	long player_index = local_player_get_player_index(0);

	if (object_index == NONE || object_index != vr_climb.vehicle_index || !vr_climb.valid || !given ||
		player_index == NONE || vr_climb.nearest > CLIMB_GLOW || vr_climb.holding)
	{
		return FALSE;
	}
	*lighting = *given;
	vr_holster_highlight_object(player_get(player_index)->unit_index, MAX(vr_climb.nearest, 0.0f),
		1.0f / METRES_PER_WORLD_UNIT, model_effect, lighting);
	return TRUE;
}

#endif
