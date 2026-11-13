/*
MENU_SPARTAN.C

The menus' Spartan in the menus' scene: up to four of them, each a scenery
object of ui.map's (the menus' models' scenery, menu_models.h), the
assault rifle's attached to its hand, playing the standing rifle idle, its
armor the change color the player's profile color gives it. Set dressing:
no unit, player, physics or AI; the engine draws it as it draws the ring, so
every renderer draws it without code of its own.

A screen asks for a Spartan each frame it draws one (menu_spartan_place:
where, in the view, how tall, which color). menu_spartan_frame, at the start
of each frame the menus draw, makes the Spartans asked for in the frame
before, places each before the camera the menus drew that frame with (so
it stands where the screen asked at any aspect ratio), and deletes those no
screen asked for (a screen closed). Without ui.map's Spartan tags (no file,
or one refused: menu_models.c), nothing is made, and the screens draw the
original pictures (menu_spartan_available).
*/

#include "cseries.h"
#include "cache/cache_files.h"
#include "math/real_math.h"
#include "models/model_animation_definitions.h"
#include "objects/objects.h"
#include "objects/scenery.h"
#include "render/render.h"
#include "saved games/player_profile.h"
#include "tag_files/tag_groups.h"

#include "menu_models.h"
#include "menu_spartan.h"

#include <stdlib.h>
#include <string.h>

/* ---------- prototypes */

void platform_log(char const *format, ...);
char *getenv(char const *name);

/* ---------- constants */

#define SPARTAN_IDLE "stand rifle idle"
/* the graph's seat and weapon class whose markers hold the rifle */
#define SPARTAN_SEAT "stand"
#define SPARTAN_WEAPON_CLASS "rifle"
#define SPARTAN_HAND_MARKER "right hand"

enum
{
	SCENERY_TAG = 'scen',
};

/* how tall the cyborg stands, in world units, from its origin at its feet
to the top of its helmet (its idle's) */
#define SPARTAN_HEIGHT 0.70f
/* how far it is turned from facing the camera, to its right (the
originals' pictures': their camera's yaw 172 to its facing) */
#define SPARTAN_TURN_DEGREES 8.0f

/* ---------- structures */

struct menu_spartan
{
	/* asked for in the frame (its frame index), and where */
	long asked_frame;
	real x, y, height;
	short color_index;
	/* made: its objects */
	long object_index;
	long rifle_index;
};

/* ---------- globals */

static struct
{
	struct menu_spartan spartans[MAXIMUM_MENU_SPARTANS];
	/* the tags' generation the objects are of (a map loaded since: none) */
	unsigned long generation;
	long frame;
	long updated_frame;
	long spartan_definition, rifle_definition, animation_graph;
	char hand_marker[TAG_STRING_LENGTH + 1];
	char grip_marker[TAG_STRING_LENGTH + 1];
	/* debug: the frames traced */
	long traced_ticks;
} menu_spartan;

/* ---------- private code */

static void spartan_forget(
	void)
{
	short index;

	for (index = 0; index < MAXIMUM_MENU_SPARTANS; index++)
	{
		menu_spartan.spartans[index].object_index = NONE;
		menu_spartan.spartans[index].rifle_index = NONE;
		menu_spartan.spartans[index].asked_frame = -100;
	}
	menu_spartan.spartan_definition = NONE;
	menu_spartan.rifle_definition = NONE;
	menu_spartan.animation_graph = NONE;
	menu_spartan.generation = cache_files_tags_generation();
}

/* the tags of the map loaded now: the scenery, its graph, and the markers
the graph holds a rifle by (its standing seat's rifle class's, as a unit's:
units.c) */
static void spartan_tags_find(
	void)
{
	long animation_graph;

	spartan_forget();
	menu_spartan.spartan_definition = menu_models_tag(SCENERY_TAG, MENU_MODELS_SPARTAN);
	menu_spartan.rifle_definition = menu_models_tag(SCENERY_TAG, MENU_MODELS_RIFLE);
	strcpy(menu_spartan.hand_marker, SPARTAN_HAND_MARKER);
	menu_spartan.grip_marker[0] = 0;
	if (menu_spartan.spartan_definition == NONE)
		return;
	animation_graph = object_definition_get(menu_spartan.spartan_definition)->object.animation_graph.index;
	menu_spartan.animation_graph = animation_graph;
	if (animation_graph != NONE)
	{
		struct animation_graph *graph = animation_graph_definition_get(animation_graph);
		long seat_index, class_index;

		for (seat_index = 0; seat_index < graph->unit_seats.count; seat_index++)
		{
			struct animation_graph_unit_seat *seat =
				(struct animation_graph_unit_seat *)graph->unit_seats.address + seat_index;

			if (strcmp(seat->label, SPARTAN_SEAT))
				continue;
			for (class_index = 0; class_index < seat->weapon_classes.count; class_index++)
			{
				struct animation_graph_weapon_class *weapon_class =
					(struct animation_graph_weapon_class *)seat->weapon_classes.address + class_index;

				if (strcmp(weapon_class->label, SPARTAN_WEAPON_CLASS))
					continue;
				if (weapon_class->hand_marker_name[0])
				{
					strncpy(menu_spartan.hand_marker, weapon_class->hand_marker_name, TAG_STRING_LENGTH);
					menu_spartan.hand_marker[TAG_STRING_LENGTH] = 0;
				}
				strncpy(menu_spartan.grip_marker, weapon_class->grip_marker_name, TAG_STRING_LENGTH);
				menu_spartan.grip_marker[TAG_STRING_LENGTH] = 0;
			}
		}
	}
}

