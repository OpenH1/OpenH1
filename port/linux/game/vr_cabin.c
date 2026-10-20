/*
VR_CABIN.C

The VR mode's cabins: what a vehicle's model lacks for the view from one of
its seats, and where the eye is there (HALO_VR; vr.vehicle_view,
vr.scorpion_view, port/linux/README.md "VR"), for vr_seat.c,
render_objects.c and render.c.

With vr.vehicle_view = "first_person" the eye is where the seated player's
head is (vr_seat.c). The vehicles' models were made for the game's camera
following them, not to be sat in: the Scorpion is a closed hull, its
driver's head in the hatch's lid over an empty slot in its top, seeing that
slot's walls about him and the sky through the lid. So for a seat of a
vehicle's model, and one of its views where the vehicle has a setting for it,
a cabin (vr_cabin_model.h, made by tools/vr_cabin_model.py) puts the eye
where it belongs, and draws about it what the model lacks, with the model's
own shaders (its textures, its lighting, its tint), each mesh from a node of
the model as the model is drawn:

- the Scorpion's driver, inside (vr.scorpion_view = "inside"): a compartment
  at the front of the hull, under the front plate, the driver seated in it,
  with vision blocks in its front, which the world is seen through (the
  hull's own faces about it face out, and are not drawn from inside);
- the Scorpion's commander (vr.scorpion_view = "hatch"): the eye
  above the turret, on its axis, so that it does not swing as the turret
  turns, a cupola ring with its lid open about it on the turret.
- the Scorpion's driver in the feed cockpit (vr.scorpion_view = "feed", the
  default): a closed compartment inside the hull, which the eye rides into
  and out of with the body's climb (vr_seat.c); about the eye in it the
  vehicle is not drawn (vr_cabin_hides_model): only the world is seen
  through the display, and not the armour the eye passes through. Its
  front wall curves gently about the eye, and the display is an opening in
  it, 85 degrees across and 40 high, which each eye sees the world through
  at its own place, as through a window: the world at its true size and
  depth. A grate of bars slides down over it once the eye is in, then the
  feed washes over the opening (xgpu_gl.c, vr.c's halo_vr_cockpit_display:
  the smart-link's look, its frame and readouts), and the bars are gone;
  getting out, it collapses dark as a screen switched off. Below it, on a holotable on the dash, the tank
  itself: its own model drawn a hand's length long with the shields' glow,
  its turret posed as the tank's is (turning to where the gun aims), tilted
  as the hull is from level.
- the Banshee's pilot: the model's canopy closes over the pilot's head in
  flight, and faces out, so only the nose was seen below; now a cockpit
  about the pilot: on the hull, a sill at the shoulders, a console ahead
  with the Covenant's glowing panels and two handles; on the canopy (so that
  it opens with it), a rail along each side, a spine overhead, a bow ahead
  framing the view, and a shell behind the head. The view ahead and above
  stays open, as through the canopy.

The view stays the player's, level and turned by the head and the stick (as
in any seat; how the view turns in a Banshee is not this file's): the Scorpion turns toward its driver's facing (the game's
steering, vehicles.c), so a view turned with the hull would chase its own
turn; the hull turns under the eye toward where the player looks, as it
does under the game's camera.

Each cabin is found by its model's tag name and its nodes and shaders by
theirs as a map is loaded, and made once a map (vertex and index buffers,
as a model part's); it is drawn only for the vehicle the first player is
seated in, once the eye is in the seat. Nothing is drawn where the model,
a node or a shader is not in the map.
*/

#ifdef HALO_VR

#include "cseries.h"
#include "math/real_math.h"
#include "game/players.h"
#include "models/model_definitions.h"
#include "models/models.h"
#include "items/weapons.h"
#include "items/weapon_definitions.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_geometry.h"
#include "rasterizer/rasterizer_model_types.h"
#include "shaders/shader_definitions.h"
#include "shaders/shaders.h"
#include "tag_files/tag_groups.h"
#include "tag_files/tag_files.h"
#include "units/units.h"
#include "units/unit_definitions.h"

