/*
RENDER_INTERPOLATION.C

Frames between the game's 30 Hz ticks, for the native ports (port/linux,
port/android, port/windows; see port/linux/README.md, "Frame rate").

The game simulates in 30 Hz ticks and originally drew one frame per tick.
The ports draw at the display's refresh rate instead, and every frame shows
the world between the last two ticks: after each tick the camera, every
object's node matrices and the first-person weapon's pose are kept, and a
frame blends the previous and the latest by how far the game clock has run
into the next tick, and so are what an object's shaders read (its function
values and change colors) and a unit's active camouflage. That puts what is
drawn one tick (33 ms) behind the simulation, the usual price of
interpolation. (A Catmull-Rom spline would also need the tick after the pair
it spans: two ticks behind.)

Rotations are blended as quaternions (normalised lerp, taking the shorter
way round), positions and scales linearly. Anything that moves further than
a tick of motion plausibly allows (teleports, respawns, camera cuts) snaps
instead of sweeping across the world, and so does a pose whose nodes moved
too far for one tick: two snapshots of different poses (a model swapped,
another weapon's skeleton, a pose left from seconds before) blended node by
node stretch vertices across the screen.

Particles, contrails and other effects move every frame (game_frame), and
read the drawn pose of what they hang off. A stream of particles from an
object, made in the tick, is held back to be made through the frames between
the ticks (render_interpolation_particle_later).

An object made in a tick has no snapshot from the tick before: one moving
(a bolt, a grenade thrown) is drawn from a tick of its motion back, not
standing a tick where the tick left it. A projectile deleted in a tick (at
what it hit) is drawn a tick longer, from its last snapshot to where it was
deleted, as the effects it made there are drawn from the start of the tick:
a ghost, drawn only here, which nothing of the game sees.

An object the distributed netcode moves to where the host has it
(port/linux/game/network_objects.c) is drawn gliding there over a few ticks
rather than jumping: its snapshots move with it, and the difference is drawn
on top of it, whole until the next tick and fading from then on (no more than
a few world units of it: further is a jump). A local player's view glides so
with their unit, and with what it rides.

At game.sim_rate 60 and 120 (sim_rate.h) an object of the micro lane (a
player's biped, a projectile, an item loose in the world: objects.h's
object_get_lane) moves in the tick's 2 or 4 sub-steps, not once a tick, and
its path through the tick is not the straight line between the tick's
snapshots: a jump's arc, a turn, a start or a stop, a grenade's bounce. Where
each is after every sub-step is kept (the fast phase's
_sim_substep_render_interpolation slot), for two ticks: the one drawn, and
the next, whose sub-steps run while the frames draw the one before
(sim_substep_next). A frame draws it between the poses of the sub-step it is
in (sim_substep_blend: the end of the sub-step before, or of the tick
before for the first, and the end of its own), still a tick behind as
everything else is. Only where it is moves so: its nodes move in the
sub-steps as the tick before posed them (bipeds.c's biped_carry_nodes) and
take the tick's new pose at its last, so the pose, the rotations and what
it carries are blended between the ticks as before, moved by where the
sub-steps put it from where the line between the ticks has it. The
first-person view, which the observer poses from the player's unit, is
moved the same way, and with it the first-person weapon, which is posed
from the view. An object's first two ticks of sub-steps (made, or let go
of: a projectile's head start, a whole tick's travel at once, and its hold)
are drawn between the ticks as before, from a tick's motion back; and one
put somewhere in a tick still open (render_interpolation_object_from: the
VR mode's grenade let go of in a sub-step) is drawn there until the frames
reach that tick. Kept here only, for the frames: the simulation never reads
it. At 30 a tick is one sub-step, nothing is kept, and the frames are as
they were.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "objects/object_definitions.h"
#include "models/models.h"
#include "rasterizer/rasterizer_model_types.h"
#include "camera/camera_scripting.h"
#include "camera/director.h"
#include "camera/observer.h"
#include "cutscene/cinematics.h"
#include "effects/particles.h"
#include "game/game.h"
#include "game/players.h"
#include "render/render.h"
#include "render/render_cameras_internal.h"
#include "units/units.h"
#include "sim_rate.h"

/* port/linux/src/port_config.c */
int config_boolean(const char *name);
unsigned long config_changes(void);
/* render_objects.c */
boolean render_objects_port_last_lighting(long object_index, struct render_lighting *lighting);

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------- constants */

/* (by absolute index: the object array's all) */
#define MAXIMUM_INTERPOLATED_OBJECTS MAXIMUM_OBJECTS_PER_MAP
#define MAXIMUM_INTERPOLATED_NODES 64

/* world units (10 feet each) a node may move in one tick before it snaps:
well beyond any vehicle, short of any teleport */
#define OBJECT_SNAP_DISTANCE 10.0f
/* ... a node may move in the object's root node's frame in one tick before
the pose is taken for a new one, not blended to: further than any limb or
part of a model moves in 33 ms (37 m/s), short of the game changing a pose
at once (an actor waking from dormancy, a model swapped), which blended
sweeps the vertices through poses it never had. (In the root's frame, so
that the whole object turning moves nothing: measured in the world, a fast
turn swung the head and weapon far enough to snap, and characters moved
like robots.) */
#define NODE_SNAP_DISTANCE 0.4f
/* ... a first-person node may move relative to the camera */
#define FIRST_PERSON_SNAP_DISTANCE 0.25f
/* a correction's difference left drawn after each tick (of 1) */
#define CORRECTION_DECAY 0.6f
/* ... and small enough to be none */
#define CORRECTION_NEGLIGIBLE 0.001f
/* ... or so large that it is no glide but a jump (corrections come one on
another: their sum is drawn): past the largest a client's own unit or
vehicle is corrected by (3 and 4 world units), short of a snap */
#define CORRECTION_MAXIMUM 8.0f
/* a camera cut: a jump or turn no player or scripted camera makes in 33 ms */
#define CAMERA_CUT_DISTANCE 3.0f
#define CAMERA_CUT_COSINE 0.5f
/* particles held back to be made between the ticks
(render_interpolation_particle_later) */
#define MAXIMUM_LATER_PARTICLES 512
/* deleted projectiles drawn a tick longer (render_interpolation_object_deleted),
and the nodes each may have */
#define MAXIMUM_GHOSTS 64
#define MAXIMUM_GHOST_NODES 8
/* how far the sub-steps' path through a tick may stray from the line
between the tick's snapshots before it is taken for a jump, not a path:
past a fast projectile's bounce, short of a teleport */
#define SUBSTEP_OFFSET_MAXIMUM 2.0f

/* ---------- structures */

struct interpolation_quaternion
{
	real i, j, k, w;
};

/* a node's rotation: whether its basis is one a quaternion can hold, and
that quaternion */
struct interpolation_rotation
{
	struct interpolation_quaternion quaternion;
	boolean is_rotation;
};

struct interpolated_object
{
	long object_index; /* NONE when unused */
	long tick; /* the tick of the latest snapshot */
	short node_count;
	short node_capacity;
	boolean has_previous;
	byte latest; /* which snapshot is the latest */
	long blended_frame;
	/* where it is drawn from where it is: a correction fading, and those
	since the last tick, drawn whole until the next begins to fade them */
	real_vector3d correction;
	real_vector3d correction_pending;
	/* [0] and [1]: the two snapshots, [2]: the blend drawn this frame */
	real_matrix4x3 *nodes;
	/* ... the two snapshots' rotations, found when first blended */
	struct interpolation_rotation *rotations;
	boolean rotations_valid[2];
	/* the two snapshots' function values, change colors and (a unit's)
	active camouflage, by snapshot, and [2] their blend drawn this frame */
	real function_values[3][NUMBER_OF_OUTGOING_OBJECT_FUNCTIONS];
	real_rgb_color change_colors[3][NUMBER_OF_OBJECT_CHANGE_COLORS];
	real camouflage[3][2];
	long values_blended_frame;
	/* where the object was at the latest snapshot, and at the one before
	(as made: a tick's motion back, or render_interpolation_object_from's) */
	real_point3d position;
	real_point3d previous_position;
};

/* an object of the micro lane, by itself (not carried): where it was after
each sub-step of a tick, for two ticks, [tick & 1]: the tick drawn, and the
next, whose sub-steps run meanwhile */
struct interpolated_substeps
{
	long object_index; /* NONE when unused */
	long tick[2]; /* (as interpolation_tick counts them) NONE: none */
	short count[2]; /* the tick's sub-steps */
	byte recorded[2]; /* those kept, a bit each */
	/* every sub-step of the tick before kept too */
	boolean continuing[2];
	real_point3d positions[2][MAXIMUM_SIM_SUBSTEPS];
	/* where it is drawn this frame, between two of the drawn tick's */
	long drawn_frame;
	boolean drawn_valid;
	real_point3d drawn;
};

struct interpolated_camera
{
	long tick;
	boolean valid;
	boolean has_previous;
	struct observer_result previous;
	struct observer_result latest;
	struct observer_result blended;
	/* its player's unit (or what it rides) corrected: as an object's */
	real_vector3d correction;
	real_vector3d correction_pending;
};

