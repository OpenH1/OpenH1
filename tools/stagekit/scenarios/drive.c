/*
DRIVE.C

Cars to drive (tools/scorecard's vehicle scenarios): one at the middle of the
floor facing x ("car"), with open floor all round it; a ramp ahead of it, 24
away, that rises 1.7 over 8 and stops, to jump from; another ("high") on a
platform 1.5 high, 30 behind the first, facing away from it, to drive off;
and, 60 to the first's left, one more facing x ("racer") with a car parked
side on 30 ahead of it ("parked"), to run into. A scenario's
cue seats the player in one (unit_enter_vehicle ... "stand", the car's one
seat), and its script drives.
*/

#include "../stagekit.h"

/* (the generator links no trigonometry, as the game's maths is renamed:
README.md) a sine and a cosine by their series, to a float's precision for
the small angles here */
static real series_sine(
	real x)
{
	return x * (1.f - x * x / 6.f * (1.f - x * x / 20.f * (1.f - x * x / 42.f * (1.f - x * x / 72.f))));
}

static real series_cosine(
	real x)
{
	return 1.f - x * x / 2.f * (1.f - x * x / 12.f * (1.f - x * x / 30.f * (1.f - x * x / 56.f)));
}

static void drive_build(
	struct stage *stage)
{
	/* (StageKit's own round numbers) */
	real ramp_pitch = stage_parameter("ramp_pitch", 12.f);
	real_rectangle3d ramp = { { -4.f, 4.f, -3.f, 3.f, -1.f, 0.f } };
	real_rectangle3d platform = { { -6.f, 6.f, -4.f, 4.f, 0.f, 1.5f } };
	real_point3d car = { { 0.f, 0.f, 0.f } };
	real_point3d racer = { { 0.f, 60.f, 0.f } };
	real_point3d parked = { { 30.f, 60.f, 0.f } };
	real_point3d high = { { -33.f, 0.f, 1.5f } };
	real_point3d ramp_center;
	real_point3d platform_center = { { -36.f, 0.f, 0.f } };
	real_point3d start = { { 0.f, -3.f, 0.f } };

	/* (the ramp's top face from the floor at 24 to its height at 24 + 8
	along x: a box 8 long, its top face at its own z = 0, pitched up) */
	ramp_center.x = 24.f + 4.f * series_cosine(DEGREES_TO_RADIANS(ramp_pitch));
	ramp_center.y = 0.f;
	ramp_center.z = 4.f * series_sine(DEGREES_TO_RADIANS(ramp_pitch));
	prop_turned_obstacle(stage, "stagekit\\ramp", &ramp, &ramp_center, 0.f, ramp_pitch, 0.f);
	prop_obstacle(stage, "stagekit\\platform", &platform, &platform_center);
	prop_vehicle(stage, "car", prop_car(), &car, 0.f);
	prop_vehicle(stage, "high", prop_car(), &high, 180.f);
	prop_vehicle(stage, "racer", prop_car(), &racer, 0.f);
	prop_vehicle(stage, "parked", prop_car(), &parked, 90.f);
	player_start(stage, &start);
}

struct stagekit_scenario const scenario_drive = {
	"drive", "cars to drive: open floor, a ramp to jump, a parked car, a platform to drive off", drive_build };
