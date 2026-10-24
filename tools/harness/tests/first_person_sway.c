/*
FIRST_PERSON_SWAY.C

The first-person weapon's sways (first_person_weapons.c) between their
steps: first_person_weapon_update steps the springs of the position (the
unit's throttle: strafing, walking), the turning and the firing push back a
tick at a time (sways_step here: the game's own lines), and a frame drawn
from the newest step has them a phase of a tick past the last
(first_person_weapon_springs_at). The position's and the turning's are drawn
from where they were before the step to where it left them, so the frames
meet each step where it begins; the push back's on the way to its next step,
which a shot's kick reaches at once.
*/

#include "harness.h"
#include <math.h>

#define _pi 3.14159265358979f
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CEILING(n, ceiling) ((n) > (ceiling) ? (ceiling) : (n))
#define PIN(n, floor, ceiling) ((n) < (floor) ? (floor) : CEILING((n), (ceiling)))

typedef struct { real i, j; } real_vector2d;
typedef struct { real yaw, pitch; } real_euler_angles2d;

#include "config.inc"

struct first_person_weapon
{
	boolean rendered;
	real_vector2d position;
	real_vector2d position_velocity;
	real_vector2d turning;
	real_vector2d turning_velocity;
	real firing_push_back;
	real firing_push_back_velocity;
	real_euler_angles2d last_render_facing;
	real_euler_angles2d render_facing;
};

struct unit_datum
{
	struct
	{
		real_vector3d throttle;
	} unit;
};

static struct first_person_weapon_sways first_person_weapon_sways_before[1];

#include "under_test.inc"

static struct first_person_weapon weapon;
static struct unit_datum unit;

/* the sways a frame draws, a phase of a tick past the last step */
struct drawn
{
	real_vector2d position;
	real_vector2d turning;
	real firing_push_back;
};

static struct drawn drawn_at(real phase)
{
	struct drawn drawn;

	first_person_weapon_springs_at(&weapon, &first_person_weapon_sways_before[0], phase, &drawn.position,
		&drawn.turning, &drawn.firing_push_back);
	return drawn;
}

static real apart(struct drawn const *a, struct drawn const *b)
{
	return fabsf(a->position.i - b->position.i) + fabsf(a->position.j - b->position.j) +
		fabsf(a->turning.i - b->turning.i) + fabsf(a->turning.j - b->turning.j) +
		fabsf(a->firing_push_back - b->firing_push_back);
}

/* (between a and b, as far as t) */
static int between(real value, real a, real b)
{
	return value >= fminf(a, b) - 1e-6f && value <= fmaxf(a, b) + 1e-6f;
}

/* a tick: the frames of the tick before drawn on to its end, the step, and
the frames after it: each sway starts where the frames before it left it,
goes no further than its step's ends on the way (the position's and the
turning's), and ends a tick on where the step left it */
static void tick_followed(int tick)
{
	static real const phases[] = { 0.125f, 0.25f, 0.5f, 0.75f };
	struct drawn end_of_tick = drawn_at(1.f);
	struct drawn start;
	unsigned int i;

	sways_step(&weapon, &unit, 0);
	start = drawn_at(0.f);
	CHECK(apart(&start, &end_of_tick) < 1e-5f,
		"tick %d: the frames jump at the step: position %f %f turning %f %f push back %f, before it %f %f, %f %f, %f",
		tick, start.position.i, start.position.j, start.turning.i, start.turning.j, start.firing_push_back,
		end_of_tick.position.i, end_of_tick.position.j, end_of_tick.turning.i, end_of_tick.turning.j,
		end_of_tick.firing_push_back);
	for (i = 0; i < sizeof(phases) / sizeof(phases[0]); i++)
	{
		struct drawn drawn = drawn_at(phases[i]);
		struct first_person_weapon_sways const *before = &first_person_weapon_sways_before[0];

		CHECK(between(drawn.position.i, before->position.i, weapon.position.i) &&
			between(drawn.position.j, before->position.j, weapon.position.j) &&
			between(drawn.turning.i, before->turning.i, weapon.turning.i) &&
			between(drawn.turning.j, before->turning.j, weapon.turning.j),
			"tick %d, %.3f of the way: position %f %f off the step's way from %f %f to %f %f", tick, phases[i],
			drawn.position.i, drawn.position.j, before->position.i, before->position.j, weapon.position.i,
			weapon.position.j);
	}
	{
		struct drawn whole = drawn_at(1.f);

		CHECK(fabsf(whole.position.i - weapon.position.i) < 1e-6f && fabsf(whole.position.j - weapon.position.j) < 1e-6f &&
			fabsf(whole.turning.i - weapon.turning.i) < 1e-6f && fabsf(whole.turning.j - weapon.turning.j) < 1e-6f,
			"tick %d: a tick on, position %f %f, the step's %f %f", tick, whole.position.i, whole.position.j,
			weapon.position.i, weapon.position.j);
	}
}

static void weapon_setup(void)
{
	memset(&weapon, 0, sizeof(weapon));
	memset(&unit, 0, sizeof(unit));
	memset(first_person_weapon_sways_before, 0, sizeof(first_person_weapon_sways_before));
	weapon.rendered = TRUE;
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";
	int tick;

	weapon_setup();
	if (!strcmp(name, "strafing"))
	{
		/* (A and D held in turn, a fifth of a second each, then a tenth, then
		walking on: the sway speeding up and slowing down all the way) */
		for (tick = 0; tick < 90; tick++)
		{
			unit.unit.throttle.j = tick < 60 ? ((tick / 6) % 2 ? -1.f : 1.f) : ((tick / 3) % 2 ? -1.f : 1.f);
			unit.unit.throttle.i = tick >= 75 ? 1.f : 0.f;
			tick_followed(tick);
		}
		CHECK(fabsf(weapon.position.i) > 0.1f, "never walked: %f", weapon.position.i);
	}
	else if (!strcmp(name, "turning"))
	{
		/* (the view turning on, faster, and back) */
		for (tick = 0; tick < 60; tick++)
		{
			real turn = tick < 20 ? 0.01f : tick < 40 ? 0.03f : -0.02f;

			weapon.last_render_facing = weapon.render_facing;
			weapon.render_facing.yaw += turn;
			weapon.render_facing.pitch += turn * 0.5f;
			tick_followed(tick);
		}
		CHECK(fabsf(weapon.turning.i) > 0.1f, "never turned: %f", weapon.turning.i);
	}
	else if (!strcmp(name, "fired"))
	{
		/* (a shot between two steps: its kick drawn at once, from the next
		frame on, then the push back eased off a step at a time as before) */
		real before_shot;
		real after_shot;

		for (tick = 0; tick < 3; tick++)
			tick_followed(tick);
		before_shot = drawn_at(0.5f).firing_push_back;
		weapon.firing_push_back_velocity += FIRST_PERSON_WEAPON_FIRING_PUSH_BACK_VELOCITY;
		after_shot = drawn_at(0.5f).firing_push_back;
		CHECK(after_shot > before_shot + 0.01f, "the kick not drawn at once: %f, %f before the shot", after_shot,
			before_shot);
		for (; tick < 40; tick++)
			tick_followed(tick);
		CHECK(weapon.firing_push_back < 0.01f, "never eased off: %f", weapon.firing_push_back);
	}
	else
	{
		fprintf(stderr, "no case %s\n", name);
		return 2;
	}
	return 0;
}
