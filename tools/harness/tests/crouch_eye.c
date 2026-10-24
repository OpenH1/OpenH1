/*
CROUCH_EYE.C

The player's own view on foot between the steps (render_interpolation.c's
render_interpolation_newest_camera): drawn where the newest step left the
unit, moved on for the time since by its velocity, and its eye by its
crouch's change (bipeds.c's biped_eye_rise_ahead). The crouch changes a
tick's change a tick (biped_physics_prepare's, crouch_tick here: the game's
own lines), toward the seat the biped is in, so a frame a fraction of a tick
after a step draws the eye that fraction of the way to the next tick's, and
never past where the crouch stops.
*/

#include "harness.h"
#include <math.h>

#include "config.inc"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

enum { _biped_airborne_bit };

struct biped_datum
{
	long definition_index;
	struct
	{
		struct
		{
			short base_seat_index;
		} animation;
	} unit;
	struct
	{
		unsigned long flags;
		real crouch;
	} biped;
};

struct biped_definition
{
	struct
	{
		real collision_height_standing;
		real collision_height_crouching;
		real standing_camera_height;
		real crouching_camera_height;
		real runtime_crouch_transition_velocity;
	} biped;
};

static struct biped_datum fake_biped;
static struct biped_definition fake_definition;

static struct biped_datum *biped_try_and_get(long index)
{
	return index == NONE ? NULL : &fake_biped;
}

static struct biped_definition *biped_definition_get(long index)
{
	(void)index;
	return &fake_definition;
}

#include "under_test.inc"

/* (StageKit's biped's: a unit tall, half crouched, its eye from 0.9 to 0.45,
crouching in a quarter second) */
static void biped_setup(real crouch, short seat)
{
	memset(&fake_biped, 0, sizeof(fake_biped));
	fake_biped.biped.crouch = crouch;
	fake_biped.unit.animation.base_seat_index = seat;
	fake_definition.biped.collision_height_standing = 1.f;
	fake_definition.biped.collision_height_crouching = 0.5f;
	fake_definition.biped.standing_camera_height = 0.9f;
	fake_definition.biped.crouching_camera_height = 0.45f;
	fake_definition.biped.runtime_crouch_transition_velocity = 1.f / (0.25f * 30.f);
}

/* the eye's height above the feet (biped_get_sight_position's) */
static real eye(real crouch)
{
	return (1.f - crouch) * fake_definition.biped.standing_camera_height +
		crouch * fake_definition.biped.crouching_camera_height;
}

/* the frames between each tick and the next, ticks of them from the biped as
set up: drawn on the line between the ticks' eyes, the last frame at the
next tick's, never past where the crouch stops; returns the ticks it took */
static int frames_follow_the_ticks(int ticks)
{
	static real const sinces[] = { 0.125f, 0.25f, 0.5f, 0.75f, 1.f };
	int tick;
	int moving = 0;

	for (tick = 0; tick < ticks; tick++)
	{
		struct biped_datum now = fake_biped;
		struct biped_datum next;
		real from = eye(fake_biped.biped.crouch);
		real to;
		real change;
		unsigned int i;

		/* (the next tick's crouch, and the frames drawn from this one's) */
		crouch_tick(&fake_biped, &fake_definition);
		next = fake_biped;
		fake_biped = now;
		to = eye(next.biped.crouch);
		change = fabsf(next.biped.crouch - fake_biped.biped.crouch);
		for (i = 0; i < sizeof(sinces) / sizeof(sinces[0]); i++)
		{
			real drawn = from + biped_eye_rise_ahead(0, sinces[i]);

			/* (a whole tick's change: on the line; the tick it stops in, no
			further than where it stops) */
			if (fabsf(change - fake_definition.biped.runtime_crouch_transition_velocity) < 1e-6f)
			{
				CHECK(fabsf(drawn - (from + (to - from) * sinces[i])) < 1e-6f,
					"tick %d, %.3f of the way: eye %f, the line's %f (from %f to %f)", tick, sinces[i], drawn,
					from + (to - from) * sinces[i], from, to);
			}
			else
			{
				CHECK(fabsf(drawn - from) <= fabsf(to - from) + 1e-6f && (drawn - from) * (to - from) >= 0.f,
					"tick %d, %.3f of the way: eye %f, past where it stops (from %f to %f)", tick, sinces[i], drawn,
					from, to);
			}
		}
		CHECK(fabsf(from + biped_eye_rise_ahead(0, 1.f) - to) < 1e-6f, "tick %d: a tick on, eye %f, the tick's %f",
			tick, from + biped_eye_rise_ahead(0, 1.f), to);
		if (change > 0.f)
			moving++;
		fake_biped = next;
	}
	return moving;
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "crouching"))
	{
		biped_setup(0.f, _unit_base_seat_crouch);
		CHECK(frames_follow_the_ticks(12) == 8, "crouched in other than 8 ticks");
		CHECK(fake_biped.biped.crouch == 1.f, "crouch %f", fake_biped.biped.crouch);
	}
	else if (!strcmp(name, "standing"))
	{
		biped_setup(1.f, _unit_base_seat_stand);
		CHECK(frames_follow_the_ticks(12) == 8, "stood in other than 8 ticks");
		CHECK(fake_biped.biped.crouch == 0.f, "crouch %f", fake_biped.biped.crouch);
	}
	else if (!strcmp(name, "reversed"))
	{
		/* (crouch let go of half way down: up again from there, three
		whole ticks' change and a part of one) */
		biped_setup(0.5f, _unit_base_seat_stand);
		CHECK(frames_follow_the_ticks(6) == 4, "stood in other than 4 ticks");
		CHECK(fake_biped.biped.crouch == 0.f, "crouch %f", fake_biped.biped.crouch);
	}
	else if (!strcmp(name, "still"))
	{
		/* (all the way down crouched, or up standing, or no biped: the eye stays) */
		biped_setup(1.f, _unit_base_seat_crouch);
		CHECK(biped_eye_rise_ahead(0, 0.5f) == 0.f, "crouched: %f", biped_eye_rise_ahead(0, 0.5f));
		biped_setup(0.f, _unit_base_seat_stand);
		CHECK(biped_eye_rise_ahead(0, 0.5f) == 0.f, "standing: %f", biped_eye_rise_ahead(0, 0.5f));
		biped_setup(0.5f, _unit_base_seat_crouch);
		CHECK(biped_eye_rise_ahead(0, 0.f) == 0.f, "no time on: %f", biped_eye_rise_ahead(0, 0.f));
		CHECK(biped_eye_rise_ahead(NONE, 0.5f) == 0.f, "no biped: %f", biped_eye_rise_ahead(NONE, 0.5f));
	}
	else
	{
		fprintf(stderr, "no case %s\n", name);
		return 2;
	}
	return 0;
}