#include "vr_cabin_model.h"
#include "../src/vr.h"

#include <math.h>
#include <string.h>

/* the platform layer's (port/linux/src/port_config.c) */
const char *config_string(char const *name);
void platform_log(char const *format, ...);
/* vr_seat.c's */
boolean vr_seat_seated_shown(void);
boolean vr_seat_feed(real *grate, real *wash, real *off);
boolean vr_seat_shown(real_point3d *position);
short vr_seat_feed_cabin(long *vehicle_index);
/* vr_render.c's */
void vr_render_world_to_head(real_point3d const *point, real_vector3d const *forward, real_vector3d const *up,
	float position[3], float head_forward[3], float head_up[3]);
real vr_render_scope_range(void);
boolean vr_render_scope_pass(void);
/* vr_holster.c's */
void vr_holster_hologram(long unit_index, real units, struct render_model_effect *model_effect,
	struct render_lighting *lighting);
/* vr.c's */
void halo_vr_cockpit_display(const struct halo_vr_cockpit_display *display);

/* a model's shader's tag group */
#define SHADER_MODEL_TAG 'soso'
/* the vehicle not drawn about an eye within this of a feed cockpit's box
(world units: 15 cm) */
#define FEED_HIDE_MARGIN (0.15f / METRES_PER_WORLD_UNIT)
/* a feed cockpit's grate, open: lifted this far into the ceiling (world
units) */
#define GRATE_TRAVEL 0.300f
/* the hologram drawn as a model this many pixels across would be (its
level of detail) */
#define HOLOGRAM_DETAIL_PIXELS 400.0f
#define METRES_PER_WORLD_UNIT 3.048f

enum
{
	NUMBER_OF_CABINS = NUMBEROF(vr_cabin_models),
	/* (models.c's) */
	_shader_type_model = 4,
	_shader_model_alpha_blended_decal_bit = 3,
};

/* a model's shader (models.c's: no header declares it) */
struct vr_cabin_shader_reference
{
	struct tag_reference shader;
	short permutation_index;
	word pad;
	long unused[3];
};

/* models.c's shader of a model's, its flags */
struct vr_cabin_shader_model
{
	struct shader shader;
	word flags;
};

/* each cabin's (vr_cabin_models'): whether this map has it (FALSE as the
game starts, before any map), its vehicle's model in this map, and each
mesh's node, shader and its permutation, and buffers */
static struct vr_cabin
{
	boolean loaded;
	long model_index;
	struct vr_cabin_mesh
	{
		short node_index;
		struct shader *shader;
		short permutation_index;
		struct vertex_buffer vertex_buffer;
		struct triangle_buffer triangle_buffer;
	} meshes[VR_CABIN_MODEL_MAXIMUM_MESHES];
} vr_cabins[NUMBER_OF_CABINS];

static void vector_set_normalized(real_vector3d *vector, real i, real j, real k)
{
	real length = (real)sqrt(i * i + j * j + k * k);

	if (length < 1e-9f)
	{
		vector->i = 0.0f;
		vector->j = 0.0f;
		vector->k = 1.0f;
		return;
	}
	vector->i = i / length;
	vector->j = j / length;
	vector->k = k / length;
}

