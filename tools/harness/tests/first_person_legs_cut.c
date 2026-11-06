/*
FIRST_PERSON_LEGS_CUT.C

The first-person body kept out of the near plane's cut without moving it
(port/linux/game/first_person_legs.c). Its pose (first_person_legs_node_
matrices: the head and arms folded away, the upper body leant back clear of
the eye, what the near plane would still cut folded away) is made for a body
of the test's own, a skeleton of the game's node names skinned with tubes,
in views of the test's own (a pinhole: the eye, its forward and up, the
tangents of its sides, a viewport off its window's middle as split
screen's are, the near distance as near_clip.c brings it in at a wide view):

- planted (#724): standing, the view tilted from level to looking straight
  down, at the stock view and wide ones: its hips, legs and feet are where
  the game has them in every frame. Set back from the eye and lowered out
  of the cut, the legs slid across the ground as the view tilted;
- crouch_jump (#692): the eye down at the hips, as crouching in the air
  brings it: none of the surface is left in the cut but where it is folded
  into the eye, and what is not folded is where the game has it;
- triangle (#692's second report): first_person_legs_cut finds the
  triangles any of which is in the cut, checked against clipping each to
  it: a thigh's plate across the cut, its corners out of it, is found.
*/

#include "harness.h"
#include <math.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) > (b) ? (b) : (a))
#define _pi 3.14159265358979323846
#define TAG_STRING_LENGTH 31

#include "types.inc"

struct render_frustum
{
	real_plane3d world_planes[6];
	real_point3d world_vertices[5];
	real z_near, z_far;
};

/* the fake world: the player's unit, its model's nodes, its mesh, the view */
struct model_node
{
	char name[TAG_STRING_LENGTH + 1];
	short parent_node_index;
};
struct tag_block
{
	long count;
	void *elements;
};
struct model
{
	struct tag_block nodes;
};
#define TAG_BLOCK_GET_ELEMENT(block, index, type) (&((type *)(block)->elements)[index])
struct object_definition
{
	struct { struct { long index; } model; } object;
};
struct object_datum
{
	long definition_index;
	struct
	{
		long parent_object_index;
		real_vector3d forward;
		struct { long size; } node_matrices;
	} object;
};
static struct
{
	struct { real_point3d position; } camera;
	struct render_frustum frustum;
} global_window_parameters;

static struct model test_model;
static struct object_definition test_definition;
static struct object_datum test_unit;
static struct object_mesh test_mesh;
#define object_try_and_get(index) (&test_unit)
#define object_definition_get(index) (&test_definition)
#define model_definition_get(index) (&test_model)
#define object_mesh_get(index) (&test_mesh)
#define render_interpolation_view_body(unit_index, move) (memset((move), 0, sizeof(*(move))), FALSE)
#define frames_clock(milliseconds) (*(milliseconds) = 0, 1)
#define system_milliseconds() 0

#include "under_test.inc"

/* the code's margin (test_first_person_legs_cut.py checks it against the
code's) */
#define MARGIN 0.01f

/* the game's near distance and the reach at a wide view (near_clip.c) */
#define GAME_NEAR 0.0625f
#define REACH 0.2f
#define FAR 1024.f

struct view
{
	real_point3d eye;
	real_vector3d forward, right, up;
	struct render_frustum frustum;
};

static real_vector3d combine(real_vector3d a, real sa, real_vector3d b, real sb)
{
	real_vector3d v = { a.i * sa + b.i * sb, a.j * sa + b.j * sb, a.k * sa + b.k * sb };
	real length = sqrtf(v.i * v.i + v.j * v.j + v.k * v.k);

	v.i /= length;
	v.j /= length;
	v.k /= length;
	return v;
}

static real dot(real_vector3d a, real_vector3d b)
{
	return a.i * b.i + a.j * b.j + a.k * b.k;
}

