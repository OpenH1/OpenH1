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
#include "objects/objects.h"
#include "tag_files/tag_groups.h"
#include "units/units.h"

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

/* ---------- objects placed for the picture (debug_object_place) */

#define MAXIMUM_PLACED_OBJECTS 16

static long placed_objects[MAXIMUM_PLACED_OBJECTS];
static short placed_object_count;

/* the loaded tag of that name, in the first of the object groups that has it */
static long camera_pose_object_tag(char const *name)
{
	static long const groups[] = { 'vehi', 'bipd', 'weap', 'eqip', 'scen', 'mach', 'ctrl', 'lifi', 'plac', 'ssce', 'garb' };
	short i;

	for (i = 0; i < (short)NUMBEROF(groups); i++)
	{
		long tag_index = tag_loaded(groups[i], name);

		if (tag_index != NONE)
		{
			return tag_index;
		}
	}
	return NONE;
}

void camera_pose_place_object(char const *line)
{
	char name[256], seat[64];
	float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
	float r, g, b;
	char const *color;
	long tag_index, object_index;
	struct object_placement_data data;

	if (line && strcmp(line, "clear") == 0)
	{
		short i;

		for (i = 0; i < placed_object_count; i++)
		{
			if (object_header_try_and_get(placed_objects[i]))
			{
				object_delete(placed_objects[i]);
			}
		}
		placed_object_count = 0;
		return;
	}
	seat[0] = 0;
	if (!line ||
		(sscanf(line, "%255s seat %63s", name, seat) != 2 &&
		 sscanf(line, "%255s %f %f %f %f %f %f", name, &x, &y, &z, &yaw, &pitch, &roll) < 5))
	{
		console_printf(FALSE, "debug_object_place \"<tag> <x> <y> <z> <yaw> [<pitch> <roll>] [color <r> <g> <b>]\", \"<tag> seat <label>\", or \"clear\"");
		return;
	}
	tag_index = camera_pose_object_tag(name);
	if (tag_index == NONE)
	{
		console_printf(FALSE, "debug_object_place: no object tag %s in this map", name);
		return;
	}
	if (placed_object_count >= MAXIMUM_PLACED_OBJECTS)
	{
		console_printf(FALSE, "debug_object_place: no more than %d objects", MAXIMUM_PLACED_OBJECTS);
		return;
	}
	object_placement_data_new(&data, tag_index, NONE);
	if (seat[0])
	{
		/* (made at the last placed vehicle, then put in its seat) */
		if (placed_object_count == 0 || !object_header_try_and_get(placed_objects[placed_object_count - 1]))
		{
			console_printf(FALSE, "debug_object_place: no vehicle placed for the seat");
			return;
		}
		object_get_origin(placed_objects[placed_object_count - 1], &data.position);
		data.position.z += 1.0f;
	}
	else
	{
		float cy = cosf(yaw * DEGREES_TO_RADIANS), sy = sinf(yaw * DEGREES_TO_RADIANS);
		float cp = cosf(pitch * DEGREES_TO_RADIANS), sp = sinf(pitch * DEGREES_TO_RADIANS);
		float cr = cosf(roll * DEGREES_TO_RADIANS), sr = sinf(roll * DEGREES_TO_RADIANS);
		real_vector3d level_up, right;

		data.position.x = x;
		data.position.y = y;
		data.position.z = z;
		data.forward.i = cy * cp;
		data.forward.j = sy * cp;
		data.forward.k = sp;
		level_up.i = -cy * sp;
		level_up.j = -sy * sp;
		level_up.k = cp;
		right.i = data.forward.j * level_up.k - data.forward.k * level_up.j;
		right.j = data.forward.k * level_up.i - data.forward.i * level_up.k;
		right.k = data.forward.i * level_up.j - data.forward.j * level_up.i;
		data.up.i = cr * level_up.i + sr * right.i;
		data.up.j = cr * level_up.j + sr * right.j;
		data.up.k = cr * level_up.k + sr * right.k;
	}
	color = strstr(line, " color ");
	if (color && sscanf(color, " color %f %f %f", &r, &g, &b) == 3)
	{
		short i;

		for (i = 0; i < NUMBER_OF_OBJECT_CHANGE_COLORS; i++)
		{
			data.change_colors[i].red = r;
			data.change_colors[i].green = g;
			data.change_colors[i].blue = b;
		}
	}
	object_index = object_new(&data);
	if (object_index == NONE)
	{
		console_printf(FALSE, "debug_object_place: %s was not made", name);
		return;
	}
	if (seat[0])
	{
		unit_scripting_enter_vehicle(object_index, placed_objects[placed_object_count - 1], seat);
	}
	placed_objects[placed_object_count++] = object_index;
}
