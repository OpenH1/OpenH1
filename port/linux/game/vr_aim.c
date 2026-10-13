/*
VR_AIM.C

The VR mode's aim with the right controller (HALO_VR; vr.aim =
"controller", port/linux/README.md "VR"): the game's side of it, for
player_control.c.

The reticle is drawn where the controller's line meets the world
(vr_render.c, vr_render_crosshairs). The game fires from its own eye, the
unit's camera, crouched or not (weapons.c, unit_adjust_projectile_ray), and
judges its red reticle and its bullets' autoaim from there along the
player's facing (aim_assist.c). Given the controller's direction alone, the
game's line was the controller's moved to its eye: parallel to it, 30 to
50 cm away with the weapon held low and to the right (more with the head
leaning, or walking in the room away from the eye), so that with the
reticle on a grunt's middle the shot went over its head, and the reticle
did not turn red. The game is given instead the direction from its eye to
the point the reticle is on (vr_aim_converge): the shot, the red reticle and
the bullets' autoaim then agree with the reticle at any range. In a
vehicle's seat, from where the game's camera aims the seat's weapon from
(director_camera_deterministic, which its bullets are aimed by).

Only the facing changes, which the netcode sends as it always has: a
network game's host fires the client's weapon from its own copy of the same
eye, along the same facing.
*/

#ifdef HALO_VR

#include "cseries.h"
#include "math/real_math.h"
#include "ai/ai_script.h"
#include "cache/cache_files.h"
#include "camera/director.h"
#include "game/game.h"
#include "game/players.h"
#include "objects/objects.h"
#include "physics/collision_usage.h"
#include "physics/collisions.h"
#include "tag_files/tag_files.h"
#include "units/biped_definitions.h"
#include "units/bipeds.h"
#include "units/units.h"

#include "../src/vr.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* the point the aim converges on no nearer along the controller's line
than this (metres), nor nearer the game's eye along it than this many times
the eye's distance from the line (the hand held out from the eye, or a
vehicle's camera behind it and above): nearer (a wall at arm's length) the
game's aim would swing by tens of degrees as the line crossed edges near
and far; at these, by 18 degrees at most, and the shot meets so near a wall
a few centimetres from the reticle anyway */
#define CONVERGE_NEAREST 1.5f
#define CONVERGE_NEAREST_TIMES 3.0f
/* debug.vr_test_target: the target placed this many seconds after the
player first aims, and again this many after it dies */
#define TEST_TARGET_SECONDS 3
#define TEST_TARGET_AGAIN_SECONDS 2

void platform_log(char const *format, ...);
char const *config_string(char const *name);
int config_boolean(char const *name);
/* vr_render.c's */
boolean vr_render_aim_hit(const float position[3], const float forward[3], real yaw, real_point3d *origin,
	real_vector3d *direction, real *distance, real *units);
void vr_render_world_to_head(real_point3d const *point, real_vector3d const *forward, real_vector3d const *up,
	float position[3], float head_forward[3], float head_up[3]);

static struct
{
	/* debug.vr_test_target: the target placed (NONE: none, or it died),
	the tick the player first aimed at, the tick the next is placed at, and
	the target's vitality last tick */
	long target_index;
	long first_aim_tick;
	long place_tick;
	real_vector3d test_forward;
	real target_body;
	real target_shield;
	/* debug.vr_aim_log: the tick logged last */
	long logged_tick;
} vr_aim = { NONE, NONE, NONE, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, NONE };

/* a point in the world as game axes from the recentred head (metres) */
static void vr_aim_to_head(real_point3d const *point, float local[3])
{
	float forward[3], up[3];

	vr_render_world_to_head(point, global_forward3d, global_up3d, local, forward, up);
}

/* the middle of a biped's autoaim pill (its body, as the game's autoaim
sees it) */
static void vr_aim_target_centre(long biped_index, real_point3d *centre)
{
	real_point3d base;
	real_vector3d height;
	real width;

	biped_get_autoaim_pill(biped_index, &base, &height, &width);
	centre->x = base.x + height.i * 0.5f;
	centre->y = base.y + height.j * 0.5f;
	centre->z = base.z + height.k * 0.5f;
}