/* a particle held back */
struct later_particle
{
	struct new_particle_data data;
	long object_index;
	short node_index;
	/* where on the node it was made */
	real_point3d node_position;
	/* made from this tick's frames on, this far into it */
	long tick;
	real fraction;
};

struct interpolated_first_person
{
	long tick;
	short node_count;
	boolean has_previous;
	real_matrix4x3 previous[MAXIMUM_INTERPOLATED_NODES];
	real_matrix4x3 latest[MAXIMUM_INTERPOLATED_NODES];
};

/* a projectile deleted in a tick, drawn through the frames after it */
struct interpolation_ghost
{
	boolean in_use;
	long object_index;
	long tick; /* drawn in the frames after this tick; NONE until it ends */
	long model_index;
	short node_count;
	short forced_shader_permutation_index;
	/* the last snapshot, and where it was then and from there when deleted */
	real_point3d position;
	real_vector3d offset;
	/* the bounding sphere, from its position */
	real_vector3d center_offset;
	real radius;
	real_matrix4x3 nodes[MAXIMUM_GHOST_NODES];
	real_matrix4x3 drawn[MAXIMUM_GHOST_NODES];
	byte region_permutations[MAXIMUM_REGIONS_PER_OBJECT];
	real function_values[NUMBER_OF_OUTGOING_OBJECT_FUNCTIONS];
	real_rgb_color change_colors[NUMBER_OF_OBJECT_CHANGE_COLORS];
	struct render_lighting lighting;
};

/* ---------- globals */

static struct interpolated_object *interpolated_objects;
static struct interpolated_camera interpolated_cameras[MAXIMUM_LOCAL_PLAYERS];
static struct interpolated_first_person interpolated_first_person[MAXIMUM_LOCAL_PLAYERS];
/* an object to be drawn from a place of this tick's choosing rather than
from where it was the tick before (render_interpolation_object_from) */
static struct
{
	long object_index;
	real_point3d position;
} interpolation_from = { NONE };
static long interpolation_tick;
static long interpolation_frame;
static boolean interpolation_rendering;
static real interpolation_fraction = 1.0f;
static struct later_particle later_particles[MAXIMUM_LATER_PARTICLES];
static short later_particle_count;
static struct interpolation_ghost ghosts[MAXIMUM_GHOSTS];
/* by absolute index, made at the first sub-step of a tick of several */
static struct interpolated_substeps *interpolated_substeps;
/* the frame's sub-steps a tick (sim_substep_count), the sub-step it is
drawn in and how far (sim_substep_blend) */
static short interpolation_substep_count = 1;
static short interpolation_substep_index;
static real interpolation_substep_fraction;

/* ---------- blending */

static real lerp(real a, real b, real t)
{
	return a + (b - a) * t;
}

static void point_lerp(real_point3d const *a, real_point3d const *b, real t, real_point3d *result)
{
	result->x = lerp(a->x, b->x, t);
	result->y = lerp(a->y, b->y, t);
	result->z = lerp(a->z, b->z, t);
}

static real vector_length(real_vector3d const *v)
{
	return (real)sqrt(v->i * v->i + v->j * v->j + v->k * v->k);
}

static void vector_nlerp(real_vector3d const *a, real_vector3d const *b, real t, real_vector3d *result)
{
	real length;

	result->i = lerp(a->i, b->i, t);
	result->j = lerp(a->j, b->j, t);
	result->k = lerp(a->k, b->k, t);
	length = vector_length(result);
	if (length > 1e-6f)
	{
		result->i /= length;
		result->j /= length;
		result->k /= length;
	}
	else
	{
		*result = *b;
	}
}

/* an orthonormal right-handed basis, which a quaternion can represent */
static boolean basis_is_rotation(real_matrix4x3 const *matrix)
{
	real_vector3d cross;
	real determinant;

	if (fabs(vector_length(&matrix->forward) - 1.0f) > 1e-2f ||
		fabs(vector_length(&matrix->left) - 1.0f) > 1e-2f ||
		fabs(vector_length(&matrix->up) - 1.0f) > 1e-2f)
	{
		return FALSE;
	}
	cross.i = matrix->forward.j * matrix->left.k - matrix->forward.k * matrix->left.j;
	cross.j = matrix->forward.k * matrix->left.i - matrix->forward.i * matrix->left.k;
	cross.k = matrix->forward.i * matrix->left.j - matrix->forward.j * matrix->left.i;
	determinant = cross.i * matrix->up.i + cross.j * matrix->up.j + cross.k * matrix->up.k;
	return determinant > 0.5f;
}

/* the basis vectors are the matrix's columns (x forward, y left, z up) */
static void quaternion_from_basis(real_matrix4x3 const *matrix, struct interpolation_quaternion *q)
{
	real m00 = matrix->forward.i, m10 = matrix->forward.j, m20 = matrix->forward.k;
	real m01 = matrix->left.i, m11 = matrix->left.j, m21 = matrix->left.k;
	real m02 = matrix->up.i, m12 = matrix->up.j, m22 = matrix->up.k;
	real trace = m00 + m11 + m22;
	real s;

	if (trace > 0.0f)
	{
		s = 0.5f / (real)sqrt(trace + 1.0f);
		q->w = 0.25f / s;
		q->i = (m21 - m12) * s;
		q->j = (m02 - m20) * s;
		q->k = (m10 - m01) * s;
	}
	else if (m00 > m11 && m00 > m22)
	{
		s = 2.0f * (real)sqrt(1.0f + m00 - m11 - m22);
		q->w = (m21 - m12) / s;
		q->i = 0.25f * s;
		q->j = (m01 + m10) / s;
		q->k = (m02 + m20) / s;
	}
	else if (m11 > m22)
	{
		s = 2.0f * (real)sqrt(1.0f + m11 - m00 - m22);
		q->w = (m02 - m20) / s;
		q->i = (m01 + m10) / s;
		q->j = 0.25f * s;
		q->k = (m12 + m21) / s;
	}
	else
	{
		s = 2.0f * (real)sqrt(1.0f + m22 - m00 - m11);
		q->w = (m10 - m01) / s;
		q->i = (m02 + m20) / s;
		q->j = (m12 + m21) / s;
		q->k = 0.25f * s;
	}
}

static void basis_from_quaternion(struct interpolation_quaternion const *q, real_matrix4x3 *matrix)
{
	real ii = q->i * q->i, jj = q->j * q->j, kk = q->k * q->k;
	real ij = q->i * q->j, ik = q->i * q->k, jk = q->j * q->k;
	real wi = q->w * q->i, wj = q->w * q->j, wk = q->w * q->k;

	matrix->forward.i = 1.0f - 2.0f * (jj + kk);
	matrix->forward.j = 2.0f * (ij + wk);
	matrix->forward.k = 2.0f * (ik - wj);
	matrix->left.i = 2.0f * (ij - wk);
	matrix->left.j = 1.0f - 2.0f * (ii + kk);
	matrix->left.k = 2.0f * (jk + wi);
	matrix->up.i = 2.0f * (ik + wj);
	matrix->up.j = 2.0f * (jk - wi);
	matrix->up.k = 1.0f - 2.0f * (ii + jj);
}

static void rotation_from_matrix(real_matrix4x3 const *matrix, struct interpolation_rotation *rotation)
{
	rotation->is_rotation = basis_is_rotation(matrix);
	if (rotation->is_rotation)
		quaternion_from_basis(matrix, &rotation->quaternion);
}

/* a matrix a fraction t of the way from a to b, their rotations found */
static void matrix_blend_rotations(
	real_matrix4x3 const *a,
	real_matrix4x3 const *b,
	struct interpolation_rotation const *rotation_a,
	struct interpolation_rotation const *rotation_b,
	real t,
	real_matrix4x3 *result)
{
	result->scale = lerp(a->scale, b->scale, t);
	point_lerp(&a->position, &b->position, t, &result->position);
	if (rotation_a->is_rotation && rotation_b->is_rotation)
	{
		struct interpolation_quaternion qa = rotation_a->quaternion, qb = rotation_b->quaternion, q;
		real length;

		/* q and -q are the same rotation: take the shorter way round */
		if (qa.i * qb.i + qa.j * qb.j + qa.k * qb.k + qa.w * qb.w < 0.0f)
		{
			qb.i = -qb.i;
			qb.j = -qb.j;
			qb.k = -qb.k;
			qb.w = -qb.w;
		}
		q.i = lerp(qa.i, qb.i, t);
		q.j = lerp(qa.j, qb.j, t);
		q.k = lerp(qa.k, qb.k, t);
		q.w = lerp(qa.w, qb.w, t);
		length = (real)sqrt(q.i * q.i + q.j * q.j + q.k * q.k + q.w * q.w);
		if (length > 1e-6f)
		{
			q.i /= length;
			q.j /= length;
			q.k /= length;
			q.w /= length;
			basis_from_quaternion(&q, result);
			return;
		}
	}
	/* a basis a quaternion cannot hold (scaled or mirrored): blend it as is */
	result->forward.i = lerp(a->forward.i, b->forward.i, t);
	result->forward.j = lerp(a->forward.j, b->forward.j, t);
	result->forward.k = lerp(a->forward.k, b->forward.k, t);
	result->left.i = lerp(a->left.i, b->left.i, t);
	result->left.j = lerp(a->left.j, b->left.j, t);
	result->left.k = lerp(a->left.k, b->left.k, t);
	result->up.i = lerp(a->up.i, b->up.i, t);
	result->up.j = lerp(a->up.j, b->up.j, t);
	result->up.k = lerp(a->up.k, b->up.k, t);
}

