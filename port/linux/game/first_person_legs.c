/*
FIRST_PERSON_LEGS.C

Your own legs in first person (Video Setup > FOV and Viewmodels > Legs): the
player's own body, which the game animates as everyone else sees it but does
not draw for its own player, drawn from the waist down. Look down while
standing, running, strafing or crouching and the legs move as the body's
animation moves them.

Only the drawing is changed: a copy of the body's pose, all but its hips and
legs folded into a lid over the waist, the whole set back a little behind
the eye so the camera never looks out through the hips. The body itself, its hit boxes and markers, are the game's
as ever. The legs are drawn in the world with the world's camera, so they
keep their place and size at any field of view.

In the VR mode the whole body is drawn but its head and arms (the arms are
the VR mode's own, from its shoulders to the controllers: vr_hands.c), and
where the real body is rather than the game's: the game's eye is where the
head was at the last recentring and the unit faces where the gun aims, but
the head moves about and turns on its own. So the copy of the pose stands
upright and square over its hips (the game's leans into its aim, the chest
turned to the gun while the feet stay planted), its neck under the real
head, turned to the body's heading (vr.c's: the head's, which it follows
only past a turn and slowly; the holsters' and the arms' shoulders' too),
and it is not set back: the chest closes the waist, and looking down shows
the chest, the legs and the feet below the head, as the arms reach from its
shoulders. Its upper body leans back about the waist as far as keeps it out
of the eyes' near plane, and the collar's opening behind the eyes
(first_person_legs_vr_lean): looking down, the eyes nod forward over the
chest.
*/

#include "cseries.h"
#include "real_math.h"
#include "objects/objects.h"
#include "models/model_definitions.h"
#include "units/units.h"
#include "render/render.h"
#include "rasterizer/rasterizer.h"
#include <xtl.h>
#include "rasterizer/xbox/rasterizer_xbox.h"

#include "object_mesh.h"


#include <string.h>

#ifdef HALO_VR
/* port/linux/game/vr_render.c */
boolean vr_render_body_anchor(real_point3d *neck, real *yaw);
boolean vr_render_body_eyes(real_point3d eyes[2], real *near, real *seconds);
#endif

/* port/linux/src/port_config.c */
int config_boolean(const char *name);
unsigned long config_changes(void);

enum
{
	FIRST_PERSON_LEGS_MAXIMUM_NODES = 64,
	FIRST_PERSON_LEGS_MAXIMUM_RIM = 64,
};

/* how far the body is set back behind the eye at least, world units (about
12 cm), and at most (about 1 m) */
#define FIRST_PERSON_LEGS_SET_BACK 0.04f
#define FIRST_PERSON_LEGS_SET_BACK_MAXIMUM 0.33f
/* how far out of the view the waist's rim is kept (world units: 3 cm) */
#define FIRST_PERSON_LEGS_OUT_OF_VIEW 0.01f
/* the point the upper body folds into: the middle of the waist's rim, a
little above it (world units: about 3 cm), so the skin that joined hips to
chest closes the waist as a low lid, flush with the belt (the body's model is
a hollow shell: folded higher, it stood up as a cone before the eye; lower,
it sank into a funnel) */
#define FIRST_PERSON_LEGS_LID_UP 0.01f

#ifdef HALO_VR
/* the VR mode's body leant back from the eyes (first_person_legs_vr_lean):
its upper body, turned back about the foot of its spine in steps of this
(radians), no farther than this, as far as keeps all of it at least the
eyes' near plane's distance times FIRST_PERSON_LEGS_CLEAR_SCALE and
FIRST_PERSON_LEGS_CLEAR_MORE (world units: about 2 cm) from either eye, and
the top of its neck FIRST_PERSON_LEGS_NECK_BEHIND behind the eyes (world
units, along its heading: about 27 cm); coming back upright no faster than
FIRST_PERSON_LEGS_UPRIGHT_SPEED (radians a second). Of the body's mesh, so
many of its vertices at most are weighed (those that can come that near) */
#define FIRST_PERSON_LEGS_LEAN_STEP 0.025f
#define FIRST_PERSON_LEGS_LEAN_MAXIMUM 0.75f
#define FIRST_PERSON_LEGS_CLEAR_SCALE 1.1f
#define FIRST_PERSON_LEGS_CLEAR_MORE 0.007f
#define FIRST_PERSON_LEGS_NECK_BEHIND 0.09f
#define FIRST_PERSON_LEGS_UPRIGHT_SPEED 1.0f
#define FIRST_PERSON_LEGS_LEAN_VERTICES 4096
#endif