/* debug.vr_test_target's place: ahead and right of the player along
forward (world units), on the ground there, turned left or right as little
as puts its middle in sight of the player's eye and of a hand half a metre
below it (by 10 degrees at a time, 60 at most; else ahead); forward is turned with it. The turn (degrees left). */
static real vr_aim_test_place(long unit_index, real_vector3d *forward, real ahead, real right, real_point3d *place)
{
	struct unit_datum *unit = unit_get(unit_index);
	real_point3d eye;
	real_vector3d facing = *forward;
	short step;

	unit_get_camera_position(unit_index, &eye);
	if (global_current_collision_user_depth >= MAXIMUM_COLLISION_USER_STACK_DEPTH)
		return 0.0f;
	global_current_collision_users[global_current_collision_user_depth++] = _collision_user_ui;
	for (step = 0; step <= 12; step++)
	{
		real turn = (step & 1 ? 1.0f : -1.0f) * (real)((step + 1) / 2) * 10.0f * _pi / 180.0f;
		struct collision_result collision;
		real_point3d above, middle, low;
		real_vector3d down = { 0.0f, 0.0f, -6.0f / 3.048f }, sight, low_sight;

		forward->i = facing.i * (real)cos(turn) - facing.j * (real)sin(turn);
		forward->j = facing.i * (real)sin(turn) + facing.j * (real)cos(turn);
		forward->k = 0.0f;
		place->x = unit->object.position.x + forward->i * ahead + forward->j * right;
		place->y = unit->object.position.y + forward->j * ahead - forward->i * right;
		place->z = unit->object.position.z;
		/* (on the ground there: a beach slopes, and placed in it, it falls
		through the world) */
		above = *place;
		above.z += 3.0f / 3.048f;
		if (collision_test_vector(FLAG(_collision_test_structure_bit), &above, &down, NONE, &collision))
			place->z = collision.point.z + 0.1f / 3.048f;
		middle = *place;
		middle.z += 0.4f / 3.048f;
		vector_from_points3d(&eye, &middle, &sight);
		low = eye;
		low.z -= 0.5f / 3.048f;
		vector_from_points3d(&low, &middle, &low_sight);
		if (!collision_test_vector(FLAG(_collision_test_structure_bit), &eye, &sight, NONE, &collision) &&
			!collision_test_vector(FLAG(_collision_test_structure_bit), &low, &low_sight, NONE, &collision))
		{
			--global_current_collision_user_depth;
			return turn * 180.0f / _pi;
		}
	}
	--global_current_collision_user_depth;
	*forward = facing;
	place->x = unit->object.position.x + forward->i * ahead + forward->j * right;
	place->y = unit->object.position.y + forward->j * ahead - forward->i * right;
	place->z = unit->object.position.z;
	return 0.0f;
}

