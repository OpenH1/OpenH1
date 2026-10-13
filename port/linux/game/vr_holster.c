/*
VR_HOLSTER.C

The VR mode's holsters (HALO_VR; vr.holsters, port/linux/README.md "VR"):
the game's side of them, for player_control.c and first_person_weapons.c.

The player carries two weapons and holds one. The other, the one the
game's weapon switch (Y) would take up, is on the body: a long gun over the
right shoulder, a pistol or the needler at the right hip (vr_render_long_
gun). The right hand reaching there and gripping draws it: port/linux/src/
vr.c presses the switch, which goes through the game's input and the
netcode as any switch does. The holsters follow the body, whose heading is
the head's, which it follows only past a turn of the head.

The stowed weapon is drawn in its holster in the first-person pass (its
model, in its default pose), and it lights up as the right hand comes to
it, so that the hand finds it without looking: the energy of the player's
own shield (the unit's modifier shader, the plasma that flares over the
body as it is hit) wraps it, in the highlight's colour, rising as the hand
nears and full within reach, as the arms' flare is drawn over them
(first_person_weapons.c); and its light and what its metal reflects take
on the colour. The plasma moves and is added to what is behind it, so it
shows on a weapon that is bright already, where a light alone barely
does; it is the game's own shader, so no new assets. A unit with no
plasma shield (a marine, a map's own biped) has the light alone. The
reload by hand lights up its pouch and where the magazine goes the same way
(vr_reload.c, vr_holster_highlight), and the grab a weapon on the ground the
right hand reaches for (vr_grab.c, vr_holster_highlight_object).
*/

#ifdef HALO_VR

#include "cseries.h"
#include "math/real_math.h"
#include "game/players.h"
#include "items/weapons.h"
#include "models/model_definitions.h"
#include "models/models.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "rasterizer/rasterizer_model_types.h"
#include "render/render.h"
#include "shaders/shader_definitions.h"
#include "tag_files/tag_files.h"
#include "units/units.h"

#include "object_mesh.h"

#include "../src/vr.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* the stowed weapon lights up, the hand from its holster's reach nearer
than this (metres): the shield's plasma over it in this colour (at the
shader's own brightness), rising to HIGHLIGHT_NEAR at the reach and full
within it (a step that says the hand is there, as its controller ticks),
and its ambient light raised toward the colour by up to HIGHLIGHT_LIGHT */
#define HIGHLIGHT_DISTANCE 0.35f
#define HIGHLIGHT_NEAR 0.7f
#define HIGHLIGHT_LIGHT 0.6f
static real_rgb_color const highlight_color = { 0.45f, 0.75f, 1.0f };
/* the shield's plasma over it stands off it no more than this (metres): a
body's shield stands off much farther, which on a pistol's few faces
splits them apart into shards */
#define HIGHLIGHT_PLASMA_OFFSET 0.02f
/* drawn at its finest detail */
#define HOLSTERED_DETAIL_PIXELS 1000.0f
/* a long gun slung across the back of the body drawn (first_person_legs.c):
on its side, its right side against the back, SLING_CLEARANCE (metres)
behind the farthest back of the upper body, its middle (half way from its
butt to its muzzle) at the middle of the back, at the foot of the chest,
its butt up over the right shoulder and its barrel down across the back to
the left, SLING_ANGLE (radians) from the chest's up: the assault rifle's
butt just above the shoulder, where the hand reaches over the shoulder for
it, a long one's (the sniper rifle's, the rocket launcher's) behind the
head and its muzzle by the left thigh. Without its model's mesh, its butt,
muzzle and right side as far as these from its grip (metres) */
#define SLING_CLEARANCE 0.02f
#define SLING_ANGLE 0.52f
#define SLING_BUTT_UNKNOWN 0.30f
#define SLING_MUZZLE_UNKNOWN 0.50f
#define SLING_SIDE_UNKNOWN 0.05f
/* a weapon at the hip: where the right hand reaches for it (vr.c's
holster), but no nearer the hips of the body drawn than HIP_CLEARANCE
(metres) beside them: a wide one (the needler, 20 cm across) is moved out
until its inner side is */
#define HIP_CLEARANCE 0.02f

enum
{
	_render_model_effect_type_none = 0,
	/* (shield_glow.c's: shader_get_and_verify_type's plasma) */
	_shader_type_transparent_plasma = 10,
};

/* the plasma shader's fields after its header that say how strong it is
drawn (rasterizer_xbox_plasma_energy.c's) */
struct vr_holster_plasma
{
	byte reserved00[4];
	short intensity_exponent_source;
	short pad06;
	real intensity_exponent;
};