/* a mesh's vertices (in the model's space, all its node's) and triangles,
from its table */
static void mesh_build(struct vr_cabin_model_mesh const *definition, struct model_vertex_uncompressed *vertices,
	word *triangles)
{
	long index;

	memset(vertices, 0, sizeof(*vertices) * definition->vertex_count);
	for (index = 0; index < definition->vertex_count; index++)
	{
		struct vr_cabin_model_vertex const *source = &definition->vertices[index];
		struct model_vertex_uncompressed *vertex = &vertices[index];
		real_vector3d other;

		vertex->position.x = source->position[0];
		vertex->position.y = source->position[1];
		vertex->position.z = source->position[2];
		vector_set_normalized(&vertex->normal, source->normal[0], source->normal[1], source->normal[2]);
		/* (the tangent in the face, across it: up crossed with the normal, or
		forward on a floor or a ceiling; the binormal from the two, as the
		model's) */
		if (fabs(vertex->normal.k) < 0.9f)
			vector_set_normalized(&other, -vertex->normal.j, vertex->normal.i, 0.0f);
		else
			vector_set_normalized(&other, 1.0f, 0.0f, 0.0f);
		vertex->tangent = other;
		cross_product3d(&vertex->normal, &vertex->tangent, &vertex->binormal);
		vector_set_normalized(&vertex->binormal, vertex->binormal.i, vertex->binormal.j, vertex->binormal.k);
		vertex->texcoord.x = source->texcoord[0];
		vertex->texcoord.y = source->texcoord[1];
		vertex->node_weights[0] = 1.0f;
	}
	memcpy(triangles, definition->triangles, sizeof(*triangles) * 3 * definition->triangle_count);
}

/* the model's shader reference with this tag name, else NULL */
static struct vr_cabin_shader_reference const *shader_find(struct model *model, char const *shader_name)
{
	short shader_index;

	for (shader_index = 0; shader_index < model->shaders.count; shader_index++)
	{
		struct vr_cabin_shader_reference const *candidate =
			TAG_BLOCK_GET_ELEMENT(&model->shaders, shader_index, struct vr_cabin_shader_reference);
		char const *name = candidate->shader.index != NONE ? tag_get_name(candidate->shader.index) : NULL;

		if (name && !strcmp(name, shader_name))
			return candidate;
	}
	return NULL;
}

static void cabin_dispose(struct vr_cabin *cabin, short mesh_count)
{
	short mesh_index;

	for (mesh_index = 0; mesh_index < mesh_count; mesh_index++)
	{
		rasterizer_triangle_buffer_delete(&cabin->meshes[mesh_index].triangle_buffer);
		rasterizer_vertex_buffer_delete(&cabin->meshes[mesh_index].vertex_buffer);
	}
	cabin->loaded = FALSE;
}

