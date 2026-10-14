/*
VR_CLIMB.C

The VR mode's climbing into and out of a vehicle with the hands (HALO_VR;
vr.vehicle_climb, port/linux/README.md "VR"), for vr_render.c, vr_seat.c and
render_objects.c.

In VR the motion that is comfortable is the hands': the hands pull and the
world follows them, felt as one's own. So a vehicle with holds (the table
below: the Scorpion's track guards, its hull's top edges and its hatch's rim)
may be climbed: a hand comes to a hold (the vehicle lights up as the holsters'
weapons do, and the controller ticks), its grip takes it (a firmer tick), and
while it holds, the eye moves against the hand's motion, 1:1 in all three
axes (vr.c's halo_vr_climb: both hands, their mean; hand over hand). Let go
of both, the eye comes back down to the body gently, at walking pace. The
eye's move is the view's alone, as the head's in the room is: the player's
unit stays where it stood beside the vehicle (no farther than
CLIMB_FARTHEST from it), and only what X would do reaches the game:

- At the hatch: lowered by the rim (the eye drawn across toward the
  opening's middle as it comes down to its plane, CLIMB_FUNNEL_SPEED, so
  that a hand anywhere on the rim brings it in), past
  CLIMB_ENTER_DEPTH below its plane, the game's seat entry happens, as X's
  (unit_enter_seat), and the eye rides on into the cockpit from where it is
  (vr_seat.c, its feed cockpit's ride: straight there, the play space turned
  to the display as it goes, and the display wakes).
- Out: from the seat, the rim glows overhead; holding it, the eye rises as
  the hands pull down, and past CLIMB_EXIT_HEIGHT above its plane the game's
  exit happens, as X's (unit_try_and_exit_seat). The eye stays where the hands
  hold it as the body is put beside the vehicle, and climbs down by the holds,
  or comes down to the body as the hands let go.

Through each jump of the eye's place (into a seat, out of one) while the
hands climb, the eye is held where it is (the offset taken again), so that
there is no jump and no blink. X keeps the hands-free ride for all. A
vehicle with no holds in the table is got into with X alone.
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
/* vr_seat.c's, vr_cabin.c's */
boolean vr_seat_seated_shown(void);
boolean vr_seat_feed_in(void);
/* vr_holster.c's */
void vr_holster_highlight_object(long unit_index, real distance, real units, struct render_model_effect *model_effect,
	struct render_lighting *lighting);

#define METRES_PER_WORLD_UNIT 3.048f
/* a vehicle climbed within this of the player's unit (world units: 6 m);
the eye no farther than this from the body (4 m); coming back down to the
body at this speed (world units a second: walking); the seat entered with
the eye this far into the hatch below its rim's plane, and left with it this
far above (30 cm); a hand this near a hold lights the vehicle up (metres) */
#define CLIMB_RANGE (6.0f / METRES_PER_WORLD_UNIT)
#define CLIMB_FARTHEST (4.0f / METRES_PER_WORLD_UNIT)
#define CLIMB_RETURN_SPEED (1.4f / METRES_PER_WORLD_UNIT)
#define CLIMB_ENTER_DEPTH (0.30f / METRES_PER_WORLD_UNIT)
#define CLIMB_EXIT_HEIGHT (0.30f / METRES_PER_WORLD_UNIT)
#define CLIMB_GLOW 0.2f
/* let go this high above the body (world units: 50 cm), the eye comes back
across first and then down */
#define CLIMB_HIGH (0.5f / METRES_PER_WORLD_UNIT)
/* lowered by the rim at or below its plane, the eye drawn across toward the
opening's middle no faster than this (world units a second: 1 m/s) */
#define CLIMB_FUNNEL_SPEED (1.0f / METRES_PER_WORLD_UNIT)
/* a jump of the eye's place (world units: 10 cm), held through */
#define CLIMB_JUMP (0.10f / METRES_PER_WORLD_UNIT)

/* a hold: a line on the vehicle (its ends, in its model's space: world units
forward, left, up), how far about it a hand takes it (world units), and
whether it is outside or the hatch's rim (which is reached from inside the
seat too) */
enum
{
	_climb_hold_outside,
	_climb_hold_rim,
};

struct vr_climb_hold
{
	real ends[2][3];
	real radius;
	short kind;
};

/* a vehicle's holds: its model's tag name, the seat its hatch enters (its
label's text), the hatch's opening (forward and left, from and to) and its
rim's height, and its holds */
struct vr_climb_vehicle
{
	char const *model_name;
	char const *seat_label;
	real opening[4];
	real rim;
	struct vr_climb_hold const *holds;
	short hold_count;
};