/* debug.vr_test_target: "<metres ahead> [<metres right> [<name>]]", a
biped whose tag's name has this in it ("grunt" if none) placed before the
player (TEST_TARGET_SECONDS after they first aim), standing still with no
mind of its own, facing them, and placed again once it dies; the level's AI
erased; the right controller held by debug.vr_test_hands points at its
middle (vr.c), and each hit on it is logged. For automated tests of the
aim. */
static void vr_aim_test_target(long unit_index, boolean aiming)
{
	char name[64] = "grunt";
	real ahead = 0.0f, right = 0.0f;
	struct biped_datum *target;
	long now = game_time_get();

	if (sscanf(config_string("debug.vr_test_target"), "%f %f %63s", &ahead, &right, name) < 1 || unit_index == NONE)
	{
		halo_vr_test_point(NULL);
		return;
	}
	if (vr_aim.first_aim_tick != NONE && now < vr_aim.first_aim_tick)
	{
		/* (a new map, or a revert) */
		vr_aim.first_aim_tick = vr_aim.place_tick = NONE;
		vr_aim.target_index = NONE;
	}
	if (vr_aim.first_aim_tick == NONE && aiming)
	{
		vr_aim.first_aim_tick = now;
		vr_aim.place_tick = now + TEST_TARGET_SECONDS * TICKS_PER_SECOND;
		player_control_get_facing_direction(0, &vr_aim.test_forward);
	}
	if (vr_aim.place_tick != NONE || vr_aim.target_index != NONE)
	{
		/* (the level's own AI erased, as the script's ai_erase_all does, once
		a second from the first target on: none but the player's shots hit
		the target, and nothing shoots the player) */
		if ((now - vr_aim.first_aim_tick) % TICKS_PER_SECOND == 0 && now >= vr_aim.place_tick)
			ai_scripting_erase_all();
	}
	target = vr_aim.target_index != NONE ? biped_try_and_get(vr_aim.target_index) : NULL;
	if (vr_aim.target_index != NONE && (!target || TEST_FLAG(target->object.damage_flags, _object_dead_bit)))
	{
		platform_log("vr aim: test target %s at tick %ld", target ? "killed" : "gone", now);
		vr_aim.target_index = NONE;
		vr_aim.place_tick = now + TEST_TARGET_AGAIN_SECONDS * TICKS_PER_SECOND;
		target = NULL;
	}
	if (!target && vr_aim.place_tick != NONE && now >= vr_aim.place_tick)
	{
		struct tag_iterator iterator;
		struct object_placement_data placement;
		struct unit_datum *unit = unit_get(unit_index);
		real_vector3d forward;
		/* (the world's metres: a world unit is 10 feet) */
		real units = 1.0f / 3.048f;
		real turn;
		long tag_index;

		vr_aim.place_tick = NONE;
		tag_iterator_new(&iterator, BIPED_DEFINITION_TAG);
		for (tag_index = tag_iterator_next(&iterator); tag_index != NONE; tag_index = tag_iterator_next(&iterator))
		{
			char const *tag_name = tag_get_name(tag_index);

			if (tag_name && strstr(tag_name, name))
				break;
		}
		forward = vr_aim.test_forward;
		forward.k = 0.0f;
		if (tag_index == NONE || normalize3d(&forward) <= 0.0f)
		{
			platform_log("vr aim: no test target \"%s\" in this map", name);
			return;
		}
		object_placement_data_new(&placement, tag_index, NONE);
		turn = vr_aim_test_place(unit_index, &forward, ahead * units, right * units, &placement.position);
		placement.forward.i = -forward.i;
		placement.forward.j = -forward.j;
		placement.forward.k = 0.0f;
		placement.up = *global_up3d;
		vr_aim.target_index = object_new(&placement);
		target = vr_aim.target_index != NONE ? biped_try_and_get(vr_aim.target_index) : NULL;
		if (!target)
		{
			if (vr_aim.target_index != NONE)
				object_delete(vr_aim.target_index);
			vr_aim.target_index = NONE;
			platform_log("vr aim: the test target \"%s\" could not be placed", name);
			return;
		}
		vr_aim.target_body = target->object.body_vitality;
		vr_aim.target_shield = target->object.shield_vitality;
		platform_log("vr aim: test target %s placed at tick %ld, %.1f m ahead and %.1f m right (turned %.0f degrees "
			"left to be in sight)", tag_get_name(tag_index), now, ahead, right, turn);
	}
	if (!target)
	{
		halo_vr_test_point(NULL);
		return;
	}
	if (target->object.body_vitality != vr_aim.target_body || target->object.shield_vitality != vr_aim.target_shield)
	{
		platform_log("vr aim: test target hit at tick %ld: body %.2f -> %.2f, shield %.2f -> %.2f", now,
			vr_aim.target_body, target->object.body_vitality, vr_aim.target_shield, target->object.shield_vitality);
		vr_aim.target_body = target->object.body_vitality;
		vr_aim.target_shield = target->object.shield_vitality;
	}
	{
		/* the controller pointed at its middle, from where it is */
		float position[3], forward[3], centre_local[3], direction[3];
		real_point3d centre;
		real yaw;
		int axis;

		vr_aim_target_centre(vr_aim.target_index, &centre);
		if (!halo_vr_aim_line(position, forward, &yaw))
			return;
		vr_aim_to_head(&centre, centre_local);
		for (axis = 0; axis < 3; axis++)
			direction[axis] = centre_local[axis] - position[axis];
		halo_vr_test_point(direction);
	}
}