/* a cabin's vehicle's model, and its meshes' buffers, if the map has it */
static void cabin_initialize(struct vr_cabin_model const *definition, struct vr_cabin *cabin)
{
	static struct model_vertex_uncompressed vertices[VR_CABIN_MODEL_MAXIMUM_VERTICES];
	static struct model_vertex_compressed compressed[VR_CABIN_MODEL_MAXIMUM_VERTICES];
	static word triangles[VR_CABIN_MODEL_MAXIMUM_TRIANGLES * 3];
	long model_index = tag_loaded(MODELS_GROUP_TAG, definition->model_name);
	long vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_model_compressed);
	long vertex_count = 0, triangle_count = 0;
	struct model *model;
	short mesh_index;

	cabin->loaded = FALSE;
	if (model_index == NONE)
		return;
	model = model_definition_get(model_index);
	for (mesh_index = 0; mesh_index < definition->mesh_count; mesh_index++)
	{
		struct vr_cabin_model_mesh const *mesh = &definition->meshes[mesh_index];
		struct vr_cabin_mesh *built = &cabin->meshes[mesh_index];
		struct vr_cabin_shader_reference const *reference = shader_find(model, mesh->shader_name);
		short node_index = model_find_node(model_index, mesh->node_name);
		/* (a shader of the model's, else another model's in this map: the
		Pelican's bay's, for the feed cockpit's) */
		long shader_index = reference ? reference->shader.index : tag_loaded(SHADER_MODEL_TAG, mesh->shader_name);
		struct shader *shader;

		if (node_index == NONE || !VALID_INDEX(node_index, MIN(model->nodes.count, MAXIMUM_NODES_PER_MODEL)) ||
			shader_index == NONE)
		{
			platform_log("vr cabin: %s has no %s or no %s: no cabin drawn", definition->model_name, mesh->node_name,
				mesh->shader_name);
			cabin_dispose(cabin, mesh_index);
			return;
		}
		/* (a model shader, opaque, as the vehicle's are: drawn as its solid
		parts are) */
		shader = shader_definition_get(shader_index);
		if (shader->base.type != _shader_type_model ||
			TEST_FLAG(((struct vr_cabin_shader_model *)shader)->flags, _shader_model_alpha_blended_decal_bit))
		{
			platform_log("vr cabin: %s is not an opaque model shader: no cabin drawn", mesh->shader_name);
			cabin_dispose(cabin, mesh_index);
			return;
		}
		mesh_build(mesh, vertices, triangles);
		rasterizer_geometry_compress_vertices(_rasterizer_vertex_type_model_uncompressed, mesh->vertex_count,
			compressed, mesh->vertex_count * vertex_size, vertices, sizeof(*vertices) * mesh->vertex_count);
		if (!rasterizer_vertex_buffer_new(&built->vertex_buffer, _rasterizer_vertex_type_model_compressed,
				mesh->vertex_count, compressed, mesh->vertex_count * vertex_size))
		{
			cabin_dispose(cabin, mesh_index);
			return;
		}
		if (!rasterizer_triangle_buffer_new(&built->triangle_buffer, _triangle_buffer_type_triangles,
				mesh->triangle_count, triangles))
		{
			rasterizer_vertex_buffer_delete(&built->vertex_buffer);
			cabin_dispose(cabin, mesh_index);
			return;
		}
		built->node_index = node_index;
		built->shader = shader;
		built->permutation_index = reference ? reference->permutation_index : 0;
		vertex_count += mesh->vertex_count;
		triangle_count += mesh->triangle_count;
	}
	cabin->loaded = TRUE;
	cabin->model_index = model_index;
	platform_log("vr cabin: %s, %s%s%s: a cabin of %d meshes (%ld vertices, %ld triangles)", definition->model_name,
		definition->seat_label, definition->view ? ", " : "", definition->view ? definition->view : "",
		definition->mesh_count, vertex_count, triangle_count);
}

/* the cabin of the seat this unit is in (its vehicle's model, the seat's
label, the vehicle's setting for its view), and the vehicle; else NONE */
static short cabin_find(long unit_index, long *vehicle_index)
{
	struct unit_datum *unit;
	struct unit_definition *definition;
	struct unit_seat *seat;
	long model_index;
	short index;

	if (unit_index == NONE || (unit = unit_get(unit_index))->object.parent_object_index == NONE ||
		unit->unit.parent_seat_index == NONE)
	{
		return NONE;
	}
	*vehicle_index = unit->object.parent_object_index;
	definition = unit_definition_get(object_get(*vehicle_index)->definition_index);
	if (unit->unit.parent_seat_index >= definition->unit.seats.count)
		return NONE;
	seat = TAG_BLOCK_GET_ELEMENT(&definition->unit.seats, unit->unit.parent_seat_index, struct unit_seat);
	model_index = definition->object.model.index;
	for (index = 0; index < NUMBER_OF_CABINS; index++)
	{
		struct vr_cabin_model const *cabin = &vr_cabin_models[index];

		if (vr_cabins[index].loaded && vr_cabins[index].model_index == model_index &&
			strstr(seat->label, cabin->seat_label) &&
			(!cabin->setting || !strcmp(config_string(cabin->setting), cabin->view)))
		{
			return index;
		}
	}
	return NONE;
}

/* the node's matrix as render_model skins a model's vertices with it: its
matrix by its default pose's inverse */
static void node_skinning(long model_index, real_matrix4x3 const *node_matrix, short node_index, real_matrix4x3 *result)
{
	struct model *model = model_definition_get(model_index);

	matrix4x3_multiply(node_matrix,
		&TAG_BLOCK_GET_ELEMENT(&model->nodes, node_index, struct model_node)->runtime_default_inverse_matrix, result);
}

