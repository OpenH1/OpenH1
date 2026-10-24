/*
CROUCH.C

The stage, its players' biped able to crouch (a crouching seat in its
animations): stage.c's crouch, from a unit tall to half of it in a quarter
second, its eye from 0.9 to 0.45 (tests/test_crouch.py: the player crouches
and stands again; the scorecard's crouch_*, the crouch and the eye at every
rate).
*/

#include "../stagekit.h"

static void crouch_build(
	struct stage *stage)
{
	prop_biped_crouch(stage);
}

struct stagekit_scenario const scenario_crouch = {
	"crouch", "a biped that crouches and stands again", crouch_build };
