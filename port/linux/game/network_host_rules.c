/*
NETWORK_HOST_RULES.C

The gametype's rules a player host changes during a game: Host Controls'
RULES (host_controls_menu.inc, reached by GAME's CHANGE RULES...), the
console's host_vehicles and host_weapons. Only the host decides:

- In the lobby a change is the lobby's game (its record, as Server Setup and
  the limits set it: network_game_server_change_game_variant), which every
  machine takes, OpenCE's too.
- In a game under way the host's own game changes, and its record (which a
  player who joins takes, and the next game starts from, unless the playlist
  has another). What only the host does changes at once in any game, Legacy
  too: the vehicles (game_engine_vehicle_set_changed: those nobody rides
  swapped for the new set's at the map's vehicle spawns, those ridden swapped
  once empty; every machine sees the host's objects come and go, as a
  respawned vehicle) and the starting weapons and weapon set (each player's
  next spawn; the weapons that spawn on the map from then). The variant's
  flags every machine draws by (shields, the motion sensor, infinite
  grenades, invisible players) are sent to the host's OpenH1 clients
  (_distributed_message_host_rules, OpenH1's protocol version 10) in an
  OpenH1 game without OpenCE's machines, which would not hear it; in another
  they wait for the next game's lobby (network_host_rules_apply_waiting).
- SAVE AS GAMETYPE saves the rules as the host's profile's gametype, as the
  gametype editor saves one (playlist_profile_save_with_options), which MAPS
  and the playlist offer as saved=<name>.

The pure parts (network_host_rules_*_for, _step, _change, _client_flags,
_message_valid) are tools/harness/tests/test_host_rules.py's.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "interface/player_ui.h"
#include "main/main.h"
#include "networking/network_game_globals.h"
#include "networking/network_game_manager.h"
#include "networking/network_server_manager.h"
#include "objects/objects.h"
#include "saved games/playlist_profile.h"
#include "tag_files/tag_files.h"
#include "text/unicode.h"
#include "network_distributed.h"
#include "network_standard.h"
#include "network_host_controls.h"
#include "network_host_game.h"
#include "network_host_rules.h"

#include <stdio.h>
#include <string.h>

/* network_server_manager.c's, network_connection.c's, console.c's */
struct network_game *network_game_server_get_game(struct network_game_server *server);
boolean network_connection_last_read_was_unreliable(void);
void console_warning(const char *format, ...);
void console_printf(boolean clear, char const *format, ...);

/* ---------- constants */

enum
{
	MAXIMUM_HOST_RULES_MACHINES = HALO_PORT_MAXIMUM_NETWORK_MACHINES,
	/* (the vehicle sets' order: game_engine.c's enum game_engine_vehicles,
	then the PC version's types, in game_engine.h's _variant_vehicle_*
	order from Warthogs) */
	HOST_RULES_VEHICLE_SETS = 8,
	/* (the Xbox's sets, of the variant's own vehicle_set) */
	HOST_RULES_XBOX_VEHICLE_SETS = 5,
	/* (a variant's system default bit: playlist_profile.c's) */
	HOST_RULES_SYSTEM_DEFAULT_BIT = 0,
};

typedef char host_rules_message_size_assert[sizeof(struct host_rules_message) == 8 ? 1 : -1];

/* ---------- globals */

static struct
{
	/* the flags that wait for the next game (HOST_RULES_CLIENT_FLAGS'
	bits), and their values */
	unsigned long waiting_mask;
	unsigned long waiting_flags;
	/* a message of another format noted once */
	boolean other_format_noted;
} host_rules = { 0, 0, FALSE };

/* ---------- the pure parts */

/* a rule's flag (its bit), or 0 */
static unsigned long host_rules_flag(
	short rule)
{
	switch (rule)
	{
	case _host_rule_shields: return FLAG(_game_variant_no_shields_bit);
	case _host_rule_motion_tracker: return FLAG(_game_variant_draw_object_in_motion_sensor_bit);
	case _host_rule_infinite_grenades: return FLAG(_game_variant_infinite_grenades_bit);
	case _host_rule_invisible: return FLAG(_game_variant_always_invisible_bit);
	default: return 0;
	}
}