void platform_log(char const *format, ...);
/* vr_render.c's */
boolean vr_render_long_gun(long weapon_index);
boolean vr_render_body_place(const float place[3], const float forward[3], const float up[3], real_matrix4x3 *matrix,
	real *units);
boolean vr_render_body_anchor(real_point3d *neck, real *yaw);
/* first_person_legs.c's */
void first_person_legs_vr_back(real_point3d const *anchor, real yaw, real_matrix4x3 *chest, real *back);
void first_person_legs_vr_hips(real_point3d const *anchor, real yaw, real_point3d *middle, real *side);

/* the highlights drawn in a frame: the stowed weapon's, the reload's
(vr_reload.c: its pouch, where the magazine goes), and the weapon on the
ground the right hand reaches for (vr_grab.c) */
enum
{
	_vr_highlight_holster = 0,
	_vr_highlight_pouch,
	_vr_highlight_well,
	_vr_highlight_grab,
	NUMBER_OF_VR_HIGHLIGHTS,
};

static struct
{
	/* the stowed weapon and its holster (HALO_VR_HOLSTER_*), and the unit
	carrying it */
	long weapon_index;
	int holster;
	long unit_index;
	/* the shield's plasma over each highlight this frame, its four
	functions' values and colours: kept until the plasma is drawn, after
	the model (the rasterizer keeps pointers to them), and the colours told
	again by vr_holster_plasma_override, with how far it stands off */
	struct
	{
		real values[4];
		real_rgb_color colors[4];
		real offset;
	} plasma[NUMBER_OF_VR_HIGHLIGHTS];
	/* the stowed weapon's extent from its grip, for its definition (NONE:
	none yet; and its tag's name: another map's may have its index): where
	its butt and its muzzle are, its right side and its left (world units),
	along its own axes */
	long extent_definition_index;
	char extent_name[128];
	real extent_butt;
	real extent_muzzle;
	real extent_right;
	real extent_left;
} vr_holster = { NONE, HALO_VR_HOLSTER_NONE, NONE, { { { 0.0f } } }, NONE, "", 0.0f, 0.0f, 0.0f, 0.0f };

/* the weapon the switch would take up: the next one carried after the one
held */
static long vr_holster_next_weapon(long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	short offset;

	for (offset = 1; offset < MAXIMUM_WEAPONS_PER_UNIT; offset++)
	{
		short index = (short)((unit->unit.current_weapon_index + offset) % MAXIMUM_WEAPONS_PER_UNIT);
		long weapon_index = unit_inventory_get_weapon(unit_index, index);

		if (weapon_index != NONE)
			return weapon_index;
	}
	return NONE;
}

/* player_control.c, each frame, for the first local player's unit (aiming:
the controller or the head aims it, on foot): the stowed weapon's holster,
told to vr.c */
void vr_holster_update(long unit_index, boolean aiming)
{
	long weapon_index = NONE;
	int holster = HALO_VR_HOLSTER_NONE;

	if (unit_index != NONE && aiming && unit_get(unit_index)->object.parent_object_index == NONE)
	{
		weapon_index = vr_holster_next_weapon(unit_index);
		if (weapon_index != NONE && unit_inventory_get_weapon(unit_index, unit_get(unit_index)->unit.current_weapon_index) !=
			weapon_index)
		{
			holster = vr_render_long_gun(weapon_index) ? HALO_VR_HOLSTER_BACK : HALO_VR_HOLSTER_HIP;
		}
		else
		{
			weapon_index = NONE;
		}
	}
	if (weapon_index != vr_holster.weapon_index || holster != vr_holster.holster)
	{
		if (weapon_index != NONE)
		{
			char const *name = tag_get_name(object_get(weapon_index)->definition_index);

			platform_log("vr holster: %s at the %s %s", name ? name : "a weapon",
				halo_vr_left_handed() ? "left" : "right", holster == HALO_VR_HOLSTER_BACK ? "shoulder" : "hip");
		}
		vr_holster.weapon_index = weapon_index;
		vr_holster.holster = holster;
	}
	vr_holster.unit_index = unit_index;
	halo_vr_holster_stowed(holster);
}

/* whether the unit and the stowed weapon kept from the frame's update are
still the first player's unit and a weapon it carries, found again from the
player: the game's ticks since (a script taking the weapons), or the level
ending (the frame drawn as it ends, the map's objects gone), may have
deleted them */
static boolean vr_holster_current(void)
{
	long player_index = local_player_get_player_index(0);
	long unit_index;
	short slot;

	if (vr_holster.unit_index == NONE || vr_holster.weapon_index == NONE || !halo_vr_running() ||
		player_index == NONE)
	{
		return FALSE;
	}
	unit_index = player_get(player_index)->unit_index;
	if (unit_index == NONE || unit_index != vr_holster.unit_index || !unit_try_and_get(unit_index))
		return FALSE;
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		if (unit_inventory_get_weapon(unit_index, slot) == vr_holster.weapon_index)
			return TRUE;
	}
	return FALSE;
}