/* the vector's parts' sum, large enough to be a correction (so written that
one not a number is none) */
static boolean correction_significant(real_vector3d const *correction)
{
	return fabs(correction->i) + fabs(correction->j) + fabs(correction->k) >= CORRECTION_NEGLIGIBLE;
}

/* a correction a tick on: what was drawn fades, what came since is drawn
whole from now on, fading from the next */
static void correction_advance(real_vector3d *correction, real_vector3d *pending)
{
	correction->i = correction->i * CORRECTION_DECAY + pending->i;
	correction->j = correction->j * CORRECTION_DECAY + pending->j;
	correction->k = correction->k * CORRECTION_DECAY + pending->k;
	*pending = *global_zero_vector3d;
	if (!correction_significant(correction))
		*correction = *global_zero_vector3d;
}

/* a correction as drawn this frame: fading through the tick as it does tick
to tick, and those since the tick whole */
static void correction_drawn(real_vector3d const *correction, real_vector3d const *pending, real_vector3d *drawn)
{
	real fade = lerp(1.0f, CORRECTION_DECAY, interpolation_fraction);

	drawn->i = correction->i * fade + pending->i;
	drawn->j = correction->j * fade + pending->j;
	drawn->k = correction->k * fade + pending->k;
}

/* a correction added (offset): all of it dropped when the sum is too large
to glide (or not a number) */
static void correction_add(real_vector3d *correction, real_vector3d *pending, real_vector3d const *offset)
{
	real i, j, k;

	pending->i += offset->i;
	pending->j += offset->j;
	pending->k += offset->k;
	i = correction->i + pending->i;
	j = correction->j + pending->j;
	k = correction->k + pending->k;
	if (!(i * i + j * j + k * k <= CORRECTION_MAXIMUM * CORRECTION_MAXIMUM))
	{
		*correction = *global_zero_vector3d;
		*pending = *global_zero_vector3d;
	}
}

static real distance_squared(real_point3d const *a, real_point3d const *b)
{
	real x = a->x - b->x, y = a->y - b->y, z = a->z - b->z;

	return x * x + y * y + z * z;
}

/* ---------- ticks */

static void render_interpolation_ghosts_tick(void);

/* the record's snapshot of the tick before, made of its latest (its nodes
already placed): the latest's rotations, values and camouflage */
static void render_interpolation_previous_values(struct interpolated_object *record)
{
	record->rotations_valid[record->latest ^ 1] = FALSE;
	memcpy(record->function_values[record->latest ^ 1], record->function_values[record->latest],
		sizeof(record->function_values[0]));
	memcpy(record->change_colors[record->latest ^ 1], record->change_colors[record->latest],
		sizeof(record->change_colors[0]));
	memcpy(record->camouflage[record->latest ^ 1], record->camouflage[record->latest],
		sizeof(record->camouflage[0]));
	record->has_previous = TRUE;
}

static void render_interpolation_substep(short substep_index, short substep_count, real seconds);

/* the fast phase's slot (sim_rate.h), at every map's start and every tick
(registering again changes nothing) */
static void render_interpolation_register(void)
{
	sim_substep_register(_sim_substep_render_interpolation, render_interpolation_substep);
}

void render_interpolation_tick(void)
{
	struct object_iterator iterator;
	struct object_datum *object;
	long previous_tick = interpolation_tick++;

	render_interpolation_register();
	render_interpolation_ghosts_tick();
	if (!halo_interpolation_enabled())
	{
		interpolation_from.object_index = NONE;
		return;
	}
	/* the cameras' corrections a tick on, as the objects' (below) */
	{
		short local_player_index;

		for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
		{
			struct interpolated_camera *camera = &interpolated_cameras[local_player_index];

			if (camera->valid)
				correction_advance(&camera->correction, &camera->correction_pending);
		}
	}
	if (!interpolated_objects)
	{
		long index;

		interpolated_objects = calloc(MAXIMUM_INTERPOLATED_OBJECTS, sizeof(*interpolated_objects));
		if (!interpolated_objects)
			return;
		for (index = 0; index < MAXIMUM_INTERPOLATED_OBJECTS; index++)
			interpolated_objects[index].object_index = NONE;
	}

	object_iterator_new(&iterator, _object_mask_all, 0);
	while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
	{
		long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.index);
		struct interpolated_object *record;
		short node_count = (short)(object->object.node_matrices.size / (short)sizeof(real_matrix4x3));
		boolean continuing;
		boolean made;

		if (absolute_index >= MAXIMUM_INTERPOLATED_OBJECTS)
			continue;
		record = &interpolated_objects[absolute_index];
		/* (made since the last tick: none of its snapshots) */
		made = record->object_index != iterator.index;
		if (node_count <= 0 || node_count > MAXIMUM_INTERPOLATED_NODES)
		{
			record->object_index = NONE;
			continue;
		}
		if (record->node_capacity < node_count)
		{
			real_matrix4x3 *nodes = realloc(record->nodes, 3 * node_count * sizeof(real_matrix4x3));
			struct interpolation_rotation *rotations;

			if (!nodes)
			{
				record->object_index = NONE;
				continue;
			}
			record->nodes = nodes;
			rotations = realloc(record->rotations, 2 * node_count * sizeof(struct interpolation_rotation));
			if (!rotations)
			{
				record->object_index = NONE;
				continue;
			}
			record->rotations = rotations;
			record->node_capacity = node_count;
			record->object_index = NONE; /* the old snapshots moved */
		}
		continuing = record->object_index == iterator.index &&
			record->node_count == node_count &&
			record->tick == previous_tick;
		if (continuing)
		{
			record->latest ^= 1;
			correction_advance(&record->correction, &record->correction_pending);
		}
		else
		{
			record->correction = *global_zero_vector3d;
			record->correction_pending = *global_zero_vector3d;
		}
		memcpy(
			record->nodes + record->latest * record->node_capacity,
			object_get_node_matrices(iterator.index),
			node_count * sizeof(real_matrix4x3));
		record->rotations_valid[record->latest] = FALSE;
		record->object_index = iterator.index;
		record->node_count = node_count;
		record->tick = interpolation_tick;
		record->has_previous = continuing;
		record->blended_frame = NONE;
		memcpy(record->function_values[record->latest], object->object.outgoing_function_values,
			sizeof(record->function_values[0]));
		memcpy(record->change_colors[record->latest], object->object.outgoing_change_colors,
			sizeof(record->change_colors[0]));
		record->camouflage[record->latest][0] = 0.0f;
		record->camouflage[record->latest][1] = 0.0f;
		if (TEST_FLAG(_object_mask_unit, object->object.type))
		{
			struct unit_datum *unit = (struct unit_datum *)object;

			record->camouflage[record->latest][0] = unit->unit.active_camouflage;
			record->camouflage[record->latest][1] = unit->unit.active_camouflage_super_amount;
		}
		record->values_blended_frame = NONE;
		record->previous_position = continuing ? record->position : object->object.position;
		record->position = object->object.position;
		/* the tick's last sub-step's pose kept: where the tick left it (what
		the tick did after its fast phase too), as its snapshot has it */
		if (interpolated_substeps)
		{
			struct interpolated_substeps *substeps = &interpolated_substeps[absolute_index];
			short buffer = (short)(interpolation_tick & 1);

			if (substeps->object_index == iterator.index && substeps->tick[buffer] == interpolation_tick)
				substeps->positions[buffer][substeps->count[buffer] - 1] = object->object.position;
		}
		/* Made in this tick, and moving (a bolt, a rocket, a grenade
		thrown): it has no snapshot from the tick before, and was drawn
		standing for a tick where this one left it (a bolt hung a tick's
		motion out from the muzzle, then set off), and a tick early, ahead
		of the world drawn about it. The tick before's snapshot: this one's,
		a tick's motion back. (Not an object something carries, which moves
		with it; one that moves further than a tick allows snaps, as
		before.) */
		if (made && object->object.parent_object_index == NONE &&
			(object->object.translational_velocity.i != 0.0f ||
				object->object.translational_velocity.j != 0.0f ||
				object->object.translational_velocity.k != 0.0f))
		{
			real_matrix4x3 const *latest = record->nodes + record->latest * record->node_capacity;
			real_matrix4x3 *previous = record->nodes + (record->latest ^ 1) * record->node_capacity;
			real_vector3d const *velocity = &object->object.translational_velocity;
			short node_index;

			memcpy(previous, latest, node_count * sizeof(real_matrix4x3));
			for (node_index = 0; node_index < node_count; node_index++)
			{
				previous[node_index].position.x -= velocity->i;
				previous[node_index].position.y -= velocity->j;
				previous[node_index].position.z -= velocity->k;
			}
			record->previous_position.x -= velocity->i;
			record->previous_position.y -= velocity->j;
			record->previous_position.z -= velocity->k;
			render_interpolation_previous_values(record);
		}
		if (iterator.index == interpolation_from.object_index)
		{
			/* the tick before's snapshot: this one's, moved to the place */
			real_matrix4x3 const *latest = record->nodes + record->latest * record->node_capacity;
			real_matrix4x3 *previous = record->nodes + (record->latest ^ 1) * record->node_capacity;
			real_vector3d offset;
			short node_index;

			offset.i = interpolation_from.position.x - latest[0].position.x;
			offset.j = interpolation_from.position.y - latest[0].position.y;
			offset.k = interpolation_from.position.z - latest[0].position.z;
			memcpy(previous, latest, node_count * sizeof(real_matrix4x3));
			for (node_index = 0; node_index < node_count; node_index++)
			{
				previous[node_index].position.x += offset.i;
				previous[node_index].position.y += offset.j;
				previous[node_index].position.z += offset.k;
			}
			record->previous_position.x = record->position.x + offset.i;
			record->previous_position.y = record->position.y + offset.j;
			record->previous_position.z = record->position.z + offset.k;
			render_interpolation_previous_values(record);
		}
	}
	interpolation_from.object_index = NONE;
}