/* the tank on the holotable: the vehicle's own model drawn a hand's length
long (the cabin's hologram[3]) at its place on the table, from its posed
nodes, so that its turret turns as the tank's does to where the gun aims;
tilted from the cockpit as the hull is from level (its pitch and roll: the
cockpit rides with the hull); dark, lit by the shields' glow (the player's
plasma, vr_holster_hologram: the HUD's blue about its edges) */
static void hologram_draw(long object_index, long model_index, struct vr_cabin_model const *definition,
	real_matrix4x3 const *node_matrices, real_rgb_color const *change_colors, real const *function_values)
{
	static real_matrix4x3 nodes[MAXIMUM_NODES_PER_MODEL];
	struct model *model = model_definition_get(model_index);
	struct object_definition *object = object_definition_get(object_get(object_index)->definition_index);
	struct render_model_effect model_effect;
	struct render_lighting lighting;
	real_matrix4x3 root, root_inverse, place, carry;
	real_vector3d level_forward, level_left;
	real_point3d centroid, foot;
	real along, scale, units;
	long player_index = local_player_get_player_index(0);
	short node_index;

	if (player_index == NONE || model->nodes.count <= 0 || model->nodes.count > MAXIMUM_NODES_PER_MODEL ||
		object->object.bounding_radius <= 0.0f)
	{
		return;
	}
	/* the model's space as the root node places it (its skinning), and
	back */
	node_skinning(model_index, &node_matrices[0], 0, &root);
	matrix4x3_inverse(&root, &root_inverse);
	units = root.scale > 0.0f ? root.scale : 1.0f;
	/* in the model's space: about its origin (its foot, under its middle),
	scaled to the length (its bounding sphere's diameter), tilted as the hull
	is from level (the hull's axes in the level frame of its heading), its
	foot at the hologram's place */
	scale = definition->hologram[3] / (2.0f * object->object.bounding_radius / units);
	along = dot_product3d(&root.forward, global_up3d);
	level_forward.i = root.forward.i - global_up3d->i * along;
	level_forward.j = root.forward.j - global_up3d->j * along;
	level_forward.k = root.forward.k - global_up3d->k * along;
	if (normalize3d(&level_forward) <= 0.0f)
		level_forward = root.forward;
	cross_product3d(global_up3d, &level_forward, &level_left);
	matrix4x3_identity(&place);
	place.scale = scale;
	place.forward.i = dot_product3d(&root.forward, &level_forward);
	place.forward.j = dot_product3d(&root.forward, &level_left);
	place.forward.k = dot_product3d(&root.forward, global_up3d);
	place.left.i = dot_product3d(&root.left, &level_forward);
	place.left.j = dot_product3d(&root.left, &level_left);
	place.left.k = dot_product3d(&root.left, global_up3d);
	place.up.i = dot_product3d(&root.up, &level_forward);
	place.up.j = dot_product3d(&root.up, &level_left);
	place.up.k = dot_product3d(&root.up, global_up3d);
	foot.x = definition->hologram[0];
	foot.y = definition->hologram[1];
	foot.z = definition->hologram[2];
	place.position = foot;
	/* each node: the root's skinning, the placing, back to the model's
	space, then the node as posed (the turret's turn its own) */
	matrix4x3_multiply(&root, &place, &carry);
	matrix4x3_multiply(&carry, &root_inverse, &carry);
	for (node_index = 0; node_index < model->nodes.count; node_index++)
		matrix4x3_multiply(&carry, &node_matrices[node_index], &nodes[node_index]);
	matrix4x3_transform_point(&root, &foot, &centroid);
	vr_holster_hologram(player_get(player_index)->unit_index, units / METRES_PER_WORLD_UNIT, &model_effect, &lighting);
	render_model(model_index, HOLOGRAM_DETAIL_PIXELS, nodes, NULL, change_colors, function_values, &lighting, &centroid,
		definition->hologram[3] * units, &model_effect, object_index, 0, 0);
}

