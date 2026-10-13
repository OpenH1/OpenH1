/*
VR_FEEL.C

The VR mode's weapon feel (HALO_VR; vr.recoil, vr.weapon_haptics,
port/linux/README.md "VR"): the game's side of it, for weapons.c,
player_effects.c and vr_render.c.

Each shot of the first player's weapon in hand kicks the first-person
weapon in the right hand: drawn turned up about the hand and pushed back,
and back again within about a tenth of a second (a spring at rest, critically
damped, struck by each shot: vr_feel_frame). The kick is the drawn weapon's
alone: the controller aims, the reticle is where it points and the shot
goes there (vr_aim.c), so neither moves with it; nor does a scope's view,
placed from the weapon unkicked (vr_render_scope_view), and while a scope is
raised to an eye the weapon does not kick at all (it is at the eye). How
far it kicks, and how fast it comes back, is each weapon's own, by its
tag's name (vr_feel_weapons): its tag says nothing that sizes it. The
firing's damage effect, which on the flat screen kicks the view and rumbles
the gamepad, has no kick of the view (its camera impulse) but for the plasma
pistol's charged shot, and its rumble is no measure (the sniper rifle's is
lighter than the pistol's); nor is the projectile's damage (the rocket's is
in its blast).

Each shot is felt in the hands too (vr.c, halo_vr_weapon_fire), by the
weapon's kind, in place of the gamepad's rumble the game's own firing gives
(player_effect_start: the firing's damage effect on the player is felt only
as that). Its hand is the weapon's; the left on a long gun's foregrip feels
it a little. The plasma pistol charging hums in the hand, rising.

All of it is the first local player's, found again each time from the
player's unit: no object's index is kept from one frame to the next but to
compare it with the one found.
*/

#ifdef HALO_VR

#include "cseries.h"
#include "math/real_math.h"
#include "game/game.h"
#include "game/players.h"
#include "items/weapons.h"
#include "items/weapon_definitions.h"
#include "objects/damage.h"
#include "objects/objects.h"
#include "tag_files/tag_files.h"
#include "units/units.h"

#include "../src/vr.h"

#include <math.h>
#include <string.h>

/* the kick (vr_feel_frame) never turns past RECOIL_MOST times a shot's,
however fast the weapon fires; the plasma pistol's charged shot kicks
RECOIL_CHARGED times its other shots */
#define RECOIL_MOST 1.6f
#define RECOIL_CHARGED 3.0f

/* each weapon by its tag's name (the first that has this in it; the last
for any other): its feel in the hands (HALO_VR_FEEL_*), and its kick: how
far the muzzle rises (degrees) and the weapon goes back (metres) at its
peak, and how fast it comes back (the spring's rate, 1/seconds: the kick
peaks after 1/rate, and at 40 is a fifth of its peak a tenth of a second
later): the pistol's snap, the assault rifle's chatter (light and quick, a
round at a time, back before the next at its 15 a second), the shotgun's
shove, the sniper rifle's heavy kick, the rocket launcher's push (back more
than up, and slower; the fuel rod gun's alike), the plasma weapons' and
the needler's lighter pulse, the flamethrower's tremble */
static struct vr_feel_weapon
{
	char const *name;
	short feel;
	real rise;
	real back;
	real rate;
} const vr_feel_weapons[] =
{
	{ "weapons\\pistol\\", HALO_VR_FEEL_PISTOL, 5.0f, 0.015f, 42.0f },
	{ "weapons\\assault rifle\\", HALO_VR_FEEL_RIFLE, 1.6f, 0.008f, 55.0f },
	{ "weapons\\shotgun\\", HALO_VR_FEEL_SHOTGUN, 8.0f, 0.035f, 22.0f },
	{ "weapons\\sniper rifle\\", HALO_VR_FEEL_SNIPER, 9.0f, 0.035f, 20.0f },
	{ "weapons\\rocket launcher\\", HALO_VR_FEEL_ROCKET, 4.0f, 0.06f, 12.0f },
	{ "weapons\\plasma pistol\\", HALO_VR_FEEL_PLASMA, 1.5f, 0.006f, 45.0f },
	{ "weapons\\plasma rifle\\", HALO_VR_FEEL_PLASMA, 1.0f, 0.005f, 55.0f },
	{ "weapons\\needler\\", HALO_VR_FEEL_NEEDLER, 1.0f, 0.004f, 55.0f },
	{ "weapons\\plasma_cannon\\", HALO_VR_FEEL_ROCKET, 5.0f, 0.05f, 14.0f },
	{ "weapons\\flamethrower\\", HALO_VR_FEEL_FLAME, 0.4f, 0.003f, 30.0f },
	{ "", HALO_VR_FEEL_TAP, 2.0f, 0.01f, 40.0f },
};

