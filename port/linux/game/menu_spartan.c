/*
MENU_SPARTAN.C

The menus' Spartan, in a view of its own among the menus' widgets: up to
four of them, each a scenery object of ui.map's (the menus' models'
scenery, menu_models.h), the assault rifle's attached to its hand, playing
the standing rifle idle, its armor the change color the player's profile
color gives it. Set dressing: no unit, player, physics or AI.

The engine draws them through its own renderer (render_object_alone, as
the scene draws an object), but not in ui.map's scene: the scene's view
leaves them out (menu_spartan_object, render_objects.c), and they are drawn
in a view of their own (rasterizer_view_begin) when the menus draw the
screen's <spartan>, in the widgets' order: over the screen's background and
what is drawn before it, under the widgets drawn after. That view has its
own camera, fixed on the Spartans' stage (where the first was made,
never moved by the scene's camera), its own depth (cleared in its
rectangle: nothing of the scene's, the ring or the sky, can cut it), no
fog, and a fixed light (spartan_lighting) in place of the scene's.

A screen asks for a Spartan each frame it draws one (menu_spartan_place:
where in the menus' 640 by 480, how tall, which color). menu_spartan_frame,
at the start of each frame the menus draw, makes the Spartans asked for in
the frame before, stands each on the stage where its view shows it in its
place, and deletes those no screen asked for (a screen closed). The view's
rectangle is the asked Spartans' boxes, grown so that a rifle or an arm
reaching out of its box is not cut. Without ui.map's Spartan tags (no
file, or one refused: menu_models.c), nothing is made, and the screens draw
the original pictures (menu_spartan_available).
*/

#include "cseries.h"
#include "cache/cache_files.h"
#include "math/real_math.h"
#include "models/model_animation_definitions.h"
#include "objects/objects.h"
#include "objects/scenery.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "render/render_cameras.h"
#include "render/render_cameras_internal.h"
#include "saved games/player_profile.h"
#include "tag_files/tag_groups.h"

#include "menu_models.h"
#include "menu_spartan.h"

#include <stdlib.h>
#include <string.h>

/* ---------- prototypes */

void platform_log(char const *format, ...);
char *getenv(char const *name);
long halo_screen_width(void);
void halo_screen_ui_offset(unsigned char centered);
void halo_hdr_interface(void);

/* ---------- constants */

#define SPARTAN_IDLE "stand rifle idle"
/* the graph's seat and weapon class whose markers hold the rifle */
#define SPARTAN_SEAT "stand"
#define SPARTAN_WEAPON_CLASS "rifle"
#define SPARTAN_HAND_MARKER "right hand"

enum
{
	SCENERY_TAG = 'scen',
	/* the menus' canvas */
	MENUS_WIDTH = 640,
	MENUS_HEIGHT = 480,
};

/* how tall the cyborg stands, in world units, from its origin to the top of
its helmet (its idle's), and how far its boots' soles are under its origin
(stood that much higher, its soles are on its box's foot) */
#define SPARTAN_HEIGHT 0.70f
#define SPARTAN_SOLES 0.026f
/* how far it is turned from facing the camera, to its right (the
originals' pictures': their camera's yaw 172 to its facing) */
#define SPARTAN_TURN_DEGREES 8.0f
/* the view's camera's vertical field of view: a portrait's, narrow, so
that the Spartan is not drawn wider at the top than at the feet */
#define SPARTAN_FIELD_OF_VIEW_DEGREES 30.0f
/* the view's rectangle about a Spartan's box, in its heights: the rifle
and the arms reach out of the box at the sides, the helmet a little above */
#define SPARTAN_VIEW_SIDE 0.45f
#define SPARTAN_VIEW_TOP 0.10f
#define SPARTAN_VIEW_FOOT 0.04f
#define SPARTAN_VIEW_NEAR 0.05f
#define SPARTAN_VIEW_FAR 100.0f

/* ---------- structures */