static real_matrix4x3 first_person_legs_matrices[FIRST_PERSON_LEGS_MAXIMUM_NODES];
static boolean first_person_legs_enabled(
	void)
{
	static unsigned long changes = (unsigned long)-1;
	static boolean enabled = FALSE;

	if (changes != config_changes())
	{
		changes = config_changes();
		enabled = config_boolean("display.first_person_legs") != 0;
	}
	return enabled;
}

/* the player's own unit, seen from inside it: its legs drawn (on foot,
alive, not hidden) */
boolean first_person_legs_wanted(
	long object_index)
{
	struct object_datum *object = object_try_and_get(object_index);

	return first_person_legs_enabled() && object && object->object.type == _object_type_biped &&
		object->object.parent_object_index == NONE &&
		!TEST_FLAG(object->object.damage_flags, _object_dead_bit) &&
		!TEST_FLAG(object->object.flags, _object_invisible_bit);
}

/* whether a node's name has part in it, whatever its case (the game's
characters name theirs "bip01 l thigh", "Bip01 Pelvis") */
static boolean first_person_legs_name_has(
	char const *name,
	char const *part)
{
	char lower[TAG_STRING_LENGTH + 1];
	short index;

	for (index = 0; index < TAG_STRING_LENGTH && name[index]; index++)
		lower[index] = (char)(name[index] >= 'A' && name[index] <= 'Z' ? name[index] - 'A' + 'a' : name[index]);
	lower[index] = 0;
	return strstr(lower, part) != NULL;
}

/* a node of the legs: the hips, thighs, calves, feet and toes */
static boolean first_person_legs_node(
	char const *name)
{
	static char const *const parts[] = { "pelvis", "thigh", "calf", "foot", "toe", "leg" };
	short index;

	for (index = 0; index < (short)NUMBEROF(parts); index++)
	{
		if (first_person_legs_name_has(name, parts[index]))
			return TRUE;
	}
	return FALSE;
}

/* the middle of the waist's rim in the model's space: the hips' vertices
on triangles that run up into the body above them (FALSE: none), kept for
the mesh it was found in (a new map forgets it: first_person_legs_reset) */
static real_point3d first_person_legs_rim[FIRST_PERSON_LEGS_MAXIMUM_RIM];
static short first_person_legs_rim_count = 0;
static long first_person_legs_waist_key = NONE;
static long first_person_legs_waist_model = NONE;
static boolean first_person_legs_waist_found = FALSE;
static real_point3d first_person_legs_waist_middle;

/* a new map: the waist found, and the mesh it was found in, forgotten (the
new map's models have indices of their own) */
void first_person_legs_reset(
	void)
{
	first_person_legs_waist_key = NONE;
	first_person_legs_waist_model = NONE;
	first_person_legs_rim_count = 0;
	object_mesh_reset();
}

