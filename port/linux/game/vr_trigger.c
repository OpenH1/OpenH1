/*
VR_TRIGGER.C

The VR mode's trigger on a first-person weapon whose model has none
(HALO_VR; port/linux/README.md "VR"), for first_person_weapons.c and
render.c.

In the VR mode the right index finger rests on the weapon's trigger and
pulls it as the controller's trigger is pulled, and the weapon's own trigger
goes back with it where the first-person model has a node for it
(vr_hands.c). The assault rifle's first-person model has no trigger at all:
the finger of the game's own animation rests in an empty gap under the
receiver, ahead of the pistol grip, and not one vertex of the model is
there. So a trigger is drawn there: a curved blade hanging from the
receiver's underside, its front face hollowed where the finger lies, made
once a map (vertex and index buffers, as a model part's) and drawn just
after the rifle's model, with the rifle's own shader (its texture, its
lighting, its tint and the camouflage's effect, as the rifle is drawn), from
the rifle's gun node, so wherever the rifle's first-person model is drawn
(both eyes; the scopes' pass draws no first-person weapon). The rifle's
texture paints a trigger, in the stock's side view, which no face of the
model samples: the blade's faces sample it. As the finger pulls, the blade
turns back about its top, where it hangs from the receiver, its face going
back as far as the finger's weapon has its trigger go (vr_hands.c's
trigger_fingers).

Nothing is drawn for any other model, nor where the rifle's first-person
model, its gun node or its shader is not in the map; the world's rifle and
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
#include "shaders/shader_definitions.h"
#include "shaders/shaders.h"
#include "tag_files/tag_groups.h"
#include "tag_files/tag_files.h"

#include "vr_hands.h"

#include <math.h>
#include <string.h>

/* the first-person model given a trigger, the node it hangs from and the
shader it is drawn with (its receiver's) */
#define BLADE_MODEL "weapons\\assault rifle\\fp\\fp"
#define BLADE_NODE "frame gun"
#define BLADE_SHADER "weapons\\assault rifle\\fp\\shaders\\gun"

/* The blade, in the gun node's space (world units: forward, left, up; the
receiver's underside is at up 0.0264, the grip's front at forward 0.0101,
the handguard's rear end at 0.0301), from its top, inside the receiver, to
its tip: at each height, its front face (forward), its thickness back from
the face and its half width. The idle animation's index finger hooks round
the gap from the right, from up 0.015 to 0.025, its crook at about forward
0.0152: the blade stands in that crook, its face 0.0001 short of the finger
(an MA5B's trigger is as near its grip), and below the finger it sweeps
forward to its tip, as an MA5B's does. */
static real const blade_rows[][4] =
{
	/* up, front, thickness, half width */
	{ 0.0272f, 0.0156f, 0.0017f, 0.0011f },
	{ 0.0255f, 0.0154f, 0.0017f, 0.0011f },
	{ 0.0235f, 0.0152f, 0.0016f, 0.0011f },
	{ 0.0215f, 0.0151f, 0.0016f, 0.0011f },
	{ 0.0195f, 0.0151f, 0.0016f, 0.0011f },
	{ 0.0175f, 0.0153f, 0.0016f, 0.0011f },
	{ 0.0158f, 0.0157f, 0.0015f, 0.0011f },
	{ 0.0143f, 0.0164f, 0.0015f, 0.0011f },
	{ 0.0131f, 0.0173f, 0.0015f, 0.0011f },
	{ 0.0123f, 0.0181f, 0.0014f, 0.0011f },
	{ 0.0119f, 0.0185f, 0.0011f, 0.0009f },
};
/* its tip, closing the last row: below its middle by this */
#define BLADE_TIP_DROP 0.0002f
/* it turns back about this point, where it meets the receiver (forward, up),
and its face goes back by the finger's travel at this height (the finger's) */
#define BLADE_PIVOT_FORWARD 0.0156f
#define BLADE_PIVOT_UP 0.0264f
#define BLADE_PULL_UP 0.0200f
/* the trigger the texture paints (texture coordinates, its middle third
across, along its length): the blade's front face samples its middle, the
sides its edges */
#define BLADE_U0 0.2773f
#define BLADE_U1 0.3086f
#define BLADE_V0 0.6660f
#define BLADE_V1 0.7559f