/* a view from an eye: degrees across at 16:9 (0: the stock 70 across 4:3,
widened as the game does), the window's shape (width over height), the
viewport's bounds in the window (-1 to 1 each way), pitched (degrees, up
positive) and turned */
static struct view view_from(real_point3d eye, double degrees, double shape, double x0, double x1, double y0,
	double y1, double pitch, double yaw)
{
	struct view view;
	double vertical = degrees ? tan(degrees * _pi / 360.0) * (9.0 / 16.0) : 0.75 * 0.85 * tan(35.0 * _pi / 180.0);
	double across = vertical * shape;
	double p = pitch * _pi / 180.0, y = yaw * _pi / 180.0;
	real_vector3d zero = { 0, 0, 0 };
	real_vector3d forward = { (real)(cos(p) * cos(y)), (real)(cos(p) * sin(y)), (real)sin(p) };
	real_vector3d up = { (real)(-sin(p) * cos(y)), (real)(-sin(p) * sin(y)), (real)cos(p) };
	real_vector3d right = { forward.j * up.k - forward.k * up.j, forward.k * up.i - forward.i * up.k,
		forward.i * up.j - forward.j * up.i };
	double corner = sqrt(1.0 + pow(MAX(fabs(x0), fabs(x1)) * across, 2) + pow(MAX(fabs(y0), fabs(y1)) * vertical, 2));
	real z_near = (real)MIN(GAME_NEAR, REACH / corner);
	double xs[4] = { x0, x1, x1, x0 }, ys[4] = { y0, y0, y1, y1 };
	short plane;

	memset(&view, 0, sizeof(view));
	view.eye = eye;
	view.forward = forward;
	view.right = right;
	view.up = up;
	/* (each side's normal outward, through the eye: left, right, bottom, top) */
	view.frustum.world_planes[0].n = combine(forward, (real)(x0 * across), right, -1.f);
	view.frustum.world_planes[1].n = combine(right, 1.f, forward, (real)(-x1 * across));
	view.frustum.world_planes[2].n = combine(forward, (real)(y0 * vertical), up, -1.f);
	view.frustum.world_planes[3].n = combine(up, 1.f, forward, (real)(-y1 * vertical));
	for (plane = 0; plane < 4; plane++)
		view.frustum.world_planes[plane].d = dot(view.frustum.world_planes[plane].n, *(real_vector3d *)&view.eye);
	/* (the near plane's outward toward the eye: its distance the near
	distance less a point's depth) */
	view.frustum.world_planes[4].n = combine(forward, -1.f, zero, 0.f);
	view.frustum.world_planes[4].d = -dot(forward, *(real_vector3d *)&view.eye) - z_near;
	view.frustum.z_near = z_near;
	view.frustum.z_far = FAR;
	/* (the far plane's corners, and the eye) */
	for (plane = 0; plane < 4; plane++)
	{
		view.frustum.world_vertices[plane].x = eye.x + FAR * (forward.i + (real)(xs[plane] * across) * right.i +
			(real)(ys[plane] * vertical) * up.i);
		view.frustum.world_vertices[plane].y = eye.y + FAR * (forward.j + (real)(xs[plane] * across) * right.j +
			(real)(ys[plane] * vertical) * up.j);
		view.frustum.world_vertices[plane].z = eye.z + FAR * (forward.k + (real)(xs[plane] * across) * right.k +
			(real)(ys[plane] * vertical) * up.k);
	}
	view.frustum.world_vertices[4] = eye;
	return view;
}

static real_point3d const test_eye = { 12.5f, -40.25f, 3.0f };

static struct view view_of(double degrees, double shape, double x0, double x1, double y0, double y1, double pitch,
	double yaw)
{
	return view_from(test_eye, degrees, shape, x0, x1, y0, y1, pitch, yaw);
}

static struct view level(double degrees)
{
	return view_of(degrees, 16.0 / 9.0, -1, 1, -1, 1, 0, 30);
}

/* a point at so far forward, left and up of the eye, level (the body's
frame: forward the way the eye looks, flattened) */
static real_point3d at(struct view const *view, real forward, real left, real up)
{
	real_vector3d ahead = { view->forward.i, view->forward.j, 0 };
	real length = sqrtf(ahead.i * ahead.i + ahead.j * ahead.j);
	real_point3d point;

	if (length < 1.0e-4f)
	{
		ahead.i = -view->up.i * (view->forward.k < 0 ? -1.f : 1.f);
		ahead.j = -view->up.j * (view->forward.k < 0 ? -1.f : 1.f);
		length = sqrtf(ahead.i * ahead.i + ahead.j * ahead.j);
	}
	ahead.i /= length;
	ahead.j /= length;
	point.x = view->eye.x + ahead.i * forward - ahead.j * left;
	point.y = view->eye.y + ahead.j * forward + ahead.i * left;
	point.z = view->eye.z + up;
	return point;
}

