/*
LOB.C

The player at the middle of the floor with a launcher of grenades: thrown at
9 units a second (grenade_speed) under the engine's gravity (grenade_gravity
scales it), they stop where they first land and go off 6 seconds after they
are thrown (grenade_fuse), one a second at most (tools/scorecard's grenade
scenario).
*/

#include "../stagekit.h"

static void lob_build(
	struct stage *stage)
{
	struct projectile_shape grenade;
	real_point3d start = { { 0.f, 0.f, 0.f } };

	/* (StageKit's own round numbers) */
	grenade.speed = stage_parameter("grenade_speed", 9.f);
	grenade.gravity_scale = stage_parameter("grenade_gravity", 1.f);
	grenade.fuse = stage_parameter("grenade_fuse", 6.f);
	grenade.bounce = stage_parameter("grenade_bounce", 0.4f);
	grenade.friction = stage_parameter("grenade_friction", 0.5f);
	/* (the chamber of a material, to bounce on: StageKit's own choice) */
	stage_chamber_material(stage, _material_stone);
	player_loadout(stage, prop_weapon("stagekit\\launcher", prop_projectile("stagekit\\grenade", &grenade),
		stage_parameter("launcher_rate", 1.f)));
	player_start(stage, &start);
}

struct stagekit_scenario const scenario_lob = {
	"lob", "a launcher of grenades that land and go off on a fuse", lob_build };