/* the object, put somewhere this tick, drawn from there to where the tick
leaves it, not from where it was the tick before: the VR mode's grenade let
go of from where the hand held it (port/linux/game/vr_grenade.c), not from
the hand of the player's body it hung from */
void render_interpolation_object_from(long object_index, real_point3d const *position)
{
	interpolation_from.object_index = object_index;
	interpolation_from.position = *position;
}

/* a new map (game.c): its objects take the indices of the last one's, and
nothing of theirs is drawn from */
void render_interpolation_reset(void)
{
	long index;

	render_interpolation_register();
	if (interpolated_objects)
	{
		for (index = 0; index < MAXIMUM_INTERPOLATED_OBJECTS; index++)
			interpolated_objects[index].object_index = NONE;
	}
	if (interpolated_substeps)
	{
		for (index = 0; index < MAXIMUM_INTERPOLATED_OBJECTS; index++)
			interpolated_substeps[index].object_index = NONE;
	}
	memset(interpolated_cameras, 0, sizeof(interpolated_cameras));
	/* (the particles held back and the ghosts are the last map's) */
	later_particle_count = 0;
	for (index = 0; index < MAXIMUM_GHOSTS; index++)
		ghosts[index].in_use = FALSE;
	for (index = 0; index < MAXIMUM_LOCAL_PLAYERS; index++)
	{
		interpolated_first_person[index].node_count = 0;
		interpolated_first_person[index].has_previous = FALSE;
	}
}

/* ---------- sub-steps */

/* (the fast phase's _sim_substep_render_interpolation slot, after every
other that moves anything) where each object of the micro lane is after the
sub-step, for the frames to draw it between them. Of the open tick, the one
after the tick drawn: kept beside the drawn tick's. Read only, of the game:
nothing of the simulation sees it */
static void render_interpolation_substep(short substep_index, short substep_count, real seconds)
{
	struct object_iterator iterator;
	struct object_datum *object;
	long tick = interpolation_tick + 1;
	short buffer = (short)(tick & 1);

	(void)seconds;
	if (substep_count <= 1 || substep_count > MAXIMUM_SIM_SUBSTEPS || !halo_interpolation_enabled())
		return;
	if (!interpolated_substeps)
	{
		long index;

		interpolated_substeps = calloc(MAXIMUM_INTERPOLATED_OBJECTS, sizeof(*interpolated_substeps));
		if (!interpolated_substeps)
			return;
		for (index = 0; index < MAXIMUM_INTERPOLATED_OBJECTS; index++)
			interpolated_substeps[index].object_index = NONE;
	}
	/* (the micro lane's types: objects.c's object_lane_micro_candidate) */
	object_iterator_new(&iterator, _object_mask_biped | _object_mask_projectile | _object_mask_item, 0);
	while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
	{
		long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.index);
		struct interpolated_substeps *substeps;

		/* (what is carried is drawn with what carries it) */
		if (absolute_index >= MAXIMUM_INTERPOLATED_OBJECTS || object->object.parent_object_index != NONE ||
			object_get_lane(iterator.index) != _object_lane_micro)
		{
			continue;
		}
		substeps = &interpolated_substeps[absolute_index];
		if (substeps->object_index != iterator.index)
		{
			substeps->object_index = iterator.index;
			substeps->tick[0] = NONE;
			substeps->tick[1] = NONE;
			substeps->drawn_frame = NONE;
		}
		/* (a tick given up and begun again begins again here) */
		if (substep_index == 0)
		{
			short other = buffer ^ 1;

			substeps->continuing[buffer] = substeps->tick[other] == tick - 1 &&
				substeps->count[other] == substep_count &&
				substeps->recorded[other] == (byte)(FLAG(substep_count) - 1);
			substeps->tick[buffer] = tick;
			substeps->count[buffer] = substep_count;
			substeps->recorded[buffer] = 0;
		}
		else if (substeps->tick[buffer] != tick || substeps->count[buffer] != substep_count)
		{
			continue;
		}
		substeps->positions[buffer][substep_index] = object->object.position;
		substeps->recorded[buffer] |= (byte)FLAG(substep_index);
	}
}

/* (the frames) how far from where the line between the drawn tick's
snapshots has it (from where the tick left it, when its pose is drawn as
that: snapped) the object is drawn this frame, by itself or with what
carries it: where the sub-steps of the tick drawn put what carries it, at
the frame's sub-step, from there. FALSE when it is drawn on the line (one
sub-step a tick, of the macro lane, made in the tick, moved further than a
tick allows) */
static boolean render_interpolation_substep_offset(long object_index, boolean snapped, real_vector3d *offset)
{
	struct interpolated_object *record;
	struct interpolated_substeps *substeps;
	short buffer = (short)(interpolation_tick & 1);
	short count = interpolation_substep_count;
	real_point3d from;
	long root_index;
	long absolute_index;

	if (count <= 1 || !interpolated_substeps || !interpolated_objects || object_index == NONE)
		return FALSE;
	root_index = object_get_ultimate_parent(object_index);
	absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(root_index);
	if (absolute_index >= MAXIMUM_INTERPOLATED_OBJECTS)
		return FALSE;
	substeps = &interpolated_substeps[absolute_index];
	record = &interpolated_objects[absolute_index];
	/* (every sub-step of the tick drawn kept, as many as the frame's, and of
	the tick before: not one made, or let go of, in either, which is drawn
	between the ticks as before. A projectile's first update is its head
	start, a whole tick's travel at once, and it then holds for a tick
	(projectiles.c): drawn so, it would dart out and wait. The line from a
	tick's motion back (or from where it was let go of) draws its first
	tick, and the next, its hold's end, as at 30) */
	if (substeps->object_index != root_index || substeps->tick[buffer] != interpolation_tick ||
		substeps->count[buffer] != count || substeps->recorded[buffer] != (byte)(FLAG(count) - 1) ||
		!substeps->continuing[buffer] ||
		record->object_index != root_index || record->tick != interpolation_tick || !record->has_previous)
	{
		return FALSE;
	}
	if (substeps->drawn_frame != interpolation_frame)
	{
		real_point3d const *before = interpolation_substep_index > 0 ?
			&substeps->positions[buffer][interpolation_substep_index - 1] :
			&record->previous_position;

		/* (so written that a position not a number is none) */
		substeps->drawn_valid = distance_squared(&record->previous_position, &record->position) <=
			OBJECT_SNAP_DISTANCE * OBJECT_SNAP_DISTANCE;
		point_lerp(before, &substeps->positions[buffer][interpolation_substep_index], interpolation_substep_fraction,
			&substeps->drawn);
		substeps->drawn_frame = interpolation_frame;
	}
	if (!substeps->drawn_valid)
		return FALSE;
	if (snapped)
		from = record->position;
	else
		point_lerp(&record->previous_position, &record->position, interpolation_fraction, &from);
	offset->i = substeps->drawn.x - from.x;
	offset->j = substeps->drawn.y - from.y;
	offset->k = substeps->drawn.z - from.z;
	/* (so written that an offset not a number is none) */
	return offset->i * offset->i + offset->j * offset->j + offset->k * offset->k <=
		SUBSTEP_OFFSET_MAXIMUM * SUBSTEP_OFFSET_MAXIMUM;
}

/* ---------- ghosts */