struct menu_spartan
{
	/* asked for in the frame (its frame index), and where, in the menus'
	640 by 480 of its screen */
	long asked_frame;
	real x, bottom, height;
	short color_index;
	/* made: its objects, and where it stands on the stage */
	long object_index;
	long rifle_index;
	real_point3d position;
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
	/* the stage: where the view's camera stands (set as the first Spartan
	is made, kept while there is one), and the view's rectangle in the
	menus' 640 by 480 of the Spartans' screen (empty: none asked for) */
	boolean staged;
	real_point3d stage;
	real view[4];
	/* debug: the frames drawn since the last trace */
	long traced_frames;
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
	menu_spartan.staged = FALSE;
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

static void spartan_color_set(
	long object_index,
	short color_index)
{
	struct object_datum *object = object_try_and_get(object_index);
	real_rgb_color color = player_profile_get_rgb_color(color_index);

	short index;

	if (!object)
		return;
	/* (every change color, as a player's body has its color: players.c; the
	armor's shader takes its third) */
	for (index = 0; index < NUMBER_OF_OBJECT_CHANGE_COLORS; index++)
	{
		object->object.base_change_colors[index] = color;
		object->object.outgoing_change_colors[index] = color;
	}
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
	/* (scenery_update holds it a frame short of its last) */
	if (object->object.animation.state.frame_index >= animation->frame_count - 2)
		scenery_animation_start(spartan->object_index, menu_spartan.animation_graph, SPARTAN_IDLE);
}

/* the view's camera's axes: fixed in the world, whatever the scene's
camera does (forward along x, up along z, right their product) */
static real_vector3d const spartan_forward = { 1.0f, 0.0f, 0.0f };
static real_vector3d const spartan_up = { 0.0f, 0.0f, 1.0f };
static real_vector3d const spartan_right = { 0.0f, -1.0f, 0.0f };

/* the view's rectangle: the asked Spartans' boxes, grown (FALSE: none) */
static boolean spartan_view_find(
	real view[4])
{
	boolean found = FALSE;
	short index;

	for (index = 0; index < MAXIMUM_MENU_SPARTANS; index++)
	{
		struct menu_spartan const *spartan = &menu_spartan.spartans[index];
		real x0, y0, x1, y1;

		if (spartan->asked_frame != menu_spartan.frame || spartan->height <= 0.0f)
			continue;
		x0 = spartan->x - spartan->height * SPARTAN_VIEW_SIDE;
		x1 = spartan->x + spartan->height * SPARTAN_VIEW_SIDE;
		y0 = spartan->bottom - spartan->height * (1.0f + SPARTAN_VIEW_TOP);
		y1 = spartan->bottom + spartan->height * SPARTAN_VIEW_FOOT;
		if (!found)
		{
			view[0] = x0;
			view[1] = y0;
			view[2] = x1;
			view[3] = y1;
			found = TRUE;
		}
		else
		{
			view[0] = MIN(view[0], x0);
			view[1] = MIN(view[1], y0);
			view[2] = MAX(view[2], x1);
			view[3] = MAX(view[3], y1);
		}
	}
	return found;
}

/* where a Spartan stands on the stage, and which way it faces, for the
view's camera to show it in its place in the view's rectangle: its feet's
middle there, as tall as it was asked */
static void spartan_stand(
	struct menu_spartan const *spartan,
	real_point3d *position,
	real_vector3d *forward,
	real_vector3d *up)
{
	real const *view = menu_spartan.view;
	real half_height = (view[3] - view[1]) * 0.5f;
	real tangent = (real)tan(SPARTAN_FIELD_OF_VIEW_DEGREES * 3.14159265f / 360.0f);
	real turn = SPARTAN_TURN_DEGREES * 3.14159265f / 180.0f;
	/* (across and up from the view's middle, in its half heights, and its
	height in them) */
	real across = (spartan->x - (view[0] + view[2]) * 0.5f) / MAX(half_height, 1.0f);
	real above = ((view[1] + view[3]) * 0.5f - spartan->bottom) / MAX(half_height, 1.0f);
	real tall = MAX(spartan->height, 1.0f) / MAX(half_height, 1.0f);
	real distance = SPARTAN_HEIGHT / (tall * tangent);
	real side = across * distance * tangent, lift = above * distance * tangent + SPARTAN_SOLES;

	position->x = menu_spartan.stage.x + spartan_forward.i * distance + spartan_right.i * side + spartan_up.i * lift;
	position->y = menu_spartan.stage.y + spartan_forward.j * distance + spartan_right.j * side + spartan_up.j * lift;
	position->z = menu_spartan.stage.z + spartan_forward.k * distance + spartan_right.k * side + spartan_up.k * lift;
	/* (upright, facing the camera, turned to its right) */
	forward->i = -spartan_forward.i * (real)cos(turn) + spartan_forward.j * (real)sin(turn);
	forward->j = -spartan_forward.i * (real)sin(turn) - spartan_forward.j * (real)cos(turn);
	forward->k = 0.0f;
	*up = spartan_up;
}

/* the view's light: fixed to its camera's axes, as a portrait's (a key
light from above, before and to the left, a little light from under it,
the rest ambient), lit as the engine lights an object from a lightmap
(object_lights.c's build_distant_lights) */
static void spartan_lighting(
	struct render_lighting *lighting)
{
	real_vector3d toward_light;