/* the shield's plasma of the unit's over a highlight (_vr_highlight_*) as
strong as this (0 to 1), the world's units a metre, or FALSE when the unit
has none: model_effect set to draw it */
static boolean vr_holster_plasma(short highlight, long unit_index, real strength, real units,
	struct render_model_effect *model_effect)
{
	struct object_definition *definition = object_definition_get(object_get(unit_index)->definition_index);
	struct shader *shader;
	struct vr_holster_plasma const *plasma;
	real value;
	short index;

	if (strength <= 0.0f || definition->object.modifier_shader.index == NONE)
		return FALSE;
	shader = shader_definition_get(definition->object.modifier_shader.index);
	if (!shader || shader->base.type != _shader_type_transparent_plasma)
		return FALSE;
	/* (the shader draws it as strong as the value to its exponent: the
	value its root, so that it is as strong as asked) */
	plasma = (struct vr_holster_plasma const *)((byte const *)shader + sizeof(struct shader));
	value = plasma->intensity_exponent > 0.0f ? (real)pow(strength, 1.0 / plasma->intensity_exponent) : strength;
	for (index = 0; index < 4; index++)
	{
		vr_holster.plasma[highlight].values[index] = value;
		vr_holster.plasma[highlight].colors[index] = highlight_color;
	}
	vr_holster.plasma[highlight].offset = HIGHLIGHT_PLASMA_OFFSET * units;
	model_effect->modifier_shader = shader;
	model_effect->modifier_animation.values = vr_holster.plasma[highlight].values;
	model_effect->modifier_animation.colors = vr_holster.plasma[highlight].colors;
	return TRUE;
}

/* rasterizer_xbox_plasma_energy.c, as a plasma is drawn with these colours:
when it is the plasma over a highlight, the highlight's colour, and how far
it stands off what it lights up at the most (world units; it is drawn no
farther); FALSE otherwise, drawn as it is */
boolean vr_holster_plasma_override(real_rgb_color const *colors, real_rgb_color *color, real *offset_amount)
{
	short highlight;

	if (!colors)
		return FALSE;
	for (highlight = 0; highlight < NUMBER_OF_VR_HIGHLIGHTS; highlight++)
	{
		if (colors == vr_holster.plasma[highlight].colors)
		{
			*color = highlight_color;
			if (*offset_amount > vr_holster.plasma[highlight].offset)
				*offset_amount = vr_holster.plasma[highlight].offset;
			return TRUE;
		}
	}
	return FALSE;
}

/* a light raised toward the highlight's colour, as strong as this (0 to 1) */
static void vr_holster_raise_light(real strength, struct render_lighting *lighting)
{
	if (strength > 0.0f)
	{
		real light = strength * HIGHLIGHT_LIGHT;

		lighting->ambient_color.red += (highlight_color.red - lighting->ambient_color.red) * light;
		lighting->ambient_color.green += (highlight_color.green - lighting->ambient_color.green) * light;
		lighting->ambient_color.blue += (highlight_color.blue - lighting->ambient_color.blue) * light;
		/* (and what its metal reflects, which makes a pistol's bright) */
		lighting->reflection_tint_color.red += (highlight_color.red - lighting->reflection_tint_color.red) * light;
		lighting->reflection_tint_color.green += (highlight_color.green - lighting->reflection_tint_color.green) * light;
		lighting->reflection_tint_color.blue += (highlight_color.blue - lighting->reflection_tint_color.blue) * light;
	}
}

/* a model lit up as strong as this (0 to 1) for the unit's player, the
world's units a metre: the shield's plasma over it (model_effect set to draw
it, else none), and its light, the unit's, raised toward the highlight's
colour (the light's, given) */
static void vr_holster_light_up(short highlight, long unit_index, real strength, real units,
	struct render_model_effect *model_effect, struct render_lighting *lighting)
{
	csmemset(model_effect, 0, sizeof(*model_effect));
	model_effect->type = _render_model_effect_type_none;
	vr_holster_plasma(highlight, unit_index, strength, units, model_effect);
	*lighting = *object_get_cached_render_lighting(unit_index, FLT_MAX);
	vr_holster_raise_light(strength, lighting);
}