static void spartan_delete(
	struct menu_spartan *spartan)
{
	/* (the rifle goes with it, attached: deleted first all the same, in
	case it was not) */
	if (spartan->rifle_index != NONE && object_try_and_get(spartan->rifle_index))
		object_delete(spartan->rifle_index);
	if (spartan->object_index != NONE && object_try_and_get(spartan->object_index))
		object_delete(spartan->object_index);
	spartan->object_index = NONE;
	spartan->rifle_index = NONE;
}

/* where a Spartan stands, and which way it faces, for the camera the menus
drew with: its feet at (x, y) in the view (-1 to 1 across and up), as tall
as height of the view's height (0 to 2) */
static void spartan_place_in_view(
	struct menu_spartan const *spartan,
	real_point3d *position,
	real_vector3d *forward,
	real_vector3d *up)
{
	struct render_camera const *camera = &render.camera;
	real half_height = (real)tan(camera->vertical_field_of_view / 2.0f);
	real width = (real)(camera->viewport_bounds.x1 - camera->viewport_bounds.x0);
	real height = (real)(camera->viewport_bounds.y1 - camera->viewport_bounds.y0);
	real aspect = height > 0.0f ? width / height : 16.0f / 9.0f;
	real distance, turn = SPARTAN_TURN_DEGREES * 3.14159265f / 180.0f;
	real_vector3d right, facing;

	if (half_height <= 0.0f)
		half_height = 0.5f;
	distance = SPARTAN_HEIGHT / (MAX(spartan->height, 0.05f) * half_height);
	/* (the camera's right: forward by up, as the world's axes go) */
	right.i = camera->forward.j * camera->up.k - camera->forward.k * camera->up.j;
	right.j = camera->forward.k * camera->up.i - camera->forward.i * camera->up.k;
	right.k = camera->forward.i * camera->up.j - camera->forward.j * camera->up.i;
	position->x = camera->position.x + camera->forward.i * distance +
		right.i * spartan->x * distance * half_height * aspect + camera->up.i * spartan->y * distance * half_height;
	position->y = camera->position.y + camera->forward.j * distance +
		right.j * spartan->x * distance * half_height * aspect + camera->up.j * spartan->y * distance * half_height;
	position->z = camera->position.z + camera->forward.k * distance +
		right.k * spartan->x * distance * half_height * aspect + camera->up.k * spartan->y * distance * half_height;
	/* (it stands upright, facing the camera across the ground, turned) */
	facing.i = -camera->forward.i;
	facing.j = -camera->forward.j;
	facing.k = 0.0f;
	if (facing.i * facing.i + facing.j * facing.j < 1e-6f)
	{
		facing.i = 1.0f;
		facing.j = 0.0f;
	}
	normalize3d(&facing);
	forward->i = facing.i * (real)cos(turn) - facing.j * (real)sin(turn);
	forward->j = facing.i * (real)sin(turn) + facing.j * (real)cos(turn);
	forward->k = 0.0f;
	up->i = 0.0f;
	up->j = 0.0f;
	up->k = 1.0f;
}

static void spartan_color_set(
	long object_index,
	short color_index)
{
	struct object_datum *object = object_try_and_get(object_index);
	real_rgb_color color = player_profile_get_rgb_color(color_index);

	if (!object)
		return;
	object->object.base_change_colors[0] = color;
	object->object.outgoing_change_colors[0] = color;
}

/* a Spartan's objects made where it stands: FALSE if they cannot be */
static boolean spartan_make(
	struct menu_spartan *spartan)
{
	struct object_placement_data data;

	object_placement_data_new(&data, menu_spartan.spartan_definition, NONE);
	spartan_place_in_view(spartan, &data.position, &data.forward, &data.up);
	data.change_colors[0] = player_profile_get_rgb_color(spartan->color_index);
	spartan->object_index = object_new(&data);
	if (spartan->object_index == NONE)
		return FALSE;
	/* (kept active, ticked and drawn, though no player sees its cluster: the
	menus have none) */
	object_set_automatic_deactivation(spartan->object_index, FALSE);
	object_activate(spartan->object_index);
	if (menu_spartan.animation_graph != NONE)
		scenery_animation_start(spartan->object_index, menu_spartan.animation_graph, SPARTAN_IDLE);
	if (menu_spartan.rifle_definition != NONE)
	{
		object_placement_data_new(&data, menu_spartan.rifle_definition, NONE);
		spartan_place_in_view(spartan, &data.position, &data.forward, &data.up);
		spartan->rifle_index = object_new(&data);
		if (spartan->rifle_index != NONE)
		{
			object_set_automatic_deactivation(spartan->rifle_index, FALSE);
			object_activate(spartan->rifle_index);
		}
		if (spartan->rifle_index != NONE)
			object_attach_to_marker(spartan->object_index, menu_spartan.hand_marker, spartan->rifle_index,
				menu_spartan.grip_marker);
	}
	return TRUE;
}