/* whether a triangle has any of it in the cut, by clipping it to the cut's
planes, each its own way (the sides within by a hundredth of a millimetre,
the near plane's depth less than the near distance and the margin): something
left, it is */
static boolean triangle_in_cut(struct view const *view, real_point3d const *corners, real margin)
{
	real_point3d polygon[2][16];
	int count = 3;
	int from = 0;
	short plane;
	int index;

	for (index = 0; index < 3; index++)
		polygon[0][index] = corners[index];
	for (plane = 0; plane < 5 && count > 0; plane++)
	{
		int kept = 0;

		for (index = 0; index < count; index++)
		{
			real_point3d a = polygon[from][index];
			real_point3d b = polygon[from][(index + 1) % count];
			real_vector3d va = { a.x - view->eye.x, a.y - view->eye.y, a.z - view->eye.z };
			real_vector3d vb = { b.x - view->eye.x, b.y - view->eye.y, b.z - view->eye.z };
			/* (each a point's distance past the plane's bound: within, less than none) */
			real da = plane < 4 ? dot(va, view->frustum.world_planes[plane].n) + 1.0e-5f :
				dot(va, view->forward) - view->frustum.z_near - margin;
			real db = plane < 4 ? dot(vb, view->frustum.world_planes[plane].n) + 1.0e-5f :
				dot(vb, view->forward) - view->frustum.z_near - margin;

			if (da < 0.f)
				polygon[!from][kept++] = a;
			if ((da < 0.f) != (db < 0.f))
			{
				real t = da / (da - db);
				real_point3d c = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };

				polygon[!from][kept++] = c;
			}
		}
		count = kept;
		from = !from;
	}
	return count > 0;
}

/* the views every case is tried in: the stock view, 90, 120, 150 and 160
across 16:9, 160 at 4:3, 21:9 and 32:9, a portrait screen, split screen's
halves (the top's and the left's), each level, looking down and up */
static struct view views_from(real_point3d eye, int index, char *name, size_t size, double pitch_override)
{
	static double const degrees[] = { 0, 90, 120, 150, 160, 160, 160, 160, 160, 160, 120 };
	static double const shapes[] = { 16.0 / 9.0, 16.0 / 9.0, 16.0 / 9.0, 16.0 / 9.0, 16.0 / 9.0, 4.0 / 3.0,
		21.0 / 9.0, 32.0 / 9.0, 9.0 / 16.0, 32.0 / 9.0, 8.0 / 9.0 };
	static double const bounds[][4] = { { -1, 1, -1, 1 }, { -1, 1, -1, 1 }, { -1, 1, -1, 1 }, { -1, 1, -1, 1 },
		{ -1, 1, -1, 1 }, { -1, 1, -1, 1 }, { -1, 1, -1, 1 }, { -1, 1, -1, 1 }, { -1, 1, -1, 1 },
		{ -1, 1, -0.5, 1.5 }, { -0.25, 1.75, -1, 1 } };
	static double const pitches[] = { 0, -40, -80, 25 };
	int shape = index / 4;
	double pitch = pitch_override < 1000.0 ? pitch_override : pitches[index % 4];

	snprintf(name, size, "%g degrees, shape %.3f, bounds %g %g %g %g, pitch %g", degrees[shape], shapes[shape],
		bounds[shape][0], bounds[shape][1], bounds[shape][2], bounds[shape][3], pitch);
	return view_from(eye, degrees[shape], shapes[shape], bounds[shape][0], bounds[shape][1], bounds[shape][2],
		bounds[shape][3], pitch, 30.0 + index * 17.0);
}
#define VIEWS 44

/* ---------- the test's body: the game's node names, a tube about each bone */