/* ---------- public code */

static boolean eye_in_cockpit(long *vehicle_index);
boolean vr_cabin_feed(short index);

/* render.c, as a map is loaded: each cabin's vehicle's model, and its
buffers, if the map has it */
void vr_cabin_initialize_for_new_map(void)
{
	short index;

	for (index = 0; index < NUMBER_OF_CABINS; index++)
		cabin_initialize(&vr_cabin_models[index], &vr_cabins[index]);
}

/* render.c, as a map is unloaded */
void vr_cabin_dispose_from_old_map(void)
{
	short index;

	for (index = 0; index < NUMBER_OF_CABINS; index++)
	{
		if (vr_cabins[index].loaded)
			cabin_dispose(&vr_cabins[index], vr_cabin_models[index].mesh_count);
	}
}

/* vr_seat.c, each VR frame the eye is in a seat (seated_unit_index): the
cabin's eye in the world (eye), where the seat has one; its index (the eye
moves to another's in a blink), else NONE and the eye as it was */
short vr_cabin_seat_eye(long seated_unit_index, real_point3d *eye)
{
	long vehicle_index = NONE;
	short index = cabin_find(seated_unit_index, &vehicle_index);
	real_matrix4x3 skinning;
	real_point3d local;

	if (index == NONE)
		return NONE;
	/* (the eye in the model's space, placed as the root node's vertices are) */
	node_skinning(vr_cabins[index].model_index, object_get_node_matrix(vehicle_index, 0), 0, &skinning);
	local.x = vr_cabin_models[index].eye[0];
	local.y = vr_cabin_models[index].eye[1];
	local.z = vr_cabin_models[index].eye[2];
	matrix4x3_transform_point(&skinning, &local, eye);
	return index;
}

/* vr_render.c: the player's body drawn in this unit's seat stands (the
Scorpion's hatch: first_person_legs.c), the seat's cabin's */
boolean vr_cabin_standing(long unit_index)
{
	long vehicle_index = NONE;
	short index = cabin_find(unit_index, &vehicle_index);

	return index != NONE && vr_cabin_models[index].standing;
}

/* render_objects.c, just after an object's model is drawn with these
(render_model's): the cabin of the first player's seat, if it is this
vehicle's and the eye is in the seat */
void vr_cabin_draw(
	long object_index,
	long model_index,
	real_matrix4x3 const *node_matrices,
	real_rgb_color const *change_colors,
	real const *function_values,
	struct render_lighting const *lighting,
	struct render_model_effect const *model_effect)
{
	long player_index = local_player_get_player_index(0);
	long vehicle_index = NONE;
	struct vr_cabin_model const *definition;
	struct vr_cabin *cabin;
	struct model *model;
	short index, mesh_index;