/* (objects.c's object_delete, in the tick) the object deleted: a projectile
(a rocket, a needle, a grenade) that hit something or ran out, whose
effects (an explosion, an impact) are made where it was. Those are drawn
from the start of the frames after the tick, but the projectile was drawn
only as far as the tick before had it: it went a tick early, short of what
it hit, as what it made there appeared. It is drawn through the frames
after this tick as it was, from its last snapshot to where it was deleted.
Only a projectile drawn with a model, with nothing on it (its lights and
its contrail go with it), and drawn before. */
void render_interpolation_object_deleted(long object_index)
{
	struct object_datum *object;
	struct object_definition *definition;
	struct interpolated_object *record;
	struct interpolation_ghost *ghost = NULL;
	real_matrix4x3 const *latest;
	long absolute_index;
	short index;

	if (!halo_interpolation_enabled() || !interpolated_objects || object_index == NONE)
		return;
	object = object_try_and_get(object_index);
	if (!object || object->object.type != _object_type_projectile ||
		object->object.parent_object_index != NONE || object->object.first_child_object_index != NONE ||
		TEST_FLAG(object->object.flags, _object_invisible_bit))
	{
		return;
	}
	definition = object_definition_get(object->definition_index);
	absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index);
	if (definition->object.model.index == NONE || absolute_index >= MAXIMUM_INTERPOLATED_OBJECTS)
		return;
	/* (its snapshot at the end of the tick before) */
	record = &interpolated_objects[absolute_index];
	if (record->object_index != object_index || record->tick != interpolation_tick ||
		record->node_count > MAXIMUM_GHOST_NODES)
	{
		return;
	}
	for (index = 0; index < MAXIMUM_GHOSTS && !ghost; index++)
	{
		if (!ghosts[index].in_use)
			ghost = &ghosts[index];
	}
	if (!ghost)
		return;
	ghost->offset.i = object->object.position.x - record->position.x;
	ghost->offset.j = object->object.position.y - record->position.y;
	ghost->offset.k = object->object.position.z - record->position.z;
	/* (so written that an offset not a number is none) */
	if (!(ghost->offset.i * ghost->offset.i + ghost->offset.j * ghost->offset.j + ghost->offset.k * ghost->offset.k <=
		OBJECT_SNAP_DISTANCE * OBJECT_SNAP_DISTANCE))
	{
		return;
	}
	/* (as last drawn; its point lights were that frame's, not one to come's) */
	if (!render_objects_port_last_lighting(object_index, &ghost->lighting))
		return;
	ghost->lighting.point_light_count = 0;
	latest = record->nodes + record->latest * record->node_capacity;
	memcpy(ghost->nodes, latest, record->node_count * sizeof(real_matrix4x3));
	memcpy(ghost->function_values, record->function_values[record->latest], sizeof(ghost->function_values));
	memcpy(ghost->change_colors, record->change_colors[record->latest], sizeof(ghost->change_colors));
	memcpy(ghost->region_permutations, object->object.region_permutations, sizeof(ghost->region_permutations));
	ghost->in_use = TRUE;
	ghost->object_index = object_index;
	ghost->tick = NONE;
	ghost->model_index = definition->object.model.index;
	ghost->node_count = record->node_count;
	ghost->forced_shader_permutation_index = object->object.forced_shader_permutation_index;
	ghost->position = record->position;
	ghost->center_offset.i = object->object.bounding_sphere_center.x - object->object.position.x;
	ghost->center_offset.j = object->object.bounding_sphere_center.y - object->object.position.y;
	ghost->center_offset.k = object->object.bounding_sphere_center.z - object->object.position.z;
	ghost->radius = object->object.bounding_sphere_radius;
}

/* (render_interpolation_tick) those deleted in the tick are drawn after it;
those drawn after the tick before are done */
static void render_interpolation_ghosts_tick(void)
{
	short index;

	for (index = 0; index < MAXIMUM_GHOSTS; index++)
	{
		struct interpolation_ghost *ghost = &ghosts[index];

		if (ghost->in_use && ghost->tick == NONE && halo_interpolation_enabled())
			ghost->tick = interpolation_tick;
		else
			ghost->in_use = FALSE;
	}
}

/* (render_objects.c, as the objects are drawn) this tick's ghosts, as far
from their last snapshot to where they were deleted as the frame is into
the tick */
void render_interpolation_ghosts_draw(void)
{
	short index;

	if (!interpolation_rendering)
		return;
	for (index = 0; index < MAXIMUM_GHOSTS; index++)
	{
		struct interpolation_ghost *ghost = &ghosts[index];
		struct render_model_effect model_effect;
		real_point3d center;
		short node_index;

		if (!ghost->in_use || ghost->tick != interpolation_tick)
			continue;
		for (node_index = 0; node_index < ghost->node_count; node_index++)
		{
			ghost->drawn[node_index] = ghost->nodes[node_index];
			ghost->drawn[node_index].position.x += ghost->offset.i * interpolation_fraction;
			ghost->drawn[node_index].position.y += ghost->offset.j * interpolation_fraction;
			ghost->drawn[node_index].position.z += ghost->offset.k * interpolation_fraction;
		}
		center.x = ghost->position.x + ghost->offset.i * interpolation_fraction + ghost->center_offset.i;
		center.y = ghost->position.y + ghost->offset.j * interpolation_fraction + ghost->center_offset.j;
		center.z = ghost->position.z + ghost->offset.k * interpolation_fraction + ghost->center_offset.k;
		/* (no effect: as render_object's) */
		memset(&model_effect, 0, sizeof(model_effect));
		render_model(
			ghost->model_index,
			render_frustum_sphere_diameter_in_pixels(&render.frustum, &center, ghost->radius),
			ghost->drawn,
			(char const *)ghost->region_permutations,
			ghost->change_colors,
			ghost->function_values,
			&ghost->lighting,
			&center,
			ghost->radius,
			&model_effect,
			ghost->object_index,
			ghost->forced_shader_permutation_index,
			0);
	}
}

/* ---------- frames */

void render_interpolation_frame_begin(void)
{
	short index;

	interpolation_rendering = halo_interpolation_enabled();
	interpolation_frame++;
	interpolation_fraction = game_time_get_tick_fraction();
	/* (the sub-step drawn: at 30 the tick's one, as far as the fraction) */
	interpolation_substep_count = sim_substep_count();
	sim_substep_blend(interpolation_fraction, &interpolation_substep_index, &interpolation_substep_fraction);
	/* the ghost of one deleted since the last tick (between the ticks: by
	the netcode, a script) is none: it was drawn as far as that tick */
	for (index = 0; index < MAXIMUM_GHOSTS; index++)
	{
		if (ghosts[index].in_use && ghosts[index].tick == NONE)
			ghosts[index].in_use = FALSE;
	}
}

void render_interpolation_frame_end(void)
{
	interpolation_rendering = FALSE;
}

real render_interpolation_fraction(void)
{
	return interpolation_rendering ? interpolation_fraction : 1.0f;
}

/* An object put somewhere (render_interpolation_object_from) in a tick of
several sub-steps that has not ended (the VR mode's grenade let go of in a
sub-step, or in the tick's own work, which runs with its first): the frames
still draw the tick before, whose snapshot has it in the hand of the
player's body, which is not drawn in first person (and no snapshot of one
made since), while the hand no longer draws it. It is drawn where it was put
(its nodes as they are, moved there) until the frames reach the tick, which
draws it from there. At 30 no frame comes between a tick's work and its end:
none is drawn so */
static real_matrix4x3 *render_interpolation_pending_from(long object_index)
{
	static real_matrix4x3 nodes[MAXIMUM_INTERPOLATED_NODES];
	struct object_datum *object;
	real_matrix4x3 const *current;
	real_vector3d offset;
	short node_count;
	short node_index;

	if (object_index != interpolation_from.object_index || !object_try_and_get(object_index))
		return NULL;
	object = object_get(object_index);
	node_count = (short)(object->object.node_matrices.size / (short)sizeof(real_matrix4x3));
	if (node_count <= 0 || node_count > MAXIMUM_INTERPOLATED_NODES)
		return NULL;
	current = (real_matrix4x3 const *)object_header_block_get(object_index, &object->object.node_matrices);
	offset.i = interpolation_from.position.x - current[0].position.x;
	offset.j = interpolation_from.position.y - current[0].position.y;
	offset.k = interpolation_from.position.z - current[0].position.z;
	for (node_index = 0; node_index < node_count; node_index++)
	{
		nodes[node_index] = current[node_index];
		nodes[node_index].position.x += offset.i;
		nodes[node_index].position.y += offset.j;
		nodes[node_index].position.z += offset.k;
	}
	return nodes;
}