/* each row's outline, round from the left of the front face (x: back from the
face as a fraction of the thickness, y: across as a fraction of the half
width): the face and its back rounded at their edges */
static real const blade_outline[][2] =
{
	{ 0.00f, 0.55f },
	{ 0.22f, 1.00f },
	{ 0.80f, 1.00f },
	{ 1.00f, 0.55f },
	{ 1.00f, -0.55f },
	{ 0.80f, -1.00f },
	{ 0.22f, -1.00f },
	{ 0.00f, -0.55f },
};

enum
{
	NUMBER_OF_BLADE_ROWS = NUMBEROF(blade_rows),
	NUMBER_OF_BLADE_SIDES = NUMBEROF(blade_outline),
	/* every row's outline, then the tip */
	NUMBER_OF_BLADE_VERTICES = NUMBER_OF_BLADE_ROWS * NUMBER_OF_BLADE_SIDES + 1,
	/* two triangles a side between rows, and one a side to the tip */
	NUMBER_OF_BLADE_TRIANGLES = (NUMBER_OF_BLADE_ROWS - 1) * NUMBER_OF_BLADE_SIDES * 2 + NUMBER_OF_BLADE_SIDES,
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

static struct
{
	/* the rifle's first-person model in this map, its gun node, the shader
	and its permutation the blade is drawn with, and its buffers; model_index
	NONE when there is none */
	long model_index;
	short node_index;
	struct shader *shader;
	short permutation_index;
	struct vertex_buffer vertex_buffer;
	struct triangle_buffer triangle_buffer;
} vr_trigger = { NONE };

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

/* the blade's vertices (in the model's space, all the node's) and triangles */
static void blade_build(struct model_vertex_uncompressed *vertices, word *triangles)
{
	short row, side;
	long triangle = 0;

	memset(vertices, 0, sizeof(*vertices) * NUMBER_OF_BLADE_VERTICES);
	for (row = 0; row < NUMBER_OF_BLADE_ROWS; row++)
	{
		/* the run of the front face down the blade here, for the normals */
		short above = row > 0 ? row - 1 : row;
		short below = row < NUMBER_OF_BLADE_ROWS - 1 ? row + 1 : row;
		real slope = (blade_rows[below][1] - blade_rows[above][1]) / (blade_rows[above][0] - blade_rows[below][0]);
		real v = BLADE_V0 + (BLADE_V1 - BLADE_V0) * row / (NUMBER_OF_BLADE_ROWS - 1);

		for (side = 0; side < NUMBER_OF_BLADE_SIDES; side++)
		{
			struct model_vertex_uncompressed *vertex = &vertices[row * NUMBER_OF_BLADE_SIDES + side];
			short previous = (side + NUMBER_OF_BLADE_SIDES - 1) % NUMBER_OF_BLADE_SIDES;
			short next = (side + 1) % NUMBER_OF_BLADE_SIDES;
			real thickness = blade_rows[row][2];
			real half_width = blade_rows[row][3];
			/* (smooth: across the outline's neighbours, outward; leaning up or
			down as the face runs down the blade) */
			real along_x = -(blade_outline[next][0] - blade_outline[previous][0]) * thickness;
			real along_y = (blade_outline[next][1] - blade_outline[previous][1]) * half_width;

			vertex->position.x = blade_rows[row][1] - blade_outline[side][0] * thickness;
			vertex->position.y = blade_outline[side][1] * half_width;
			vertex->position.z = blade_rows[row][0];
			vector_set_normalized(&vertex->normal, along_y, -along_x, slope * along_y);
			vector_set_normalized(&vertex->tangent, 0.0f, 1.0f, 0.0f);
			cross_product3d(&vertex->normal, &vertex->tangent, &vertex->binormal);
			vector_set_normalized(&vertex->binormal, vertex->binormal.i, vertex->binormal.j, vertex->binormal.k);
			vertex->texcoord.x = BLADE_U0 + (BLADE_U1 - BLADE_U0) * (0.5f - 0.5f * blade_outline[side][1]);
			vertex->texcoord.y = v;
			vertex->nodes[0] = 0;
			vertex->nodes[1] = 0;
			vertex->node_weights[0] = 1.0f;
			vertex->node_weights[1] = 0.0f;
		}
	}
	{
		struct model_vertex_uncompressed *tip = &vertices[NUMBER_OF_BLADE_VERTICES - 1];
		real const *last = blade_rows[NUMBER_OF_BLADE_ROWS - 1];

		tip->position.x = last[1] - last[2] * 0.5f;
		tip->position.y = 0.0f;
		tip->position.z = last[0] - BLADE_TIP_DROP;
		vector_set_normalized(&tip->normal, 0.0f, 0.0f, -1.0f);
		vector_set_normalized(&tip->tangent, 0.0f, 1.0f, 0.0f);
		vector_set_normalized(&tip->binormal, 1.0f, 0.0f, 0.0f);
		tip->texcoord.x = (BLADE_U0 + BLADE_U1) * 0.5f;
		tip->texcoord.y = BLADE_V1;
		tip->node_weights[0] = 1.0f;
	}

	/* (wound as the model's faces are, its outside drawn) */
	for (row = 0; row < NUMBER_OF_BLADE_ROWS - 1; row++)
	{
		for (side = 0; side < NUMBER_OF_BLADE_SIDES; side++)
		{
			word a = (word)(row * NUMBER_OF_BLADE_SIDES + side);
			word b = (word)(row * NUMBER_OF_BLADE_SIDES + (side + 1) % NUMBER_OF_BLADE_SIDES);
			word c = (word)(a + NUMBER_OF_BLADE_SIDES);
			word d = (word)(b + NUMBER_OF_BLADE_SIDES);

			triangles[triangle * 3 + 0] = a;
			triangles[triangle * 3 + 1] = c;
			triangles[triangle * 3 + 2] = b;
			triangle++;
			triangles[triangle * 3 + 0] = b;
			triangles[triangle * 3 + 1] = c;
			triangles[triangle * 3 + 2] = d;
			triangle++;
		}
	}
	for (side = 0; side < NUMBER_OF_BLADE_SIDES; side++)
	{
		word a = (word)((NUMBER_OF_BLADE_ROWS - 1) * NUMBER_OF_BLADE_SIDES + side);
		word b = (word)((NUMBER_OF_BLADE_ROWS - 1) * NUMBER_OF_BLADE_SIDES + (side + 1) % NUMBER_OF_BLADE_SIDES);

		triangles[triangle * 3 + 0] = a;
		triangles[triangle * 3 + 1] = (word)(NUMBER_OF_BLADE_VERTICES - 1);
		triangles[triangle * 3 + 2] = b;
		triangle++;
	}
}

/* ---------- public code */

/* render.c, as a map is loaded: the rifle's first-person model, and the
blade's buffers, if the map has it */
void vr_trigger_initialize_for_new_map(void)
{
	static struct model_vertex_uncompressed vertices[NUMBER_OF_BLADE_VERTICES];
	static struct model_vertex_compressed compressed[NUMBER_OF_BLADE_VERTICES];
	static word triangles[NUMBER_OF_BLADE_TRIANGLES * 3];
	long model_index = tag_loaded(MODELS_GROUP_TAG, BLADE_MODEL);
	long vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_model_compressed);
	struct model *model;
	struct vr_trigger_shader_reference const *reference = NULL;
	struct shader *shader;
	short node_index, shader_index;

