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
it, so that the hand finds it without looking.
*/

#ifdef HALO_VR

#include "cseries.h"
#include "math/real_math.h"
#include "items/weapons.h"
#include "models/model_definitions.h"
#include "models/models.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "render/render.h"
#include "tag_files/tag_files.h"
#include "units/units.h"

#include "../src/vr.h"

#include <float.h>
#include <math.h>

/* the stowed weapon lights up, the hand from its holster's reach nearer
than this (metres), the most within it: its ambient light raised toward
this colour, by up to this much */
#define HIGHLIGHT_DISTANCE 0.25f
#define HIGHLIGHT_STRENGTH 0.9f
static real_rgb_color const highlight_color = { 0.45f, 0.75f, 1.0f };
/* drawn at its finest detail */
#define HOLSTERED_DETAIL_PIXELS 1000.0f

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
} vr_holster = { NONE, HALO_VR_HOLSTER_NONE, NONE };

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

/* first_person_weapons.c, as the first-person weapon is drawn (flags: its
model's): the stowed weapon in its holster, lit up as the right hand comes
to it */
void vr_holster_draw(unsigned long flags)
{
	struct halo_vr_holsters holsters;
	struct render_lighting lighting;
	struct model *model;
	real_matrix4x3 node_matrices[MAXIMUM_NODES_PER_MODEL];
	real_matrix4x3 holstered;
	real c, s, units, light;
	float forward[3], up[3];
	long model_index;

	if (vr_holster.weapon_index == NONE || vr_holster.unit_index == NONE || !halo_vr_holsters(&holsters))
		return;
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

	/* the light: the unit's, raised as the hand comes */
	lighting = *object_get_cached_render_lighting(vr_holster.unit_index, FLT_MAX);
	light = 1.0f - holsters.distance[vr_holster.holster] / HIGHLIGHT_DISTANCE;
	if (light > 0.0f)
	{
		light *= light * HIGHLIGHT_STRENGTH;
		lighting.ambient_color.red += (highlight_color.red - lighting.ambient_color.red) * light;
		lighting.ambient_color.green += (highlight_color.green - lighting.ambient_color.green) * light;
		lighting.ambient_color.blue += (highlight_color.blue - lighting.ambient_color.blue) * light;
	}
	render_model(model_index, HOLSTERED_DETAIL_PIXELS, node_matrices, NULL, NULL, NULL, &lighting,
		&holstered.position, 0.0f, NULL, vr_holster.weapon_index, 0, flags);
}

#endif