real_matrix4x3 *render_interpolation_object_node_matrices(long object_index)
{
	struct interpolated_object *record;
	long absolute_index;
	real_matrix4x3 *pending;

	if (!interpolation_rendering || !interpolated_objects || object_index == NONE)
		return NULL;
	if ((pending = render_interpolation_pending_from(object_index)) != NULL)
		return pending;
	absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index);
	if (absolute_index >= MAXIMUM_INTERPOLATED_OBJECTS)
		return NULL;
	record = &interpolated_objects[absolute_index];
	if (record->object_index != object_index || record->tick != interpolation_tick || !record->has_previous)
		return NULL;
	if (record->blended_frame != interpolation_frame)
	{
		real_matrix4x3 const *previous = record->nodes + (record->latest ^ 1) * record->node_capacity;
		real_matrix4x3 const *latest = record->nodes + record->latest * record->node_capacity;
		real_matrix4x3 *blended = record->nodes + 2 * record->node_capacity;
		short node_index;
		/* (so written that a position not a number snaps) */
		boolean jumped = !(distance_squared(&previous[0].position, &latest[0].position) <=
			OBJECT_SNAP_DISTANCE * OBJECT_SNAP_DISTANCE);
		boolean snap = jumped;
		real_vector3d offset;

		/* a node moved further in the root's frame than a tick allows: the
		two snapshots are different poses, not one moving (so written that
		a position not a number snaps) */
		for (node_index = 1; !snap && node_index < record->node_count; node_index++)
		{
			real_point3d previous_local, latest_local;

			matrix4x3_inverse_transform_point(&previous[0], &previous[node_index].position, &previous_local);
			matrix4x3_inverse_transform_point(&latest[0], &latest[node_index].position, &latest_local);
			snap = !(distance_squared(&previous_local, &latest_local) <= NODE_SNAP_DISTANCE * NODE_SNAP_DISTANCE);
		}
		if (!snap)
		{
			struct interpolation_rotation *previous_rotations =
				record->rotations + (record->latest ^ 1) * record->node_capacity;
			struct interpolation_rotation *latest_rotations =
				record->rotations + record->latest * record->node_capacity;
			short snapshot;

			/* (each snapshot's rotations found once, not every frame: its
			nodes' positions may move with a correction, their rotations
			never) */
			for (snapshot = 0; snapshot < 2; snapshot++)
			{
				if (!record->rotations_valid[snapshot])
				{
					real_matrix4x3 const *nodes = record->nodes + snapshot * record->node_capacity;
					struct interpolation_rotation *rotations = record->rotations + snapshot * record->node_capacity;

					for (node_index = 0; node_index < record->node_count; node_index++)
						rotation_from_matrix(&nodes[node_index], &rotations[node_index]);
					record->rotations_valid[snapshot] = TRUE;
				}
			}
			for (node_index = 0; node_index < record->node_count; node_index++)
			{
				matrix_blend_rotations(&previous[node_index], &latest[node_index], &previous_rotations[node_index],
					&latest_rotations[node_index], interpolation_fraction, &blended[node_index]);
			}
		}
		if (snap)
			memcpy(blended, latest, record->node_count * sizeof(real_matrix4x3));
		/* the micro lane's: moved onto the sub-steps' path */
		if (!jumped && render_interpolation_substep_offset(object_index, snap, &offset))
		{
			for (node_index = 0; node_index < record->node_count; node_index++)
			{
				blended[node_index].position.x += offset.i;
				blended[node_index].position.y += offset.j;
				blended[node_index].position.z += offset.k;
			}
		}
		if (correction_significant(&record->correction) || correction_significant(&record->correction_pending))
		{
			real_vector3d drawn;

			correction_drawn(&record->correction, &record->correction_pending, &drawn);
			for (node_index = 0; node_index < record->node_count; node_index++)
			{
				blended[node_index].position.x += drawn.i;
				blended[node_index].position.y += drawn.j;
				blended[node_index].position.z += drawn.k;
			}
		}
		record->blended_frame = interpolation_frame;
	}
	return record->nodes + 2 * record->node_capacity;
}

/* ---------- what objects are drawn with */

/* An object's function values (a weapon's charge or heat, a vehicle's
speed, a shield's flare: what its shaders' animations and glows read), its
change colors and a unit's active camouflage, which the tick computes: the
blend of the last two ticks', as its pose is, rather than a step a tick */
static struct interpolated_object *render_interpolation_object_values(long object_index)
{
	struct interpolated_object *record;
	long absolute_index;

	if (!interpolation_rendering || !interpolated_objects || object_index == NONE)
		return NULL;
	absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index);
	if (absolute_index >= MAXIMUM_INTERPOLATED_OBJECTS)
		return NULL;
	record = &interpolated_objects[absolute_index];
	if (record->object_index != object_index || record->tick != interpolation_tick || !record->has_previous)
		return NULL;
	if (record->values_blended_frame != interpolation_frame)
	{
		short previous = record->latest ^ 1, latest = record->latest;
		short index;

		for (index = 0; index < NUMBER_OF_OUTGOING_OBJECT_FUNCTIONS; index++)
		{
			record->function_values[2][index] = lerp(record->function_values[previous][index],
				record->function_values[latest][index], interpolation_fraction);
		}
		for (index = 0; index < NUMBER_OF_OBJECT_CHANGE_COLORS; index++)
		{
			real_rgb_color const *a = &record->change_colors[previous][index];
			real_rgb_color const *b = &record->change_colors[latest][index];

			record->change_colors[2][index].red = lerp(a->red, b->red, interpolation_fraction);
			record->change_colors[2][index].green = lerp(a->green, b->green, interpolation_fraction);
			record->change_colors[2][index].blue = lerp(a->blue, b->blue, interpolation_fraction);
		}
		for (index = 0; index < 2; index++)
		{
			record->camouflage[2][index] = lerp(record->camouflage[previous][index],
				record->camouflage[latest][index], interpolation_fraction);
		}
		record->values_blended_frame = interpolation_frame;
	}
	return record;
}

real const *render_interpolation_object_function_values(long object_index)
{
	struct interpolated_object *record = render_interpolation_object_values(object_index);

	return record ? record->function_values[2] : object_get(object_index)->object.outgoing_function_values;
}

real_rgb_color const *render_interpolation_object_change_colors(long object_index)
{
	struct interpolated_object *record = render_interpolation_object_values(object_index);

	return record ? record->change_colors[2] : object_get(object_index)->object.outgoing_change_colors;
}

/* ... and how much it is camouflaged (super_amount: the camouflage's
super amount) */
real render_interpolation_unit_camouflage(long unit_index, real *super_amount)
{
	struct interpolated_object *record = render_interpolation_object_values(unit_index);
	struct unit_datum *unit;

	if (record)
	{
		*super_amount = record->camouflage[2][1];
		return record->camouflage[2][0];
	}
	unit = unit_get(unit_index);
	*super_amount = unit->unit.active_camouflage_super_amount;
	return unit->unit.active_camouflage;
}

/* a point that moves with the object's root node (its bounding sphere's
centre: a nav point over it), from where the last tick left it to where the
object is drawn this frame; left as it is when the object is drawn as the
tick left it */
void render_interpolation_object_point(long object_index, real_point3d *point)
{
	real_matrix4x3 const *drawn = render_interpolation_object_node_matrices(object_index);
	real_matrix4x3 const *nodes;
	real_point3d local;

	if (!drawn)
		return;
	/* (as of the last tick: object_get_node_matrices gives the drawn pose
	while a frame is drawn) */
	nodes = (real_matrix4x3 const *)object_header_block_get(object_index, &object_get(object_index)->object.node_matrices);
	matrix4x3_inverse_transform_point(&nodes[0], point, &local);
	matrix4x3_transform_point(&drawn[0], &local, point);
}

/* ---------- corrections */

/* the object (and what it carries) moved by the netcode from where it was,
offset from where it is now: drawn from there, gliding */
void render_interpolation_correct_object(long object_index, real_vector3d const *offset)
{
	struct object_datum *object;
	long child_index;
	long absolute_index;

	/* (so written that an offset not a number is none) */
	if (!interpolated_objects || object_index == NONE ||
		!(offset->i * offset->i + offset->j * offset->j + offset->k * offset->k <= OBJECT_SNAP_DISTANCE * OBJECT_SNAP_DISTANCE))
	{
		return;
	}
	absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index);
	if (absolute_index < MAXIMUM_INTERPOLATED_OBJECTS &&
		interpolated_objects[absolute_index].object_index == object_index)
	{
		struct interpolated_object *record = &interpolated_objects[absolute_index];
		short snapshot;
		short node_index;

		/* the snapshots where it would have been, the difference drawn */
		for (snapshot = 0; snapshot < 2; snapshot++)
		{
			real_matrix4x3 *nodes = record->nodes + snapshot * record->node_capacity;

			for (node_index = 0; node_index < record->node_count; node_index++)
			{
				nodes[node_index].position.x -= offset->i;
				nodes[node_index].position.y -= offset->j;
				nodes[node_index].position.z -= offset->k;
			}
		}
		record->position.x -= offset->i;
		record->position.y -= offset->j;
		record->position.z -= offset->k;
		record->previous_position.x -= offset->i;
		record->previous_position.y -= offset->j;
		record->previous_position.z -= offset->k;
		correction_add(&record->correction, &record->correction_pending, offset);
		record->blended_frame = NONE;
		/* (and its sub-steps' poses: as its snapshots, where it would have
		been) */
		if (interpolated_substeps && interpolated_substeps[absolute_index].object_index == object_index)
		{
			struct interpolated_substeps *substeps = &interpolated_substeps[absolute_index];
			short buffer;
			short substep_index;

			for (buffer = 0; buffer < 2; buffer++)
			{
				for (substep_index = 0; substep_index < MAXIMUM_SIM_SUBSTEPS; substep_index++)
				{
					substeps->positions[buffer][substep_index].x -= offset->i;
					substeps->positions[buffer][substep_index].y -= offset->j;
					substeps->positions[buffer][substep_index].z -= offset->k;
				}
			}
			substeps->drawn_frame = NONE;
		}
	}
	/* a local player's unit (by itself, or with what it rides): its
	first-person view, which the observer poses from it, glides with it */
	{
		short local_player_index;

		for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
		{
			struct interpolated_camera *camera = &interpolated_cameras[local_player_index];

			if (!camera->valid || player_control_get_unit_index(local_player_index) != object_index)
				continue;
			camera->previous.position.x -= offset->i;
			camera->previous.position.y -= offset->j;
			camera->previous.position.z -= offset->k;
			camera->latest.position.x -= offset->i;
			camera->latest.position.y -= offset->j;
			camera->latest.position.z -= offset->k;
			correction_add(&camera->correction, &camera->correction_pending, offset);
		}
	}
	object = object_get(object_index);
	for (child_index = object->object.first_child_object_index; child_index != NONE;
		child_index = object_get(child_index)->object.next_object_index)
	{
		render_interpolation_correct_object(child_index, offset);
	}
}