byte network_host_rules_vehicle_step(
	byte set,
	short step,
	boolean custom)
{
	byte sets[HOST_RULES_VEHICLE_SETS + 2];
	short count = 0;
	short index;

	for (index = 0; index < HOST_RULES_VEHICLE_SETS; index++)
		sets[count++] = (byte)index;
	sets[count++] = VARIANT_VEHICLE_SET_PC;
	if (custom)
		sets[count++] = VARIANT_VEHICLE_SET_CUSTOM;
	for (index = 0; index < count && sets[index] != set; index++)
	{
	}
	/* (one not listed: the first) */
	if (index == count)
		return sets[0];
	return sets[(index + (step < 0 ? count - 1 : 1)) % count];
}

void network_host_rules_set_vehicles(
	struct game_variant *variant,
	struct game_variant_options *options,
	short side,
	byte set)
{
	options->vehicle_set[side] = set;
	/* (free for all: red's, both sides') */
	if (!variant->universal_variant.teams)
		options->vehicle_set[1] = set;
	/* (the Xbox's set, red's if it is one of the Xbox's, which the remap
	takes where both sides have one: menu_functions.c's) */
	variant->universal_variant.vehicle_set = options->vehicle_set[0] < HOST_RULES_XBOX_VEHICLE_SETS ?
		options->vehicle_set[0] : 0;
}

boolean network_host_rules_change(
	struct game_variant *variant,
	struct game_variant_options *options,
	short rule,
	short step,
	boolean custom)
{
	unsigned long flag = host_rules_flag(rule);

	switch (rule)
	{
	case _host_rule_vehicles:
	case _host_rule_vehicles_blue:
	{
		short side = rule == _host_rule_vehicles_blue ? 1 : 0;
		byte set = network_host_rules_vehicle_step(options->vehicle_set[side], step, custom);

		if (set == options->vehicle_set[side])
			return FALSE;
		network_host_rules_set_vehicles(variant, options, side, set);
		return TRUE;
	}
	case _host_rule_weapons:
		options->loadout = options->loadout == _loadout_custom ? _loadout_category : _loadout_custom;
		return TRUE;
	case _host_rule_primary:
	case _host_rule_secondary:
	{
		byte *weapon = rule == _host_rule_primary ? &options->primary_weapon : &options->secondary_weapon;

		*weapon = (byte)((*weapon + (step < 0 ? NUMBER_OF_LOADOUT_WEAPONS - 1 : 1)) % NUMBER_OF_LOADOUT_WEAPONS);
		return TRUE;
	}
	case _host_rule_weapon_set:
		variant->universal_variant.weapon_set = (variant->universal_variant.weapon_set +
			(step < 0 ? HOST_RULES_WEAPON_SETS - 1 : 1)) % HOST_RULES_WEAPON_SETS;
		return TRUE;
	default:
		if (!flag)
			return FALSE;
		variant->universal_variant.flags ^= flag;
		return TRUE;
	}
}

boolean network_host_rules_client_side(
	short rule)
{
	return host_rules_flag(rule) != 0;
}

boolean network_host_rules_live_for(
	short rule,
	boolean lobby,
	boolean standard,
	short opence_machines)
{
	/* (the lobby's record every machine takes; what the host alone does;
	the flags in a game whose machines all hear the host's message) */
	return lobby || !network_host_rules_client_side(rule) || (standard && opence_machines == 0);
}

unsigned long network_host_rules_client_flags(
	unsigned long current,
	unsigned long sent)
{
	return (current & ~HOST_RULES_CLIENT_FLAGS) | (sent & HOST_RULES_CLIENT_FLAGS);
}

boolean network_host_rules_message_valid(
	struct host_rules_message const *message)
{
	return message->format == HOST_RULES_FORMAT && !message->pad[0] && !message->pad[1] && !message->pad[2] &&
		!(message->flags & ~HOST_RULES_CLIENT_FLAGS);
}

/* ---------- the host */

boolean network_host_rules_available(
	void)
{
	struct network_game_server *server = global_network_game_server_get();

	return server && network_host_controls_available() && network_host_game_hosting() &&
		network_game_server_get_game(server)->variant.game_engine_index != 0;
}

/* whether the host is in its lobby (its record is the game) */
static boolean host_rules_lobby(
	void)
{
	return network_host_game_in_lobby() || !game_engine_running();
}

/* the rules as they are: the lobby's record, else the game's */
static boolean host_rules_current(
	struct game_variant *variant,
	struct game_variant_options *options)
{
	struct network_game_server *server = global_network_game_server_get();

