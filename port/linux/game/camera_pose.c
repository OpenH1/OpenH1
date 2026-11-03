/*
CAMERA_POSE.C

The camera put at an exact place and facing from the console
(debug_camera_set), for pictures of a map made from numbers: the script's
camera (camera_control's), set with no transition, the observer cleared
first so that it takes the camera at once as its first command (in the step
the script's camera takes on being set), so the next frame is drawn from
there, also with the game's time stopped (game_speed 0), when the observer
takes no steps of its own. The first-person weapon is not drawn from the
script's camera; the HUD is show_hud's.
*/

#include "cseries.h"
#include "camera_pose.h"
#include "camera/director.h"
#include "camera/observer.h"
#include "main/console.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void scripted_camera_set_camera_point_absolute(real_point3d const *position, real_vector3d const *forward,
	real_vector3d const *up, real field_of_view, word transition_time);

#define DEGREES_TO_RADIANS (3.14159265358979f / 180.0f)

void camera_pose_set(char const *pose)
{
	float x, y, z, yaw, pitch, roll, field_of_view;
	float cy, sy, cp, sp, cr, sr;
	real_point3d position;
	real_vector3d forward, level_up, right, up;

	if (pose && strcmp(pose, "off") == 0)
	{
		director_script_camera(FALSE);
		return;
	}
	if (!pose || sscanf(pose, "%f %f %f %f %f %f %f", &x, &y, &z, &yaw, &pitch, &roll, &field_of_view) != 7 ||
		field_of_view <= 0.0f || field_of_view >= 180.0f)
	{
		console_printf(FALSE, "debug_camera_set \"<x> <y> <z> <yaw> <pitch> <roll> <field of view>\" (degrees), or \"off\"");
		return;
	}
	cy = cosf(yaw * DEGREES_TO_RADIANS);
	sy = sinf(yaw * DEGREES_TO_RADIANS);
	cp = cosf(pitch * DEGREES_TO_RADIANS);
	sp = sinf(pitch * DEGREES_TO_RADIANS);
	cr = cosf(roll * DEGREES_TO_RADIANS);
	sr = sinf(roll * DEGREES_TO_RADIANS);
	position.x = x;
	position.y = y;
	position.z = z;
	forward.i = cy * cp;
	forward.j = sy * cp;
	forward.k = sp;
	/* (up before the roll, and right: forward cross it) */
	level_up.i = -cy * sp;
	level_up.j = -sy * sp;
	level_up.k = cp;
	right.i = forward.j * level_up.k - forward.k * level_up.j;
	right.j = forward.k * level_up.i - forward.i * level_up.k;
	right.k = forward.i * level_up.j - forward.j * level_up.i;
	up.i = cr * level_up.i + sr * right.i;
	up.j = cr * level_up.j + sr * right.j;
	up.k = cr * level_up.k + sr * right.k;
	director_script_camera(TRUE);
	observer_obsolete_position(0);
	scripted_camera_set_camera_point_absolute(&position, &forward, &up, field_of_view * DEGREES_TO_RADIANS, 0);
}
