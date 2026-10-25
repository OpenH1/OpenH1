/*
VR_TRIGGER.C

The VR mode's triggers on the first-person weapons whose models have none
(HALO_VR; port/linux/README.md "VR"), for first_person_weapons.c and
render.c.

In the VR mode the right index finger rests on the weapon's trigger and
pulls it as the controller's trigger is pulled, and the weapon's own trigger
goes back with it where the first-person model has a node for it
(vr_hands.c). Some first-person models have no trigger at all: the finger of
the game's own animation rests in an empty gap under the receiver, ahead of
the grip, and not one vertex of the model is there. So a trigger is drawn
there, modelled for each such weapon to sit in the finger's crook
(vr_trigger_model.h, made by tools/vr_trigger_model.py: the assault rifle's,
an MA5B's, the sniper rifle's, the shotgun's and the flamethrower's, out of
its handle's underside): a curved blade of rounded section, its
front face crowned where the finger lies, broad where it hangs from the
receiver and curling forward to its tip below the finger, coming out of a
low housing on the receiver's underside. Each is found by its model's tag
name as a map is loaded, made once a map (vertex and index buffers, as a
model part's) and drawn just after its weapon's model, with the weapon's own
shader (its texture, its lighting, its tint and the camouflage's effect, as
the weapon is drawn), from the weapon's gun node, so wherever the weapon's
first-person model is drawn (both eyes; the scopes' pass draws no
first-person weapon). The blade's faces sample a fitting part of the
weapon's texture (the trigger the rifle's paints, which no face of its model
samples; the others' receivers' undersides); the housing's sample the
receiver's underside as the receiver's own faces do. As the finger pulls,
the blade turns back about its top, where it comes out of the housing, its
face going back as far as the finger's weapon has its trigger go
(vr_hands.c's trigger_fingers); the housing stays.

Nothing is drawn for any other model, nor where a weapon's first-person
model, its gun node or its shader is not in the map; the world's weapons and
the third person are other models.
*/

#ifdef HALO_VR

#include "cseries.h"
#include "math/real_math.h"
#include "models/model_definitions.h"
#include "models/models.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_geometry.h"
#include "rasterizer/rasterizer_model_types.h"
#include "rasterizer/rasterizer_models.h"
#include "shaders/shader_definitions.h"
#include "shaders/shaders.h"
#include "tag_files/tag_groups.h"
#include "tag_files/tag_files.h"

#include "vr_hands.h"
#include "vr_trigger_model.h"

#include <math.h>
#include <string.h>

enum
{
	NUMBER_OF_TRIGGERS = NUMBEROF(vr_trigger_models),
	/* the blade's vertices are node 0's, the housing's node 1's
	(vr_trigger_model.h): two matrices, the first turned back */
	NUMBER_OF_TRIGGER_NODES = 2,
	/* (models.c's) */
	_shader_type_model = 4,
	_shader_model_alpha_blended_decal_bit = 3,
	_rasterizer_geometry_first_person_bit = 7,
	_render_model_first_person_bit = 3,
};

/* a model's shader (models.c's: no header declares it) */
struct vr_trigger_shader_reference
{
	struct tag_reference shader;
	short permutation_index;
	word pad;
	long unused[3];
};

/* models.c's shader of a model's, its flags */
struct vr_trigger_shader_model
{
	struct shader shader;
	word flags;
};

/* each trigger's (vr_trigger_models'): whether this map has it (FALSE as the
game starts, before any map), its weapon's first-person model in this map,
its gun node, the shader and its permutation it is drawn with, and its
buffers */
static struct vr_trigger
{
	boolean loaded;
	long model_index;
	short node_index;
	struct shader *shader;
	short permutation_index;
	struct vertex_buffer vertex_buffer;
	struct triangle_buffer triangle_buffer;
} vr_triggers[NUMBER_OF_TRIGGERS];

void platform_log(char const *format, ...);

/* ---------- the blade */

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