static boolean first_person_legs_waist(
	struct object_mesh const *mesh,
	struct model const *model,
	short pelvis,
	real_point3d *middle)
{
	real_point3d sum = { 0.f, 0.f, 0.f };
	long found = 0;
	long index;

	if (mesh->key == first_person_legs_waist_key && mesh->model_index == first_person_legs_waist_model)
	{
		*middle = first_person_legs_waist_middle;
		return first_person_legs_waist_found;
	}
	first_person_legs_rim_count = 0;
	for (index = 0; index < mesh->triangle_count; index++)
	{
		boolean hips = FALSE;
		boolean above = FALSE;
		short corner;

		for (corner = 0; corner < 3; corner++)
		{
			struct object_mesh_vertex const *vertex = &mesh->vertices[mesh->triangles[index][corner]];
			short n;

			if (vertex->nodes[0] == pelvis && (vertex->nodes[1] == pelvis || vertex->weight >= 32767))
				hips = TRUE;
			for (n = 0; n < 2; n++)
			{
				if (vertex->nodes[n] < model->nodes.count && (n == 0 || vertex->weight < 32767) &&
					!first_person_legs_node(TAG_BLOCK_GET_ELEMENT(&model->nodes, vertex->nodes[n], struct model_node)->name))
				{
					above = TRUE;
				}
			}
		}
		if (!hips || !above)
			continue;
		for (corner = 0; corner < 3; corner++)
		{
			struct object_mesh_vertex const *vertex = &mesh->vertices[mesh->triangles[index][corner]];

			if (vertex->nodes[0] == pelvis && (vertex->nodes[1] == pelvis || vertex->weight >= 32767))
			{
				sum.x += vertex->position.x;
				sum.y += vertex->position.y;
				sum.z += vertex->position.z;
				found++;
				if (first_person_legs_rim_count < FIRST_PERSON_LEGS_MAXIMUM_RIM)
					first_person_legs_rim[first_person_legs_rim_count++] = vertex->position;
			}
		}
	}
	first_person_legs_waist_key = mesh->key;
	first_person_legs_waist_model = mesh->model_index;
	first_person_legs_waist_found = found > 0;
	if (found > 0)
	{
		first_person_legs_waist_middle.x = sum.x / found;
		first_person_legs_waist_middle.y = sum.y / found;
		first_person_legs_waist_middle.z = sum.z / found + FIRST_PERSON_LEGS_LID_UP;
	}
	*middle = first_person_legs_waist_middle;
	return first_person_legs_waist_found;
}

#ifdef HALO_VR
/* a vector turned by the rotation from the frame from (forward, left, up)
to the frame to */
static void first_person_legs_turn(
	real_vector3d *v,
	real_vector3d const from[3],
	real_vector3d const to[3])
{
	real along[3];
	short axis;

	for (axis = 0; axis < 3; axis++)
		along[axis] = dot_product3d(v, &from[axis]);
	v->i = to[0].i * along[0] + to[1].i * along[1] + to[2].i * along[2];
	v->j = to[0].j * along[0] + to[1].j * along[1] + to[2].j * along[2];
	v->k = to[0].k * along[0] + to[1].k * along[1] + to[2].k * along[2];
}

/* a node's matrix turned so (first_person_legs_turn) about pivot, then
moved by move */
static void first_person_legs_rotate(
	real_matrix4x3 *matrix,
	real_vector3d const from[3],
	real_vector3d const to[3],
	real_point3d const *pivot,
	real_vector3d const *move)
{
	real_vector3d offset;

	first_person_legs_turn(&matrix->forward, from, to);
	first_person_legs_turn(&matrix->left, from, to);
	first_person_legs_turn(&matrix->up, from, to);
	vector_from_points3d(pivot, &matrix->position, &offset);
	first_person_legs_turn(&offset, from, to);
	matrix->position.x = pivot->x + offset.i + move->i;
	matrix->position.y = pivot->y + offset.j + move->j;
	matrix->position.z = pivot->z + offset.k + move->k;
}