/* the Scorpion (6.8 m across its track guards): each side's track guard's
outer edge (1.6 m up), a tie-down rail along the middle of its top, a rail
at the foot of the hull's side and the hull's top edge (each a long reach
from the last, 80 to 90 cm), and the rim of the hatch over the driver's feed
cockpit (vr_cabin.c: its eye at forward 0.465, left 0.2), a square 75 cm
across */
static struct vr_climb_hold const vr_climb_scorpion_holds[] =
{
	{ { { -0.80f, 1.12f, 0.52f }, { 1.20f, 1.03f, 0.52f } }, 0.05f, _climb_hold_outside },
	{ { { -0.80f, -1.12f, 0.52f }, { 1.20f, -1.03f, 0.52f } }, 0.05f, _climb_hold_outside },
	{ { { -0.60f, 0.86f, 0.53f }, { 1.10f, 0.82f, 0.53f } }, 0.05f, _climb_hold_outside },
	{ { { -0.60f, -0.86f, 0.53f }, { 1.10f, -0.82f, 0.53f } }, 0.05f, _climb_hold_outside },
	{ { { -0.40f, 0.60f, 0.57f }, { 1.00f, 0.60f, 0.57f } }, 0.05f, _climb_hold_outside },
	{ { { -0.40f, -0.60f, 0.57f }, { 1.00f, -0.60f, 0.57f } }, 0.05f, _climb_hold_outside },
	{ { { -0.40f, 0.56f, 0.74f }, { 1.00f, 0.56f, 0.70f } }, 0.05f, _climb_hold_outside },
	{ { { -0.40f, -0.56f, 0.74f }, { 1.00f, -0.56f, 0.70f } }, 0.05f, _climb_hold_outside },
	{ { { 0.345f, 0.080f, 0.750f }, { 0.585f, 0.080f, 0.750f } }, 0.04f, _climb_hold_rim },
	{ { { 0.585f, 0.080f, 0.750f }, { 0.585f, 0.320f, 0.750f } }, 0.04f, _climb_hold_rim },
	{ { { 0.585f, 0.320f, 0.750f }, { 0.345f, 0.320f, 0.750f } }, 0.04f, _climb_hold_rim },
	{ { { 0.345f, 0.320f, 0.750f }, { 0.345f, 0.080f, 0.750f } }, 0.04f, _climb_hold_rim },
};

static struct vr_climb_vehicle const vr_climb_vehicles[] =
{
	{ "vehicles\\scorpion\\scorpion", "scorpion-driver", { 0.345f, 0.080f, 0.585f, 0.320f }, 0.750f,
		vr_climb_scorpion_holds, NUMBEROF(vr_climb_scorpion_holds) },
};

static struct
{
	/* the vehicle climbed (else NONE) and its table's entry; the eye's
	offset from its place (the world's), the eye's place and where it was
	shown last frame; how many hands hold, the nearest hand's distance from
	a hold (metres beyond its reach); the seat got into or out of by the
	hands (the eye's place there given to vr_seat.c) */
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
	boolean logged_entry;
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
skins its vertices), and a point's place in it */
static void vr_climb_model_space(long vehicle_index, real_matrix4x3 *skinning)
{
	long model_index = object_definition_get(object_get(vehicle_index)->definition_index)->object.model.index;
	struct model *model = model_definition_get(model_index);

	matrix4x3_multiply(object_get_node_matrix(vehicle_index, 0),
		&TAG_BLOCK_GET_ELEMENT(&model->nodes, 0, struct model_node)->runtime_default_inverse_matrix, skinning);
}

