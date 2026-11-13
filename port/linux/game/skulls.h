/*
SKULLS.H

The campaign's skulls (skulls.c): optional rules, as Halo: Combat Evolved
Anniversary's, that the original game never had.
*/

#ifndef SKULLS_H
#define SKULLS_H
#include "cseries.h"

/* ---------- constants */

/* (the bits of a set of skulls; their order is the network message's and
the demos' header's, so a skull is only ever added at the end) */
enum
{
	/* any player's death reverts every player to the last checkpoint; a
	death alone, with nobody left, restarts the level */
	_skull_iron,
	/* a player's shields recharge only when they melee an enemy */
	_skull_black_eye,
	/* the enemies' health and shields doubled */
	_skull_mythic,
	/* the enemies one rank up, where their variant has a major upgrade */
	_skull_thunderstorm,
	/* the enemies throw grenades far more often */
	_skull_catch,
	/* the enemies always dive, take cover and evade where they can */
	_skull_tough_luck,
	/* the weapons the enemies drop carry half their ammunition */
	_skull_famine,
	/* no motion tracker */
	_skull_fog,
	/* the damage types' strengths and weaknesses against materials (plasma
	against shields, bullets against flesh) exaggerated */
	_skull_tilt,
	/* no HUD and no first-person weapon */
	_skull_blind,
	NUMBER_OF_SKULLS
};

#define SKULLS_ALL ((1UL << NUMBER_OF_SKULLS) - 1)
/* the presets: "Mythic" (meant for Legendary), and LASO (all of them, meant
for Legendary) */
#define SKULLS_PRESET_MYTHIC (FLAG(_skull_mythic) | FLAG(_skull_thunderstorm) | FLAG(_skull_catch))
#define SKULLS_PRESET_LASO SKULLS_ALL

/* Mythic's factor on the enemies' health and shields, Catch's on their
chance to throw a grenade and on the wait between an encounter's throws */
#define SKULLS_MYTHIC_VITALITY_SCALE 2.0f
#define SKULLS_CATCH_GRENADE_CHANCE_SCALE 4.0f
#define SKULLS_CATCH_GRENADE_TIMER_SCALE 0.25f

/* ---------- prototypes */

/* a skull's name (game.skulls's word for it), NULL past the last */
char const *skull_name(short skull);
/* the skulls named in text (game.skulls: names separated by spaces or
commas; an unknown name is left out) */
unsigned long skulls_parse(char const *text);
/* the names of the skulls of a set, as game.skulls takes them */
void skulls_format(unsigned long skulls, char *text, long size);

/* the skulls of the game about to begin, taken as its map's objects and AI
are placed (game.c): a campaign game of this computer's, game.skulls; a
network co-op host's, game.skulls in an OpenH1 game that no OpenCE machine
can join (none otherwise: a Legacy game plays stock); a client's, none
until the host's word comes (network_skulls.c); a demo's, its recording's */
void skulls_initialize_for_new_map(void);
/* the game's skulls, which hold until its map is left; and a client's from
its host (network_skulls.c), taken in a campaign game only */
unsigned long skulls_game(void);
void skulls_set_game(unsigned long skulls);
/* whether a skull plays: one of the game's, in the campaign (a multiplayer
game has none) */
boolean skull_active(short skull);

/* the hooks in the game's systems, each the value unchanged with the skull
off: */
/* ... a difficulty value of an enemy's (game_globals.c): Mythic's vitality
and shields, Catch's grenades */
real skulls_enemy_difficulty_value(short value_type, real value);
/* ... a damage effect's modifier against a material (damage.c): Tilt */
real skulls_material_modifier(real modifier);
/* ... the chance of an AI's dodge, dive or evasion, which its definition
gives (actions.c, actor_perception.c): Tough Luck's every one it can (a
chance above 0) */
real skulls_evasion_chance(real chance);
/* ... a squad's major upgrade (encounters.c): Thunderstorm's, an enemy
team's, whose variant has one */
boolean skulls_force_major_upgrade(short team_index);
/* ... the weapon an AI unit drops as it dies (actors.c): Famine's, half its
ammunition */
void skulls_actor_dropped_weapon(long weapon_index);
/* ... whether a player's shields recharge on their own (damage.c): not with
Black Eye; and whether a hit (a melee's or not), dealt by a unit to a
unit, recharges the dealer's shields (Black Eye's melee hit on an enemy) */
boolean skulls_shields_recharge(long unit_index);
boolean skulls_melee_recharges_shields(boolean melee, long dealer_unit_index, short dealer_team_index,
	short target_team_index);
/* ... whether the motion tracker (game_engine.c), the HUD (hud.c) and the
first-person weapon (first_person_weapons.c) are drawn: Fog's, Blind's */
boolean skulls_motion_sensor_hidden(void);
boolean skulls_hud_hidden(void);

#endif