/* a trigger's vertices (in its model's space: the blade's node 0's, the
housing's node 1's) and triangles, from its table */
static void trigger_build(struct vr_trigger_model const *definition, struct model_vertex_uncompressed *vertices,
	word *triangles)
{
	long index;

	memset(vertices, 0, sizeof(*vertices) * definition->vertex_count);
	for (index = 0; index < definition->vertex_count; index++)
	{
		struct vr_trigger_model_vertex const *source = &definition->vertices[index];
		struct model_vertex_uncompressed *vertex = &vertices[index];
		real across;

		vertex->position.x = source->position[0];
		vertex->position.y = source->position[1];
		vertex->position.z = source->position[2];
		vector_set_normalized(&vertex->normal, source->normal[0], source->normal[1], source->normal[2]);
		/* (the tangent across the trigger, the left axis less its part along
		the normal; the binormal from the two, as the model's) */
		across = vertex->normal.j;
		vector_set_normalized(&vertex->tangent, -across * vertex->normal.i, 1.0f - across * vertex->normal.j,
			-across * vertex->normal.k);
		cross_product3d(&vertex->normal, &vertex->tangent, &vertex->binormal);
		vector_set_normalized(&vertex->binormal, vertex->binormal.i, vertex->binormal.j, vertex->binormal.k);
		vertex->texcoord.x = source->texcoord[0];
		vertex->texcoord.y = source->texcoord[1];
		vertex->nodes[0] = source->node;
		vertex->nodes[1] = source->node;
		vertex->node_weights[0] = 1.0f;
		vertex->node_weights[1] = 0.0f;
	}
	memcpy(triangles, definition->triangles, sizeof(*triangles) * 3 * definition->triangle_count);
}

/* ---------- public code */

/* a trigger's weapon's first-person model, and the trigger's buffers, if
the map has it */
static void trigger_initialize(struct vr_trigger_model const *definition, struct vr_trigger *trigger)
{
	static struct model_vertex_uncompressed vertices[VR_TRIGGER_MODEL_MAXIMUM_VERTICES];
	static struct model_vertex_compressed compressed[VR_TRIGGER_MODEL_MAXIMUM_VERTICES];
	static word triangles[VR_TRIGGER_MODEL_MAXIMUM_TRIANGLES * 3];
	long model_index = tag_loaded(MODELS_GROUP_TAG, definition->model_name);
	long vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_model_compressed);
	struct model *model;
	struct vr_trigger_shader_reference const *reference = NULL;
	struct shader *shader;
	short node_index, shader_index;

	trigger->loaded = FALSE;
	if (model_index == NONE)
		return;
	model = model_definition_get(model_index);
	node_index = model_find_node(model_index, definition->node_name);
	for (shader_index = 0; shader_index < model->shaders.count; shader_index++)
	{
		struct vr_trigger_shader_reference const *candidate =
			TAG_BLOCK_GET_ELEMENT(&model->shaders, shader_index, struct vr_trigger_shader_reference);
		char const *name = candidate->shader.index != NONE ? tag_get_name(candidate->shader.index) : NULL;

		if (name && !strcmp(name, definition->shader_name))
		{
			reference = candidate;
			break;
		}
	}
	if (node_index == NONE || !VALID_INDEX(node_index, MIN(model->nodes.count, MAXIMUM_NODES_PER_MODEL)) || !reference)
	{
		platform_log("vr trigger: %s has no %s or no %s: no trigger drawn", definition->model_name,
			definition->node_name, definition->shader_name);
		return;
	}
	/* (a model shader, opaque, as the receiver's is: drawn as the weapon's
	solid parts are) */
	shader = shader_definition_get(reference->shader.index);
	if (shader->base.type != _shader_type_model ||
		TEST_FLAG(((struct vr_trigger_shader_model *)shader)->flags, _shader_model_alpha_blended_decal_bit))
	{
		platform_log("vr trigger: %s is not an opaque model shader: no trigger drawn", definition->shader_name);
		return;
	}

	trigger_build(definition, vertices, triangles);
	rasterizer_geometry_compress_vertices(_rasterizer_vertex_type_model_uncompressed, definition->vertex_count,
		compressed, definition->vertex_count * vertex_size, vertices, sizeof(*vertices) * definition->vertex_count);
	if (!rasterizer_vertex_buffer_new(&trigger->vertex_buffer, _rasterizer_vertex_type_model_compressed,
			definition->vertex_count, compressed, definition->vertex_count * vertex_size) ||
		!rasterizer_triangle_buffer_new(&trigger->triangle_buffer, _triangle_buffer_type_triangles,
			definition->triangle_count, triangles))
	{
		rasterizer_vertex_buffer_delete(&trigger->vertex_buffer);
		return;
	}
	trigger->loaded = TRUE;
	trigger->model_index = model_index;
	trigger->node_index = node_index;
	trigger->shader = shader;
	trigger->permutation_index = reference->permutation_index;
	platform_log("vr trigger: %s: a trigger drawn from %s with %s (%d vertices, %d triangles; base map scale %.3f %.3f)",
		definition->model_name, definition->node_name, definition->shader_name, definition->vertex_count,
		definition->triangle_count, model->base_map_scale.i, model->base_map_scale.j);
}

/* render.c, as a map is loaded: each trigger's weapon's first-person model,
and the trigger's buffers, if the map has it */
void vr_trigger_initialize_for_new_map(void)
{
	short index;

	for (index = 0; index < NUMBER_OF_TRIGGERS; index++)
		trigger_initialize(&vr_trigger_models[index], &vr_triggers[index]);
}

