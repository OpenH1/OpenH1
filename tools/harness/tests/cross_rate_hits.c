/*
CROSS_RATE_HITS.C (test)

Shots between players of different rates in a mixed game (an OpenCE player
at 30, OpenH1 players at 60 or 120: port/linux/game/network_standard.c),
resolved by the host's check of where the shooter saw the target
(network_damage.c's distributed_target_seen). The shooter's own machine
decides what its shot hit, at its own rate: a 30 Hz shooter sees its target
where a tick left it, a 60 or 120 Hz one where a sub-step did, between two
ticks' poses. The host keeps where each player was at each of its ticks and
takes a report whose target is within REPORT_HISTORY_TOLERANCE (and a few
ticks' movement) of where it had the target at a tick as far back as the
report was made: a sub-step's pose is less than a tick's movement from a
tick's, so a hit from either rate is taken alike, and one far from anywhere
the target was is refused at either. A shot taken gives the host's own pose
of the target nearest the report, which its line of sight check traces to
(distributed_report_line_of_sight): one of the ticks', at either rate.
test_cross_rate_hits.py takes the code (under_test.inc: network_damage.c's
distributed_distance_squared and distributed_target_seen) and the constants
(config.inc).
*/

#include "harness.h"
#include <math.h>

#ifndef NUMBEROF
#define NUMBEROF(array) (sizeof(array) / sizeof((array)[0]))
#endif

#include "config.inc"

enum
{
	MAXIMUM_TRACKED_PLAYERS = 4,
	TARGET = 0x10001,
	TARGET_PLAYER = 1,
};

static long now;

static long game_time_get(void)
{
	return now;
}

/* the host's history (network_damage.c's damage_history) */
static struct damage_history_tick
{
	long time;
	struct
	{
		long object_index;
		real_point3d position;
		real speed;
		boolean driving;
	} objects[MAXIMUM_TRACKED_PLAYERS][2];
} damage_history[TARGET_HISTORY_TICKS];

#include "under_test.inc"

/* where the target is at a time in ticks (a fraction between two ticks: a
sub-step's end), running along x at speed world units a tick, and turning
back every 20 ticks (a strafe) */
static real_point3d target_at(real time, real speed)
{
	real period = 20.0f;
	real phase = fmodf(time, 2.0f * period);
	real_point3d point;

	point.x = speed * (phase < period ? phase : 2.0f * period - phase);
	point.y = 5.0f;
	point.z = 0.5f;
	return point;
}

/* the host's history of the target up to its tick now */
static void history(real speed)
{
	long back;

	memset(damage_history, 0, sizeof(damage_history));
	for (back = 0; back < TARGET_HISTORY_TICKS; back++)
	{
		long time = now - back;
		struct damage_history_tick *tick = &damage_history[time & (TARGET_HISTORY_TICKS - 1)];
		short player;

		tick->time = time;
		for (player = 0; player < MAXIMUM_TRACKED_PLAYERS; player++)
		{
			tick->objects[player][0].object_index = NONE;
			tick->objects[player][1].object_index = NONE;
		}
		tick->objects[TARGET_PLAYER][0].object_index = TARGET;
		tick->objects[TARGET_PLAYER][0].position = target_at((real)time, speed);
		tick->objects[TARGET_PLAYER][0].speed = speed;
	}
}

