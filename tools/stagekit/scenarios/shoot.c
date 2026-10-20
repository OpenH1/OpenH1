/*
SHOOT.C

The player at the middle of the floor with a rifle whose shots fly straight
at 120 units a second (bullet_speed; bullet_gravity scales the engine's
gravity on them, 0 by default) and go off where they strike, five a second;
and walls to shoot at: one whose face is 2 units ahead (+x), one 10 units to
the left (+y) and one 40 units behind (-x) (wall_near, wall_mid, wall_far),
each 2 high (tools/scorecard's first_shot and impact scenarios).
*/

#include "../stagekit.h"

static void shoot_build(
	struct stage *stage)
{
	struct projectile_shape bullet;
	real near = stage_parameter("wall_near", 2.f);
	real mid = stage_parameter("wall_mid", 10.f);
	real far = stage_parameter("wall_far", 40.f);
	real_rectangle3d across_x = { { 0.f, 0.5f, -1.f, 1.f, 0.f, 2.f } };
	real_rectangle3d across_y = { { -2.f, 2.f, 0.f, 0.5f, 0.f, 2.f } };
	real_rectangle3d behind = { { -0.5f, 0.f, -4.f, 4.f, 0.f, 2.f } };
	real_point3d start = { { 0.f, 0.f, 0.f } };
	real_point3d near_position = { { 0.f, 0.f, 0.f } };
	real_point3d mid_position = { { 0.f, 0.f, 0.f } };
	real_point3d far_position = { { 0.f, 0.f, 0.f } };

	/* (StageKit's own round numbers) */
	bullet.speed = stage_parameter("bullet_speed", 120.f);
	bullet.gravity_scale = stage_parameter("bullet_gravity", 0.f);
	bullet.fuse = 0.f;
	bullet.bounce = bullet.friction = 0.f;
	player_loadout(stage, prop_weapon("stagekit\\rifle", prop_projectile("stagekit\\bullet", &bullet),
		stage_parameter("rifle_rate", 5.f)));
	near_position.x = near;
	mid_position.y = mid;
	far_position.x = -far;
	prop_obstacle(stage, "stagekit\\wall_near", &across_x, &near_position);
	prop_obstacle(stage, "stagekit\\wall_mid", &across_y, &mid_position);
	prop_obstacle(stage, "stagekit\\wall_far", &behind, &far_position);
	player_start(stage, &start);
}

struct stagekit_scenario const scenario_shoot = {
	"shoot", "a rifle of straight, fast shots and walls 2, 10 and 40 away", shoot_build };
