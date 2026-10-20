/*
LIFT.C

The elevator's platform (elevator.c's: 6 by 6, 0.25 high, rising 5 units in 2
seconds when set to, coming down when set back) with an AI's biped standing
on it, which the console makes an actor of (ai_attach_free rider rider) and
has walk across it and back (ai_command_list_by_unit rider across, then
back: 3 units along x, then 3 back), and the player's start on the floor
beside it: tests/test_lift.py, and the scorecard's elevator_ai_rider
scenarios, the rider's path at every rate (a lift of the macro lane under an
AI's biped of the micro lane).

Two parameters (stage_parameter) set how the lift and its rider meet:

- rider_late 1: the rider is not placed with the map but by the console
  (object_create rider), after the lift, so it is updated after the lift in
  each tick; placed with the map (the bipeds are placed before the machines)
  it is updated before it.
- rider_body 1: the players' biped, and so the rider, has a body, a collision
  model (prop_biped_body), as the game's bipeds do: the lift then carries it
  by its movement (device_machines.c); without one, it rides on the lift's
  floor alone.
*/

#include "../stagekit.h"

static void lift_build(
	struct stage *stage)
{
	/* (elevator.c's platform; StageKit's own round numbers for where the
	rider and the player stand, and the rider's walk) */
	static struct ai_walker_move const across = { 0.f, 3.f }, back = { 180.f, 3.f };
	real_rectangle3d platform = { { -3.f, 3.f, -3.f, 3.f, 0.f, 0.25f } };
	real_point3d position = { { 0.f, 0.f, 0.f } };
	real_point3d rider = { { -1.5f, 0.f, 0.35f } };
	real_point3d start = { { -6.f, 0.f, 0.f } };

	if (stage_parameter("rider_body", 0.f) != 0.f)
		prop_biped_body(stage);
	prop_elevator(stage, "lift", &platform, &position, 5.f, 2.f);
	prop_ai_walker(stage, "rider", "rider", &rider);
	prop_ai_command_list(stage, "across", &across, 1);
	prop_ai_command_list(stage, "back", &back, 1);
	if (stage_parameter("rider_late", 0.f) != 0.f)
	{
		/* (the placement prop_ai_walker made: a biped's, 0x78 bytes,
		props.c's biped_placement, whose object datum comes first) */
		struct scenario_object_datum *placement = (struct scenario_object_datum *)
			((unsigned char *)stage->scenario->bipeds.address + (stage->scenario->bipeds.count - 1) * 0x78);

		placement->placement_flags |= FLAG(_scenario_object_placement_not_automatic_bit);
	}
	player_start(stage, &start);
}

struct stagekit_scenario const scenario_lift = {
	"lift", "an AI's biped rides a platform up and down", lift_build };