	if (model_index == NONE || player_index == NONE || !vr_seat_seated_shown())
		return;
	index = cabin_find(player_get(player_index)->unit_index, &vehicle_index);
	if (index == NONE)
		index = vr_seat_feed_cabin(&vehicle_index);
	/* (a feed cockpit, only about an eye in it: the eye riding in and out
	sees the vehicle, not the cockpit within it, from outside) */
	if (index != NONE && vr_cabin_feed(index) && !eye_in_cockpit(&vehicle_index))
		return;
	if (index == NONE || vehicle_index != object_index || vr_cabins[index].model_index != model_index)
		return;
	definition = &vr_cabin_models[index];
	cabin = &vr_cabins[index];
	model = model_definition_get(model_index);
	for (mesh_index = 0; mesh_index < definition->mesh_count; mesh_index++)
	{
		struct vr_cabin_mesh *mesh = &cabin->meshes[mesh_index];
		struct rasterizer_model_begin_parameters parameters;
		real_matrix4x3 skinning;
		real_point3d middle;
		real lift = 0.0f;

		/* (a feed cockpit's grate: lifted into the ceiling as it opens, and
		gone once the feed has washed over the opening) */
		if (mesh_index == definition->grate_mesh)
		{
			real grate, wash, off;

			if (!vr_seat_feed(&grate, &wash, &off) || wash >= 1.0f || grate <= 0.0f)
				continue;
			lift = (1.0f - grate) * GRATE_TRAVEL;
		}
		node_skinning(model_index, &node_matrices[mesh->node_index], mesh->node_index, &skinning);
		skinning.position.x += skinning.up.i * lift * skinning.scale;
		skinning.position.y += skinning.up.j * lift * skinning.scale;
		skinning.position.z += skinning.up.k * lift * skinning.scale;
		/* (lit at the eye, its middle) */
		middle.x = definition->eye[0];
		middle.y = definition->eye[1];
		middle.z = definition->eye[2];

		/* (as render_model begins a model) */
		memset(&parameters, 0, sizeof(parameters));
		parameters.unique_identifier = object_index;
		parameters.lighting = *lighting;
		matrix4x3_transform_point(&skinning, &middle, &parameters.centroid);
		parameters.radius = 0.0f;
		parameters.effect = *model_effect;
		parameters.animation.colors = change_colors;
		parameters.animation.values = function_values;
		parameters.skinning.node_matrices = &skinning;
		parameters.skinning.node_matrix_count = 1;
		parameters.geometry_flags = 0;
		parameters.base_map_scale = model->base_map_scale;
		rasterizer_model_begin(&parameters, FALSE);
		rasterizer_model_draw(mesh->shader, mesh->permutation_index, &mesh->triangle_buffer, NONE,
			mesh->triangle_buffer.count, &mesh->vertex_buffer, NONE);
		rasterizer_model_end();
	}
	if (definition->hologram[3] > 0.0f)
		hologram_draw(object_index, model_index, definition, node_matrices, change_colors, function_values);
}

/* the cabin the first player's seat has, shown (the eye in it), its
vehicle; else NONE */
static short cabin_shown(long *vehicle_index)
{
	long player_index = local_player_get_player_index(0);
	short index;

	if (player_index == NONE || !vr_seat_seated_shown())
		return NONE;
	index = cabin_find(player_get(player_index)->unit_index, vehicle_index);
	if (index == NONE)
		index = vr_seat_feed_cabin(vehicle_index);
	return index;
}

/* vr_seat.c: whether the cabin is a feed cockpit (its display), which the
eye rides into and out of */
boolean vr_cabin_feed(short index)
{
	return index >= 0 && index < NUMBER_OF_CABINS && vr_cabin_models[index].display[2] > 0.0f;
}

