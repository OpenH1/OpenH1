/*
THROW.C

The player at the middle of the floor with no weapon and four grenades in
hand, which it throws (debug.test_input "throw": its throw animation lets go
a third of a second after the press) at 9 units a second along its aim under
the engine's gravity (lob.c's grenade, its fuse 6 seconds from its making,
bouncing on a stone floor): tests/test_throw.py, and the scorecard's
grenade_thrown, each grenade's flight at every rate. The grenade's and the
throw's values (stage_parameter: grenade_speed, _gravity, _fuse, _bounce,
_friction; throw_speed) the scorecard's hidden scenarios may vary.
*/

#include "../stagekit.h"

static void throw_build(
	struct stage *stage)
{
	struct projectile_shape grenade;
	real_point3d start = { { 0.f, 0.f, 0.f } };

	/* (lob.c's grenade: StageKit's own round numbers; its speed the throw's, at
	its making in the hand, which the throw sets) */
	grenade.speed = stage_parameter("grenade_speed", 9.f);
	grenade.gravity_scale = stage_parameter("grenade_gravity", 1.f);
	grenade.fuse = stage_parameter("grenade_fuse", 6.f);
	grenade.bounce = stage_parameter("grenade_bounce", 0.4f);
	grenade.friction = stage_parameter("grenade_friction", 0.5f);
	stage_chamber_material(stage, _material_stone);
	/* (StageKit's own: four grenades, thrown at 9 units a second) */
	prop_grenades(stage, prop_projectile("stagekit\\grenade", &grenade), 4, stage_parameter("throw_speed", 9.f));
	player_start(stage, &start);
}

struct stagekit_scenario const scenario_throw = {
	"throw", "grenades thrown by hand, which land and go off on a fuse", throw_build };