/* render.c, as a map is unloaded */
void vr_trigger_dispose_from_old_map(void)
{
	short index;

	for (index = 0; index < NUMBER_OF_TRIGGERS; index++)
	{
		struct vr_trigger *trigger = &vr_triggers[index];

		if (!trigger->loaded)
			continue;
		rasterizer_triangle_buffer_delete(&trigger->triangle_buffer);
		rasterizer_vertex_buffer_delete(&trigger->vertex_buffer);
		trigger->loaded = FALSE;
	}
}

/* first_person_weapons.c, just after a first-person weapon's model is drawn
with these (render_model's): its trigger, if it has one, its blade back as
far as the finger pulls it */
void vr_trigger_draw(
	long model_index,
	real_matrix4x3 const *node_matrices,
	real_rgb_color const *change_colors,
	real const *function_values,
	struct render_lighting const *lighting,
	struct render_model_effect const *model_effect,
	long unique_identifier,
	unsigned long flags)
{
	struct vr_trigger_model const *definition = NULL;
	struct vr_trigger const *trigger = NULL;
	struct model *model;
	struct rasterizer_model_begin_parameters parameters;
	/* the blade's (turned) and the housing's */
	real_matrix4x3 relative[NUMBER_OF_TRIGGER_NODES], turn;
	real_point3d middle;
	real angle, cosine, sine;
	short index;

	if (model_index == NONE)
		return;
	for (index = 0; index < NUMBER_OF_TRIGGERS; index++)
	{
		if (vr_triggers[index].loaded && vr_triggers[index].model_index == model_index)
		{
			definition = &vr_trigger_models[index];
			trigger = &vr_triggers[index];
			break;
		}
	}
	if (!trigger)
		return;
	model = model_definition_get(model_index);
	/* the node's, as render_model skins a model's vertices: its matrix by its
	default pose's inverse */
	matrix4x3_multiply(&node_matrices[trigger->node_index],
		&TAG_BLOCK_GET_ELEMENT(&model->nodes, trigger->node_index, struct model_node)->runtime_default_inverse_matrix,
		&relative[1]);
	relative[0] = relative[1];
	/* the blade turned back about its top, its face at the finger's height
	as far back as the finger pulls it (about the left axis: the tip back,
	toward the grip); or, its pivot below the finger (the flamethrower's),
	the other way, its face going back and up along the finger, which on that
	weapon cannot curl back to follow a face turned away from it */
	angle = vr_hands_trigger_travel() / (definition->pivot_up - definition->pull_up);
	if (angle != 0.0f)
	{
		cosine = (real)cos(angle);
		sine = (real)sin(angle);
		turn.scale = 1.0f;
		turn.forward.i = cosine;
		turn.forward.j = 0.0f;
		turn.forward.k = -sine;
		turn.left.i = 0.0f;
		turn.left.j = 1.0f;
		turn.left.k = 0.0f;
		turn.up.i = sine;
		turn.up.j = 0.0f;
		turn.up.k = cosine;
		turn.position.x = definition->pivot_forward - (cosine * definition->pivot_forward + sine * definition->pivot_up);
		turn.position.y = 0.0f;
		turn.position.z = definition->pivot_up - (-sine * definition->pivot_forward + cosine * definition->pivot_up);
		matrix4x3_multiply(&relative[1], &turn, &relative[0]);
	}
	middle.x = definition->middle_forward;
	middle.y = 0.0f;
	middle.z = definition->middle_up;

	/* (as render_model begins a model) */
	memset(&parameters, 0, sizeof(parameters));
	parameters.unique_identifier = unique_identifier;
	parameters.lighting = *lighting;
	/* (its middle is in the model's space, as its vertices are) */
	matrix4x3_transform_point(&relative[1], &middle, &parameters.centroid);
	parameters.radius = 0.0f;
	parameters.effect = *model_effect;
	parameters.animation.colors = change_colors;
	parameters.animation.values = function_values;
	parameters.skinning.node_matrices = relative;
	parameters.skinning.node_matrix_count = NUMBER_OF_TRIGGER_NODES;
	parameters.geometry_flags = 0;
	SET_FLAG(parameters.geometry_flags, _rasterizer_geometry_first_person_bit,
		TEST_FLAG(flags, _render_model_first_person_bit));
	parameters.base_map_scale = model->base_map_scale;
	rasterizer_model_begin(&parameters, FALSE);
	rasterizer_model_draw(trigger->shader, trigger->permutation_index, &trigger->triangle_buffer, NONE,
		trigger->triangle_buffer.count, &trigger->vertex_buffer, NONE);
	rasterizer_model_end();
}

#endif