	if (!server)
		return FALSE;
	if (host_rules_lobby())
	{
		*variant = network_game_server_get_game(server)->variant;
		*options = network_game_server_get_game(server)->variant_options;
	}
	else
	{
		*variant = *game_engine_get_variant();
		*options = *game_variant_options_get();
	}
	return TRUE;
}

/* whether the gametype has a custom vehicle set's counts to keep */
static boolean host_rules_custom_vehicles(
	struct game_variant_options const *options)
{
	short side, index;

	for (side = 0; side < 2; side++)
	{
		for (index = 0; index < NUMBER_OF_VARIANT_VEHICLES; index++)
		{
			if (options->vehicle_counts[side][index])
				return TRUE;
		}
	}
	return FALSE;
}

/* the game's OpenCE machines */
static short host_rules_opence_machines(
	void)
{
	long machines[MAXIMUM_HOST_RULES_MACHINES];
	short count = distributed_client_machines(machines, MAXIMUM_HOST_RULES_MACHINES);
	short opence = 0;
	short index;

	for (index = 0; index < count; index++)
	{
		if (network_standard_machine_is_opence(machines[index]))
			opence++;
	}
	return opence;
}

boolean network_host_rules_waits(
	short rule)
{
	return !network_host_rules_live_for(rule, host_rules_lobby(), network_standard_hosting(),
		host_rules_opence_machines());
}

short network_host_rules_rows(
	short *rows,
	short maximum)
{
	struct game_variant variant;
	struct game_variant_options options;
	short count = 0;
	short rule;

	if (!network_host_rules_available() || !host_rules_current(&variant, &options))
		return 0;
	for (rule = 0; rule < NUMBER_OF_HOST_RULES && count < maximum; rule++)
	{
		if ((rule == _host_rule_vehicles_blue && !variant.universal_variant.teams) ||
			((rule == _host_rule_primary || rule == _host_rule_secondary) && options.loadout != _loadout_custom) ||
			(rule == _host_rule_weapon_set && options.loadout == _loadout_custom))
		{
			continue;
		}
		rows[count++] = rule;
	}
	return count;
}

static wchar_t const *host_rules_vehicle_name(
	byte set)
{
	static wchar_t const *const names[HOST_RULES_VEHICLE_SETS] =
	{
		L"DEFAULT", L"NONE", L"WARTHOGS", L"GHOSTS", L"SCORPIONS", L"ROCKET WARTHOGS", L"BANSHEES", L"GUN TURRETS"
	};

	if (set == VARIANT_VEHICLE_SET_PC)
		return L"ALL THE MAP'S";
	if (set == VARIANT_VEHICLE_SET_CUSTOM)
		return L"CUSTOM";
	return set < HOST_RULES_VEHICLE_SETS ? names[set] : L"";
}

static wchar_t const *host_rules_weapon_name(
	byte weapon)
{
	static wchar_t const *const names[NUMBER_OF_LOADOUT_WEAPONS] =
	{
		L"NONE", L"RANDOM", L"ASSAULT RIFLE", L"PISTOL", L"SHOTGUN", L"SNIPER RIFLE", L"ROCKET LAUNCHER",
		L"PLASMA PISTOL", L"PLASMA RIFLE", L"NEEDLER"
	};

	return weapon < NUMBER_OF_LOADOUT_WEAPONS ? names[weapon] : L"";
}

static wchar_t const *host_rules_weapon_set_name(
	long set)
{
	static wchar_t const *const names[HOST_RULES_WEAPON_SETS] =
	{
		L"NORMAL", L"PISTOLS", L"RIFLES", L"PLASMA", L"SNIPERS", L"NO SNIPING", L"ROCKETS", L"SHOTGUNS",
		L"SHORT RANGE", L"HUMAN", L"NO GRENADES", L"COVENANT", L"CLASSIC", L"HEAVY"
	};

	return set >= 0 && set < HOST_RULES_WEAPON_SETS ? names[set] : L"";
}