/* vr_reload.c: its pouch (well: where the magazine goes) lit up as the
holsters are, as strong as this (0 to 1) */
void vr_holster_highlight(boolean well, long unit_index, real strength, real units,
	struct render_model_effect *model_effect, struct render_lighting *lighting)
{
	vr_holster_light_up(well ? _vr_highlight_well : _vr_highlight_pouch, unit_index, strength, units, model_effect,
		lighting);
}

/* the stowed weapon's extent from its grip, along its own axes (its
model's mesh, once for each of its kind: the mesh kept is the body's, read
again after), in the world's units (units a metre) */
static void vr_holster_extent(long weapon_index, real units)
{
	long definition_index = object_get(weapon_index)->definition_index;
	char const *name = tag_get_name(definition_index);
	struct object_mesh const *mesh;
	long index;

	if (!name)
		name = "";
	if (definition_index == vr_holster.extent_definition_index && !strcmp(name, vr_holster.extent_name))
		return;
	vr_holster.extent_definition_index = definition_index;
	snprintf(vr_holster.extent_name, sizeof(vr_holster.extent_name), "%s", name);
	vr_holster.extent_butt = -SLING_BUTT_UNKNOWN * units;
	vr_holster.extent_muzzle = SLING_MUZZLE_UNKNOWN * units;
	vr_holster.extent_right = -SLING_SIDE_UNKNOWN * units;
	vr_holster.extent_left = SLING_SIDE_UNKNOWN * units;
	mesh = object_mesh_get(weapon_index);
	if (!mesh || mesh->vertex_count <= 0)
		return;
	vr_holster.extent_butt = vr_holster.extent_muzzle = vr_holster.extent_right = vr_holster.extent_left = 0.0f;
	for (index = 0; index < mesh->vertex_count; index++)
	{
		real_point3d const *p = &mesh->vertices[index].position;

		vr_holster.extent_butt = MIN(vr_holster.extent_butt, p->x);
		vr_holster.extent_muzzle = MAX(vr_holster.extent_muzzle, p->x);
		vr_holster.extent_right = MIN(vr_holster.extent_right, p->y);
		vr_holster.extent_left = MAX(vr_holster.extent_left, p->y);
	}
	platform_log("vr holster: %s: %.2f m long, %.2f m across, its butt %.2f m behind its grip",
		name, (vr_holster.extent_muzzle - vr_holster.extent_butt) / units,
		(vr_holster.extent_left - vr_holster.extent_right) / units, -vr_holster.extent_butt / units);
}

/* vr_grab.c: a weapon in the world that the unit's player's right hand
reaches for, lit up as the stowed one is as the hand comes (distance: the
hand's from its reach, metres; 0 within it), the world's units a metre: the
shield's plasma over it (model_effect's, else left as it is) and its own
light (given) raised */
void vr_holster_highlight_object(long unit_index, real distance, real units, struct render_model_effect *model_effect,
	struct render_lighting *lighting)
{
	real strength;

	if (distance <= 0.0f)
		strength = 1.0f;
	else if (distance < HIGHLIGHT_DISTANCE)
		strength = (1.0f - distance / HIGHLIGHT_DISTANCE) * HIGHLIGHT_NEAR;
	else
		return;
	vr_holster_plasma(_vr_highlight_grab, unit_index, strength, units, model_effect);
	vr_holster_raise_light(strength, lighting);
}

