/*
BOTS_TACTICS.C (test)

The geometry and the rules of port/linux/game/bots_tactics.c that a team's
bots move by: two bots on one enemy strafing round it apart (crossfire), a
fighting bot strafing away from a teammate close by, a walking bot turning
away from one (spacing), and when a team loses the fight. Each bot moves as
bots.c moves it: a strafe to the side 1 is the direction to its target
turned 90 degrees anticlockwise. test_bots_tactics.py takes the constants
(config.inc) and the code (under_test.inc).
*/

#include "harness.h"

#include <math.h>

#include "config.inc"
#include "under_test.inc"

static real distance2d(real_point3d const *a, real_point3d const *b)
{
	return (real)sqrt((a->x - b->x) * (a->x - b->x) + (a->y - b->y) * (a->y - b->y));
}

/* the bot strafed a step to that side, facing the target, as bots.c moves
it while it fights */
static void strafe(real_point3d *position, real_point3d const *target, real side, real step)
{
	real to_target = (real)atan2(target->y - position->y, target->x - position->x);

	position->x += (real)cos(to_target + TACTICS_PI * 0.5f) * side * step;
	position->y += (real)sin(to_target + TACTICS_PI * 0.5f) * side * step;
}

/* two bots round one target, at those angles (degrees) and 8 units away,
strafing as tactics asks a tick at a time (a run's speed) until their angles
are 60 degrees apart: the ticks it took, or -1 if they never got there or
closed in on each other on the way */
static long crossfire(real a_degrees, real b_degrees)
{
	real_point3d target = { 0.0f, 0.0f, 0.0f };
	real_point3d a = { 8.0f * (real)cos(a_degrees * TACTICS_DEGREES), 8.0f * (real)sin(a_degrees * TACTICS_DEGREES), 0.0f };
	real_point3d b = { 8.0f * (real)cos(b_degrees * TACTICS_DEGREES), 8.0f * (real)sin(b_degrees * TACTICS_DEGREES), 0.0f };
	real last = (real)fabs(tactics_angle_round(&a, &target, &b));
	long tick;

	for (tick = 0; tick < 30 * 20; tick++)
	{
		real side_a = tactics_crossfire_side(&a, &target, &b, TRUE);
		real side_b = tactics_crossfire_side(&b, &target, &a, FALSE);
		real separation;

		strafe(&a, &target, side_a, 0.075f);
		strafe(&b, &target, side_b, 0.075f);
		separation = (real)fabs(tactics_angle_round(&a, &target, &b));
		if (separation + 0.0001f < last)
			return -1;
		last = separation;
		if (separation >= 60.0f * TACTICS_DEGREES)
			return tick;
	}
	return -1;
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	/* two bots on one enemy take angles apart, from either side, across the
	half turn, and from one line */
	CASE("crossfire")
	{
		long ticks = crossfire(10.0f, 0.0f);

		CHECK(ticks >= 0 && ticks < 30 * 6, "10 degrees apart: %ld ticks to 60", ticks);
		ticks = crossfire(-20.0f, 15.0f);
		CHECK(ticks >= 0 && ticks < 30 * 6, "35 degrees apart: %ld ticks to 60", ticks);
		ticks = crossfire(170.0f, -175.0f);
		CHECK(ticks >= 0 && ticks < 30 * 6, "across the half turn: %ld ticks to 60", ticks);
		ticks = crossfire(45.0f, 45.0f);
		CHECK(ticks >= 0 && ticks < 30 * 6, "on one line: %ld ticks to 60", ticks);
		return 0;
	}

	/* a fighting bot strafes away from a teammate close by, on any side */
	CASE("spacing-strafe")
	{
		real_point3d target = { 10.0f, 0.0f, 0.0f };
		short trial;

		for (trial = 0; trial < 8; trial++)
		{
			real angle = (real)trial * 45.0f * TACTICS_DEGREES + 0.2f;
			real_point3d position = { 0.0f, 0.0f, 0.0f };
			real_point3d mate = { (real)cos(angle), (real)sin(angle), 0.0f };
			real before = distance2d(&position, &mate);
			short tick;

			for (tick = 0; tick < 10; tick++)
				strafe(&position, &target, tactics_side_away(&position, &target, &mate), 0.075f);
			CHECK(distance2d(&position, &mate) > before, "a teammate at %.0f degrees: %.2f units away, from %.2f",
				angle / TACTICS_DEGREES, distance2d(&position, &mate), before);
		}
		return 0;
	}

	/* two bots walking side by side, half a unit apart, open to the
	spacing within three seconds, each turned 40 degrees at most */
	CASE("steer")
	{
		real_point3d a = { 0.0f, 0.0f, 0.0f }, b = { 0.0f, 0.5f, 0.0f };
		real throttle = 1.0f, direction;
		short tick;

		for (tick = 0; tick < 30 * 3; tick++)
		{
			real distance = distance2d(&a, &b);
			real a_direction = 0.0f, b_direction = 0.0f, a_throttle = 1.0f, b_throttle = 1.0f;

			if (distance < TACTICS_SPACING)
			{
				a_direction = tactics_steer_away(0.0f, b.x - a.x, b.y - a.y, distance, &a_throttle);
				b_direction = tactics_steer_away(0.0f, a.x - b.x, a.y - b.y, distance, &b_throttle);
			}
			CHECK(fabs(a_direction) <= 40.0f * TACTICS_DEGREES + 0.001f, "turned %.1f degrees",
				a_direction / TACTICS_DEGREES);
			a.x += (real)cos(a_direction) * 0.075f * a_throttle;
			a.y += (real)sin(a_direction) * 0.075f * a_throttle;
			b.x += (real)cos(b_direction) * 0.075f * b_throttle;
			b.y += (real)sin(b_direction) * 0.075f * b_throttle;
		}
		CHECK(distance2d(&a, &b) >= TACTICS_SPACING - 0.1f, "%.2f units apart after 3 seconds", distance2d(&a, &b));
		/* (one close ahead: eased off behind it; one beside it: not) */
		direction = tactics_steer_away(0.0f, 0.8f, 0.1f, 0.81f, &throttle);
		CHECK(throttle < 1.0f && direction < 0.0f, "a teammate ahead and to the left: throttle %.2f, turn %.1f",
			throttle, direction / TACTICS_DEGREES);
		throttle = 1.0f;
		direction = tactics_steer_away(0.0f, 0.0f, -1.0f, 1.0f, &throttle);
		CHECK(throttle == 1.0f && direction > 0.0f, "a teammate to the right: throttle %.2f, turn %.1f", throttle,
			direction / TACTICS_DEGREES);
		return 0;
	}

	/* a team loses the fight with two deaths more than kills in ten seconds */
	CASE("losing")
	{
		long deaths[TACTICS_EVENTS], kills[TACTICS_EVENTS];
		long now = 1000 * TICKS_PER_SECOND;

		memset(deaths, 0, sizeof(deaths));
		memset(kills, 0, sizeof(kills));
		deaths[0] = now - TICKS_PER_SECOND;
		deaths[1] = now - TICKS_PER_SECOND * 4;
		CHECK(tactics_losing(deaths, kills, now), "two deaths and no kills");
		kills[0] = now - TICKS_PER_SECOND * 2;
		CHECK(!tactics_losing(deaths, kills, now), "two deaths and a kill");
		deaths[2] = now - TICKS_PER_SECOND * 9;
		CHECK(tactics_losing(deaths, kills, now), "three deaths and a kill");
		deaths[2] = now - TICKS_PER_SECOND * 11;
		CHECK(!tactics_losing(deaths, kills, now), "a death eleven seconds ago counted");
		return 0;
	}

	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