/* a shooter's report, at its rate (sub-steps a tick), of the target where
its simulation had it at each of its steps of the ticks latency ticks behind
the host's now (the host's tick it had heard of then, as the report says),
off by error world units across the target's way (a remote player's copy a
little off the host's): each taken, or (taken FALSE) each refused */
static void shots(short substeps, real speed, long latency, real error, boolean taken)
{
	long tick;

	for (tick = now - latency - 3; tick <= now - latency; tick++)
	{
		short substep;

		for (substep = 0; substep < substeps; substep++)
		{
			/* (a sub-step's end: a fraction of the way from the tick before) */
			real time = (real)(tick - 1) + (real)(substep + 1) / (real)substeps;
			real_point3d seen = target_at(time, speed);
			/* (the host's check looks back from the report's tick, with its
			slack: network_damage.c's distributed_report_valid) */
			long ticks = latency + TARGET_HISTORY_SLACK_TICKS;
			real_point3d host_pose = { -1000.0f, -1000.0f, -1000.0f };
			short result;

			seen.y += error;
			result = distributed_target_seen(TARGET, &seen, ticks, &host_pose);
			CHECK(taken ? result == TRUE : result == FALSE,
				"a %d Hz shot at tick %ld sub-step %d (%.3f world units a tick, %ld ticks late, %.2f off): %s",
				30 * substeps, tick, substep, speed, latency, error, result == TRUE ? "taken" : "refused");
			/* (the line of sight is traced to the host's pose of a tick, the
			nearest of those in reach: within reach of the report, at either
			rate) */
			if (result == TRUE)
			{
				real reach = REPORT_HISTORY_TOLERANCE + REPORT_HISTORY_LEAD_TICKS * speed;

				CHECK(distributed_distance_squared(&seen, &host_pose) <= reach * reach,
					"a %d Hz shot at tick %ld sub-step %d: traced to (%.2f %.2f %.2f), not a pose near the report",
					30 * substeps, tick, substep, host_pose.x, host_pose.y, host_pose.z);
			}
		}
	}
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	/* world units a tick: a player running (0.08-0.25 with a boost of a
	blast), a vehicle (a Warthog flat out, about 1) */
	real const speeds[] = { 0.08f, 0.25f, 1.0f };
	long const latencies[] = { 0, 2, 6, 15 };
	short speed_index, latency_index;

	now = 1000;

	/* a 30 Hz shooter (an OpenCE player) sees the target where each tick
	left it: taken, at every speed and latency the history reaches */
	CASE("opence-shooter")
	{
		for (speed_index = 0; speed_index < (short)NUMBEROF(speeds); speed_index++)
		{
			history(speeds[speed_index]);
			for (latency_index = 0; latency_index < (short)NUMBEROF(latencies); latency_index++)
				shots(1, speeds[speed_index], latencies[latency_index], 0.0f, TRUE);
		}
		return 0;
	}

	/* a 60 or 120 Hz shooter (an OpenH1 player) sees it where each sub-step
	left it, between the ticks' poses: taken alike */
	CASE("openh1-shooter")
	{
		short substeps;

		for (substeps = 2; substeps <= 4; substeps *= 2)
		{
			for (speed_index = 0; speed_index < (short)NUMBEROF(speeds); speed_index++)
			{
				history(speeds[speed_index]);
				for (latency_index = 0; latency_index < (short)NUMBEROF(latencies); latency_index++)
					shots(substeps, speeds[speed_index], latencies[latency_index], 0.0f, TRUE);
			}
		}
		return 0;
	}

	/* a shooter's copy of the target a little off the host's (a remote
	player's correction is a world unit at most before it is put right):
	taken at either rate */
	CASE("copy-off")
	{
		history(0.25f);
		shots(1, 0.25f, 6, 1.0f, TRUE);
		shots(4, 0.25f, 6, 1.0f, TRUE);
		return 0;
	}

	/* a target where the host never had it (further than the tolerance and
	the few ticks' movement): refused at either rate */
	CASE("never-there")
	{
		history(0.25f);
		shots(1, 0.25f, 6, REPORT_HISTORY_TOLERANCE + REPORT_HISTORY_LEAD_TICKS * 0.25f + 0.5f, FALSE);
		shots(4, 0.25f, 6, REPORT_HISTORY_TOLERANCE + REPORT_HISTORY_LEAD_TICKS * 0.25f + 0.5f, FALSE);
		return 0;
	}

	fprintf(stderr, "no case '%s'\n", case_name);
	return 2;
}