void network_host_rules_row(
	short rule,
	wchar_t *label,
	wchar_t *value,
	wchar_t *help,
	short size)
{
	struct game_variant variant;
	struct game_variant_options options;
	boolean lobby = host_rules_lobby();
	unsigned long flag = host_rules_flag(rule);
	boolean waits = network_host_rules_waits(rule);
	unsigned long flags;

	label[0] = value[0] = help[0] = 0;
	if (!host_rules_current(&variant, &options))
		return;
	flags = variant.universal_variant.flags;
	/* (a change waiting for the next game shown as it will be) */
	if (flag & host_rules.waiting_mask)
		flags = (flags & ~flag) | (host_rules.waiting_flags & flag);
	switch (rule)
	{
	case _host_rule_vehicles:
	case _host_rule_vehicles_blue:
		usnprintf(label, size - 1, !variant.universal_variant.teams ? L"VEHICLES" :
			rule == _host_rule_vehicles ? L"RED VEHICLES" : L"BLUE VEHICLES");
		usnprintf(value, size - 1, L"%s", host_rules_vehicle_name(options.vehicle_set[rule == _host_rule_vehicles_blue]));
		usnprintf(help, size - 1, lobby ? L"The vehicles on the map, where it places them." :
			L"Now: the vehicles nobody rides are swapped at the map's vehicle spawns; a ridden one once it is "
			L"empty.");
		break;
	case _host_rule_weapons:
		usnprintf(label, size - 1, L"STARTING WEAPONS");
		usnprintf(value, size - 1, options.loadout == _loadout_custom ? L"CUSTOM" : L"MAP'S");
		usnprintf(help, size - 1, lobby ? L"The map's starting weapons, or a primary and secondary you pick." :
			L"The map's starting weapons, or a primary and secondary you pick: from each player's next spawn.");
		break;
	case _host_rule_primary:
	case _host_rule_secondary:
		usnprintf(label, size - 1, rule == _host_rule_primary ? L"PRIMARY WEAPON" : L"SECONDARY WEAPON");
		usnprintf(value, size - 1, L"%s", host_rules_weapon_name(rule == _host_rule_primary ? options.primary_weapon :
			options.secondary_weapon));
		usnprintf(help, size - 1, lobby ? L"What every player starts with." :
			L"What every player starts with, from their next spawn. Those alive keep theirs.");
		break;
	case _host_rule_weapon_set:
		usnprintf(label, size - 1, L"WEAPON SET");
		usnprintf(value, size - 1, L"%s", host_rules_weapon_set_name(variant.universal_variant.weapon_set));
		usnprintf(help, size - 1, lobby ? L"The weapons the map's spawns and the players' starts are swapped for." :
			L"The weapons the map's spawns and the players' starts are swapped for: those that spawn from now.");
		break;
	case _host_rule_shields:
	case _host_rule_motion_tracker:
	case _host_rule_infinite_grenades:
	case _host_rule_invisible:
	{
		boolean on = (flags & flag) != 0;

		/* (shields: the variant's flag says none) */
		if (rule == _host_rule_shields)
			on = !on;
		usnprintf(label, size - 1, rule == _host_rule_shields ? L"SHIELDS" : rule == _host_rule_motion_tracker ?
			L"MOTION TRACKER" : rule == _host_rule_infinite_grenades ? L"INFINITE GRENADES" : L"INVISIBLE PLAYERS");
		usnprintf(value, size - 1, waits && (flag & host_rules.waiting_mask) ? (on ? L"ON NEXT GAME" : L"OFF NEXT GAME") :
			on ? L"ON" : L"OFF");
		usnprintf(help, size - 1, !waits ? L"Every player's, at once." :
			network_standard_hosting() ? L"From the next game: OpenCE players' games can't change it during one." :
			L"From the next game: a Legacy game's machines can't change it during one.");
		break;
	}
	case _host_rule_save:
		usnprintf(label, size - 1, L"SAVE AS GAMETYPE...");
		usnprintf(help, size - 1, L"Type a name: these rules are saved as your gametype, for MAPS and the "
			L"playlist. Enter saves.");
		break;
	}
	label[size - 1] = value[size - 1] = help[size - 1] = 0;
}

/* the host's OpenH1 clients told the game's flags */
static void host_rules_send(
	unsigned long flags)
{
	struct
	{
		struct distributed_message_header header;
		struct host_rules_message rules;
	} message;
	long machines[MAXIMUM_HOST_RULES_MACHINES];
	short count = distributed_client_machines(machines, MAXIMUM_HOST_RULES_MACHINES);
	short index;

	csmemset(&message, 0, sizeof(message));
	message.rules.format = HOST_RULES_FORMAT;
	message.rules.flags = flags & HOST_RULES_CLIENT_FLAGS;
	for (index = 0; index < count; index++)
	{
		/* (each an OpenH1 machine of this protocol: the flags change in no
		game of OpenCE's machines) */
		if (network_standard_machine_is_openh1(machines[index]))
		{
			distributed_send_to_machine_reliably(machines[index], &message, _distributed_message_host_rules, 1,
				(word)sizeof(message));
		}
	}
}

