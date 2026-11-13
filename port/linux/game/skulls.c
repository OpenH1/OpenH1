/*
SKULLS.C

The campaign's skulls: optional rules in the style of Halo: Combat Evolved
Anniversary's, which the original game never had. Each changes one of the
game's own systems where it decides, and nothing else:

- Iron: main.c's death handling. Any player's death reverts every player to
  the last checkpoint (a network co-op host's revert, which its clients
  follow); with nobody left alive in a game of this computer's, the level
  starts again, as Anniversary's solo Iron does.
- Black Eye: damage.c's shield recharge. A player's shields do not recharge
  on their own; a melee hit on an enemy fills them.
- Mythic: the enemies' difficulty values (game_globals.c): their health
  and shields doubled.
- Thunderstorm: a squad's major upgrade (encounters.c): an enemy whose
  variant has a major one is always placed as it.
- Catch: the enemies' difficulty values again: their chance to throw a
  grenade four times, the wait between an encounter's throws a quarter.
- Tough Luck: the AI's chances to dive from a grenade, seek cover from
  danger and notice what it could dodge (actions.c, actor_perception.c):
  every one its definition gives it (a chance above 0) is taken.
- Famine: an AI unit's weapon as it dies (actors.c): half its ammunition.
- Fog: the motion tracker (game_engine.c's
  game_engine_hud_draw_motion_sensor) is not drawn.
- Tilt: a damage effect's modifiers against materials (damage.c) squared:
  what does more than normal damage to a material does more still, and
  what does less, less.
- Blind: the HUD (hud.c) and the first-person weapon
  (first_person_weapons.c) are not drawn.

Anniversary's Grunt Birthday Party is left out: no map has a confetti
effect to burst into.

A game's skulls are taken as its map begins (skulls_initialize_for_new_map)
and hold until it is left: a revert or a reset keeps them. They are the
campaign's alone (skull_active: no game engine). A network co-op game's are
its host's game.skulls, in an OpenH1 game that no OpenCE machine can join;
the host sends them to its OpenH1 clients (network_skulls.c), and a Legacy
game plays none. A demo plays its recording's, which its header keeps
(demo.c).
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "game/game_allegiance.h"
#include "game/game_engine.h"
#include "game/game_globals.h"
#include "game/players.h"
#include "items/weapons.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "demo.h"
#include "network_standard.h"
#include "skulls.h"

#include <string.h>

char const *config_string(char const *name);
int config_write(char const *name, char const *value);

/* ---------- globals */

static char const *const skull_names[NUMBER_OF_SKULLS] =
{
	"iron", "black_eye", "mythic", "thunderstorm", "catch", "tough_luck", "famine", "fog", "tilt", "blind"
};

static unsigned long skulls_of_game;

/* ---------- public code */

char const *skull_name(
	short skull)
{
	return skull >= 0 && skull < NUMBER_OF_SKULLS ? skull_names[skull] : NULL;
}

unsigned long skulls_parse(
	char const *text)
{
	unsigned long skulls = 0;

	while (text && *text)
	{
		char const *end;
		short skull;

		while (*text == ' ' || *text == ',')
			text++;
		for (end = text; *end && *end != ' ' && *end != ','; end++)
		{
		}
		for (skull = 0; skull < NUMBER_OF_SKULLS; skull++)
		{
			if ((long)strlen(skull_names[skull]) == end - text &&
				!strncmp(skull_names[skull], text, (size_t)(end - text)))
			{
				skulls |= FLAG(skull);
			}
		}
		text = end;
	}
	return skulls;
}

void skulls_format(
	unsigned long skulls,
	char *text,
	long size)
{
	long length = 0;
	short skull;

	if (size <= 0)
		return;
	text[0] = 0;
	for (skull = 0; skull < NUMBER_OF_SKULLS; skull++)
	{
		long name_length = (long)strlen(skull_names[skull]);

		if (!TEST_FLAG(skulls, skull))
			continue;
		if (length + (length ? 1 : 0) + name_length + 1 > size)
			break;
		if (length)
			text[length++] = ' ';
		memcpy(text + length, skull_names[skull], (size_t)name_length);
		length += name_length;
		text[length] = 0;
	}
}

