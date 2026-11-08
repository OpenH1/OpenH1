/* The players' pings (network_markers.c): the points the host takes from
another machine (untrusted), a ping that takes the last away (the same
object, or a point near the last), and the pings a player sends at once and
over time; and where a ping of an object is drawn in a frame between the
ticks. */
#include "harness.h"
#include <math.h>
#include "config.inc"

/* an object drawn between the ticks (render_interpolation.c): its middle as
the last tick left it, drawn moved by drawn_motion; a biped's head marker is
read from the pose drawn (object_get_node_matrices while a frame draws) */
enum { _object_mask_biped = 1 };
static boolean object_is_biped;
static real_point3d const tick_middle = { 10.0f, 20.0f, 1.0f };
static real const object_radius = 0.8f;
static real_vector3d drawn_motion;
/* the head over the middle, as drawn */
static real const head_above = 0.6f;
static void object_get_bounding_sphere(long object_index, real_point3d *center, real *radius)
{
	(void)object_index;
	*center = tick_middle;
	*radius = object_radius;
}
static void render_interpolation_object_point(long object_index, real_point3d *point)
{
	(void)object_index;
	point->x += drawn_motion.i;
	point->y += drawn_motion.j;
	point->z += drawn_motion.k;
}
static boolean object_try_and_get_and_verify_type(long object_index, long mask)
{
	(void)object_index;
	return object_is_biped && (mask & _object_mask_biped) != 0;
}
static void unit_get_head_position(long unit_index, real_point3d *head)
{
	(void)unit_index;
	head->x = tick_middle.x + drawn_motion.i;
	head->y = tick_middle.y + drawn_motion.j;
	head->z = tick_middle.z + drawn_motion.k + head_above;
}

#include "under_test.inc"

int main(int argc, char **argv) {
	char const *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "point"))
	{
		real nan = 0.0f / 0.0f;
		real big = 1.0e30f;

		CHECK(network_markers_point_valid(10.0f, -20.0f, 3.0f), "a level's point refused");
		CHECK(network_markers_point_valid(-MARKER_WORLD_LIMIT + 1.0f, MARKER_WORLD_LIMIT - 1.0f, 0.0f),
			"the world's edge refused");
		CHECK(!network_markers_point_valid(nan, 0.0f, 0.0f), "a NaN x taken");
		CHECK(!network_markers_point_valid(0.0f, nan, 0.0f), "a NaN y taken");
		CHECK(!network_markers_point_valid(0.0f, 0.0f, nan), "a NaN z taken");
		CHECK(!network_markers_point_valid(big * big, 0.0f, 0.0f), "an infinite x taken");
		CHECK(!network_markers_point_valid(0.0f, -big * big, 0.0f), "an infinite y taken");
		CHECK(!network_markers_point_valid(0.0f, 0.0f, MARKER_WORLD_LIMIT + 1.0f), "a point past the world taken");
	}
	else if (!strcmp(name, "same-spot"))
	{
		real here[3] = { 10.0f, 10.0f, 1.0f };
		real near[3] = { 10.5f, 10.5f, 1.0f };
		real far[3] = { 14.0f, 10.0f, 1.0f };

		CHECK(network_markers_same_spot(NONE, near, NONE, here), "a point near the last is another");
		CHECK(!network_markers_same_spot(NONE, far, NONE, here), "a point far from the last clears it");
		CHECK(network_markers_same_spot(0x10002, far, 0x10002, here), "the same object (moved) is another");
		CHECK(!network_markers_same_spot(0x10002, here, 0x20003, here), "another object at the spot clears it");
		CHECK(!network_markers_same_spot(0x10002, here, NONE, here), "an object at a point's spot clears it");
		CHECK(!network_markers_same_spot(NONE, here, 0x10002, here), "a point at an object's spot clears it");
	}
	else if (!strcmp(name, "rate-burst"))
	{
		struct marker_rate rate = { 0 };
		short taken = 0;
		short index;

		for (index = 0; index < 10; index++)
			taken += markers_rate_take(&rate, 1000);
		CHECK(taken == MARKER_BURST, "%d taken at once", taken);
	}
	else if (!strcmp(name, "rate-refill"))
	{
		struct marker_rate rate = { 0 };
		unsigned long now = 5000;
		short taken = 0;
		short index;

		for (index = 0; index < MARKER_BURST; index++)
			markers_rate_take(&rate, now);
		/* (a press each 100 ms for 12 s: one each refill) */
		for (index = 0; index < 120; index++)
		{
			now += 100;
			taken += markers_rate_take(&rate, now);
		}
		CHECK(taken == 12000 / MARKER_REFILL_MILLISECONDS, "%d taken in 12 s", taken);
		/* (rested: a burst again, no more) */
		now += 60000;
		taken = 0;
		for (index = 0; index < 10; index++)
			taken += markers_rate_take(&rate, now);
		CHECK(taken == MARKER_BURST, "%d taken after a rest", taken);
	}
	else if (!strcmp(name, "rate-wrap"))
	{
		struct marker_rate rate = { 0 };
		short taken = 0;
		short index;

		/* (the clock wrapping: no more than a burst, and on) */
		for (index = 0; index < MARKER_BURST; index++)
			markers_rate_take(&rate, 0xFFFFFF00UL);
		for (index = 0; index < 10; index++)
			taken += markers_rate_take(&rate, 0xFFFFFF00UL + MARKER_REFILL_MILLISECONDS + 5UL);
		CHECK(taken == 1, "%d taken past the wrap", taken);
	}
	else if (!strcmp(name, "object-point"))
	{
		/* (a frame a quarter, a half and all of the way between two ticks of
		an object rising and moving on: a biped, then another kind) */
		static real const parts[] = { 0.0f, 0.25f, 0.5f, 1.0f };
		short kind;
		short index;

		for (kind = 0; kind < 2; kind++)
		{
			object_is_biped = kind == 0;
			for (index = 0; index < (short)(sizeof(parts) / sizeof(parts[0])); index++)
			{
				real_point3d at;
				real_point3d position;
				real expected_z;

				drawn_motion.i = 0.4f * parts[index];
				drawn_motion.j = -0.3f * parts[index];
				drawn_motion.k = 0.2f * parts[index];
				markers_object_drawn_point(0x10002, &at, &position);
				CHECK(at.x == tick_middle.x && at.y == tick_middle.y && at.z == tick_middle.z,
					"the place kept (%f %f %f) not the tick's middle", at.x, at.y, at.z);
				CHECK(fabsf(position.x - (tick_middle.x + drawn_motion.i)) < 1e-5f &&
					fabsf(position.y - (tick_middle.y + drawn_motion.j)) < 1e-5f,
					"drawn at (%f %f), not over the object as drawn", position.x, position.y);
				expected_z = object_is_biped ?
					tick_middle.z + drawn_motion.k + head_above + MARKER_HEAD_HEIGHT :
					tick_middle.z + drawn_motion.k + object_radius * MARKER_OBJECT_RADIUS_PART + MARKER_OBJECT_HEIGHT;
				CHECK(fabsf(position.z - expected_z) < 1e-5f, "a %s's ping drawn at height %f, not %f (%.2f of the "
					"way between the ticks)", object_is_biped ? "biped" : "vehicle", position.z, expected_z,
					parts[index]);
			}
		}
	}
	else
	{
		fprintf(stderr, "no case %s\n", name);
		return 2;
	}
	return 0;
}