/* the idle played again from its start as it ends (a scenery's animation
holds its last frame: scenery_update) */
static void spartan_idle_loop(
	struct menu_spartan *spartan)
{
	struct object_datum *object = object_try_and_get(spartan->object_index);
	struct animation_graph *graph;
	struct animation *animation;

	if (!object || menu_spartan.animation_graph == NONE || object->object.animation.state.index == NONE)
		return;
	graph = animation_graph_definition_get(menu_spartan.animation_graph);
	if (object->object.animation.state.index < 0 || object->object.animation.state.index >= graph->animations.count)
		return;
	animation = (struct animation *)graph->animations.address + object->object.animation.state.index;
	if (object->object.animation.state.frame_index >= animation->frame_count - 1)
		scenery_animation_start(spartan->object_index, menu_spartan.animation_graph, SPARTAN_IDLE);
}

/* (debug: each Spartan's idle's frame, a line a second) */
static void spartan_trace(
	void)
{
	short index;

	if (!getenv("HALO_MENU_SPARTAN_TRACE"))
		return;
	if (++menu_spartan.traced_ticks < 30)
		return;
	menu_spartan.traced_ticks = 0;
	for (index = 0; index < MAXIMUM_MENU_SPARTANS; index++)
	{
		struct menu_spartan const *spartan = &menu_spartan.spartans[index];
		struct object_datum *object = spartan->object_index == NONE ? NULL : object_try_and_get(spartan->object_index);

		if (!object)
			continue;
		platform_log("menu spartan: %d: color %d, animation %d frame %d, at %.2f %.2f %.2f, rifle %s", index,
			spartan->color_index, object->object.animation.state.index, object->object.animation.state.frame_index,
			object->object.position.x, object->object.position.y, object->object.position.z,
			spartan->rifle_index != NONE ? "held" : "none");
	}
}

/* ---------- public code */

boolean menu_spartan_available(
	void)
{
	if (menu_spartan.generation != cache_files_tags_generation())
		spartan_tags_find();
	return menu_spartan.spartan_definition != NONE;
}

void menu_spartan_place(
	short index,
	short color_index,
	real x,
	real y,
	real height)
{
	struct menu_spartan *spartan;

	if (index < 0 || index >= MAXIMUM_MENU_SPARTANS || !menu_spartan_available())
		return;
	spartan = &menu_spartan.spartans[index];
	spartan->asked_frame = menu_spartan.frame;
	spartan->x = x;
	spartan->y = y;
	spartan->height = height;
	spartan->color_index = color_index;
}

void menu_spartan_frame(
	void)
{
	short index;

	/* (once a frame: the screens drawn after it ask for this frame's) */
	if (menu_spartan.updated_frame == render.frame_index && menu_spartan.frame)
		return;
	menu_spartan.updated_frame = render.frame_index;
	if (!menu_spartan_available())
	{
		menu_spartan.frame++;
		return;
	}
	/* (debug: Spartans of these colors side by side, on whatever screen) */
	{
		char const *test = getenv("HALO_MENU_SPARTAN_TEST");

		if (test && *test)
		{
			char *end;
			short count = 0;
			long colors[MAXIMUM_MENU_SPARTANS];

			while (count < MAXIMUM_MENU_SPARTANS)
			{
				colors[count] = strtol(test, &end, 10);
				if (end == test)
					break;
				test = end;
				count++;
			}
			for (index = 0; index < count; index++)
			{
				menu_spartan_place(index, (short)colors[index], 0.55f + 0.8f * (index - (count - 1) / 2.0f) / MAX(count, 1),
					-0.75f, 1.3f / MAX(1, count > 2 ? 2 : 1));
			}
		}
	}
	for (index = 0; index < MAXIMUM_MENU_SPARTANS; index++)
	{
		struct menu_spartan *spartan = &menu_spartan.spartans[index];
		boolean asked = spartan->asked_frame == menu_spartan.frame;

		if (spartan->object_index != NONE && !object_try_and_get(spartan->object_index))
		{
			spartan->object_index = NONE;
			spartan->rifle_index = NONE;
		}
		if (!asked)
		{
			if (spartan->object_index != NONE)
				spartan_delete(spartan);
			continue;
		}
		if (spartan->object_index == NONE)
		{
			if (!spartan_make(spartan))
				continue;
		}
		else
		{
			real_point3d position;
			real_vector3d forward, up;

			spartan_place_in_view(spartan, &position, &forward, &up);
			object_set_position(spartan->object_index, &position, &forward, &up);
			spartan_idle_loop(spartan);
		}
		spartan_color_set(spartan->object_index, spartan->color_index);
	}
	spartan_trace();
	menu_spartan.frame++;
}