unsigned long skulls_chosen(
	void)
{
	return skulls_parse(config_string("game.skulls"));
}

void skulls_choose(
	unsigned long skulls)
{
	char names[128];

	skulls_format(skulls & SKULLS_ALL, names, sizeof(names));
	config_write("game.skulls", names);
}

void skulls_initialize_for_new_map(
	void)
{
	unsigned long recorded;
	struct scenario *scenario = global_scenario_get();

	skulls_of_game = 0;
	if (!scenario || scenario->type != _scenario_type_solo)
		return;
	if (demo_playback_skulls(&recorded))
	{
		skulls_of_game = recorded & SKULLS_ALL;
		return;
	}
	switch (game_connection())
	{
	case _game_connection_local:
		skulls_of_game = skulls_parse(config_string("game.skulls"));
		break;
	case _game_connection_network_server:
		/* (an OpenH1 game's alone, which its OpenCE machines could never
		play: a Legacy game, or one OpenCE's players may join, plays stock) */
		if (network_standard_hosting() && !network_standard_hosting_mixed())
			skulls_of_game = skulls_parse(config_string("game.skulls"));
		break;
	default:
		/* (a client's: the host's word, network_skulls.c) */
		break;
	}
	if (skulls_of_game)
	{
		char names[128];

		skulls_format(skulls_of_game, names, sizeof(names));
		error(_error_silent, "skulls: %s", names);
	}
}

unsigned long skulls_game(
	void)
{
	return skulls_of_game;
}

void skulls_set_game(
	unsigned long skulls)
{
	struct scenario *scenario = global_scenario_get();

	if (scenario && scenario->type == _scenario_type_solo)
		skulls_of_game = skulls & SKULLS_ALL;
}

boolean skull_active(
	short skull)
{
	return skulls_of_game && TEST_FLAG(skulls_of_game, skull) && !game_engine_running();
}

real skulls_enemy_difficulty_value(
	short value_type,
	real value)
{
	switch (value_type)
	{
	case _game_difficulty_value_enemy_vitality:
	case _game_difficulty_value_enemy_shield:
		if (skull_active(_skull_mythic))
			value *= SKULLS_MYTHIC_VITALITY_SCALE;
		break;
	case _game_difficulty_value_grenade_chance_scale:
		if (skull_active(_skull_catch))
			value *= SKULLS_CATCH_GRENADE_CHANCE_SCALE;
		break;
	case _game_difficulty_value_grenade_timer_scale:
		if (skull_active(_skull_catch))
			value *= SKULLS_CATCH_GRENADE_TIMER_SCALE;
		break;
	}
	return value;
}

real skulls_material_modifier(
	real modifier)
{
	return skull_active(_skull_tilt) ? modifier * modifier : modifier;
}

real skulls_evasion_chance(
	real chance)
{
	return chance > 0.0f && skull_active(_skull_tough_luck) ? 1.0f : chance;
}

boolean skulls_force_major_upgrade(
	short team_index)
{
	return skull_active(_skull_thunderstorm) && game_team_is_enemy(_game_team_player, team_index);
}

void skulls_actor_dropped_weapon(
	long weapon_index)
{
	if (weapon_index != NONE && skull_active(_skull_famine))
		weapon_halve_ammunition(weapon_index);
}

boolean skulls_shields_recharge(
	long unit_index)
{
	return !skull_active(_skull_black_eye) || player_index_from_unit_index(unit_index) == NONE;
}

boolean skulls_melee_recharges_shields(
	boolean melee,
	long dealer_unit_index,
	short dealer_team_index,
	short target_team_index)
{
	return melee && dealer_unit_index != NONE && skull_active(_skull_black_eye) &&
		player_index_from_unit_index(dealer_unit_index) != NONE &&
		game_team_is_enemy(dealer_team_index, target_team_index);
}

boolean skulls_motion_sensor_hidden(
	void)
{
	return skull_active(_skull_fog);
}

boolean skulls_hud_hidden(
	void)
{
	return skull_active(_skull_blind);
}