/* the vehicles in the game, logged: each kind's count */
static void host_rules_log_vehicles(
	void)
{
	struct object_iterator iterator;
	long kinds[16];
	short counts[16];
	short kind_count = 0;
	short index;
	char text[512];
	long length = 0;

	object_iterator_new(&iterator, _object_mask_vehicle, 0);
	while (object_iterator_next(&iterator))
	{
		long definition_index = object_get(iterator.index)->definition_index;

		/* (those deleted, which the objects' update takes out) */
		if (TEST_FLAG(object_header_get(iterator.index)->flags, _object_header_being_deleted_bit))
			continue;

		for (index = 0; index < kind_count && kinds[index] != definition_index; index++)
		{
		}
		if (index == kind_count && kind_count < NUMBEROF(kinds))
		{
			kinds[kind_count] = definition_index;
			counts[kind_count++] = 0;
		}
		if (index < kind_count)
			counts[index]++;
	}
	text[0] = 0;
	for (index = 0; index < kind_count && length < (long)sizeof(text) - 1; index++)
	{
		char const *name = tag_get_name(kinds[index]);
		char const *slash = name ? strrchr(name, '\\') : NULL;

		length += snprintf(text + length, sizeof(text) - (size_t)length, "%s%d %s", index ? ", " : "",
			counts[index], slash ? slash + 1 : name ? name : "?");
	}
	error(_error_log, "host rules: the vehicles now: %s", kind_count ? text : "none");
}

/* the game under way's rules set (the host's and its record's), and what
follows: the vehicles swapped, the clients told */
static void host_rules_set_game(
	short rule,
	struct game_variant const *variant,
	struct game_variant_options const *options)
{
	struct network_game_server *server = global_network_game_server_get();
	struct network_game *game = network_game_server_get_game(server);

	*game_engine_get_variant() = *variant;
	game_set_game_variant_options(options);
	/* (a player who joins takes the record, and the next game starts from
	it) */
	game->variant = *variant;
	game->variant_options = *game_variant_options_get();
	if (rule == _host_rule_vehicles || rule == _host_rule_vehicles_blue)
	{
		short removed, placed, waiting;

		game_engine_vehicle_set_changed(&removed, &placed, &waiting);
		error(_error_log, "host rules: vehicles %d/%d: %d gone, %d placed, %d waiting for their pads",
			(int)options->vehicle_set[0], (int)options->vehicle_set[1], removed, placed, waiting);
		host_rules_log_vehicles();
	}
	else if (network_host_rules_client_side(rule))
	{
		host_rules_send(variant->universal_variant.flags);
		error(_error_log, "host rules: the flags are %lx, every OpenH1 client told",
			variant->universal_variant.flags & HOST_RULES_CLIENT_FLAGS);
	}
	else
	{
		error(_error_log, "host rules: rule %d changed, from the next spawns: loadout %d, weapons %d and %d, "
			"weapon set %ld", rule, (int)options->loadout, (int)options->primary_weapon, (int)options->secondary_weapon,
			variant->universal_variant.weapon_set);
	}
}

/* the rules changed: the lobby's record, else the game's */
static boolean host_rules_apply(
	short rule,
	struct game_variant *variant,
	struct game_variant_options *options)
{
	struct network_game_server *server = global_network_game_server_get();

	if (host_rules_lobby())
	{
		if (network_game_server_playing(server) || network_game_server_port_start_sent(server))
			return FALSE;
		player_ui_set_game_variant(variant);
		player_ui_set_game_variant_options(options);
		network_game_server_change_game_variant(server, variant);
		return TRUE;
	}
	host_rules_set_game(rule, variant, options);
	return TRUE;
}