enum
{
	PELVIS, SPINE, SPINE1, NECK, HEAD, LEFT_ARM, RIGHT_ARM,
	LEFT_THIGH, LEFT_CALF, LEFT_FOOT, RIGHT_THIGH, RIGHT_CALF, RIGHT_FOOT,
	NODES
};
static struct model_node nodes_named[NODES] =
{
	{ "bip01 pelvis", NONE }, { "bip01 spine", PELVIS }, { "bip01 spine1", SPINE }, { "bip01 neck", SPINE1 },
	{ "bip01 head", NECK }, { "bip01 l upperarm", SPINE1 }, { "bip01 r upperarm", SPINE1 },
	{ "bip01 l thigh", PELVIS }, { "bip01 l calf", LEFT_THIGH }, { "bip01 l foot", LEFT_CALF },
	{ "bip01 r thigh", PELVIS }, { "bip01 r calf", RIGHT_THIGH }, { "bip01 r foot", RIGHT_CALF },
};
/* each node's place in the body's frame (forward, left, up from between its
feet on the ground, world units: the cyborg's, standing, its eye 0.26 over
its hips), the bone's end (its child's place, or for a foot its toes) and
the tube's radius */
static real const bones[NODES][7] =
{
	{ 0.f, 0.f, 0.36f, 0.f, 0.f, 0.42f, 0.075f },
	{ 0.f, 0.f, 0.42f, 0.f, 0.f, 0.50f, 0.08f },
	{ 0.f, 0.f, 0.50f, 0.f, 0.f, 0.56f, 0.085f },
	{ 0.f, 0.f, 0.56f, 0.f, 0.f, 0.60f, 0.03f },
	{ 0.f, 0.f, 0.60f, 0.f, 0.f, 0.66f, 0.04f },
	{ 0.f, 0.08f, 0.54f, 0.f, 0.1f, 0.44f, 0.03f },
	{ 0.f, -0.08f, 0.54f, 0.f, -0.1f, 0.44f, 0.03f },
	{ 0.f, 0.04f, 0.34f, 0.01f, 0.04f, 0.18f, 0.035f },
	{ 0.01f, 0.04f, 0.18f, 0.f, 0.04f, 0.03f, 0.03f },
	{ 0.f, 0.04f, 0.03f, 0.07f, 0.04f, 0.01f, 0.025f },
	{ 0.f, -0.04f, 0.34f, 0.01f, -0.04f, 0.18f, 0.035f },
	{ 0.01f, -0.04f, 0.18f, 0.f, -0.04f, 0.03f, 0.03f },
	{ 0.f, -0.04f, 0.03f, 0.07f, -0.04f, 0.01f, 0.025f },
};
#define SIDES 8
#define RINGS 4

static struct object_mesh_vertex mesh_vertices[NODES * RINGS * SIDES];
static unsigned short mesh_triangles[NODES * (RINGS - 1) * SIDES * 2][3];
static real_matrix4x3 game_pose[NODES];

/* a matrix at a place, turned by yaw (radians) about the vertical */
static real_matrix4x3 placed(real x, real y, real z, real yaw)
{
	real_matrix4x3 matrix;

	memset(&matrix, 0, sizeof(matrix));
	matrix.scale = 1.f;
	matrix.forward.i = cosf(yaw);
	matrix.forward.j = sinf(yaw);
	matrix.left.i = -sinf(yaw);
	matrix.left.j = cosf(yaw);
	matrix.up.k = 1.f;
	matrix.position.x = x;
	matrix.position.y = y;
	matrix.position.z = z;
	return matrix;
}

