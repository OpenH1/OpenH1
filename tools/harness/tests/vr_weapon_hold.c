/*
VR_WEAPON_HOLD.C (test)

The VR mode's first-person weapon held in the hand (port/linux/game/
vr_hands.c's weapon_hold, weapon_root_find and support_hold, with
source/math/matrix_math.c's matrices): the game's animation swings the
whole weapon and both arms about the camera (a charged shot, a vent, a
melee, a reload), and the weapon is held where the idle pose has it from
the camera (in the controller, the shots' kick in the camera), what moves
within it moving still; the weapon's root is its model's gun node, not a
part of it hanging apart nor the graph's root; and the left hand on the
foregrip follows the animation's hand near the grip (a pump) and stays on
the weapon, in the idle pose's grip, as the animation takes it away.
test_vr_weapon_hold.py takes the declarations (decls.inc) and the code
(under_test.inc) from the sources.
*/

#include "harness.h"

#include <math.h>

typedef unsigned short word;
#define PIN(x, low, high) ((x) < (low) ? (low) : ((x) > (high) ? (high) : (x)))

#include "decls.inc"

/* ---------- the fake world */

struct tag_block
{
	long count;
	void *address;
	void *definition;
};

#define TAG_BLOCK_GET_ELEMENT(block, index, type) (&((type *)(block)->address)[index])

struct animation_graph
{
	struct tag_block nodes;
};

/* what of vr_hands.c's state the code reads */
static struct
{
	short weapon_root;
	short wrist[NUMBER_OF_SIDES];
	short node_count;
	real_matrix4x3 idle[MAXIMUM_NODES_PER_ANIMATION];
	byte roles[MAXIMUM_NODES_PER_ANIMATION];
} vr_hands;

#include "under_test.inc"

/* ---------- the weapon in hand: a long gun's graph, as the game's are */

enum
{
	_base = 0,
	_l_upperarm,
	_r_upperarm,
	_l_forearm,
	_r_forearm,
	_l_wrist,
	_r_wrist,
	/* a part of the weapon's model hanging apart from the gun, from the
	graph's root (before the gun, as a first found would take it) */
	_stray,
	_l_index,
	_l_pinky,
	_r_index,
	_gun,
	_slide,
	_pump,
	_bullet,
	NODE_COUNT,
};

static const short parents[NODE_COUNT] =
{
	/* (the root its own parent, as the game's graphs have it) */
	_base, _base, _base, _l_upperarm, _r_upperarm, _l_forearm, _r_forearm, _base,
	_l_wrist, _l_wrist, _r_wrist, _r_wrist, _gun, _gun, _slide,
};

/* 1 the weapon's model, 2 the arms' (first_person_weapon_vr_idle_pose) */
static const byte models[NODE_COUNT] =
{
	3, 2, 2, 2, 2, 2, 3, 1, 2, 2, 2, 1, 1, 1, 1,
};

static struct vr_hands_graph_node nodes[NODE_COUNT];
static struct animation_graph graph;

/* world units a metre */
#define UNITS (1.0f / 3.048f)

static void graph_make(void)
{
	short index;

	memset(nodes, 0, sizeof(nodes));
	for (index = 0; index < NODE_COUNT; index++)
		nodes[index].parent_node_index = parents[index];
	graph.nodes.count = NODE_COUNT;
	graph.nodes.address = nodes;
}

/* a frame: at (x, y, z), turned by yaw about up, then pitch about left */
static real_matrix4x3 frame(real x, real y, real z, real yaw, real pitch)
{
	real_matrix4x3 m;
	real cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);

	m.scale = 1.0f;
	m.forward.i = cy * cp;
	m.forward.j = sy * cp;
	m.forward.k = -sp;
	m.left.i = -sy;
	m.left.j = cy;
	m.left.k = 0.0f;
	m.up.i = cy * sp;
	m.up.j = sy * sp;
	m.up.k = cp;
	m.position.x = x;
	m.position.y = y;
	m.position.z = z;
	return m;
}

