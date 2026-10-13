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
- the Scorpion's commander (vr.scorpion_view = "hatch", the default): the eye
  above the turret, on its axis, so that it does not swing as the turret
  turns, a cupola ring with its lid open about it on the turret.
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

#include <math.h>
#include <string.h>

/* the platform layer's (port/linux/src/port_config.c) */
const char *config_string(char const *name);
void platform_log(char const *format, ...);
/* vr_seat.c's */
boolean vr_seat_seated_shown(void);

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
		struct shader *shader;

		if (node_index == NONE || !VALID_INDEX(node_index, MIN(model->nodes.count, MAXIMUM_NODES_PER_MODEL)) || !reference)
		{
			platform_log("vr cabin: %s has no %s or no %s: no cabin drawn", definition->model_name, mesh->node_name,
				mesh->shader_name);
			cabin_dispose(cabin, mesh_index);
			return;
		}
		/* (a model shader, opaque, as the vehicle's are: drawn as its solid
		parts are) */
		shader = shader_definition_get(reference->shader.index);
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
		built->permutation_index = reference->permutation_index;
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

/* ---------- public code */

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

		node_skinning(model_index, &node_matrices[mesh->node_index], mesh->node_index, &skinning);
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
}

#endif