/* the body standing on the ground below the eye (its hips under it, so
far below: 0.26 standing), facing yaw, its knees raised (0: standing; 1:
crouched in the air, thighs ahead and level) */
static void body(real hips_below, real yaw, real knees)
{
	real_point3d ground = { test_eye.x, test_eye.y, test_eye.z - hips_below - 0.36f };
	short index;
	long vertex = 0, triangle = 0;

	test_model.nodes.count = NODES;
	test_model.nodes.elements = nodes_named;
	test_definition.object.model.index = 1;
	test_unit.object.parent_object_index = NONE;
	test_unit.object.forward.i = cosf(yaw);
	test_unit.object.forward.j = sinf(yaw);
	test_unit.object.forward.k = 0.f;
	test_unit.object.node_matrices.size = NODES * (long)sizeof(real_matrix4x3);
	test_mesh.node_count = NODES;
	for (index = 0; index < NODES; index++)
	{
		real const *bone = bones[index];
		real from[3] = { bone[0], bone[1], bone[2] }, to[3] = { bone[3], bone[4], bone[5] };
		real_matrix4x3 rest;
		short ring, side;

		/* (the thighs raised ahead, level, and the calves hanging below the
		knees, by knees) */
		if (index == LEFT_THIGH || index == RIGHT_THIGH)
		{
			to[0] += 0.16f * knees;
			to[2] += 0.16f * knees;
		}
		if (index >= LEFT_CALF && index != RIGHT_THIGH && index != LEFT_THIGH)
		{
			from[0] += 0.16f * knees;
			to[0] += 0.16f * knees;
			from[2] += 0.16f * knees;
			to[2] += 0.16f * knees;
		}
		/* (the model's space the standing pose's: each node's inverse its
		place's, the game's pose the body's frame on the ground) */
		rest = placed(bone[0], bone[1], bone[2], 0.f);
		test_mesh.inverse[index] = placed(-bone[0], -bone[1], -bone[2], 0.f);
		{
			real_matrix4x3 frame = placed(ground.x, ground.y, ground.z, yaw);
			real_matrix4x3 at_place = placed(from[0], from[1], from[2], 0.f);

			matrix4x3_multiply(&frame, &at_place, &game_pose[index]);
		}
		(void)rest;
		/* (rings along the bone, in the model's space, each bound to the
		node, the last half to its child: the skin between them) */
		for (ring = 0; ring < RINGS; ring++)
		{
			real t = (real)ring / (RINGS - 1);
			real cx = bone[0] + (bone[3] - bone[0]) * t, cy = bone[1] + (bone[4] - bone[1]) * t,
				cz = bone[2] + (bone[5] - bone[2]) * t;
			short child = NONE, other;

			for (other = 0; other < NODES; other++)
			{
				if (nodes_named[other].parent_node_index == index && child == NONE)
					child = other;
			}
			for (side = 0; side < SIDES; side++)
			{
				struct object_mesh_vertex *v = &mesh_vertices[vertex + ring * SIDES + side];
				real angle = (real)(2.0 * _pi * side / SIDES);
				real r = bone[6];

				/* (about the bone: around its vertical, or for a level bone,
				the foot, around its length) */
				if (fabsf(bone[5] - bone[2]) > 0.05f)
				{
					v->position.x = cx + r * cosf(angle);
					v->position.y = cy + r * sinf(angle);
					v->position.z = cz;
				}
				else
				{
					v->position.x = cx;
					v->position.y = cy + r * cosf(angle);
					v->position.z = cz + r * sinf(angle);
				}
				v->nodes[0] = (byte)index;
				v->nodes[1] = (byte)(ring == RINGS - 1 && child != NONE && child != LEFT_ARM && child != RIGHT_ARM ?
					child : index);
				v->weight = (short)(v->nodes[1] == index ? 32767 : 16384);
			}
		}
		for (ring = 0; ring + 1 < RINGS; ring++)
		{
			for (side = 0; side < SIDES; side++)
			{
				unsigned short a = (unsigned short)(vertex + ring * SIDES + side);
				unsigned short b = (unsigned short)(vertex + ring * SIDES + (side + 1) % SIDES);

				mesh_triangles[triangle][0] = a;
				mesh_triangles[triangle][1] = b;
				mesh_triangles[triangle][2] = (unsigned short)(a + SIDES);
				triangle++;
				mesh_triangles[triangle][0] = b;
				mesh_triangles[triangle][1] = (unsigned short)(b + SIDES);
				mesh_triangles[triangle][2] = (unsigned short)(a + SIDES);
				triangle++;
			}
		}
		vertex += RINGS * SIDES;
	}
	test_mesh.vertices = mesh_vertices;
	test_mesh.triangles = mesh_triangles;
	test_mesh.vertex_count = vertex;
	test_mesh.triangle_count = triangle;
}

/* the pose drawn in a view */
static real_matrix4x3 const *posed(struct view const *view)
{
	global_window_parameters.camera.position = view->eye;
	global_window_parameters.frustum = view->frustum;
	return first_person_legs_node_matrices(0, game_pose);
}

static boolean same_place(real_matrix4x3 const *a, real_matrix4x3 const *b)
{
	return a->position.x == b->position.x && a->position.y == b->position.y && a->position.z == b->position.z &&
		a->scale == b->scale;
}

static boolean is_leg(short node)
{
	return node == PELVIS || node >= LEFT_THIGH;
}