	vr_trigger.model_index = NONE;
	if (model_index == NONE)
		return;
	model = model_definition_get(model_index);
	node_index = model_find_node(model_index, BLADE_NODE);
	for (shader_index = 0; shader_index < model->shaders.count; shader_index++)
	{
		struct vr_trigger_shader_reference const *candidate =
			TAG_BLOCK_GET_ELEMENT(&model->shaders, shader_index, struct vr_trigger_shader_reference);
		char const *name = candidate->shader.index != NONE ? tag_get_name(candidate->shader.index) : NULL;

		if (name && !strcmp(name, BLADE_SHADER))
		{
			reference = candidate;
			break;
		}
	}
	if (node_index == NONE || !VALID_INDEX(node_index, MIN(model->nodes.count, MAXIMUM_NODES_PER_MODEL)) || !reference)
	{
		platform_log("vr trigger: %s has no %s or no %s: no trigger drawn", BLADE_MODEL, BLADE_NODE, BLADE_SHADER);
		return;
	}
	/* (a model shader, opaque, as the receiver's is: drawn as the rifle's
	solid parts are) */
	shader = shader_definition_get(reference->shader.index);
	if (shader->base.type != _shader_type_model ||
		TEST_FLAG(((struct vr_trigger_shader_model *)shader)->flags, _shader_model_alpha_blended_decal_bit))
	{
		platform_log("vr trigger: %s is not an opaque model shader: no trigger drawn", BLADE_SHADER);
		return;
	}

