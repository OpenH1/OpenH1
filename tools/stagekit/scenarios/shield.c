/*
SHIELD.C

The stage, its players' biped given a body (a collision model) with a body
and a shield of 75 each, the shield recharging in 2 seconds once a second
has passed since it was struck (tests/test_shield.py: the console drops the
shield, which recharges; the scorecard's shield_recharge, the shield at every
rate). Its recharge time (stage_parameter shield_recharge) the scorecard's
hidden scenarios may vary.
*/

#include "../stagekit.h"

static void shield_build(
	struct stage *stage)
{
	/* (StageKit's own round numbers: vitalities of 75, a recharge of 2
	seconds, a stun of 1) */
	prop_biped_shield(stage, 75.f, 75.f, stage_parameter("shield_recharge", 2.f), 1.f);
}

struct stagekit_scenario const scenario_shield = {
	"shield", "a biped with a shield, which recharges", shield_build };