/* ---------- camera */

static struct observer_result direct_cameras[MAXIMUM_LOCAL_PLAYERS];

static struct observer_result const *render_interpolation_blended_camera(
	short local_player_index,
	struct observer_result const *observer);

/* A first-person view is posed from the player's facing, which the input
turns every frame (player_control.c), but the observer keeps it as of the
last tick and the blend above draws it a tick later still. On foot, the view
points where the player aims now (display.direct_camera). In a vehicle's
seat or a cinematic the view is the seat's or the script's: left as it is.
Desktop only: display.direct_camera is not an Android setting, and its
default would otherwise apply there. */
static struct observer_result const *render_interpolation_direct_camera(
	short local_player_index,
	struct observer_result const *observer)
{
#ifdef HALO_ANDROID
	(void)local_player_index;
	return observer;
#else
	static int enabled;
	static unsigned long read_at = (unsigned long)-1;
	struct observer_result *direct;
	long unit_index;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		enabled = config_boolean("display.direct_camera");
	}
	if (!enabled || !observer ||
		director_get_perspective(local_player_index) != _director_perspective_first_person ||
		director_inhibited_facing(local_player_index) ||
		cinematic_in_progress())
	{
		return observer;
	}
	unit_index = player_control_get_unit_index(local_player_index);
	if (unit_index == NONE || object_get(unit_index)->object.parent_object_index != NONE)
		return observer;

	direct = &direct_cameras[local_player_index];
	*direct = *observer;
	player_control_get_facing_direction(local_player_index, &direct->forward);
	observer_up_from_forward(&direct->forward, &direct->up);
	return direct;
#endif
}

struct observer_result const *render_interpolation_camera(
	short local_player_index,
	struct observer_result const *observer)
{
	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS)
		return observer;
	return render_interpolation_direct_camera(local_player_index,
		render_interpolation_blended_camera(local_player_index, observer));
}

/* between two of the observer's results: the position, the axes (up kept
square to forward) and the field of view */
static void observer_blend(
	struct observer_result const *a,
	struct observer_result const *b,
	real t,
	struct observer_result *result)
{
	real along, length;

	*result = *b;
	point_lerp(&a->position, &b->position, t, &result->position);
	vector_nlerp(&a->forward, &b->forward, t, &result->forward);
	vector_nlerp(&a->up, &b->up, t, &result->up);
	along = result->up.i * result->forward.i + result->up.j * result->forward.j + result->up.k * result->forward.k;
	result->up.i -= result->forward.i * along;
	result->up.j -= result->forward.j * along;
	result->up.k -= result->forward.k * along;
	length = vector_length(&result->up);
	if (length > 1e-6f)
	{
		result->up.i /= length;
		result->up.j /= length;
		result->up.k /= length;
	}
	else
	{
		result->up = b->up;
	}
	result->field_of_view = lerp(a->field_of_view, b->field_of_view, t);
}

/* a cut between two of the observer's results (so written that a position
or direction not a number cuts) */
static boolean observer_cut(
	struct observer_result const *a,
	struct observer_result const *b)
{
	return !(distance_squared(&a->position, &b->position) <= CAMERA_CUT_DISTANCE * CAMERA_CUT_DISTANCE) ||
		!(a->forward.i * b->forward.i + a->forward.j * b->forward.j + a->forward.k * b->forward.k >= CAMERA_CUT_COSINE);
}

/* The first-person view of a unit of the micro lane (a player's biped on
foot), which the observer poses from the unit as it is now: NONE for any
other view (in a seat, a cinematic, another perspective), or at 30. The
observer, as the frame after the tick took it, may have the next tick's
first sub-steps in it (a frame slower than a sub-step runs several, the
tick's last and the next's first); the view is drawn from the snapshots
moved back to where the tick left the unit, then moved with the unit onto
its sub-steps' path, as its body is (render_interpolation_substep_offset) */
static long render_interpolation_camera_substep_unit(short local_player_index)
{
	long unit_index;

	if (interpolation_substep_count <= 1 ||
		director_get_perspective(local_player_index) != _director_perspective_first_person ||
		cinematic_in_progress())
	{
		return NONE;
	}
	unit_index = player_control_get_unit_index(local_player_index);
	if (unit_index == NONE || !object_try_and_get_and_verify_type(unit_index, _object_mask_unit) ||
		object_get(unit_index)->object.parent_object_index != NONE ||
		object_get_lane(unit_index) != _object_lane_micro)
	{
		return NONE;
	}
	return unit_index;
}

/* the observer (latest) as of the tick's end, where its unit (of the micro
lane) was as the tick left it: not with the next tick's sub-steps run since */
static void render_interpolation_camera_at_tick(long unit_index, struct observer_result *latest)
{
	struct interpolated_object *record;
	long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(unit_index);
	real_point3d const *position;
	real_vector3d since;

	if (!interpolated_objects || absolute_index >= MAXIMUM_INTERPOLATED_OBJECTS)
		return;
	record = &interpolated_objects[absolute_index];
	if (record->object_index != unit_index || record->tick != interpolation_tick)
		return;
	position = &object_get(unit_index)->object.position;
	since.i = position->x - record->position.x;
	since.j = position->y - record->position.y;
	since.k = position->z - record->position.z;
	/* (so written that a difference not a number is none) */
	if (!(since.i * since.i + since.j * since.j + since.k * since.k <= CAMERA_CUT_DISTANCE * CAMERA_CUT_DISTANCE))
		return;
	latest->position.x -= since.i;
	latest->position.y -= since.j;
	latest->position.z -= since.k;
}

static struct observer_result const *render_interpolation_blended_camera(
	short local_player_index,
	struct observer_result const *observer)
{
	struct interpolated_camera *camera;
	real t = interpolation_fraction;
	long substep_unit_index;
	real_vector3d offset;

	if (!interpolation_rendering || !observer)
	{
		return observer;
	}
	camera = &interpolated_cameras[local_player_index];
	substep_unit_index = render_interpolation_camera_substep_unit(local_player_index);
	/* A scripted camera that moves with an object (a cinematic's camera
	point relative to a lifepod or dropship, or a unit's eyes in one) is
	placed by the object as it stands after the last tick. Blended from its
	own snapshots (taken at the first frame after each tick, a different
	part of a tick each tick), it did not follow the object as drawn, which
	is blended from the object's snapshots, or steps from tick to tick when
	its pose snaps: it ran up to 2 m ahead of or behind the object, out
	through the lifepod's hull in a30. It is drawn where the observer has
	it, moved with the object from where the object is to where it is
	drawn. (An animated camera has no such object; a scripted first-person
	camera looks along its unit's aim, which turns once a tick, and is
	blended from its snapshots as the player's camera is.) */
	if (director_get_perspective(local_player_index) == _director_perspective_scripted)
	{
		long relative_object_index = scripted_camera_object_relative_to();
		real_matrix4x3 const *drawn;

		if (relative_object_index != NONE &&
			!scripted_camera_object_is_first_person_camera(relative_object_index) &&
			object_try_and_get_and_verify_type(relative_object_index, _object_mask_all) &&
			(drawn = render_interpolation_object_node_matrices(relative_object_index)) != NULL)
		{
			/* (as of the last tick: object_get_node_matrices gives the drawn
			pose while a frame is drawn) */
			real_matrix4x3 const *nodes = (real_matrix4x3 const *)object_header_block_get(
				relative_object_index, &object_get(relative_object_index)->object.node_matrices);

			camera->blended = *observer;
			camera->blended.position.x += drawn[0].position.x - nodes[0].position.x;
			camera->blended.position.y += drawn[0].position.y - nodes[0].position.y;
			camera->blended.position.z += drawn[0].position.z - nodes[0].position.z;
			/* (the snapshots start again after) */
			camera->valid = FALSE;
			return &camera->blended;
		}
	}
	/* the observer as it stood after each tick (the first frame drawn
	after the tick) */
	if (!camera->valid || camera->tick != interpolation_tick)
	{
		/* (its correction taken on each tick, render_interpolation_tick) */
		if (!camera->valid)
		{
			camera->correction = *global_zero_vector3d;
			camera->correction_pending = *global_zero_vector3d;
		}
		camera->has_previous = camera->valid;
		camera->previous = camera->latest;
		camera->latest = *observer;
		if (substep_unit_index != NONE)
			render_interpolation_camera_at_tick(substep_unit_index, &camera->latest);
		/* Several ticks since the last snapshot (a long frame): the objects
		are drawn between the last two ticks, so the camera's previous is
		where it was a tick ago, as nearly as a steady move from the last
		snapshot tells. (Blended across all the ticks since, the camera
		moved further each frame than the world it is in, out through a
		Pelican's hull for a frame.) A cut stays a cut. */
		if (camera->has_previous && interpolation_tick - camera->tick > 1 &&
			!observer_cut(&camera->previous, &camera->latest))
		{
			real ticks = (real)(interpolation_tick - camera->tick);
			struct observer_result previous = camera->previous;

			observer_blend(&previous, &camera->latest, (ticks - 1.0f) / ticks, &camera->previous);
		}
		camera->tick = interpolation_tick;
		camera->valid = TRUE;
	}
	if (!camera->has_previous || observer_cut(&camera->previous, &camera->latest))
	{
		return observer;
	}