/* the idle pose, from a camera at the origin looking along x (world units) */
static void idle_make(void)
{
	static const real places[NODE_COUNT][5] =
	{
		{ -0.05f, 0.0f, -0.10f, 0.0f, 0.0f },
		{ -0.03f, 0.06f, -0.08f, 0.3f, 0.2f },
		{ -0.03f, -0.06f, -0.08f, -0.3f, 0.2f },
		{ 0.04f, 0.05f, -0.10f, 0.2f, 0.1f },
		{ 0.04f, -0.06f, -0.10f, -0.2f, 0.1f },
		{ 0.08f, 0.01f, -0.06f, -0.4f, -0.1f },
		{ 0.03f, -0.03f, -0.06f, 0.1f, 0.3f },
		{ 0.02f, 0.03f, -0.02f, 0.5f, 0.0f },
		{ 0.09f, 0.00f, -0.05f, -0.4f, 0.0f },
		{ 0.08f, 0.02f, -0.06f, -0.4f, 0.1f },
		{ 0.04f, -0.03f, -0.05f, 0.1f, 0.4f },
		{ 0.04f, -0.02f, -0.04f, 0.02f, 0.05f },
		{ 0.06f, -0.02f, -0.03f, 0.02f, 0.05f },
		{ 0.09f, -0.01f, -0.05f, 0.02f, 0.05f },
		{ 0.065f, -0.02f, -0.028f, 0.02f, 0.05f },
	};
	short index;

	memset(&vr_hands, 0, sizeof(vr_hands));
	for (index = 0; index < NODE_COUNT; index++)
	{
		vr_hands.idle[index] = frame(places[index][0], places[index][1], places[index][2], places[index][3],
			places[index][4]);
	}
	vr_hands.node_count = NODE_COUNT;
	vr_hands.weapon_root = _gun;
	vr_hands.wrist[_left] = _l_wrist;
	vr_hands.wrist[_right] = _r_wrist;
	vr_hands.roles[_l_wrist] = vr_hands.roles[_l_index] = vr_hands.roles[_l_pinky] = _node_left_hand;
	vr_hands.roles[_r_wrist] = vr_hands.roles[_r_index] = _node_right_hand;
	vr_hands.roles[_l_upperarm] = vr_hands.roles[_l_forearm] = _node_left_arm;
	vr_hands.roles[_r_upperarm] = vr_hands.roles[_r_forearm] = _node_right_arm;
}

/* the camera the weapon is posed from: somewhere in the world, turned, the
shots' kick in it */
static real_matrix4x3 camera_make(void)
{
	return frame(10.0f, 5.0f, 2.0f, 0.52f, -0.17f - 0.09f);
}

static real distance(real_point3d const *a, real_point3d const *b)
{
	real x = a->x - b->x, y = a->y - b->y, z = a->z - b->z;

	return sqrtf(x * x + y * y + z * z);
}

/* the same pose, within a tolerance (world units, and the axes') */
static boolean same(real_matrix4x3 const *a, real_matrix4x3 const *b, real tolerance)
{
	short row, column;

	for (row = 0; row < 4; row++)
	{
		for (column = 0; column < 3; column++)
		{
			if (fabsf(a->n[row][column] - b->n[row][column]) > tolerance)
				return FALSE;
		}
	}
	return fabsf(a->scale - b->scale) <= tolerance * a->scale + 1e-7f;
}