/* the VR mode's body leant back about the foot of its spine (pivot; the
top of its neck; its heading's forward, and left), as little as keeps it
clear of the eyes: the eyes nod forward over the chest as the head looks
down (the neck stays under them), and the near plane, about 19 cm before
them, would cut into the armour of its chest and collar, showing it hollow.
Its hips and legs stand where they are, as do the holsters and the pouch
about them; its upper body leans back, out of the eyes' way, as far as it
must. Nor are the eyes over the opening of its collar (the head folded
away, the dark top of the neck, a pit before the eyes), which a chin would
hide: the eyes are before the neck, and looking down sees the chest, as
crouched, where the game's body is lower than the head. Each vertex of the
upper body is where the game would draw it: its nodes' blend, the part on
nodes that do not lean and the pivot (base), and the part turned about the
pivot (arm) */
static void first_person_legs_vr_lean(
	long object_index,
	struct model const *model,
	long count,
	real_point3d const *pivot,
	real_point3d const *neck_top,
	real_vector3d const *forward,
	real_vector3d const *left)
{
	static real_point3d base[FIRST_PERSON_LEGS_LEAN_VERTICES];
	static real_vector3d arm[FIRST_PERSON_LEGS_LEAN_VERTICES];
	static real lean = 0.f;
	struct object_mesh const *mesh = object_mesh_get(object_index);
	real_matrix4x3 skin[FIRST_PERSON_LEGS_MAXIMUM_NODES];
	boolean leans[FIRST_PERSON_LEGS_MAXIMUM_NODES];
	real_point3d eyes[2];
	real_vector3d from[3], to[3], move;
	real near, seconds, clear, wanted, angle, eyes_ahead, neck_forward, neck_up;
	long index, weighed = 0;
	short eye;

	if (!mesh || mesh->node_count < count || !vr_render_body_eyes(eyes, &near, &seconds))
		return;
	clear = near * FIRST_PERSON_LEGS_CLEAR_SCALE + FIRST_PERSON_LEGS_CLEAR_MORE;
	/* (the eyes ahead of the pivot, and the top of the neck from it, along
	the heading and up) */
	eyes_ahead = ((eyes[0].x + eyes[1].x) * 0.5f - pivot->x) * forward->i +
		((eyes[0].y + eyes[1].y) * 0.5f - pivot->y) * forward->j;
	neck_forward = (neck_top->x - pivot->x) * forward->i + (neck_top->y - pivot->y) * forward->j;
	neck_up = neck_top->z - pivot->z;
	for (index = 0; index < count; index++)
	{
		struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node);

		leans[index] = node->parent_node_index != NONE && !first_person_legs_node(node->name);
		matrix4x3_multiply(&first_person_legs_matrices[index], &mesh->inverse[index], &skin[index]);
	}

	/* (the upper body's vertices that a lean could bring within clear of an
	eye: its arm's reach about the pivot crosses that sphere) */
	for (index = 0; index < mesh->vertex_count && weighed < FIRST_PERSON_LEGS_LEAN_VERTICES; index++)
	{
		struct object_mesh_vertex const *vertex = &mesh->vertices[index];
		real weights[2], reach, distance;
		real_point3d b = { 0.f, 0.f, 0.f };
		real_vector3d a = { 0.f, 0.f, 0.f };
		boolean near_eye = FALSE, shown = FALSE;
		short n;

		weights[0] = vertex->weight >= 32767 ? 1.f : vertex->weight / 32767.f;
		weights[1] = 1.f - weights[0];
		for (n = 0; n < 2; n++)
		{
			real_point3d p;

			if (weights[n] <= 0.f || vertex->nodes[n] >= count)
				continue;
			matrix4x3_transform_point(&skin[vertex->nodes[n]], &vertex->position, &p);
			if (first_person_legs_matrices[vertex->nodes[n]].scale > 0.01f)
				shown = TRUE;
			if (leans[vertex->nodes[n]])
			{
				a.i += (p.x - pivot->x) * weights[n];
				a.j += (p.y - pivot->y) * weights[n];
				a.k += (p.z - pivot->z) * weights[n];
				b.x += pivot->x * weights[n];
				b.y += pivot->y * weights[n];
				b.z += pivot->z * weights[n];
			}
			else
			{
				b.x += p.x * weights[n];
				b.y += p.y * weights[n];
				b.z += p.z * weights[n];
			}
		}
		/* (not the head and arms folded away: a point, not seen) */
		reach = magnitude3d(&a);
		if (reach <= 0.f || !shown)
			continue;
		for (eye = 0; eye < 2; eye++)
		{
			distance = distance3d(&b, &eyes[eye]);
			if (distance - reach < clear && reach - distance < clear)
				near_eye = TRUE;
		}
		if (!near_eye)
			continue;
		base[weighed] = b;
		arm[weighed] = a;
		weighed++;
	}

	/* (the least lean that keeps them all clear; leaning back at once, as
	far as needed, and coming upright again smoothly) */
	for (wanted = 0.f; wanted < FIRST_PERSON_LEGS_LEAN_MAXIMUM; wanted += FIRST_PERSON_LEGS_LEAN_STEP)
	{
		real c = (real)cos(wanted), s = (real)sin(wanted);
		boolean too_near = neck_forward * c - neck_up * s > eyes_ahead - FIRST_PERSON_LEGS_NECK_BEHIND;

		for (index = 0; index < weighed && !too_near; index++)
		{
			real_vector3d const *a = &arm[index];
			real along_forward = dot_product3d(a, forward), along_up = a->k;
			real_point3d p;

			/* (turned back: forward toward up, up toward backward) */
			p.x = base[index].x + a->i + forward->i * (along_forward * (c - 1.f) - along_up * s);
			p.y = base[index].y + a->j + forward->j * (along_forward * (c - 1.f) - along_up * s);
			p.z = base[index].z + a->k + along_forward * s + along_up * (c - 1.f);
			for (eye = 0; eye < 2; eye++)
			{
				if (distance_squared3d(&p, &eyes[eye]) < clear * clear)
					too_near = TRUE;
			}
		}
		if (!too_near)
			break;
	}
	if (wanted > FIRST_PERSON_LEGS_LEAN_MAXIMUM)
		wanted = FIRST_PERSON_LEGS_LEAN_MAXIMUM;
	if (wanted < lean - FIRST_PERSON_LEGS_UPRIGHT_SPEED * seconds)
		wanted = lean - FIRST_PERSON_LEGS_UPRIGHT_SPEED * seconds;
	lean = wanted;
	if (lean <= 0.f)
		return;

	angle = lean;
	from[0] = *forward;
	from[1] = *left;
	from[2] = *global_up3d;
	to[0].i = forward->i * (real)cos(angle);
	to[0].j = forward->j * (real)cos(angle);
	to[0].k = (real)sin(angle);
	to[1] = *left;
	to[2].i = -forward->i * (real)sin(angle);
	to[2].j = -forward->j * (real)sin(angle);
	to[2].k = (real)cos(angle);
	move = *global_zero_vector3d;
	for (index = 0; index < count; index++)
	{
		if (leans[index])
			first_person_legs_rotate(&first_person_legs_matrices[index], from, to, pivot, &move);
	}
}

