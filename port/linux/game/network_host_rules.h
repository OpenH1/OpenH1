/*
NETWORK_HOST_RULES.H

The gametype's rules a player host changes during a game (network_host_rules.c):
Host Controls' RULES (host_controls_menu.inc), the console's host_vehicles
and host_weapons.
*/

#ifndef __NETWORK_HOST_RULES_H
#define __NETWORK_HOST_RULES_H
#pragma once

/* ---------- constants */

/* RULES' rows */
enum
{
	/* the vehicles: free for all's, else red's, and blue's */
	_host_rule_vehicles,
	_host_rule_vehicles_blue,
	/* the starting weapons: the weapon set's (the map's), or each player's
	primary and secondary; the weapon set */
	_host_rule_weapons,
	_host_rule_primary,
	_host_rule_secondary,
	_host_rule_weapon_set,
	/* the variant's flags every machine draws by */
	_host_rule_shields,
	_host_rule_motion_tracker,
	_host_rule_infinite_grenades,
	_host_rule_invisible,
	/* the rules saved as a gametype (a name typed) */
	_host_rule_save,
	NUMBER_OF_HOST_RULES,
	/* (the weapon sets: game_engine.c's enum game_engine_weapons) */
	HOST_RULES_WEAPON_SETS = 14,
	/* the message's format: a machine takes its own alone */
	HOST_RULES_FORMAT = 1,
	/* a saved gametype's name, at most (the variant's description) */
	HOST_RULES_NAME_LENGTH = 11,
};

/* the variant's flags the message carries (game_engine.h's
_game_variant_*_bit: no shields, the motion sensor, infinite grenades,
invisible players) */
#define HOST_RULES_CLIENT_FLAGS 0x1DUL

/* ---------- structures */

struct game_variant;
struct game_variant_options;

/* on the wire, the host's to its OpenH1 clients, reliably: the variant's
flags of the game under way */
struct host_rules_message
{
	byte format;
	byte pad[3];
	unsigned long flags;
};

/* ---------- prototypes/NETWORK_HOST_RULES.C */

/* the pure parts (tools/harness/tests/test_host_rules.py): a team's vehicle
set stepped (default, none, Warthogs, Ghosts, Scorpions, Rocket Warthogs,
Banshees, Gun Turrets, all the map's; and custom, if the gametype's own
counts are kept: custom); a rule changed on the variant and options (step
-1 left, 1 right or A; FALSE if it cannot be); whether a rule changes the
game under way at once (the lobby's always; the host's alone, as the
vehicles and weapons are, always; the flags every machine draws by in an
OpenH1 game without OpenCE's machines, which alone hear the host's message),
else from the next game; a client's flags as the message has them; the
message whole */
byte network_host_rules_vehicle_step(byte set, short step, boolean custom);
/* a team's vehicle set set (free for all: both sides'), and the variant's
own (red's, if it is one of the Xbox's) */
void network_host_rules_set_vehicles(struct game_variant *variant, struct game_variant_options *options, short side,
	byte set);
boolean network_host_rules_change(struct game_variant *variant, struct game_variant_options *options, short rule,
	short step, boolean custom);
boolean network_host_rules_client_side(short rule);
boolean network_host_rules_live_for(short rule, boolean lobby, boolean standard, short opence_machines);
unsigned long network_host_rules_client_flags(unsigned long current, unsigned long sent);
boolean network_host_rules_message_valid(struct host_rules_message const *message);

/* whether this machine's player hosts a network game, in its lobby or
playing it, of a gametype (not co-op): RULES is its */
boolean network_host_rules_available(void);
/* RULES' rows (a team game's both vehicles; the custom weapons' primary
and secondary, else the weapon set), their texts, and a row changed:
FALSE (said in reason, if given) if it cannot be */
short network_host_rules_rows(short *rows, short maximum);
void network_host_rules_row(short rule, wchar_t *label, wchar_t *value, wchar_t *help, short size);
boolean network_host_rules_step(short rule, short step);
/* whether the row's change waits for the next game */
boolean network_host_rules_waits(short rule);
/* the rules saved as a gametype of that name (the host's profile's, as the
gametype editor saves them): FALSE and why */
boolean network_host_rules_save(char const *name, char *reason, long reason_size);
/* (network_host_game.c) the changes that wait set on a variant (the next
game's as its lobby takes it: taken, and forgotten); whether one waits */
void network_host_rules_apply_waiting(struct game_variant *variant, boolean taken);
boolean network_host_rules_waiting(void);
/* each frame (network_host_controls_update): what waits forgotten once the
server goes */
void network_host_rules_update(void);
/* the console's host_vehicles and host_weapons: TRUE if the expression was
one (its result) */
boolean network_host_rules_console(char const *expression, boolean *result);
/* the host's message (an OpenH1 game's: network_distributed.c) */
boolean network_host_rules_handles_message(word const *message, word size);
void network_host_rules_handle_message(long machine_index, word const *message, word size, boolean from_stream);

#endif // __NETWORK_HOST_RULES_H