/* the idle pose in the world about this camera, swung by the game's
animation (turned 70 degrees about an axis across the view, carried down
and aside: the charged shot's fling), the slide back within the gun, an
arm's node collapsed */
static void animated_make(real_matrix4x3 const *camera, real_matrix4x3 nodes_out[NODE_COUNT])
{
	real_matrix4x3 fling = frame(0.05f, -0.10f, -0.08f, 0.6f, 1.2f), swung;
	short index;

	matrix4x3_multiply(camera, &fling, &swung);
	for (index = 0; index < NODE_COUNT; index++)
		matrix4x3_multiply(&swung, &vr_hands.idle[index], &nodes_out[index]);
	for (index = _slide; index <= _bullet; index += _bullet - _slide)
	{
		nodes_out[index].position.x -= nodes_out[_gun].forward.i * 0.01f;
		nodes_out[index].position.y -= nodes_out[_gun].forward.j * 0.01f;
		nodes_out[index].position.z -= nodes_out[_gun].forward.k * 0.01f;
	}
	nodes_out[_l_upperarm].scale = 0.0001f;
}

/* ---------- the cases */

int main(int argc, char **argv)
{
	char const *case_name;

	CHECK(argc == 2, "a case");
	case_name = argv[1];
	graph_make();
	idle_make();

	CASE("root")
	{
		byte none[NODE_COUNT];

		/* the gun, with the weapon's parts below it: not the part hanging
		apart from the root (found first), nor the root both models draw */
		CHECK(weapon_root_find(&graph, NODE_COUNT, models) == _gun, "the weapon's root is %d, not the gun",
			weapon_root_find(&graph, NODE_COUNT, models));
		memset(none, 2, sizeof(none));
		CHECK(weapon_root_find(&graph, NODE_COUNT, none) == NONE, "a weapon with no nodes has a root");
	}
	else CASE("held")
	{
		real_matrix4x3 camera = camera_make(), held[NODE_COUNT], animated[NODE_COUNT], expected, a, b, inverse;
		short index, other;

		animated_make(&camera, animated);
		memcpy(held, animated, sizeof(held));
		weapon_hold(held, NODE_COUNT, &camera);
		/* the gun where the idle pose has it from the camera (kicked) */
		matrix4x3_multiply(&camera, &vr_hands.idle[_gun], &expected);
		CHECK(same(&held[_gun], &expected, 1e-4f), "the gun is %.3f from where it is held, out of the hand",
			distance(&held[_gun].position, &expected.position));
		/* the right hand on it, where the idle pose has it */
		matrix4x3_multiply(&camera, &vr_hands.idle[_r_wrist], &expected);
		CHECK(same(&held[_r_wrist], &expected, 1e-4f), "the right hand is %.3f from where it holds the gun",
			distance(&held[_r_wrist].position, &expected.position));
		/* the slide where the animation has it in the gun */
		matrix4x3_inverse(&animated[_gun], &inverse);
		matrix4x3_multiply(&inverse, &animated[_slide], &a);
		matrix4x3_inverse(&held[_gun], &inverse);
		matrix4x3_multiply(&inverse, &held[_slide], &b);
		CHECK(same(&a, &b, 1e-4f), "the slide no longer moves within the gun as animated");
		/* nothing changes its shape or size */
		for (index = 0; index < NODE_COUNT; index++)
		{
			CHECK(fabsf(held[index].scale - animated[index].scale) < 1e-7f, "node %d's size changed", index);
			for (other = 0; other < NODE_COUNT; other++)
			{
				CHECK(fabsf(distance(&held[index].position, &held[other].position) -
					distance(&animated[index].position, &animated[other].position)) < 1e-4f,
					"nodes %d and %d moved apart", index, other);
			}
		}
		/* already held, it stays */
		memcpy(animated, held, sizeof(held));
		weapon_hold(held, NODE_COUNT, &camera);
		for (index = 0; index < NODE_COUNT; index++)
			CHECK(same(&held[index], &animated[index], 1e-4f), "node %d moved, held already", index);
	}
	else CASE("support_near")
	{
		real_matrix4x3 camera = camera_make(), held[NODE_COUNT], animated[NODE_COUNT];
		real_point3d controller;
		short index;

		for (index = 0; index < NODE_COUNT; index++)
			matrix4x3_multiply(&camera, &vr_hands.idle[index], &animated[index]);
		/* the animation's left hand pumping, 6 cm back along the gun */
		for (index = 0; index < NODE_COUNT; index++)
		{
			if (vr_hands.roles[index] == _node_left_hand)
			{
				animated[index].position.x -= animated[_gun].forward.i * 0.06f * UNITS;
				animated[index].position.y -= animated[_gun].forward.j * 0.06f * UNITS;
				animated[index].position.z -= animated[_gun].forward.k * 0.06f * UNITS;
			}
		}
		/* the left hand in its controller, a little away */
		memcpy(held, animated, sizeof(held));
		for (index = 0; index < NODE_COUNT; index++)
		{
			if (vr_hands.roles[index] == _node_left_hand)
				held[index].position.z += 0.03f * UNITS;
		}
		controller = held[_l_wrist].position;
		support_hold(held, animated, 0.0f, UNITS);
		CHECK(distance(&held[_l_wrist].position, &controller) < 1e-6f, "the hand left its controller, not holding");
		support_hold(held, animated, 1.0f, UNITS);
		for (index = 0; index < NODE_COUNT; index++)
		{
			if (vr_hands.roles[index] == _node_left_hand)
				CHECK(same(&held[index], &animated[index], 1e-3f), "node %d does not pump with the animation", index);
		}
	}
	else CASE("support_far")
	{
		real_matrix4x3 camera = camera_make(), held[NODE_COUNT], animated[NODE_COUNT], swing, grip, inverse, on_weapon;
		short index;

		/* the weapon not where the idle pose has it (not held): its grip is
		on it as it is */
		swing = frame(0.02f, 0.03f, -0.01f, 0.2f, -0.3f);
		for (index = 0; index < NODE_COUNT; index++)
		{
			real_matrix4x3 swung;

			matrix4x3_multiply(&swing, &vr_hands.idle[index], &swung);
			matrix4x3_multiply(&camera, &swung, &animated[index]);
		}
		/* the animation's left hand off to a magazine, 40 cm away */
		for (index = 0; index < NODE_COUNT; index++)
		{
			if (vr_hands.roles[index] == _node_left_hand)
			{
				animated[index].position.z -= 0.40f * UNITS;
				animated[index].position.y += 0.05f * UNITS;
			}
		}
		memcpy(held, animated, sizeof(held));
		support_hold(held, animated, 1.0f, UNITS);
		matrix4x3_inverse(&vr_hands.idle[_gun], &inverse);
		matrix4x3_multiply(&animated[_gun], &inverse, &on_weapon);
		for (index = 0; index < NODE_COUNT; index++)
		{
			if (vr_hands.roles[index] != _node_left_hand)
				continue;
			matrix4x3_multiply(&on_weapon, &vr_hands.idle[index], &grip);
			CHECK(same(&held[index], &grip, 1e-3f), "node %d is %.3f m off the gun's grip, taken away", index,
				distance(&held[index].position, &grip.position) / UNITS);
		}
		/* half way (18 cm): between, nearer neither all the way */
		for (index = 0; index < NODE_COUNT; index++)
		{
			if (vr_hands.roles[index] == _node_left_hand)
			{
				animated[index].position.z += 0.40f * UNITS;
				animated[index].position.y -= 0.05f * UNITS;
				animated[index].position.z -= 0.18f * UNITS;
			}
		}
		memcpy(held, animated, sizeof(held));
		support_hold(held, animated, 1.0f, UNITS);
		matrix4x3_multiply(&on_weapon, &vr_hands.idle[_l_wrist], &grip);
		CHECK(distance(&held[_l_wrist].position, &grip.position) > 0.04f * UNITS &&
			distance(&held[_l_wrist].position, &animated[_l_wrist].position) > 0.04f * UNITS,
			"half way, the hand is %.3f m from the grip and %.3f m from the animation's",
			distance(&held[_l_wrist].position, &grip.position) / UNITS,
			distance(&held[_l_wrist].position, &animated[_l_wrist].position) / UNITS);
	}
	else
	{
		return 2;
	}
	return 0;
}