/* the seat of the vehicle whose label has the text, else NONE */
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
	vr_climb.entering = vr_climb.exiting = FALSE;
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
	short entry = NONE, index, told[HALO_VR_CLIMB_HOLDS];
	boolean seated_in = FALSE;
	real moved_x, moved_y;

	if (!vr_climb_enabled() || !aiming || player_index == NONE || units <= 0.0f ||
		(unit_index = player_get(player_index)->unit_index) == NONE ||
		TEST_FLAG(object_get(unit_index)->object.damage_flags, _object_dead_bit))
	{
		vr_climb_reset();
		return;
	}
	/* the vehicle: the one the unit is in (its hatch's seat, seated: the
	rim from inside), else the nearest with holds */
	if (seated_unit_index != NONE)
	{
		struct unit_datum *unit = unit_get(seated_unit_index);

		vehicle_index = unit->object.parent_object_index;
		entry = vr_climb_entry(vehicle_index);
		seated_in = entry != NONE && unit->unit.parent_seat_index ==
			vr_climb_seat(vehicle_index, vr_climb_vehicles[entry].seat_label) &&
			unit->unit.animation.state != _unit_state_entering_seat && (vr_seat_feed_in() || vr_climb.exiting);
		if (entry != NONE && unit->unit.animation.state == _unit_state_entering_seat)
		{
			/* (getting in: the ride carries the eye on from where it was:
			vr_seat.c) */
			vr_climb.offset.i = vr_climb.offset.j = vr_climb.offset.k = 0.0f;
			vr_climb.holding = 0;
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
	/* (a jump of the eye's place, into a seat or out of one, held through
	while the offset is in use: no jump, no blink) */
	if (vr_climb.valid && (vr_climb.holding || magnitude_squared3d(&vr_climb.offset) > 0.0f || vr_climb.exiting) &&
		distance3d(&base, &vr_climb.last_base) > CLIMB_JUMP && !vr_climb.entering)
	{
		vector_from_points3d(&base, &vr_climb.last_eye, &vr_climb.offset);
		platform_log("vr: climb: the eye's place jumped %.2f m; held where it was",
			distance3d(&base, &vr_climb.last_base) / units);
	}
	if (vr_climb.entering && seated_unit_index != NONE)
		vr_climb.entering = FALSE;
	if (vr_climb.exiting && seated_unit_index == NONE)
		vr_climb.exiting = FALSE;

	/* the holds told to vr.c, as the eye is placed now (the recentred
	head's place in the world: the eye with its offset) */
	csmemset(&climb, 0, sizeof(climb));
	origin.x = base.x + vr_climb.offset.i;
	origin.y = base.y + vr_climb.offset.j;
	origin.z = base.z + vr_climb.offset.k;
	table = entry != NONE ? &vr_climb_vehicles[entry] : NULL;
	if (table && vehicle_index != NONE && (seated_unit_index == NONE || seated_in))
	{
		vr_climb_model_space(vehicle_index, &skinning);
		for (index = 0; index < table->hold_count && climb.count < HALO_VR_CLIMB_HOLDS; index++)
		{
			struct vr_climb_hold const *hold = &table->holds[index];
			short end;

			if (seated_in && hold->kind != _climb_hold_rim)
				continue;
			for (end = 0; end < 2; end++)
			{
				real_point3d point, world;
				real x, y;

				point.x = hold->ends[end][0];
				point.y = hold->ends[end][1];
				point.z = hold->ends[end][2];
				matrix4x3_transform_point(&skinning, &point, &world);
				yaw_turn(-base_yaw, (world.x - origin.x) / units, (world.y - origin.y) / units, &x, &y);
				climb.ends[climb.count][end][0] = (float)x;
				climb.ends[climb.count][end][1] = (float)y;
				climb.ends[climb.count][end][2] = (float)((world.z - origin.z) / units);
			}
			climb.radius[climb.count] = (float)(hold->radius * METRES_PER_WORLD_UNIT);
			told[climb.count] = index;
			climb.count++;
		}
	}
	vr_climb.holding = (short)halo_vr_climb(&climb);
	/* (debug.vr_test_seat "... climb": the holds where they are from the
	recentred head, once a second, and the eye's offset, each frame; with
	debug.screenshot_directory, the eyes every twelfth frame a hand holds) */
	if (strstr(config_string("debug.vr_test_seat"), "climb"))
	{
		static long logged_tick = -1000;

		if (climb.count && (game_time_get() >= logged_tick + TICKS_PER_SECOND || game_time_get() < logged_tick))
		{
			logged_tick = game_time_get();
			for (index = 0; index < climb.count; index++)
			{
				platform_log("vr: climb: hold %d: %.3f %.3f %.3f to %.3f %.3f %.3f (forward, left, up from the "
					"recentred head, m), reach %.2f", index, climb.ends[index][0][0], climb.ends[index][0][1],
					climb.ends[index][0][2], climb.ends[index][1][0], climb.ends[index][1][1], climb.ends[index][1][2],
					climb.radius[index]);
			}
		}
		{
			void halo_vr_test_screenshot(void);
			static long shot;

			if (vr_climb.holding && (++shot % 12) == 0)
				halo_vr_test_screenshot();
		}
		platform_log("vr: climb: tick %ld: %d hands hold (%d %d), nearest %.2f %.2f m, eye offset %.3f %.3f %.3f m, "
			"hands %.2f %.2f %.2f and %.2f %.2f %.2f",
			game_time_get(), vr_climb.holding, climb.held[0], climb.held[1], climb.distance[0], climb.distance[1],
			vr_climb.offset.i / units, vr_climb.offset.j / units, vr_climb.offset.k / units, climb.hand[0][0],
			climb.hand[0][1], climb.hand[0][2], climb.hand[1][0], climb.hand[1][1], climb.hand[1][2]);
	}
	vr_climb.nearest = MIN(climb.distance[0], climb.distance[1]);

	/* the eye moved by the hands (against them), or back down to the body
	at walking pace as they let go */
	yaw_turn(base_yaw, climb.moved[0], climb.moved[1], &moved_x, &moved_y);
	vr_climb.offset.i += moved_x * units;
	vr_climb.offset.j += moved_y * units;
	vr_climb.offset.k += climb.moved[2] * units;
	if (!vr_climb.holding && !(vr_climb.exiting && seated_unit_index != NONE))
	{
		/* (high, across first and then down: off the vehicle beside it, not
		through it) */
		real across = (real)sqrt(vr_climb.offset.i * vr_climb.offset.i + vr_climb.offset.j * vr_climb.offset.j);
		real step = CLIMB_RETURN_SPEED * PIN(seconds, 0.0f, 0.1f);
		boolean high = vr_climb.offset.k > CLIMB_HIGH;

		if (across > 0.0f && (high || vr_climb.offset.k <= 0.0f || across > step))
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
	{
		real length = (real)sqrt(magnitude_squared3d(&vr_climb.offset));

		if (length > CLIMB_FARTHEST)
		{
			vr_climb.offset.i *= CLIMB_FARTHEST / length;
			vr_climb.offset.j *= CLIMB_FARTHEST / length;
			vr_climb.offset.k *= CLIMB_FARTHEST / length;
		}
	}
	/* (lowering by the rim: the eye drawn across toward the opening's
	middle as it comes down to and past the rim's plane) */
	if (table && vehicle_index != NONE && seated_unit_index == NONE &&
		((climb.held[0] >= 0 && table->holds[told[climb.held[0]]].kind == _climb_hold_rim) ||
		(climb.held[1] >= 0 && table->holds[told[climb.held[1]]].kind == _climb_hold_rim)))
	{
		real_matrix4x3 inverse;
		real_point3d place;
		real dx, dy, length, step = CLIMB_FUNNEL_SPEED * PIN(seconds, 0.0f, 0.1f);

		place.x = base.x + vr_climb.offset.i;
		place.y = base.y + vr_climb.offset.j;
		place.z = base.z + vr_climb.offset.k;
		vr_climb_model_space(vehicle_index, &skinning);
		matrix4x3_inverse(&skinning, &inverse);
		matrix4x3_transform_point(&inverse, &place, &local);
		if (local.z < table->rim + CLIMB_EXIT_HEIGHT)
		{
			dx = (table->opening[0] + table->opening[2]) * 0.5f - local.x;
			dy = (table->opening[1] + table->opening[3]) * 0.5f - local.y;
			length = (real)sqrt(dx * dx + dy * dy);
			if (length > step && length > 0.0f)
			{
				dx *= step / length;
				dy *= step / length;
			}
			vr_climb.offset.i += (skinning.forward.i * dx + skinning.left.i * dy) * skinning.scale;
			vr_climb.offset.j += (skinning.forward.j * dx + skinning.left.j * dy) * skinning.scale;
			vr_climb.offset.k += (skinning.forward.k * dx + skinning.left.k * dy) * skinning.scale;
		}
	}
	eye->x = base.x + vr_climb.offset.i;
	eye->y = base.y + vr_climb.offset.j;
	eye->z = base.z + vr_climb.offset.k;

	/* the hatch: lowered into it by the rim, the game's seat entry (X's);
	risen out of it from the seat, the game's exit (X's) */
	if (table && vehicle_index != NONE)
	{
		real_matrix4x3 inverse;

		vr_climb_model_space(vehicle_index, &skinning);
		matrix4x3_inverse(&skinning, &inverse);
		matrix4x3_transform_point(&inverse, eye, &local);
		if (seated_unit_index == NONE && vr_climb.holding && local.x > table->opening[0] && local.x < table->opening[2] &&
			local.y > table->opening[1] && local.y < table->opening[3] && local.z < table->rim - CLIMB_ENTER_DEPTH)
		{
			short seat_index = vr_climb_seat(vehicle_index, table->seat_label);

			if (seat_index != NONE && unit_enter_seat(unit_index, vehicle_index, seat_index))
			{
				platform_log("vr: climb: lowered into %s's hatch: into its seat %d", tag_get_name(
					object_get(vehicle_index)->definition_index), seat_index);
				vr_climb.entering = TRUE;
				vr_climb.entering_eye = *eye;
			}
		}
		else if (seated_in && vr_climb.holding && local.z > table->rim + CLIMB_EXIT_HEIGHT && !vr_climb.exiting)
		{
			if (unit_try_and_exit_seat(unit_index))
			{
				platform_log("vr: climb: risen out of %s's hatch: out of its seat",
					tag_get_name(object_get(vehicle_index)->definition_index));
				vr_climb.exiting = TRUE;
			}
		}
	}
	if (strstr(config_string("debug.vr_test_seat"), "climb"))
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
	return vr_climb.valid && (vr_climb.holding || vr_climb.exiting || vr_climb.entering ||
		magnitude_squared3d(&vr_climb.offset) > 0.0f);
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