/* how many of the surface's triangles drawn with a pose are in the cut,
clipped to it, but those folded into the eye (seen edge on) */
static long cut_seen(struct view const *view, real_matrix4x3 const *pose)
{
	long count = first_person_legs_surface(&test_mesh, pose, NODES);
	long found = 0;
	long index;

	for (index = 0; index < test_mesh.triangle_count; index++)
	{
		real_point3d corners[3];
		boolean at_eye = FALSE;
		short corner;

		for (corner = 0; corner < 3; corner++)
		{
			struct object_mesh_vertex const *vertex = &test_mesh.vertices[test_mesh.triangles[index][corner]];
			short n;

			corners[corner] = first_person_legs_points[test_mesh.triangles[index][corner]];
			for (n = 0; n < 2; n++)
			{
				real_matrix4x3 const *node = &pose[vertex->nodes[n]];

				if (node->scale < 0.01f && node->position.x == view->eye.x && node->position.y == view->eye.y &&
					node->position.z == view->eye.z)
				{
					at_eye = TRUE;
				}
			}
		}
		/* (not one folded to a point, under a millimetre: the head's and
		the arms') */
		if (count && !at_eye && triangle_in_cut(view, corners, MARGIN * 0.99f) &&
			!(pose[test_mesh.vertices[test_mesh.triangles[index][0]].nodes[0]].scale < 0.01f &&
				fabsf(corners[0].x - corners[1].x) + fabsf(corners[0].y - corners[1].y) +
				fabsf(corners[0].z - corners[1].z) + fabsf(corners[0].x - corners[2].x) +
				fabsf(corners[0].y - corners[2].y) + fabsf(corners[0].z - corners[2].z) < 0.001f))
		{
			found++;
		}
	}
	return found;
}

static real_point3d points[64];
static real point_spans[64][2];
static unsigned short triangles[64][3];
static boolean marked[64];

/* first_person_legs_cut's mark of each triangle is clipping's */
static void check_marks(struct view const *view, long count, long triangle_count, char const *what)
{
	long found = first_person_legs_cut(&view->frustum, points, count, (unsigned short const (*)[3])triangles,
		triangle_count, point_spans, marked);
	long index, expected = 0;

	for (index = 0; index < triangle_count; index++)
	{
		real_point3d corners[3] = { points[triangles[index][0]], points[triangles[index][1]],
			points[triangles[index][2]] };
		/* (a hair inside and outside the margin: what is on its bound
		either way) */
		boolean inside = triangle_in_cut(view, corners, MARGIN * 0.999f);
		boolean outside = triangle_in_cut(view, corners, MARGIN * 1.001f);

		expected += inside;
		CHECK(marked[index] == inside || marked[index] == outside, "%s: triangle %ld marked %d, clipped %d", what,
			index, marked[index], inside);
	}
	CHECK(found >= 0 && found <= triangle_count, "%s: found %ld", what, found);
	(void)expected;
}