	csmemset(lighting, 0, sizeof(*lighting));
	lighting->ambient_color.red = 0.30f;
	lighting->ambient_color.green = 0.31f;
	lighting->ambient_color.blue = 0.34f;
	lighting->distant_light_count = 2;
	toward_light.i = -0.65f * spartan_forward.i + 0.65f * spartan_up.i - 0.40f * spartan_right.i;
	toward_light.j = -0.65f * spartan_forward.j + 0.65f * spartan_up.j - 0.40f * spartan_right.j;
	toward_light.k = -0.65f * spartan_forward.k + 0.65f * spartan_up.k - 0.40f * spartan_right.k;
	normalize3d(&toward_light);
	/* (a distant light's direction is the way its light goes) */
	lighting->distant_lights[0].color.red = 0.85f;
	lighting->distant_lights[0].color.green = 0.84f;
	lighting->distant_lights[0].color.blue = 0.80f;
	lighting->distant_lights[0].direction.i = -toward_light.i;
	lighting->distant_lights[0].direction.j = -toward_light.j;
	lighting->distant_lights[0].direction.k = -toward_light.k;
	lighting->distant_lights[1].color.red = 0.16f;
	lighting->distant_lights[1].color.green = 0.17f;
	lighting->distant_lights[1].color.blue = 0.20f;
	lighting->distant_lights[1].direction = spartan_up;
	lighting->point_light_count = 0;
	lighting->reflection_tint_color.alpha = 0.9f;
	lighting->reflection_tint_color.red = 0.75f;
	lighting->reflection_tint_color.green = 0.75f;
	lighting->reflection_tint_color.blue = 0.75f;
	lighting->shadow_vector.k = -1.0f;
	lighting->shadow_color.red = 1.0f;
	lighting->shadow_color.green = 1.0f;
	lighting->shadow_color.blue = 1.0f;
}

/* a Spartan's objects made where it stands: FALSE if they cannot be */
static boolean spartan_make(
	struct menu_spartan *spartan)
{
	struct object_placement_data data;
	short color;