boolean network_host_rules_step(
	short rule,
	short step)
{
	struct game_variant variant;
	struct game_variant_options options;
	unsigned long flag = host_rules_flag(rule);

	if (!network_host_rules_available() || !host_rules_current(&variant, &options))
		return FALSE;
	/* (a flag that waits: the next game's, as it will be) */
	if (network_host_rules_waits(rule))
	{
		unsigned long flags = (variant.universal_variant.flags & ~host_rules.waiting_mask) |
			(host_rules.waiting_flags & host_rules.waiting_mask);

		host_rules.waiting_mask |= flag;
		host_rules.waiting_flags = (host_rules.waiting_flags & ~flag) | ((flags ^ flag) & flag);
		error(_error_log, "host rules: rule %d waits for the next game", rule);
		return TRUE;
	}
	if (!network_host_rules_change(&variant, &options, rule, step, host_rules_custom_vehicles(&options)))
		return FALSE;
	return host_rules_apply(rule, &variant, &options);
}

boolean network_host_rules_save(
	char const *name,
	char *reason,
	long reason_size)
{
	struct game_variant variant;
	struct game_variant_options options;
	wchar_t wide[HOST_RULES_NAME_LENGTH + 1];
	long profile_index;
	short index;

	if (!network_host_rules_available() || !host_rules_current(&variant, &options))
	{
		snprintf(reason, (size_t)reason_size, "only the host of a game has its rules");
		return FALSE;
	}
	for (index = 0; index < HOST_RULES_NAME_LENGTH && name[index]; index++)
		wide[index] = (wchar_t)(byte)name[index];
	wide[index] = 0;
	while (index > 0 && wide[index - 1] == ' ')
		wide[--index] = 0;
	if (!index)
	{
		snprintf(reason, (size_t)reason_size, "give it a name");
		return FALSE;
	}
	/* (the flags that wait are the rules saved) */
	network_host_rules_apply_waiting(&variant, FALSE);
	csmemset(variant.human_readable_game_description, 0, sizeof(variant.human_readable_game_description));
	ustrncpy(variant.human_readable_game_description, wide, NUMBEROF(variant.human_readable_game_description) - 1);
	SET_FLAG(variant.flags, HOST_RULES_SYSTEM_DEFAULT_BIT, FALSE);
	profile_index = playlist_profile_new(0, wide);
	if (profile_index == NONE)
	{
		snprintf(reason, (size_t)reason_size, "it could not be saved: a gametype of that name, or too many");
		return FALSE;
	}
	playlist_profile_save_with_options(profile_index, &variant, &options);
	error(_error_log, "host rules: saved as the gametype %s", name);
	return TRUE;
}

void network_host_rules_apply_waiting(
	struct game_variant *variant,
	boolean taken)
{
	variant->universal_variant.flags = (variant->universal_variant.flags & ~host_rules.waiting_mask) |
		(host_rules.waiting_flags & host_rules.waiting_mask);
	if (taken)
		host_rules.waiting_mask = host_rules.waiting_flags = 0;
}

boolean network_host_rules_waiting(
	void)
{
	return host_rules.waiting_mask != 0;
}

void network_host_rules_update(
	void)
{
	if (!global_network_game_server_get())
		host_rules.waiting_mask = host_rules.waiting_flags = 0;
}

/* ---------- the console */

/* the text after the command word the expression starts with (else NULL) */
static char const *host_rules_word(
	char const *expression,
	char const *word)
{
	size_t length = strlen(word);

	while (*expression == ' ' || *expression == '\t' || *expression == '(')
		expression++;
	if (strncmp(expression, word, length) || (expression[length] && expression[length] != ' ' &&
		expression[length] != '\t' && expression[length] != ')'))
	{
		return NULL;
	}
	expression += length;
	while (*expression == ' ' || *expression == '\t' || *expression == '"')
		expression++;
	return expression;
}

/* the index of the word (up to a space, ")" or a quote) among names, else
NONE */
static short host_rules_named(
	char const *text,
	char const *const *names,
	short count)
{
	size_t length = strcspn(text, " \t)\"");
	short index;

	for (index = 0; index < count; index++)
	{
		if (length == strlen(names[index]) && !strncmp(text, names[index], length))
			return index;
	}
	return NONE;
}

