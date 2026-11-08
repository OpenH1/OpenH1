/* The players' pings (network_markers.c): the points the host takes from
another machine (untrusted), a ping that takes the last away (the same
object, or a point near the last), and the pings a player sends at once and
over time. */
#include "harness.h"
#include "config.inc"
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
	else
	{
		fprintf(stderr, "no case %s\n", name);
		return 2;
	}
	return 0;
}
