/*
LEDGE.C

A ledge: a block 2 high, from x -6 to 1 and y -5 to 5, with the player's
start on it, a unit from its edge at x 1 and facing it; past the edge the
chamber's floor, 2 below (tests/test_ledge.py: the player walks off it and
lands on the floor; the scorecard's ledge_fall, the fall at every rate). Its
height (stage_parameter ledge_height) the scorecard's hidden scenarios may
vary.
*/

#include "../stagekit.h"

static void ledge_build(
	struct stage *stage)
{
	/* (StageKit's own round numbers: a drop of 2, which the player's biped,
	immune to falling damage, survives) */
	real height = stage_parameter("ledge_height", 2.f);
	real_rectangle3d block = { { -6.f, 1.f, -5.f, 5.f, 0.f, height } };
	real_point3d position = { { 0.f, 0.f, 0.f } };
	real_point3d start = { { 0.f, 0.f, height } };

	prop_obstacle(stage, "stagekit\\ledge", &block, &position);
	player_start(stage, &start);
}

struct stagekit_scenario const scenario_ledge = {
	"ledge", "a ledge the player walks off and falls from", ledge_build };