/* the pose to draw the body with in the VR mode, the head's place and the
body's heading the real ones (vr_render_body_anchor); NULL: the game's
(no eye pass, a body without a spine, a neck and upper arms named) */
static real_matrix4x3 const *first_person_legs_vr_body(
	long object_index,
	struct object_datum const *object,
	struct model const *model,
	long count,
	real_matrix4x3 const *nodes)
{
	short spine = NONE, neck = NONE, head = NONE, left_arm = NONE, right_arm = NONE;
	short index;
	real_point3d anchor, collar;
	real_vector3d from[3], to[3], move;
	real heading, along, c, s;

	if (!vr_render_body_anchor(&anchor, &heading))
		return NULL;
	for (index = 0; index < count; index++)
	{
		char const *name = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node)->name;

		if (spine == NONE && first_person_legs_name_has(name, "spine"))
			spine = index;
		if (neck == NONE && first_person_legs_name_has(name, "neck"))
			neck = index;
		if (head == NONE && first_person_legs_name_has(name, "head"))
			head = index;
		if (left_arm == NONE && first_person_legs_name_has(name, "l upperarm"))
			left_arm = index;
		if (right_arm == NONE && first_person_legs_name_has(name, "r upperarm"))
			right_arm = index;
	}
	if (spine == NONE || neck == NONE || head == NONE || left_arm == NONE || right_arm == NONE)
		return NULL;

	/* (the upper body upright and square over the hips: the frame of its
	spine, from its foot up to the neck and across the shoulders, turned
	about its foot to the body's own, upright and to the unit's left) */
	vector_from_points3d(&nodes[spine].position, &nodes[neck].position, &from[2]);
	vector_from_points3d(&nodes[right_arm].position, &nodes[left_arm].position, &from[1]);
	if (normalize3d(&from[2]) <= 0.f)
		return NULL;
	along = dot_product3d(&from[1], &from[2]);
	from[1].i -= from[2].i * along;
	from[1].j -= from[2].j * along;
	from[1].k -= from[2].k * along;
	if (normalize3d(&from[1]) <= 0.f)
		return NULL;
	cross_product3d(&from[1], &from[2], &from[0]);
	to[0].i = object->object.forward.i;
	to[0].j = object->object.forward.j;
	to[0].k = 0.f;
	if (normalize3d(&to[0]) <= 0.f)
		return NULL;
	to[1].i = -to[0].j;
	to[1].j = to[0].i;
	to[1].k = 0.f;
	to[2] = *global_up3d;
	move = *global_zero_vector3d;
	for (index = 0; index < count; index++)
	{
		struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node);

		first_person_legs_matrices[index] = nodes[index];
		if (node->parent_node_index != NONE && !first_person_legs_node(node->name))
			first_person_legs_rotate(&first_person_legs_matrices[index], from, to, &nodes[spine].position, &move);
	}
	/* (the head folded into the top of the neck, and each arm into its
	shoulder, closing them: folded into the base of the neck, the neck
	opened a dark hole before the eyes, and the arms drew the armpits up
	across the chest; the collarbones are the shoulders' tops) */
	collar = first_person_legs_matrices[neck].position;
	for (index = 0; index < count; index++)
	{
		short up;

		for (up = index; up != NONE && up != head && up != left_arm && up != right_arm;
			up = TAG_BLOCK_GET_ELEMENT(&model->nodes, up, struct model_node)->parent_node_index);
		if (up != NONE)
		{
			first_person_legs_matrices[index].position = first_person_legs_matrices[up].position;
			first_person_legs_matrices[index].scale = 0.001f;
		}
	}

	/* (all of it turned from the unit's facing to the body's heading about
	the base of the neck, and that moved under the top of the real neck, at
	its own height: the body stands where it stands, the head bowing or
	not) */
	from[0] = to[0];
	from[1] = to[1];
	from[2] = to[2];
	c = (real)cos(heading);
	s = (real)sin(heading);
	to[0].i = c;
	to[0].j = s;
	to[0].k = 0.f;
	to[1].i = -s;
	to[1].j = c;
	to[1].k = 0.f;
	move.i = anchor.x - collar.x;
	move.j = anchor.y - collar.y;
	move.k = 0.f;
	for (index = 0; index < count; index++)
		first_person_legs_rotate(&first_person_legs_matrices[index], from, to, &collar, &move);
	/* (and its upper body leant back, as far as keeps it clear of the eyes) */
	first_person_legs_vr_lean(object_index, model, count, &first_person_legs_matrices[spine].position,
		&first_person_legs_matrices[head].position, &to[0], &to[1]);
	return first_person_legs_matrices;
}
#endif