	blade_build(vertices, triangles);
	rasterizer_geometry_compress_vertices(_rasterizer_vertex_type_model_uncompressed, NUMBER_OF_BLADE_VERTICES,
		compressed, NUMBER_OF_BLADE_VERTICES * vertex_size, vertices, sizeof(vertices));
	if (!rasterizer_vertex_buffer_new(&vr_trigger.vertex_buffer, _rasterizer_vertex_type_model_compressed,
			NUMBER_OF_BLADE_VERTICES, compressed, NUMBER_OF_BLADE_VERTICES * vertex_size) ||
		!rasterizer_triangle_buffer_new(&vr_trigger.triangle_buffer, _triangle_buffer_type_triangles,
			NUMBER_OF_BLADE_TRIANGLES, triangles))
	{
		rasterizer_vertex_buffer_delete(&vr_trigger.vertex_buffer);
		return;
	}
	vr_trigger.model_index = model_index;
	vr_trigger.node_index = node_index;
	vr_trigger.shader = shader;
	vr_trigger.permutation_index = reference->permutation_index;
	platform_log("vr trigger: %s: a trigger drawn from %s with %s (%d vertices, %d triangles; base map scale %.3f %.3f)",
		BLADE_MODEL, BLADE_NODE, BLADE_SHADER, NUMBER_OF_BLADE_VERTICES, NUMBER_OF_BLADE_TRIANGLES,
		model->base_map_scale.i, model->base_map_scale.j);
}

/* render.c, as a map is unloaded */
void vr_trigger_dispose_from_old_map(void)
{
	if (vr_trigger.model_index == NONE)
		return;
	rasterizer_triangle_buffer_delete(&vr_trigger.triangle_buffer);
	rasterizer_vertex_buffer_delete(&vr_trigger.vertex_buffer);
	vr_trigger.model_index = NONE;
}

/* first_person_weapons.c, just after a first-person weapon's model is drawn
with these (render_model's): the blade, if it is the rifle's, back as far
as the finger pulls it */
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
	struct model *model;
	struct rasterizer_model_begin_parameters parameters;
	real_matrix4x3 relative, turn, turned;
	real_point3d middle;
	real angle, cosine, sine;

	if (model_index == NONE || model_index != vr_trigger.model_index)
		return;
	model = model_definition_get(model_index);
	/* the node's, as render_model skins a model's vertices: its matrix by its
	default pose's inverse */
	matrix4x3_multiply(&node_matrices[vr_trigger.node_index],
		&TAG_BLOCK_GET_ELEMENT(&model->nodes, vr_trigger.node_index, struct model_node)->runtime_default_inverse_matrix,
		&relative);
	/* turned back about its top, its face at the finger's height as far
	back as the finger pulls it (about the left axis: the tip back, toward
	the grip) */
	angle = vr_hands_trigger_travel() / (BLADE_PIVOT_UP - BLADE_PULL_UP);
	if (angle > 0.0f)
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
		turn.position.x = BLADE_PIVOT_FORWARD - (cosine * BLADE_PIVOT_FORWARD + sine * BLADE_PIVOT_UP);
		turn.position.y = 0.0f;
		turn.position.z = BLADE_PIVOT_UP - (-sine * BLADE_PIVOT_FORWARD + cosine * BLADE_PIVOT_UP);
		matrix4x3_multiply(&relative, &turn, &turned);
		relative = turned;
	}
	middle.x = blade_rows[NUMBER_OF_BLADE_ROWS / 2][1];
	middle.y = 0.0f;
	middle.z = blade_rows[NUMBER_OF_BLADE_ROWS / 2][0];

	/* (as render_model begins a model) */
	memset(&parameters, 0, sizeof(parameters));
	parameters.unique_identifier = unique_identifier;
	parameters.lighting = *lighting;
	matrix4x3_transform_point(&node_matrices[vr_trigger.node_index], &middle, &parameters.centroid);
	parameters.radius = 0.0f;
	parameters.effect = *model_effect;
	parameters.animation.colors = change_colors;
	parameters.animation.values = function_values;
	parameters.skinning.node_matrices = &relative;
	parameters.skinning.node_matrix_count = 1;
	parameters.geometry_flags = 0;
	SET_FLAG(parameters.geometry_flags, _rasterizer_geometry_first_person_bit,
		TEST_FLAG(flags, _render_model_first_person_bit));
	parameters.base_map_scale = model->base_map_scale;
	rasterizer_model_begin(&parameters, FALSE);
	rasterizer_model_draw(vr_trigger.shader, vr_trigger.permutation_index, &vr_trigger.triangle_buffer, NONE,
		vr_trigger.triangle_buffer.count, &vr_trigger.vertex_buffer, NONE);
	rasterizer_model_end();
}

#endif