/* debug.vr_aim_log: once a tick, the aim: where the controller's line
meets the world, how far the game's aim was turned to meet it there, how far
from that point the unit's aim passes (as it fires: weapons.c), and the
reticle red or not (the game's autoaim, as the HUD has it); with
debug.vr_test_target, how far the target's middle is from the game's aim
and from the controller's line */
static void vr_aim_log(real_point3d const *origin, real_vector3d const *direction, real distance, real units,
	real_point3d const *eye, real_vector3d const *aim)
{
	long now = game_time_get();
	real turned = (real)acos(PIN(dot_product3d(direction, aim), -1.0f, 1.0f)) * 180.0f / _pi;
	struct biped_datum *target = vr_aim.target_index != NONE ? biped_try_and_get(vr_aim.target_index) : NULL;
	char text[160] = "";

	if (now == vr_aim.logged_tick || !config_boolean("debug.vr_aim_log"))
		return;
	vr_aim.logged_tick = now;
	if (target)
	{
		real_point3d centre;
		real_vector3d to_target;
		real from_aim, from_line;

		vr_aim_target_centre(vr_aim.target_index, &centre);
		vector_from_points3d(eye, &centre, &to_target);
		normalize3d(&to_target);
		from_aim = (real)acos(PIN(dot_product3d(&to_target, aim), -1.0f, 1.0f)) * 180.0f / _pi;
		vector_from_points3d(origin, &centre, &to_target);
		normalize3d(&to_target);
		from_line = (real)acos(PIN(dot_product3d(&to_target, direction), -1.0f, 1.0f)) * 180.0f / _pi;
		snprintf(text, sizeof(text), "; the target's middle %.2f degrees from the game's aim, %.2f from the "
			"controller's line", from_aim, from_line);
	}
	{
		/* where the unit aims now (its aiming vector, the game's facing of
		the tick before reached as fast as the unit turns), from its eye:
		how far from the reticle's point it passes */
		long player_index = local_player_get_player_index(0);
		struct unit_datum *unit = player_index != NONE ? unit_try_and_get(player_get(player_index)->unit_index) : NULL;
		real_point3d point;
		real_vector3d to_point;
		real along, miss = -1.0f;

		point.x = origin->x + direction->i * distance;
		point.y = origin->y + direction->j * distance;
		point.z = origin->z + direction->k * distance;
		if (unit)
		{
			vector_from_points3d(eye, &point, &to_point);
			along = dot_product3d(&to_point, &unit->unit.aiming_vector);
			miss = (real)sqrt(MAX(magnitude_squared3d(&to_point) - along * along, 0.0f)) / units;
		}
		platform_log("vr aim: tick %ld, the line meets the world %.2f m out, the controller %.2f m from the game's "
			"eye, the aim turned %.2f degrees to it, the unit's aim passing %.0f cm from the point; autoaim %.2f%s%s",
			now, distance / units, (real)sqrt(distance_squared3d(origin, eye)) / units, turned, miss * 100.0f,
			player_control_get_autoaim_level(0), player_control_get_autoaim_level(0) >= 1.0f ? " (red)" : "", text);
	}
}

/* player_control.c, each update of the first local player (aiming: the VR
mode aims, halo_vr_aim's 1): the player's facing, as halo_vr_aim gave it
along the controller, turned to the direction from the game's eye to where
the controller's line meets the world, as the reticle is drawn (no nearer
than CONVERGE_NEAREST); left as it is without the controller's line (the
head or a gamepad aims, no frame drawn yet) */
void vr_aim_converge(long unit_index, boolean aiming, real *yaw, real *pitch)
{
	float position[3], forward[3];
	real_point3d origin, eye, point;
	real_vector3d direction, camera_forward, aim;
	real body_yaw, distance, hit, units, nearest;

	vr_aim_test_target(unit_index, aiming);
	if (!aiming || unit_index == NONE || !halo_vr_aim_line(position, forward, &body_yaw) ||
		!vr_render_aim_hit(position, forward, body_yaw, &origin, &direction, &distance, &units))
	{
		return;
	}
	director_camera_deterministic(unit_index, &eye, &camera_forward);
	{
		/* (the eye along the line from the controller, and across it) */
		real_vector3d from_line;
		real along, across;

		vector_from_points3d(&origin, &eye, &from_line);
		along = dot_product3d(&from_line, &direction);
		across = (real)sqrt(MAX(magnitude_squared3d(&from_line) - along * along, 0.0f));
		nearest = MAX(CONVERGE_NEAREST * units, CONVERGE_NEAREST_TIMES * across + along);
	}
	hit = distance;
	distance = MAX(distance, nearest);
	point.x = origin.x + direction.i * distance;
	point.y = origin.y + direction.j * distance;
	point.z = origin.z + direction.k * distance;
	vector_from_points3d(&eye, &point, &aim);
	if (normalize3d(&aim) <= 0.0f)
		return;
	*yaw = (real)atan2(aim.j, aim.i);
	*pitch = (real)asin(PIN(aim.k, -1.0f, 1.0f));
	vr_aim_log(&origin, &direction, hit, units, &eye, &aim);
}

#endif