boolean network_host_rules_console(
	char const *expression,
	boolean *result)
{
	static char const *const vehicles[HOST_RULES_VEHICLE_SETS + 1] =
	{
		"default", "none", "warthog", "ghost", "scorpion", "rocket_warthog", "banshee", "turret", "pc"
	};
	static char const *const weapons[NUMBER_OF_LOADOUT_WEAPONS + 1] =
	{
		"none", "random", "assault_rifle", "pistol", "shotgun", "sniper_rifle", "rocket_launcher", "plasma_pistol",
		"plasma_rifle", "needler", "map"
	};
	struct game_variant variant;
	struct game_variant_options options;
	char const *rest;

	if ((rest = host_rules_word(expression, "host_vehicles")) != NULL)
	{
		short set = host_rules_named(rest, vehicles, NUMBEROF(vehicles));
		char const *team = rest + strcspn(rest, " \t)\"");
		short side;

		while (*team == ' ' || *team == '\t')
			team++;
		side = !strncmp(team, "blue", 4) ? 1 : 0;
		*result = FALSE;
		if (set != NONE && network_host_rules_available() && host_rules_current(&variant, &options) &&
			(!side || variant.universal_variant.teams))
		{
			network_host_rules_set_vehicles(&variant, &options, side,
				set == HOST_RULES_VEHICLE_SETS ? VARIANT_VEHICLE_SET_PC : (byte)set);
			*result = host_rules_apply(side ? _host_rule_vehicles_blue : _host_rule_vehicles, &variant, &options);
		}
		if (!*result)
		{
			console_warning("host_vehicles: give default, none, warthog, ghost, scorpion, rocket_warthog, banshee, "
				"turret or pc (and red or blue, in a team game), as the host of a network game");
		}
		else
			console_printf(FALSE, "vehicles set");
		return TRUE;
	}
	if ((rest = host_rules_word(expression, "host_weapons")) != NULL)
	{
		short primary = host_rules_named(rest, weapons, NUMBEROF(weapons));
		char const *second = rest + strcspn(rest, " \t)\"");
		short secondary;

		while (*second == ' ' || *second == '\t')
			second++;
		secondary = *second && *second != ')' ? host_rules_named(second, weapons, NUMBER_OF_LOADOUT_WEAPONS) :
			_loadout_weapon_none;
		*result = FALSE;
		if (primary != NONE && secondary != NONE && network_host_rules_available() &&
			host_rules_current(&variant, &options))
		{
			/* ("map": the weapon set's, the map's own) */
			options.loadout = primary == NUMBER_OF_LOADOUT_WEAPONS ? _loadout_category : _loadout_custom;
			if (options.loadout == _loadout_custom)
			{
				options.primary_weapon = (byte)primary;
				options.secondary_weapon = (byte)secondary;
			}
			*result = host_rules_apply(_host_rule_weapons, &variant, &options);
		}
		if (!*result)
		{
			console_warning("host_weapons: give map, or a primary and secondary (none, random, assault_rifle, pistol, "
				"shotgun, sniper_rifle, rocket_launcher, plasma_pistol, plasma_rifle, needler), as the host");
		}
		else
			console_printf(FALSE, "starting weapons set");
		return TRUE;
	}
	return FALSE;
}

/* ---------- the message */

boolean network_host_rules_handles_message(
	word const *message,
	word size)
{
	struct distributed_message_header header;

	if (size < sizeof(header))
		return FALSE;
	csmemcpy(&header, message, sizeof(header));
	return header.type == _distributed_message_host_rules;
}

/* (the host's alone, over its stream, whole, in an OpenH1 game under way) */
void network_host_rules_handle_message(
	long machine_index,
	word const *message,
	word size,
	boolean from_stream)
{
	struct distributed_message_header header;
	struct host_rules_message rules;
	struct game_variant *variant;

	if (machine_index != NONE || !from_stream || game_connection() != _game_connection_network_client ||
		!network_standard_agreed_rate() || !game_in_progress() || !game_engine_running() ||
		size != sizeof(header) + sizeof(rules))
	{
		return;
	}
	csmemcpy(&header, message, sizeof(header));
	csmemcpy(&rules, (byte const *)message + sizeof(header), sizeof(rules));
	if (header.count != 1)
		return;
	if (rules.format != HOST_RULES_FORMAT)
	{
		if (!host_rules.other_format_noted)
			error(_error_log, "host rules: a message of another format (%d), not taken", rules.format);
		host_rules.other_format_noted = TRUE;
		return;
	}
	if (!network_host_rules_message_valid(&rules))
		return;
	variant = game_engine_get_variant();
	variant->universal_variant.flags = network_host_rules_client_flags(variant->universal_variant.flags, rules.flags);
	error(_error_log, "host rules: the host's flags are %lx", rules.flags);
}