static struct
{
	/* the kick now (vr_feel_frame): the spring's turn (degrees) and its
	speed, and the shots' blows since the last frame (degrees a second);
	how far back for each degree it is up, and how fast it comes back, as
	the last shot's weapon has it; the most it turns */
	real angle;
	real speed;
	real struck;
	real back_per_degree;
	real rate;
	real most;
	/* the weapon in hand fired last (to compare: its firing's damage
	effects, which reach the player as the game's own damage) and those
	effects, each trigger's */
	long weapon_index;
	long firing_damage[2];
	/* the player's unit last frame (NONE: none) */
	long unit_index;
	/* the plasma pistol charging: the game's time it began (NONE: not) */
	long charge_tick;
	real charge_time;
} vr_feel = { 0.0f, 0.0f, 0.0f, 0.0f, 40.0f, 0.0f, NONE, { NONE, NONE }, NONE, NONE, 0.0f };

/* the first player's unit, NONE */
static long vr_feel_unit(void)
{
	long player_index = local_player_get_player_index(0);

	return player_index != NONE ? player_get(player_index)->unit_index : NONE;
}

/* the weapon's kind (vr_feel_weapons) */
static struct vr_feel_weapon const *vr_feel_weapon_find(long definition_index)
{
	char const *name = tag_get_name(definition_index);
	short index;

	for (index = 0; index < (short)NUMBEROF(vr_feel_weapons) - 1; index++)
	{
		if (name && strstr(name, vr_feel_weapons[index].name))
			break;
	}
	return &vr_feel_weapons[index];
}

/* weapons.c, as a trigger fires (the round spent, its projectiles about to
be made), with the firing's damage effect (NONE: none) that it will deal
the weapon's owner: the first player's weapon in hand kicks and is felt */
void vr_feel_weapon_fired(long weapon_index, short trigger_index, long owner_index, long damage_effect_index,
	boolean misfired)
{
	long unit_index = vr_feel_unit();
	struct weapon_definition const *definition;
	struct vr_feel_weapon const *kind;
	short feel;

	if (unit_index == NONE || owner_index != unit_index ||
		unit_inventory_get_weapon(unit_index, unit_get(unit_index)->unit.current_weapon_index) != weapon_index)
	{
		return;
	}
	if (vr_feel.weapon_index != weapon_index)
	{
		vr_feel.weapon_index = weapon_index;
		vr_feel.firing_damage[0] = vr_feel.firing_damage[1] = NONE;
	}
	if (trigger_index >= 0 && trigger_index < 2 && damage_effect_index != NONE)
		vr_feel.firing_damage[trigger_index] = damage_effect_index;
	if (misfired)
		return;
	definition = weapon_definition_get(object_get(weapon_index)->definition_index);
	kind = vr_feel_weapon_find(object_get(weapon_index)->definition_index);
	feel = kind->feel;
	if (definition->weapon.weapon_type == _weapon_type_plasma_pistol && trigger_index == 1)
		feel = HALO_VR_FEEL_CHARGED;
	halo_vr_weapon_fire(feel);
	{
		real rise = kind->rise * (feel == HALO_VR_FEEL_CHARGED ? RECOIL_CHARGED : 1.0f);

		/* (a blow that, from rest, peaks at the weapon's rise: the spring's
		peak is its speed over the rate and e) */
		vr_feel.struck += rise * kind->rate * 2.7182818f;
		vr_feel.back_per_degree = kind->back / kind->rise;
		vr_feel.rate = kind->rate;
		vr_feel.most = rise * RECOIL_MOST;
	}
}