	object_placement_data_new(&data, menu_spartan.spartan_definition, NONE);
	spartan_stand(spartan, &data.position, &data.forward, &data.up);
	for (color = 0; color < NUMBER_OF_OBJECT_CHANGE_COLORS; color++)
		data.change_colors[color] = player_profile_get_rgb_color(spartan->color_index);
	spartan->object_index = object_new(&data);
	if (spartan->object_index == NONE)
		return FALSE;
	spartan->position = data.position;
	/* (kept active, ticked, though no player sees its cluster: the menus
	have none) */
	object_set_automatic_deactivation(spartan->object_index, FALSE);
	object_activate(spartan->object_index);
	if (menu_spartan.animation_graph != NONE)
		scenery_animation_start(spartan->object_index, menu_spartan.animation_graph, SPARTAN_IDLE);
	if (menu_spartan.rifle_definition != NONE)
	{
		object_placement_data_new(&data, menu_spartan.rifle_definition, NONE);
		spartan_stand(spartan, &data.position, &data.forward, &data.up);
		spartan->rifle_index = object_new(&data);
		if (spartan->rifle_index != NONE)
		{
			object_set_automatic_deactivation(spartan->rifle_index, FALSE);
			object_activate(spartan->rifle_index);
			object_attach_to_marker(spartan->object_index, menu_spartan.hand_marker, spartan->rifle_index,
				menu_spartan.grip_marker);
		}
	}
	return TRUE;
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
	real bottom,
	real height)
{
	struct menu_spartan *spartan;

	if (index < 0 || index >= MAXIMUM_MENU_SPARTANS || !menu_spartan_available())
		return;
	spartan = &menu_spartan.spartans[index];
	spartan->asked_frame = menu_spartan.frame;
	spartan->x = x;
	spartan->bottom = bottom;
	spartan->height = height;
	spartan->color_index = color_index;
}

short menu_spartan_test_colors(
	short colors[MAXIMUM_MENU_SPARTANS])
{
	char const *test = getenv("HALO_MENU_SPARTAN_TEST");
	short count = 0;

	while (test && *test && count < MAXIMUM_MENU_SPARTANS)
	{
		char *end;
		long color = strtol(test, &end, 10);

		if (end == test)
			break;
		colors[count++] = (short)color;
		test = end;
	}
	return count;
}

boolean menu_spartan_object(
	long object_index)
{
	short index;

	if (object_index == NONE)
		return FALSE;
	for (index = 0; index < MAXIMUM_MENU_SPARTANS; index++)
	{
		if (menu_spartan.spartans[index].object_index == object_index ||
			menu_spartan.spartans[index].rifle_index == object_index)
		{
			return TRUE;
		}
	}
	return FALSE;
}

void menu_spartan_frame(
	void)
{
	short index;
	boolean any = FALSE;

	/* (once a frame: the screens drawn after it ask for this frame's) */
	if (menu_spartan.updated_frame == render.frame_index && menu_spartan.frame)
		return;
	menu_spartan.updated_frame = render.frame_index;
	if (!menu_spartan_available())
	{
		menu_spartan.frame++;
		return;
	}
	if (!spartan_view_find(menu_spartan.view))
		csmemset(menu_spartan.view, 0, sizeof(menu_spartan.view));
	for (index = 0; index < MAXIMUM_MENU_SPARTANS; index++)
	{
		struct menu_spartan *spartan = &menu_spartan.spartans[index];

		if (spartan->object_index != NONE && !object_try_and_get(spartan->object_index))
		{
			spartan->object_index = NONE;
			spartan->rifle_index = NONE;
		}
		any |= spartan->object_index != NONE;
	}
	/* (the stage where the scene's camera is as the first is made: inside
	the map, where nothing of the scene is drawn in the Spartans' view, nor
	they in the scene's) */
	if (!any)
	{
		menu_spartan.stage = render.camera.position;
		menu_spartan.staged = TRUE;
	}
	for (index = 0; index < MAXIMUM_MENU_SPARTANS; index++)
	{
		struct menu_spartan *spartan = &menu_spartan.spartans[index];
		boolean asked = spartan->asked_frame == menu_spartan.frame;

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

			/* (moved only as its place in the view changes: the screen's
			layout, the screen's shape) */
			spartan_stand(spartan, &position, &forward, &up);
			if (memcmp(&position, &spartan->position, sizeof(position)))
			{
				object_set_position(spartan->object_index, &position, &forward, &up);
				spartan->position = position;
			}
			spartan_idle_loop(spartan);
		}
		spartan_color_set(spartan->object_index, spartan->color_index);
	}
	menu_spartan.frame++;
}

