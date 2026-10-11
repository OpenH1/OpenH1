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
plasma shield (a marine, a map's own biped) has the light alone.
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

#include "../src/vr.h"

#include <float.h>
#include <math.h>

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

static struct
{
	/* the stowed weapon and its holster (HALO_VR_HOLSTER_*), and the unit
	carrying it */
	long weapon_index;
	int holster;
	long unit_index;
	/* the shield's plasma over it this frame, its four functions' values
	and colours: kept until the plasma is drawn, after the model (the
	rasterizer keeps pointers to them), and the colours told again by
	vr_holster_plasma_override, with how far it stands off the weapon */
	real plasma_values[4];
	real_rgb_color plasma_colors[4];
	real plasma_offset;
} vr_holster = { NONE, HALO_VR_HOLSTER_NONE, NONE };

/* the first player's unit, which the VR mode plays; NONE in the menus and
between maps */
static long vr_holster_player_unit(void)
{
	long player_index = local_player_get_player_index(0);

	if (!halo_vr_running() || player_index == NONE)
		return NONE;
	return player_get(player_index)->unit_index;
}

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

			platform_log("vr holster: %s at the %s", name ? name : "a weapon",
				holster == HALO_VR_HOLSTER_BACK ? "right shoulder" : "right hip");
		}
		vr_holster.weapon_index = weapon_index;
		vr_holster.holster = holster;
	}
	vr_holster.unit_index = unit_index;
	halo_vr_holster_stowed(holster);
}

/* the shield's plasma over the stowed weapon as strong as this (0 to 1),
the world's units a metre, or FALSE when the unit has none: model_effect
set to draw it */
static boolean vr_holster_plasma(real strength, real units, struct render_model_effect *model_effect)
{
	struct object_definition *definition = object_definition_get(object_get(vr_holster.unit_index)->definition_index);
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
		vr_holster.plasma_values[index] = value;
		vr_holster.plasma_colors[index] = highlight_color;
	}
	vr_holster.plasma_offset = HIGHLIGHT_PLASMA_OFFSET * units;
	model_effect->modifier_shader = shader;
	model_effect->modifier_animation.values = vr_holster.plasma_values;
	model_effect->modifier_animation.colors = vr_holster.plasma_colors;
	return TRUE;
}

/* rasterizer_xbox_plasma_energy.c, as a plasma is drawn with these colours:
when it is the plasma over the stowed weapon, the highlight's colour, and
how far it stands off the weapon at the most (world units; it is drawn no
farther); FALSE otherwise, drawn as it is */
boolean vr_holster_plasma_override(real_rgb_color const *colors, real_rgb_color *color, real *offset_amount)
{
	if (!colors || colors != vr_holster.plasma_colors)
		return FALSE;
	*color = highlight_color;
	if (*offset_amount > vr_holster.plasma_offset)
		*offset_amount = vr_holster.plasma_offset;
	return TRUE;
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

	if (vr_holster.weapon_index == NONE || vr_holster.unit_index == NONE || !halo_vr_holsters(&holsters))
		return;
	/* (the weapon and unit kept from player_control.c's last frame only
	while they are still the player's: a frame drawn as a map ends, before
	the next map's first tick, has the old map's handles, gone) */
	unit_index = vr_holster_player_unit();
	if (unit_index == NONE || unit_index != vr_holster.unit_index ||
		vr_holster_next_weapon(unit_index) != vr_holster.weapon_index)
	{
		return;
	}
	model_index = object_definition_get(object_get(vr_holster.weapon_index)->definition_index)->object.model.index;
	if (model_index == NONE)
		return;
	model = model_definition_get(model_index);
	if (model->nodes.count <= 0 || model->nodes.count > MAXIMUM_NODES_PER_MODEL)
		return;
	/* along the body: a long gun across the back, its grip up at the
	shoulder and its muzzle down to the left, its top away from the back; a
	pistol muzzle down, its grip back, its top forward */
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
	model_get_node_matrices(model, node_matrices, &holstered.position, &holstered.forward, &holstered.up);

	/* the highlight, rising as the hand comes, full within reach: the
	shield's plasma over it, and its light, the unit's, raised */
	if (holsters.reach == vr_holster.holster)
		highlight = 1.0f;
	else if (holsters.distance[vr_holster.holster] < HIGHLIGHT_DISTANCE)
		highlight = (1.0f - holsters.distance[vr_holster.holster] / HIGHLIGHT_DISTANCE) * HIGHLIGHT_NEAR;
	else
		highlight = 0.0f;
	csmemset(&model_effect, 0, sizeof(model_effect));
	model_effect.type = _render_model_effect_type_none;
	vr_holster_plasma(highlight, units, &model_effect);
	lighting = *object_get_cached_render_lighting(vr_holster.unit_index, FLT_MAX);
	if (highlight > 0.0f)
	{
		real light = highlight * HIGHLIGHT_LIGHT;

		lighting.ambient_color.red += (highlight_color.red - lighting.ambient_color.red) * light;
		lighting.ambient_color.green += (highlight_color.green - lighting.ambient_color.green) * light;
		lighting.ambient_color.blue += (highlight_color.blue - lighting.ambient_color.blue) * light;
		/* (and what its metal reflects, which makes a pistol's bright) */
		lighting.reflection_tint_color.red += (highlight_color.red - lighting.reflection_tint_color.red) * light;
		lighting.reflection_tint_color.green += (highlight_color.green - lighting.reflection_tint_color.green) * light;
		lighting.reflection_tint_color.blue += (highlight_color.blue - lighting.reflection_tint_color.blue) * light;
	}
	render_model(model_index, HOLSTERED_DETAIL_PIXELS, node_matrices, NULL, NULL, NULL, &lighting,
		&holstered.position, 0.0f, &model_effect, vr_holster.weapon_index, 0, flags);
}

#endif