/* render.c, each VR frame before the eye pass: a feed cockpit's display
told to vr.c (halo_vr_cockpit_display: xgpu_gl.c draws its feed over the
opening) while the eye is in it, the grate and the feed's wash as vr_seat.c
has them, its readouts the range where the aim meets the world and the
vehicle's weapon's rounds (the Scorpion's shells, and its machine gun's);
else none */
void vr_cabin_display_frame(void)
{
	struct halo_vr_cockpit_display display;
	struct vr_cabin_model const *definition;
	real_matrix4x3 skinning;
	real_point3d local, axis;
	real_vector3d forward, up;
	long vehicle_index = NONE;
	short index = cabin_shown(&vehicle_index);
	real grate, wash, off, units;
	struct unit_datum *vehicle;
	long weapon_index;

	if (index == NONE || !vr_cabin_feed(index) || !vr_seat_feed(&grate, &wash, &off) || !eye_in_cockpit(&vehicle_index))
	{
		halo_vr_cockpit_display(NULL);
		return;
	}
	definition = &vr_cabin_models[index];
	node_skinning(vr_cabins[index].model_index, object_get_node_matrix(vehicle_index, 0), 0, &skinning);
	/* (the wall's axis at the display's middle height, the hull's forward
	and up) */
	local.x = definition->display[0];
	local.y = definition->display[1];
	local.z = (definition->display[4] + definition->display[5]) * 0.5f;
	matrix4x3_transform_point(&skinning, &local, &axis);
	forward = skinning.forward;
	up = skinning.up;
	units = skinning.scale > 0.0f ? skinning.scale : 1.0f;
	csmemset(&display, 0, sizeof(display));
	vr_render_world_to_head(&axis, &forward, &up, display.position, display.forward, display.up);
	display.radius = definition->display[2] * units * METRES_PER_WORLD_UNIT;
	display.half_angle = definition->display[3] * 3.14159265f / 180.0f;
	display.half_height = (definition->display[5] - definition->display[4]) * 0.5f * units * METRES_PER_WORLD_UNIT;
	display.wash = wash;
	display.off = off;
	display.range = vr_render_scope_range();
	display.rounds_loaded = display.rounds_carried = display.second_loaded = display.second_carried = -1;
	vehicle = unit_get(vehicle_index);
	weapon_index = unit_inventory_get_weapon(vehicle_index, vehicle->unit.current_weapon_index);
	if (weapon_index != NONE)
	{
		struct weapon_definition const *weapon = weapon_definition_get(object_get(weapon_index)->definition_index);
		struct weapon_datum *datum = weapon_get(weapon_index);

		if (weapon->weapon.magazines.count > 0)
		{
			display.rounds_loaded = datum->weapon.magazines[0].rounds_loaded;
			display.rounds_carried = datum->weapon.magazines[0].rounds_total;
		}
		if (weapon->weapon.magazines.count > 1)
		{
			display.second_loaded = datum->weapon.magazines[1].rounds_loaded;
			display.second_carried = datum->weapon.magazines[1].rounds_total;
		}
	}
	halo_vr_cockpit_display(&display);
}

/* the eye (this frame's) in the cockpit shown or within FEED_HIDE_MARGIN
of it (its box: the cabin's inside), and the cockpit's vehicle; FALSE else */
static boolean eye_in_cockpit(long *vehicle_index)
{
	struct vr_cabin_model const *definition;
	real_matrix4x3 skinning, inverse;
	real_point3d eye, local;
	short index = cabin_shown(vehicle_index);

	if (index == NONE || !vr_cabin_feed(index) || !vr_seat_shown(&eye))
		return FALSE;
	definition = &vr_cabin_models[index];
	node_skinning(vr_cabins[index].model_index, object_get_node_matrix(*vehicle_index, 0), 0, &skinning);
	matrix4x3_inverse(&skinning, &inverse);
	matrix4x3_transform_point(&inverse, &eye, &local);
	return local.x > definition->inside[0] - FEED_HIDE_MARGIN && local.x < definition->inside[3] + FEED_HIDE_MARGIN &&
		local.y > definition->inside[1] - FEED_HIDE_MARGIN && local.y < definition->inside[4] + FEED_HIDE_MARGIN &&
		local.z > definition->inside[2] - FEED_HIDE_MARGIN && local.z < definition->inside[5] + FEED_HIDE_MARGIN;
}

/* render_objects.c: whether this object's model is not drawn, the eye in a
feed cockpit (or coming within FEED_HIDE_MARGIN of it): its vehicle, which
the eye is inside (from inside, its armour's plates are seen through the
display's opening, and the eye would pass through them as it comes in; the
cockpit, drawn after its model, stays), and what is attached to the vehicle
(hidden altogether: vr_render_hides_object); the eye pass's alone */
boolean vr_cabin_hides_model(long object_index)
{
	long vehicle_index = NONE;

	return object_index != NONE && !vr_render_scope_pass() && eye_in_cockpit(&vehicle_index) &&
		object_index == vehicle_index;
}

boolean vr_cabin_hides_attached(long object_index)
{
	long vehicle_index = NONE;

	return object_index != NONE && !vr_render_scope_pass() && eye_in_cockpit(&vehicle_index) &&
		object_index != vehicle_index && object_get_ultimate_parent(object_index) == vehicle_index;
}

#endif