/* the pose to draw the legs with: the body's own (as it is drawn now), all
but the legs folded into the hips, the whole set back behind the eye; NULL:
none (a body without named legs) */
real_matrix4x3 const *first_person_legs_node_matrices(
	long object_index,
	real_matrix4x3 const *nodes)
{
	struct object_datum *object = object_try_and_get(object_index);
	struct object_definition *definition;
	struct model const *model;
	long count;
	short pelvis = NONE;
	short thighs = 0;
	short index;
	real_point3d fold;
	real_vector3d back;
	real_matrix4x3 skin;
	real set_back;

	if (!object || !nodes)
		return NULL;
	definition = object_definition_get(object->definition_index);
	if (!definition || definition->object.model.index == NONE)
		return NULL;
	model = model_definition_get(definition->object.model.index);
	count = object->object.node_matrices.size / (long)sizeof(real_matrix4x3);
	if (!model || count <= 0 || count > FIRST_PERSON_LEGS_MAXIMUM_NODES || model->nodes.count < count)
		return NULL;

	for (index = 0; index < count; index++)
	{
		struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node);

		if (pelvis == NONE && first_person_legs_name_has(node->name, "pelvis"))
			pelvis = index;
		if (first_person_legs_name_has(node->name, "thigh"))
			thighs++;
	}
	if (pelvis == NONE || thighs == 0)
		return NULL;
