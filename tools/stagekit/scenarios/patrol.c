/*
PATROL.C

An AI's biped in the chamber, beside the player's start: the console makes an
actor of it (ai_attach_free walker walker) and has it walk command lists
(ai_command_list_by_unit walker <list>), one after another: 3 units along x
(leg1), 2 at 25 degrees from it (leg2), then 2 along x again (leg3). It sets
off only within about 30 degrees of the way it faces, which it turns to only
as it moves (StageKit's biped has no turning animations), so each leg turns
from the one before by 25 degrees: tests/test_patrol.py, and the scorecard's
AI scenario, its path at every rate.
*/

#include "../stagekit.h"

static void patrol_build(
	struct stage *stage)
{
	/* (StageKit's own round numbers: a walk of three legs, two units from the player) */
	static struct ai_walker_move const leg1 = { 0.f, 3.f }, leg2 = { 25.f, 2.f }, leg3 = { 0.f, 2.f };
	real_point3d position = { { 2.f, 0.f, 0.1f } };

	prop_ai_walker(stage, "walker", "walker", &position);
	prop_ai_command_list(stage, "leg1", &leg1, 1);
	prop_ai_command_list(stage, "leg2", &leg2, 1);
	prop_ai_command_list(stage, "leg3", &leg3, 1);
}

struct stagekit_scenario const scenario_patrol = {
	"patrol", "an AI's biped walks a command list", patrol_build };