void menu_spartan_draw(
	short x,
	short y)
{
	real const *view = menu_spartan.view;
	long ui_offset = (halo_screen_width() - MENUS_WIDTH) / 2;
	struct render_camera camera, scene_camera;
	struct render_frustum frustum, scene_frustum;
	struct render_lighting lighting;
	real_rectangle2d bounds;
	short index, drawn = 0;

	if (!menu_spartan.staged || view[2] <= view[0] || view[3] <= view[1])
		return;
	/* the view's camera: on the stage, its window the view's rectangle on
	the screen (the menus' 640 centred in its width), its viewport that
	rectangle's part on the screen */
	csmemset(&camera, 0, sizeof(camera));
	camera.position = menu_spartan.stage;
	camera.forward = spartan_forward;
	camera.up = spartan_up;
	camera.vertical_field_of_view = SPARTAN_FIELD_OF_VIEW_DEGREES * 3.14159265f / 180.0f;
	camera.z_near = SPARTAN_VIEW_NEAR;
	camera.z_far = SPARTAN_VIEW_FAR;
	camera.window_bounds.x0 = (short)(view[0] + x + ui_offset);
	camera.window_bounds.y0 = (short)(view[1] + y);
	camera.window_bounds.x1 = (short)(view[2] + x + ui_offset);
	camera.window_bounds.y1 = (short)(view[3] + y);
	camera.viewport_bounds.x0 = (short)MAX(camera.window_bounds.x0, 0);
	camera.viewport_bounds.y0 = (short)MAX(camera.window_bounds.y0, 0);
	camera.viewport_bounds.x1 = (short)MIN(camera.window_bounds.x1, halo_screen_width());
	camera.viewport_bounds.y1 = (short)MIN(camera.window_bounds.y1, MENUS_HEIGHT);
	if (camera.viewport_bounds.x1 <= camera.viewport_bounds.x0 ||
		camera.viewport_bounds.y1 <= camera.viewport_bounds.y0)
	{
		return;
	}
	render_camera_build_frustum_bounds(&camera, &bounds);
	render_camera_build_frustum(&camera, &bounds, &frustum, TRUE);
	spartan_lighting(&lighting);
	/* (the renderer's camera the view's while it draws, as the models' level
	of detail is chosen by it; the scene's again after) */
	scene_camera = render.camera;
	scene_frustum = render.frustum;
	render.camera = camera;
	render.frustum = frustum;
	/* (its viewport where it is on the screen, not shifted with the menus) */
	halo_screen_ui_offset(FALSE);
	rasterizer_view_begin(&camera, &frustum);
	rasterizer_models_begin(FALSE);
	for (index = 0; index < MAXIMUM_MENU_SPARTANS; index++)
	{
		struct menu_spartan const *spartan = &menu_spartan.spartans[index];

		if (spartan->object_index == NONE || !object_try_and_get(spartan->object_index))
			continue;
		render_object_alone(spartan->object_index, &lighting);
		drawn++;
	}
	rasterizer_models_end();
	rasterizer_view_end();
	halo_screen_ui_offset(TRUE);
	halo_hdr_interface();
	render.camera = scene_camera;
	render.frustum = scene_frustum;
	/* (debug: where each is drawn, and the scene's camera, a line a
	second or so) */
	if (getenv("HALO_MENU_SPARTAN_TRACE") && ++menu_spartan.traced_frames >= 60)
	{
		menu_spartan.traced_frames = 0;
		platform_log("menu spartan: view %d %d %d %d (viewport %d %d %d %d), %d drawn; scene camera at %.3f %.3f %.3f "
			"facing %.3f %.3f %.3f", camera.window_bounds.x0, camera.window_bounds.y0, camera.window_bounds.x1,
			camera.window_bounds.y1, camera.viewport_bounds.x0, camera.viewport_bounds.y0, camera.viewport_bounds.x1,
			camera.viewport_bounds.y1, drawn, scene_camera.position.x, scene_camera.position.y, scene_camera.position.z,
			scene_camera.forward.i, scene_camera.forward.j, scene_camera.forward.k);
		for (index = 0; index < MAXIMUM_MENU_SPARTANS; index++)
		{
			struct menu_spartan const *spartan = &menu_spartan.spartans[index];
			struct object_datum *object = spartan->object_index == NONE ? NULL : object_try_and_get(spartan->object_index);
			real_point2d feet, head;
			real_point3d top;

			if (!object)
				continue;
			top = object->object.position;
			top.z += SPARTAN_HEIGHT;
			feet.x = feet.y = head.x = head.y = 0.0f;
			render_camera_world_to_screen(&camera, &frustum, &object->object.position, &feet);
			render_camera_world_to_screen(&camera, &frustum, &top, &head);
			platform_log("menu spartan: %d: color %d, animation %d frame %d, on the screen feet %.1f %.1f head %.1f %.1f, "
				"facing %.3f %.3f %.3f up %.3f %.3f %.3f, rifle %s", index, spartan->color_index,
				object->object.animation.state.index, object->object.animation.state.frame_index, feet.x, feet.y,
				head.x, head.y, object->object.forward.i, object->object.forward.j, object->object.forward.k,
				object->object.up.i, object->object.up.j, object->object.up.k,
				spartan->rifle_index != NONE ? "held" : "none");
		}
	}
}