#ifdef HALO_VR
	{
		real_matrix4x3 const *body = first_person_legs_vr_body(object_index, object, model, count, nodes);

		if (body)
			return body;
	}
#endif
	/* (the waist's middle as the hips are posed now) */
	{
		struct object_mesh const *mesh = object_mesh_get(object_index);
		real_point3d middle;

		if (!mesh || pelvis >= mesh->node_count || !first_person_legs_waist(mesh, model, pelvis, &middle))
			return NULL;
		matrix4x3_multiply(&nodes[pelvis], &mesh->inverse[pelvis], &skin);
		matrix4x3_transform_point(&skin, &middle, &fold);
	}

	/* (set back away from the way the eye looks, level) */
	back.i = -global_window_parameters.camera.forward.i;
	back.j = -global_window_parameters.camera.forward.j;
	back.k = 0.f;
	if (normalize3d(&back) <= 0.f)
		back = *global_zero_vector3d;

	/* (as far back as keeps the waist's rim out of the bottom of the view,
	as Halo 2 keeps its first person body's open top out of it: looking
	ahead, the least; looking down, the legs stand behind the eye, the
	thighs, knees and feet seen as they swing forward) */
	set_back = FIRST_PERSON_LEGS_SET_BACK;
	{
		real_plane3d const *bottom = &global_window_parameters.frustum.world_planes[2];
		real toward = dot_product3d(&bottom->n, &back);
		short corner;

		if (toward > 0.05f)
		{
			for (corner = 0; corner < first_person_legs_rim_count; corner++)
			{
				real_point3d p;
				real distance;

				matrix4x3_transform_point(&skin, &first_person_legs_rim[corner], &p);
				distance = plane3d_distance_to_point(bottom, &p) + toward * FIRST_PERSON_LEGS_SET_BACK;
				if (distance < FIRST_PERSON_LEGS_OUT_OF_VIEW)
				{
					real needed = FIRST_PERSON_LEGS_SET_BACK + (FIRST_PERSON_LEGS_OUT_OF_VIEW - distance) / toward;

					if (needed > set_back)
						set_back = needed;
				}
			}
		}
		if (set_back > FIRST_PERSON_LEGS_SET_BACK_MAXIMUM)
			set_back = FIRST_PERSON_LEGS_SET_BACK_MAXIMUM;
	}

	for (index = 0; index < count; index++)
	{
		struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node);
		real_matrix4x3 *matrix = &first_person_legs_matrices[index];

		*matrix = nodes[index];
		/* (the root kept: it is the body's frame, not a part of it) */
		if (node->parent_node_index != NONE && !first_person_legs_node(node->name))
		{
			matrix->position = fold;
			matrix->scale = 0.001f;
		}
		matrix->position.x += back.i * set_back;
		matrix->position.y += back.j * set_back;
		matrix->position.z += back.k * set_back;
	}
	return first_person_legs_matrices;
}