	observer_blend(&camera->previous, &camera->latest, t, &camera->blended);
	if (correction_significant(&camera->correction) || correction_significant(&camera->correction_pending))
	{
		real_vector3d drawn;

		correction_drawn(&camera->correction, &camera->correction_pending, &drawn);
		camera->blended.position.x += drawn.i;
		camera->blended.position.y += drawn.j;
		camera->blended.position.z += drawn.k;
	}
	/* the micro lane's unit's view: moved onto its sub-steps' path */
	if (substep_unit_index != NONE && render_interpolation_substep_offset(substep_unit_index, FALSE, &offset))
	{
		camera->blended.position.x += offset.i;
		camera->blended.position.y += offset.j;
		camera->blended.position.z += offset.k;
	}
	return &camera->blended;
}

/* ---------- first-person weapon */

/* The first-person weapon and hands are posed in world space from the drawn
camera each frame, from animation state that changes once a tick: blend the
pose relative to the camera. */
void render_interpolation_first_person(
	short local_player_index,
	real_matrix4x3 *node_matrices,
	short node_count,
	struct render_camera const *camera)
{
	struct interpolated_first_person *first_person;
	real_matrix4x3 camera_matrix;
	real_matrix4x3 inverse_camera;
	struct interpolation_rotation previous_rotations[MAXIMUM_INTERPOLATED_NODES];
	struct interpolation_rotation latest_rotations[MAXIMUM_INTERPOLATED_NODES];
	short node_index;

	if (!interpolation_rendering ||
		local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS ||
		node_count <= 0 || node_count > MAXIMUM_INTERPOLATED_NODES)
	{
		return;
	}
	first_person = &interpolated_first_person[local_player_index];
	matrix4x3_from_point_and_vectors(&camera_matrix, &camera->position, &camera->forward, &camera->up);
	matrix4x3_inverse(&camera_matrix, &inverse_camera);
	if (first_person->tick != interpolation_tick)
	{
		/* the pose drawn last, if that was at the end of the tick just
		before: not one left from before the view was last away from first
		person (zoomed, in a vehicle, dead, in a cinematic), seconds old */
		first_person->has_previous = first_person->tick == interpolation_tick - 1 &&
			first_person->node_count == node_count;
		memcpy(first_person->previous, first_person->latest, sizeof(first_person->previous));
		first_person->tick = interpolation_tick;
	}
	for (node_index = 0; node_index < node_count; node_index++)
		matrix4x3_multiply(&inverse_camera, &node_matrices[node_index], &first_person->latest[node_index]);
	first_person->node_count = node_count;
	if (!first_person->has_previous)
		return;
	/* a node that jumped further in the camera's frame than a tick allows:
	the last pose was another weapon's skeleton (of as many nodes), not this
	one moving (so written that a position not a number snaps) */
	for (node_index = 0; node_index < node_count; node_index++)
	{
		real_matrix4x3 const *previous = &first_person->previous[node_index];
		real_matrix4x3 const *latest = &first_person->latest[node_index];

		if (!(distance_squared(&previous->position, &latest->position) <=
			FIRST_PERSON_SNAP_DISTANCE * FIRST_PERSON_SNAP_DISTANCE))
		{
			return;
		}
		rotation_from_matrix(previous, &previous_rotations[node_index]);
		rotation_from_matrix(latest, &latest_rotations[node_index]);
	}
	for (node_index = 0; node_index < node_count; node_index++)
	{
		real_matrix4x3 blended;

		matrix_blend_rotations(
			&first_person->previous[node_index],
			&first_person->latest[node_index],
			&previous_rotations[node_index],
			&latest_rotations[node_index],
			interpolation_fraction,
			&blended);
		matrix4x3_multiply(&camera_matrix, &blended, &node_matrices[node_index]);
	}
}

/* ---------- particles */

/* An effect's particles are made in the tick for all of the tick's time
(effects.c), at once and at the emitter's place as the tick begins: a
stream from an object (a looping effect's: a vehicle's exhaust, a wreck's
smoke) came in clumps 30 times a second, from a moving object each a tick's
motion behind the last, which the object drawn between the ticks then moved
away from. Such a particle, the nth of a tick's count, is held back for that
part of the tick and made in the first frame drawn that far into the tick
after, where the emitter is drawn then: a stream, from the emitter as it is
seen. */

/* (effects.c, in the tick) the particle data (its position in the world)
from the object's node, made this part of the way into the tick's time:
TRUE as it is held back to be made then (particle_new), FALSE to be made
now (frames are not drawn between the ticks, the object is not drawn
between two poses, too many are held) */
boolean render_interpolation_particle_later(
	struct new_particle_data const *data,
	long object_index,
	short node_index,
	real fraction)
{
	struct interpolated_object *record;
	struct later_particle *later;
	long absolute_index;

	if (!halo_interpolation_enabled() || !interpolated_objects || object_index == NONE ||
		!(fraction > 0.0f && fraction < 1.0f) || later_particle_count >= MAXIMUM_LATER_PARTICLES)
	{
		return FALSE;
	}
	absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index);
	if (absolute_index >= MAXIMUM_INTERPOLATED_OBJECTS)
		return FALSE;
	record = &interpolated_objects[absolute_index];
	if (record->object_index != object_index || !record->has_previous ||
		node_index < 0 || node_index >= record->node_count)
	{
		return FALSE;
	}
	later = &later_particles[later_particle_count++];
	later->data = *data;
	later->object_index = object_index;
	later->node_index = node_index;
	/* (the tick's node: object_get_node_matrix gives it in the tick) */
	matrix4x3_inverse_transform_point(object_get_node_matrix(object_index, node_index), &data->position,
		&later->node_position);
	/* (the tick's frames come after render_interpolation_tick) */
	later->tick = interpolation_tick + 1;
	later->fraction = fraction;
	return TRUE;
}

/* (particles.c, each frame before the particles move) the particles held
back that this frame has reached, made on their emitters as drawn */
void render_interpolation_particles_due(void)
{
	short index = 0;

	while (index < later_particle_count)
	{
		struct later_particle *later = &later_particles[index];

		if (interpolation_rendering && (interpolation_tick < later->tick ||
			(interpolation_tick == later->tick && interpolation_fraction < later->fraction)))
		{
			index++;
			continue;
		}
		if (object_try_and_get(later->object_index))
		{
			matrix4x3_transform_point(object_get_node_matrix(later->object_index, later->node_index),
				&later->node_position, &later->data.position);
		}
		particle_new(&later->data);
		*later = later_particles[--later_particle_count];
	}
}

/* ---------- time */

/* game time for animated shaders, continuous between ticks: the time of
the frame drawn (a tick behind the simulation, like the objects) */
real render_interpolation_game_time_sec(long ticks)
{
	real time;

	if (!interpolation_rendering)
		return (real)ticks * (1.0f / TICKS_PER_SECOND);
	time = ((real)ticks - 1.0f + interpolation_fraction) * (1.0f / TICKS_PER_SECOND);
	return time > 0.0f ? time : 0.0f;
}

/* how many ticks the frame drawn is behind the game's time: the part of a
tick still to run to the last tick's (as the objects, above), none when
frames are not drawn between ticks */
real render_interpolation_ticks_behind(void)
{
	return interpolation_rendering ? 1.0f - interpolation_fraction : 0.0f;
}

/* a timer that the frames count down by the ticks each ran (by
game_time_get_elapsed(), after reading it: the screen's flash, the camera's
impulse and shake), read before this frame's count as it stands at the time
of the frame drawn: what is left after this frame's ticks, and the part of a
tick the frame is behind them. When frames are not drawn between ticks, as
it is (before this frame's ticks), as the game read it. */
real render_interpolation_ticks_left(long ticks_left)
{
	if (!interpolation_rendering)
		return (real)ticks_left;
	return (real)(ticks_left - game_time_get_elapsed()) + 1.0f - interpolation_fraction;
}