int main(int argc, char **argv)
{
	char const *which = argc > 1 ? argv[1] : "";
	char name[160];
	int index;

	if (!strcmp(which, "planted"))
	{
		/* standing still, the view tilted from level to looking straight
		down (and turned with it, as the body's facing stays): the hips,
		legs and feet where the game has them in every frame, at every view.
		The upper body leans back as it must (so the test weighs a lean) */
		boolean leant = FALSE;
		int pitch;

		body(0.26f, (real)(30.0 * _pi / 180.0), 0.f);
		for (index = 0; index < VIEWS; index++)
		{
			for (pitch = 0; pitch >= -85; pitch -= 5)
			{
				struct view view = views_from(test_eye, index, name, sizeof(name), pitch);
				real_matrix4x3 const *pose;
				short node;

				body(0.26f, (real)((30.0 + index * 17.0) * _pi / 180.0), 0.f);
				pose = posed(&view);
				CHECK(pose != NULL, "%s: no pose", name);
				for (node = 0; node < NODES; node++)
				{
					/* (a leg's node where the game has it, or folded away where
					the near plane would cut it: the hips, at the widest
					views; never a foot) */
					if (is_leg(node) && (pose[node].scale == 1.f || node == LEFT_FOOT || node == RIGHT_FOOT))
					{
						CHECK(same_place(&pose[node], &game_pose[node]),
							"%s: %s moved to %.4f %.4f %.4f from %.4f %.4f %.4f", name, nodes_named[node].name,
							pose[node].position.x, pose[node].position.y, pose[node].position.z,
							game_pose[node].position.x, game_pose[node].position.y, game_pose[node].position.z);
					}
				}
				leant |= !same_place(&pose[SPINE1], &game_pose[SPINE1]) && pose[SPINE1].scale == 1.f;
			}
		}
		CHECK(leant, "the upper body never leant");
	}
	else if (!strcmp(which, "crouch_jump"))
	{
		/* crouching in the air: the eye down at the hips (0.02 below it), the
		thighs ahead, beside and before it. None of the surface is left in
		the cut but what is folded into the eye, and what is not folded is
		where the game has it; at a wide view, looking down, the thighs are
		folded (so the test weighs a fold) */
		boolean folded = FALSE;

		for (index = 0; index < VIEWS; index++)
		{
			struct view view = views_from(test_eye, index, name, sizeof(name), 1000.0);
			real_matrix4x3 const *pose;
			short node;

			body(-0.02f, (real)((30.0 + index * 17.0) * _pi / 180.0), 1.f);
			pose = posed(&view);
			CHECK(pose != NULL, "%s: no pose", name);
			CHECK(cut_seen(&view, pose) == 0, "%s: %ld triangles in the cut", name, cut_seen(&view, pose));
			for (node = 0; node < NODES; node++)
			{
				if (is_leg(node) && pose[node].scale == 1.f)
					CHECK(same_place(&pose[node], &game_pose[node]), "%s: %s moved", name, nodes_named[node].name);
				if ((node == LEFT_THIGH || node == RIGHT_THIGH) && pose[node].scale < 0.01f)
					folded = TRUE;
			}
		}
		CHECK(folded, "no thigh folded at any view");
	}
	else if (!strcmp(which, "triangle"))
	{
		/* a triangle whose corners are all out of the cut, its middle in it:
		two beside the eye, out past the view's sides, nearer than the near
		plane, one far ahead (the thigh's plate, crouching in the air): found */
		struct view view = level(120);
		boolean beyond_corners = FALSE;

		points[0] = at(&view, 0.01f, 0.08f, 0.01f);
		points[1] = at(&view, 0.01f, -0.08f, 0.01f);
		points[2] = at(&view, 0.3f, 0.0f, 0.02f);
		triangles[0][0] = 0;
		triangles[0][1] = 1;
		triangles[0][2] = 2;
		CHECK(triangle_in_cut(&view, points, MARGIN), "the case's triangle is not in the cut");
		check_marks(&view, 3, 1, "the bridge");
		CHECK(marked[0], "the bridge is not found");
		/* (a roof: a broad level one just above the eye, over all the cut,
		its corners and edges far out of it) */
		points[0] = at(&view, -1.f, 1.5f, 0.005f);
		points[1] = at(&view, -1.f, -1.5f, 0.005f);
		points[2] = at(&view, 1.5f, 0.0f, 0.005f);
		check_marks(&view, 3, 1, "the roof");
		CHECK(marked[0], "the roof is not found");
		/* (and broad triangles about the hips and thighs crouching in the
		air, at every view: found as clipping finds them, some with no
		corner in the cut) */
		for (index = 0; index < VIEWS; index++)
		{
			struct view each = views_from(test_eye, index, name, sizeof(name), 1000.0);
			unsigned long seed = 12345u + (unsigned long)index * 7919u;
			long triangle_count = 20;
			long corner;

			for (corner = 0; corner < triangle_count * 3; corner++)
			{
				real f, l, u;

				seed = seed * 1103515245u + 12345u;
				f = -0.08f + (real)((seed >> 8) % 1000) / 1000.f * 0.33f;
				seed = seed * 1103515245u + 12345u;
				l = -0.15f + (real)((seed >> 8) % 1000) / 1000.f * 0.3f;
				seed = seed * 1103515245u + 12345u;
				u = -0.4f + (real)((seed >> 8) % 1000) / 1000.f * 0.46f;
				points[corner] = at(&each, f, l, u);
				triangles[corner / 3][corner % 3] = (unsigned short)corner;
			}
			check_marks(&each, triangle_count * 3, triangle_count, name);
			for (corner = 0; corner < triangle_count; corner++)
			{
				real_point3d const *p = &points[corner * 3];
				short k, held = 0;

				for (k = 0; k < 3; k++)
				{
					real_point3d one[3] = { p[k], p[k], p[k] };

					held += triangle_in_cut(&each, one, MARGIN);
				}
				beyond_corners |= marked[corner] && !held;
			}
		}
		CHECK(beyond_corners, "no triangle found with none of its corners in the cut");
	}
	else
	{
		fprintf(stderr, "unknown case %s\n", which);
		return 2;
	}
	return 0;
}