/* player_effects.c, as the game shakes a local player's view and rumbles
their gamepad for damage they took (on a network game's client, the
host's, replayed): TRUE where the VR mode's controllers feel it instead
(the gamepad's rumble left out): the first player's weapon's own firing,
felt as it fired */
boolean vr_feel_damage(long player_index, struct damage_data const *damage, real total_damage)
{
	long unit_index = player_get(player_index)->unit_index;

	if (player_get(player_index)->local_player_index != 0 || unit_index == NONE || !halo_vr_weapon_feel())
		return FALSE;
	/* (the firing of the weapon in hand: from no one, its trigger's effect) */
	return damage->owner_object_index == NONE && vr_feel.weapon_index != NONE &&
		unit_inventory_get_weapon(unit_index, unit_get(unit_index)->unit.current_weapon_index) == vr_feel.weapon_index &&
		(damage->definition_index == vr_feel.firing_damage[0] || damage->definition_index == vr_feel.firing_damage[1]);
}

/* vr_render.c, once a frame (its time since the last, seconds) before the
first-person weapon is posed: the kick moved on, the plasma pistol's charge
told */
void vr_feel_frame(real seconds)
{
	long unit_index = vr_feel_unit();
	struct unit_datum *unit = unit_index != NONE ? unit_get(unit_index) : NULL;
	long weapon_index = NONE;
	real charge = 0.0f;

	/* the kick: x(t) = (x + (v + rx) t) e^(-rt), its speed
	(v - r (v + rx) t) e^(-rt), the shots' blows added first */
	if (seconds > 0.1f)
		seconds = 0.1f;
	if (seconds > 0.0f)
	{
		real r = vr_feel.rate;
		real x = vr_feel.angle;
		real v = vr_feel.speed + vr_feel.struck;
		real fade = (real)exp(-r * seconds);

		vr_feel.angle = (x + (v + r * x) * seconds) * fade;
		vr_feel.speed = (v - r * (v + r * x) * seconds) * fade;
		vr_feel.struck = 0.0f;
		if (vr_feel.angle > vr_feel.most)
		{
			vr_feel.angle = vr_feel.most;
			if (vr_feel.speed > 0.0f)
				vr_feel.speed = 0.0f;
		}
		if (fabsf(vr_feel.angle) < 0.001f && fabsf(vr_feel.speed) < 0.01f)
			vr_feel.angle = vr_feel.speed = 0.0f;
	}

	/* (another unit, another map: its weapons are others) */
	if (unit_index != vr_feel.unit_index)
		vr_feel.weapon_index = NONE;
	vr_feel.unit_index = unit_index;

	/* the plasma pistol charging (or charged, held), its charge rising over
	its trigger's charging time */
	if (unit && !TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
		weapon_index = unit_inventory_get_weapon(unit_index, unit->unit.current_weapon_index);
	if (weapon_index != NONE && weapon_definition_get(object_get(weapon_index)->definition_index)->weapon.weapon_type ==
		_weapon_type_plasma_pistol && weapon_overcharged(weapon_index))
	{
		struct weapon_definition const *definition = weapon_definition_get(object_get(weapon_index)->definition_index);

		if (vr_feel.charge_tick == NONE)
		{
			vr_feel.charge_tick = game_time_get();
			vr_feel.charge_time = definition->weapon.triggers.count > 0 ?
				TAG_BLOCK_GET_ELEMENT(&definition->weapon.triggers, 0, struct weapon_trigger_definition)->charging_time :
				0.0f;
		}
		charge = vr_feel.charge_time > 0.0f ?
			(real)(game_time_get() - vr_feel.charge_tick) / (vr_feel.charge_time * TICKS_PER_SECOND) : 1.0f;
		if (charge > 1.0f)
			charge = 1.0f;
		if (charge < 0.05f)
			charge = 0.05f;
	}
	else
	{
		vr_feel.charge_tick = NONE;
	}
	halo_vr_weapon_charge(charge);
}

/* vr_render.c: the first-person weapon's kick now: how far it is turned up
about the hand (degrees) and pushed back (metres), as vr.recoil scales it */
void vr_feel_kick(real *rise, real *back)
{
	real scale = halo_vr_recoil();

	*rise = vr_feel.angle * scale;
	*back = vr_feel.angle * vr_feel.back_per_degree * scale;
}

#endif