/* first_person_weapons.c, as the first-person weapon is drawn (flags: its
model's): the stowed weapon in its holster, lit up as the right hand comes
to it */
void vr_holster_draw(unsigned long flags)
{
	struct halo_vr_holsters holsters;
	struct render_lighting lighting;
	struct render_model_effect model_effect;
	struct model *model;
	real_matrix4x3 node_matrices[MAXIMUM_NODES_PER_MODEL];
	real_matrix4x3 holstered;
	real c, s, units, highlight;
	float forward[3], up[3];
	long model_index, unit_index;

	if (!vr_holster_current() || !halo_vr_holsters(&holsters))
		return;
	model_index = object_definition_get(object_get(vr_holster.weapon_index)->definition_index)->object.model.index;
	if (model_index == NONE)
		return;
	model = model_definition_get(model_index);
	if (model->nodes.count <= 0 || model->nodes.count > MAXIMUM_NODES_PER_MODEL)
		return;
	/* along the body: a long gun across the back (where the body's
	is not drawn, as in a cinematic), its grip up at the shoulder and its
	muzzle down to the left, its top away from the back; a pistol muzzle
	down, its grip back, its top forward */
	c = (real)cos(holsters.body_yaw);
	s = (real)sin(holsters.body_yaw);
	if (vr_holster.holster == HALO_VR_HOLSTER_BACK)
	{
		/* (forward: down 0.8, left 0.6; up: backward) */
		forward[0] = -0.6f * s;
		forward[1] = 0.6f * c;
		forward[2] = -0.8f;
		up[0] = -c;
		up[1] = -s;
		up[2] = 0.0f;
	}
	else
	{
		/* (forward: down 0.95, forward 0.3; up: forward 0.95, up 0.3) */
		forward[0] = 0.3f * c;
		forward[1] = 0.3f * s;
		forward[2] = -0.95f;
		up[0] = 0.95f * c;
		up[1] = 0.95f * s;
		up[2] = 0.3f;
	}
	if (!vr_render_body_place(holsters.place[vr_holster.holster], forward, up, &holstered, &units))
		return;
	if (vr_holster.holster == HALO_VR_HOLSTER_BACK)
	{
		real_point3d neck;
		real yaw;

		/* (a long gun slung across the back of the body as it is drawn:
		on its side, clear of its armour behind the shoulders, leaning
		back with it as it leans back from the eyes; drawn at the grab's
		place, over the right shoulder, it lay through the shoulder and the
		collar, as far as 19 cm into them. Where the hand reaches for it
		stays over the shoulder: vr.c's holster) */
		if (vr_render_body_anchor(&neck, &yaw))
		{
			real_matrix4x3 chest;
			real back, middle, across = (real)sin(SLING_ANGLE), down = (real)cos(SLING_ANGLE);

			vr_holster_extent(vr_holster.weapon_index, units);
			first_person_legs_vr_back(&neck, yaw, &chest, &back);
			/* (the barrel down to the left, the left side out, the top
			up to the left) */
			holstered.forward.i = chest.left.i * across - chest.up.i * down;
			holstered.forward.j = chest.left.j * across - chest.up.j * down;
			holstered.forward.k = chest.left.k * across - chest.up.k * down;
			holstered.left.i = -chest.forward.i;
			holstered.left.j = -chest.forward.j;
			holstered.left.k = -chest.forward.k;
			cross_product3d(&holstered.forward, &holstered.left, &holstered.up);
			/* (its right side SLING_CLEARANCE behind the back, its middle
			at the back's) */
			middle = (vr_holster.extent_butt + vr_holster.extent_muzzle) * 0.5f;
			back += vr_holster.extent_right - SLING_CLEARANCE * units;
			holstered.position.x = chest.position.x + chest.forward.i * back - holstered.forward.i * middle;
			holstered.position.y = chest.position.y + chest.forward.j * back - holstered.forward.j * middle;
			holstered.position.z = chest.position.z + chest.forward.k * back - holstered.forward.k * middle;
		}
	}
	else
	{
		real_point3d neck, hips;
		real yaw, side;

		/* (out beside the hips of the body as it is drawn, if it was in
		them: its left side, toward them, HIP_CLEARANCE from their right
		side, level along the heading's left) */
		if (vr_render_body_anchor(&neck, &yaw))
		{
			real_vector3d left, offset;
			real inner, beside;

			vr_holster_extent(vr_holster.weapon_index, units);
			first_person_legs_vr_hips(&neck, yaw, &hips, &side);
			left.i = -(real)sin(yaw);
			left.j = (real)cos(yaw);
			left.k = 0.0f;
			vector_from_points3d(&hips, &holstered.position, &offset);
			inner = dot_product3d(&offset, &left) + vr_holster.extent_left;
			beside = side - HIP_CLEARANCE * units;
			if (inner > beside)
			{
				holstered.position.x -= left.i * (inner - beside);
				holstered.position.y -= left.j * (inner - beside);
			}
		}
	}
	model_get_node_matrices(model, node_matrices, &holstered.position, &holstered.forward, &holstered.up);

	/* the highlight, rising as the hand comes, full within reach: the
	shield's plasma over it, and its light, the unit's, raised */
	if (holsters.reach == vr_holster.holster)
		highlight = 1.0f;
	else if (holsters.distance[vr_holster.holster] < HIGHLIGHT_DISTANCE)
		highlight = (1.0f - holsters.distance[vr_holster.holster] / HIGHLIGHT_DISTANCE) * HIGHLIGHT_NEAR;
	else
		highlight = 0.0f;
	vr_holster_light_up(_vr_highlight_holster, vr_holster.unit_index, highlight, units, &model_effect, &lighting);
	render_model(model_index, HOLSTERED_DETAIL_PIXELS, node_matrices, NULL, NULL, NULL, &lighting,
		&holstered.position, 0.0f, &model_effect, vr_holster.weapon_index, 0, flags);
}

#endif
