/*
BOTS.C

Computer players (bots) for the multiplayer games this computer hosts
(bots.count): the lobby of a Slayer game, local (split screen) or system link
or internet, takes as many as the setting asks for (network_server_manager.c
adds them, and in a network game makes room for the players who join:
bots.make_room), and each plays as a player does -
spawns, respawns, scores, is in the scores and the game's rules - with its
controls from here instead of a controller: the game tick takes every bot's
action from bots_update_actions (players_update_before_game), where a
player's comes from its pad. A bot has no view: nothing is drawn for it.

What a bot does each tick, in a player's terms:

- It looks for enemies (every third tick, the bots in turn): the nearest
  player of the other team (anyone in a game without teams) within its
  sight's reach and field of view whom a line of sight reaches (the AI's own
  sight test: the level, vehicles, scenery and machines block it), nearer
  for one in active camouflage; and it notices who shot it.
- It reacts after a moment (its skill's reaction time), then turns towards
  the target as a player's hand does (a spring: the less skilled flick past
  the target and come back), with an aim error that settles while it keeps
  the target in sight but never to nothing ("the aim's hand": a wander, its
  form's streaks, a flinch when hit, a target's change of motion taken in
  late), aims where its weapon is best (the sniper rifle and the pistol at
  the head once the shield is down, rockets and the fuel rod gun
  at the feet, the rest at the body; the less skilled go for the head less
  often), leads a moving target by its weapon's projectile speed, and fires
  in bursts while its aim is near the target (released between bursts, so
  semi-automatic weapons fire too). It plays as players do close up
  (bots_bot_fight_close): opens on a whole shield at middle range with a
  grenade now and then, goes in with a grenade and a blow, shoots the
  shield down with the pistol and strikes, and strikes at a blow's reach
  (the engine's). It strafes one way while its target's aim is off it and
  turns about once it reacts to the aim on it (a jump with it now and
  then), crouches at range while no one aims at it, and keeps its
  distance.
- Its shield down in a fight it cannot win soon, it backs off to cover
  (out of the enemy's sight, a health pack when hurt) to let the shield
  come back; badly hurt, it goes for a health pack.
- Otherwise it goes where an enemy was last seen, else about the map (the
  player starting locations, the weapon and item spawns), most often to the
  place nearest where an enemy it knows of was (bots_knowledge.h: what it
  and, a callout's delay later, its team saw; no motion tracker of others'
  places), now and then near a teammate; it goes for the power weapons and
  powerups, timing their spawns by the game's clock as a player does and
  believing an item is there as it last saw the spawn; now and then it
  picks up a better weapon lying near (the action button, as a player holds
  it), swaps to the weapon better for the range in a fight, and to its
  other weapon when this one is empty. With bots.tactics a team's bots
  also play together (bots_tactics.h).
- It walks the paths the campaign's AI finds: the engine's own pathfinding
  over the level's walkable surfaces (source/ai/path.c), which every stock
  multiplayer map carries (tool builds it with the structure BSP: Blood
  Gulch has 2807 walkable surfaces of 4916; only the AI's placements,
  encounters and firing positions are missing). A bot that stops moving
  jumps and sidesteps, then finds another path. The multiplayer maps were
  never made for the AI, and the bots' searches read their own copy of the
  walkable surfaces: those with no room above them (a floor going on under
  a wall: Longest's bases) closed a few a tick from the game's start, and
  those under scenery or a machine the path's obstacle avoidance once found
  no way past (Sidewinder's blast door between the bases) closed from then
  on. A goal on ground the walkable surfaces do not reach is gone to as near
  as they go and on with jumps; an item it finds no way to is left be a
  while. Bots in each other's way give way by their order.

Skill (bots.skill): easy, normal, heroic, legendary - how soon it reacts,
how far and how wide it sees, how large its aim error is, how fast that
settles and what it never settles below, how fast and how steadily its hand
turns and follows, how hard a hit knocks its aim, how tight its shots must
be before it fires. halo.log's minute line has the aim's tally: each weapon
family's rounds that struck of those fired.

The simulation: a bot decides from the game state at the tick and its own
random numbers (seeded per bot and game), and its path searches run in the
tick, so the bots do the same in two runs of the same input. Nothing of a
bot is in the game state: multiplayer games are never saved. Only the
machine that hosts the game runs the bots. In a system link or internet
game each bot is a player of a machine with no connection (bots.h), and
the host's tick takes its controls as it takes a remote player's input and
gives them to the clients with the others' (the relayed input:
update_server_take_bot_action), so a client replays a bot as any other
machine's player, and an OpenCE client sees an ordinary player: no message
of the network protocol changes. bots_update_actions does nothing on a
client. debug.bot_player has a bot of its own drive the first local player
too (the trailer's captures: the camera rides with a fighter). halo.log has
the map's preparation and each minute of play's bots line: the searches and how many failed, the times a bot stood stuck, shots
and each bot's kills and deaths; debug.bot_trace logs each bot's state.
*/

#include "cseries.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/game_allegiance.h"
#include "game/players.h"
#include "objects/objects.h"
#include "units/units.h"
#include "units/bipeds.h"
#include "items/items.h"
#include "items/weapons.h"
#include "items/weapon_definitions.h"
#include "items/projectile_definitions.h"
#include "physics/collisions.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "structures/structure_bsp_definitions.h"
#include "physics/collision_bsp_definitions.h"
#include "networking/network_game_globals.h"
#include "ai/path.h"
#include "devices/devices.h"
#include "devices/device_definitions.h"
#include "tag_files/tag_groups.h"
#include "tag_files/tag_files.h"
#include "cache/cache_files.h"

#include "bots.h"
#include "math/real_math.h"
#include "bots_knowledge.h"
#include "bots_tactics.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* source/game/player_queues_new.c */
void update_server_take_bot_action(long absolute_player_index, struct player_action const *action);

/* port/linux/src/port_config.c */
const char *config_string(char const *name);
long config_integer(char const *name);
int config_boolean(char const *name);
void platform_log(char const *format, ...);

/* ---------- constants */

#define BOTS_PI 3.14159265f
#define BOTS_DEGREES (BOTS_PI / 180.0f)
/* the most places a bot roams to (player starting locations, item spawns) */
#define BOTS_MAXIMUM_ROAM_POINTS 96
/* the path searches the bots make in one tick (each up to the engine's
1024 surfaces); a bot whose search waits asks again next tick */
#define BOTS_SEARCHES_A_TICK 3
/* the bots' size as the path searches know it (a biped's radius) */
#define BOTS_PATH_RADIUS 0.25f
/* how near a path's step a bot is when it has reached it (across) */
#define BOTS_STEP_REACHED 0.35f
/* a pathfinding surface's flag: walkable (path.c's) */
#define BOTS_PATHFINDING_WALKABLE_BIT 6
/* the items a bot remembers it found no way to, at once */
#define BOTS_UNREACHABLE_ITEMS 8

enum
{
	_bots_skill_easy = 0,
	_bots_skill_normal,
	_bots_skill_heroic,
	_bots_skill_legendary,
	NUMBER_OF_BOTS_SKILLS,
};

/* where a bot is going */
enum
{
	_bots_goal_none = 0,
	_bots_goal_enemy,
	_bots_goal_roam,
	/* a better weapon to pick up */
	_bots_goal_item,
	/* a vehicle to board (debug.bot_player_vehicle) */
	_bots_goal_vehicle,
	/* cover, to let its shield come back */
	_bots_goal_cover,
	/* a power item's spawn, to be there as it comes back */
	_bots_goal_power,
	/* where its team asks it to be (bots_tactics.h): after a teammate's
	killer, closing in, falling back, with a human, back to a teammate */
	_bots_goal_tactic,
};

/* a weapon's family, as the bots aim it (bots_weapon_kind) */
enum
{
	_bots_family_other = 0,
	_bots_family_assault_rifle,
	/* the pistol (not the plasma pistol) */
	_bots_family_pistol,
	_bots_family_sniper,
	/* the rocket launcher, the fuel rod gun: a blast */
	_bots_family_explosive,
	/* the plasma rifle and pistol, the needler */
	_bots_family_plasma,
	NUMBER_OF_BOTS_FAMILIES,
};

/* how far a player's blow reaches (world units from the head: the rays of
unit_cause_player_melee_damage), to the target's surface */
#define BOTS_MELEE_REACH 0.8f

/* the play a bot is in the middle of, close up (bots_bot_fight) */
enum
{
	_bots_combo_none = 0,
	/* a grenade, then shooting */
	_bots_combo_grenade_shoot,
	/* a grenade, then in for a blow */
	_bots_combo_grenade_blow,
	/* shooting the shield down (the pistol), then in for a blow; or in
	for a blow with a weapon that only strikes */
	_bots_combo_shoot_blow,
};

/* where on its target a bot aims (bots_aim_point_kind) */
enum
{
	_bots_aim_body = 0,
	_bots_aim_head,
	/* the ground at its feet (a blast's reach) */
	_bots_aim_feet,
};

/* ---------- structures */

struct bots_skill_definition
{
	char const *name;
	/* seconds from first seeing an enemy to firing */
	real reaction_seconds;
	/* how far it sees an enemy (world units), and how wide (degrees) */
	real sight_distance;
	real field_of_view_degrees;
	/* the aim error on taking a target (degrees), and the seconds it takes
	to settle to a third */
	real aim_error_degrees;
	real aim_settle_seconds;
	/* the most it turns a tick (degrees) */
	real turn_degrees_per_tick;
	/* its hand on the aim (bots_turn): a spring of this frequency (radians
	a second) and damping (1 settles on the mark; less flicks past it and
	comes back, as a less practised hand does) */
	real aim_frequency;
	real aim_damping;
	/* the error it never settles below: a slow wander of its aim of this
	size (degrees, about; scaled by its form and by its own moves), and the
	flinch of its aim when it is hit (degrees, for a hit that takes half
	of its health or its shield) */
	real aim_floor_degrees;
	real flinch_degrees;
	/* how much of a target's crossing of its view its hand follows, and the
	fastest it follows (radians a second): a faster target is trailed */
	real track_gain;
	real track_most;
	/* the chance a second the sniper rifle fires early, as the flick
	crosses the target */
	real early_shot_chance;
	/* fires while the aim is within this many times the target's size */
	real fire_cone;
	/* how much of a moving target's lead it takes (0 none, 1 all) */
	real lead;
	/* the chance it opens a fight with a grenade (at middle range, the
	target's shield up), and goes in with a grenade and a blow close up; the
	chance it jumps as it dodges */
	real grenade_chance;
	real jump_chance;
	/* the ticks of a burst, and of a pause between bursts */
	short burst_ticks;
	short pause_ticks;
	/* the chance it goes for the head with a weapon that wants it (the
	sniper rifle, the pistol, on a target whose shield is down), taken for
	each target and every two seconds (bots_aim_head_wanted) */
	real head_chance;
	/* aimed at, the seconds until it strafes the other way, and the chance
	it does (bots_strafe) */
	real dodge_reaction_seconds;
	real dodge_chance;
};

static struct bots_skill_definition const bots_skills[NUMBER_OF_BOTS_SKILLS] =
{
	{ "easy", 0.75f, 22.0f, 100.0f, 12.0f, 1.4f, 8.0f, 7.0f, 0.45f, 1.8f, 4.0f, 0.70f, 1.5f, 0.6f, 2.2f, 0.3f, 0.20f, 0.10f,
		6, 14, 0.35f, 0.60f, 0.40f },
	{ "normal", 0.45f, 32.0f, 120.0f, 7.0f, 0.9f, 13.0f, 9.0f, 0.6f, 1.1f, 3.0f, 0.80f, 2.2f, 0.4f, 1.8f, 0.7f, 0.40f, 0.25f,
		9, 9, 0.6f, 0.40f, 0.65f },
	{ "heroic", 0.30f, 42.0f, 140.0f, 4.0f, 0.6f, 20.0f, 15.0f, 0.65f, 0.5f, 1.8f, 0.88f, 3.0f, 0.15f, 1.5f, 0.8f, 0.60f, 0.40f,
		12, 6, 0.85f, 0.30f, 0.85f },
	{ "legendary", 0.18f, 55.0f, 160.0f, 2.2f, 0.35f, 30.0f, 20.0f, 1.0f, 0.25f, 1.2f, 0.95f, 4.0f, 0.05f, 1.25f, 1.0f, 0.75f,
		0.50f, 16, 4, 1.0f, 0.20f, 1.00f },
};

struct bots_bot
{
	long player_index;
	long unit_index;
	unsigned long random;

	/* where it looks (radians) */
	real yaw;
	real pitch;
	/* debug.bot_player's (the trailer's camera: its own weapon, vehicle and
	grenades, a steady hand) */
	boolean smooth;
	/* how fast its aim turns now (radians a second), carried from tick to
	tick: it turns as a player's hand does (bots_turn) */
	real yaw_rate;
	real pitch_rate;
	/* the direction to the target at its last aimed tick (radians), and that
	tick: how fast the target crosses the view, which the aim tracks */
	real target_yaw;
	real target_pitch;
	long target_aim_tick;

	/* its target */
	long target_player_index;
	long target_seen_tick;
	long target_first_seen_tick;
	real_point3d target_last_position;
	long target_last_surface_index;
	real aim_error_yaw;
	real aim_error_pitch;
	/* its aim's wander, which never settles: a point going round the mark
	(radians off it, across and up), at an angle (radians) that turns at a
	spin (radians a tick) that wanders, and at a distance (a part of the
	skill's floor) that wanders between 0.4 and 1.6; its form (its error's
	scale, about 1: good and bad stretches), changed once a second */
	real wander_yaw;
	real wander_pitch;
	real wander_angle;
	real wander_spin;
	real wander_radius;
	real form;
	/* the target's motion as the bot has taken it in (world units a tick);
	a change of it seen and not reacted to yet (its tick, NONE: none); how
	far the target has gone from where that motion had it, which the aim is
	off by until it reacts */
	real_vector3d seen_velocity;
	long motion_change_tick;
	real_vector3d motion_drift;
	/* its shield last tick (a shield popped: a flinch) */
	real last_shield;
	/* it goes for the head (bots_aim_head_wanted), until it next thinks of
	it */
	boolean head_wanted;
	long head_tick;
	long next_look_tick;
	/* the last time it was hurt, and its vitality then */
	real last_vitality;
	long hurt_tick;

	/* where it is going */
	short goal_kind;
	real_point3d goal_point;
	long goal_surface_index;
	long goal_tick;
	short roam_index;
	/* the roaming place is towards an enemy (looked at again sooner) */
	boolean hunting;
	/* the item it fetches (_bots_goal_item), and when it last pressed the
	action button for it; when it first pressed it for this item */
	long item_index;
	/* the vehicle it boards (_bots_goal_vehicle) */
	long vehicle_index;
	long item_press_tick;
	long item_first_press_tick;
	/* the path there (steps of the engine's path), and when it was found */
	struct path_result path;
	long path_tick;
	long path_failures;
	/* off a surface closed to the searches (beside a door): away from what
	closed it, until then */
	long escape_until_tick;
	real escape_direction;
	/* its path ends short of the goal (the search found no way there: a
	platform the walkable surfaces do not reach); at its end, straight on
	with jumps until then */
	boolean path_short;
	long climb_until_tick;
	long climb_start_tick;
	real climb_best_distance;
	/* where it was last tick (a teleporter's jump) */
	real_point3d last_position;
	/* an item it found no way to (on a ledge), not fetched again until then */
	long unreachable_item_indices[BOTS_UNREACHABLE_ITEMS];
	long unreachable_item_until_ticks[BOTS_UNREACHABLE_ITEMS];
	long unreachable_item_next;
	/* whether it moves, checked once a second */
	real_point3d stuck_position;
	long stuck_tick;
	short stuck_count;
	long unstick_until_tick;
	real unstick_direction;

	/* fighting: the side it strafes to and until when (no one aiming at
	it); since when its target aims at it (NONE: not), whether it dodges
	that and when next; bursts */
	real strafe_sign;
	long strafe_until_tick;
	long aimed_since_tick;
	boolean dodging;
	long dodge_tick;
	/* the play it is in close up (_bots_combo_*), until when, and not again
	before when; the target it opened on with a grenade */
	short combo;
	long combo_until_tick;
	long combo_next_tick;
	long opened_player_index;
	long burst_until_tick;
	long pause_until_tick;
	long grenade_tick;
	long melee_tick;
	long item_check_tick;

	/* the buttons held last tick (a press is a tick held, then released) */
	unsigned long previous_control_flags;

	/* (debug.bot_trace) its path searches */
	long searches;

	/* its number (bots_globals.bots', BOTS_MAXIMUM for debug.bot_player's);
	it fought last tick; its next look at every enemy and item spawn in its
	view (bots_bot_survey) */
	long index;
	boolean fighting;
	long survey_tick;
	/* looking about where it stays: the way it turns to, until when */
	real watch_yaw;
	long watch_tick;
	/* falling back to let its shield come back, until then (and not again
	before the next); where to */
	long recharge_until_tick;
	long recharge_next_tick;
	real_point3d cover_point;
	long cover_surface_index;
	/* the power item it goes for (its spawn, bots_knowledge_item), when it
	looked for one last */
	long power_item_index;
	long power_check_tick;
	/* when it last swapped to the weapon better for the range */
	long range_swap_tick;
	/* (_bots_goal_tactic) the team's point it goes to */
	real_point3d tactic_point;

	/* the aim's tally (bots_tally_shots): the weapon in its hands last tick,
	its family and when it last fired; its aim on the target's head as it
	pressed the trigger (the sniper's); when it last threw a grenade and
	struck a blow (the hits of those are not its gun's) */
	long tally_weapon_index;
	short tally_family;
	long tally_fired_tick;
	boolean tally_head;
	long throw_tick;
	long blow_tick;
};

/* ---------- globals */

static struct
{
	boolean map_ready;
	long roam_point_count;
	real_point3d roam_points[BOTS_MAXIMUM_ROAM_POINTS];
	long roam_surfaces[BOTS_MAXIMUM_ROAM_POINTS];
	struct bots_bot bots[BOTS_MAXIMUM];
	/* the first local player's, with debug.bot_player (its index
	BOTS_MAXIMUM: after the bots') */
	struct bots_bot player_bot;
	/* the skill setting, read once a tick for every bot */
	short skill;
	/* the path search's state (80 KB: the engine's 1024 nodes, heap and
	table), allocated once a game has bots; the searches made this tick */
	struct path_state *search;
	long tick_searches;

	/* the statistics of halo.log's line */
	long ticks;
	long searches;
	long search_failures;
	/* searches that stopped short of the goal (a path to as near as they
	came), and those that ran out of nodes */
	long search_partials;
	long search_overflows;
	/* paths taken without the obstacle avoidance, which found no way */
	long search_unavoided;
	/* the paths whose smoothing went nowhere, taken unsmoothed */
	long search_unsmoothed;
	/* the times a bot stood stuck while it walked, and the goals given up
	after three */
	long stuck_events;
	long stuck_giveups;
	long last_report_tick;
	/* the aim's tally, by weapon family: rounds fired, and the ticks a bot's
	rounds struck someone (the game's record of who hurt a unit); the
	sniper's rounds fired with the aim on the head; the tick the hits were
	last looked for */
	long aim_shots[NUMBER_OF_BOTS_FAMILIES];
	long aim_hits[NUMBER_OF_BOTS_FAMILIES];
	long aim_head_shots;
	long aim_scan_tick;
	/* the fights' habits: combos (_bots_combo_*), blows, dodges of an aim,
	jumps with them, crouches */
	long combos[4];
	long blows;
	long dodges;
	long dodge_jumps;
	long crouches;
	/* the bots' own choices: falling back to recharge, going for health,
	for a power item (and waiting for one to come back), roaming to a
	teammate */
	long recharges;
	long health_fetches;
	long power_fetches;
	long power_waits;
	long regroups;
	/* the power items the bots go for: the bot (its number plus one; 0 none)
	and until when, so that two of a team do not */
	long power_claims[BOTS_KNOWLEDGE_MAXIMUM_ITEMS];
	long power_claim_ticks[BOTS_KNOWLEDGE_MAXIMUM_ITEMS];
} bots_globals;

/* the walkable surfaces the bots' searches read: a copy of the structure's
pathfinding surfaces, less those learnt to be in the way (scenery and
machines) and those with no room above them */
static struct
{
	struct structure_bsp const *source;
	struct structure_bsp structure;
	byte *walkable;
	/* the objects whose surfaces are closed */
	long learned[32];
	long learned_count;
	long closed_count;
	/* the surfaces looked at for room above them, and those closed */
	long scan_index;
	boolean scan_done;
	long roofed_count;
	long scan_ticks;
} bots_paths;

/* ---------- settings */

/* a matchmaking game's bots (bots_set_matchmaking): how many, and the most
players and bots they fill it to; NONE: the settings' */
static struct
{
	long count;
	long fill_to;
} bots_matchmaking = { NONE, 0 };

void bots_set_matchmaking(long count, long fill_to)
{
	bots_matchmaking.count = count < 0 ? NONE : count;
	bots_matchmaking.fill_to = fill_to;
}

int bots_matchmaking_on(void)
{
	return bots_matchmaking.count != NONE;
}

long bots_wanted_count(void)
{
	long count = bots_matchmaking.count != NONE ? bots_matchmaking.count : config_integer("bots.count");

	return count < 0 ? 0 : count > BOTS_MAXIMUM ? BOTS_MAXIMUM : count;
}

short bots_skill(void)
{
	char const *name = config_string("bots.skill");
	short skill;

	for (skill = 0; skill < NUMBER_OF_BOTS_SKILLS; skill++)
	{
		if (!strcmp(name, bots_skills[skill].name))
			return skill;
	}
	return _bots_skill_normal;
}

short bots_teams(void)
{
	return strcmp(config_string("bots.teams"), "against") ? _bots_teams_even : _bots_teams_against;
}

int bots_make_room(void)
{
	return bots_matchmaking.count != NONE || config_boolean("bots.make_room") != 0;
}

long bots_fill_to(void)
{
	long count = bots_matchmaking.count != NONE ? bots_matchmaking.fill_to : config_integer("bots.fill_to");

	return count < 2 ? 2 : count > HALO_PORT_MAXIMUM_NETWORK_PLAYERS ? HALO_PORT_MAXIMUM_NETWORK_PLAYERS : count;
}

int bots_play_game_engine(long game_engine_index)
{
	return game_engine_index == game_engine_slayer;
}

int bots_machine_is_bot(long machine_index)
{
	return machine_index >= BOTS_FIRST_MACHINE && machine_index < BOTS_FIRST_MACHINE + BOTS_MAXIMUM;
}

int bots_player_is_bot(long player_index)
{
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;

	return player && player->local_player_index == NONE &&
		bots_machine_is_bot(player->network_player_data.machine_index);
}

void bots_network_player(long bot_index, struct network_player *player)
{
	/* (the Marines of Halo's campaign; eleven characters at most) */
	static char const *const names[BOTS_MAXIMUM] =
	{
		"Johnson", "Jenkins", "Mendoza", "Bisenti", "Stacker", "Polaski", "Chips", "Kojo",
		"Rawley", "Riley", "Wellsley", "Hall", "Fitzgerald", "Locklear", "Mitchell",
	};
	char const *name = names[bot_index % BOTS_MAXIMUM];
	long index;

	csmemset(player, 0, sizeof(*player));
	for (index = 0; index < (long)NUMBEROF(player->name) - 1 && name[index]; index++)
		player->name[index] = (wchar_t)name[index];
	player->name[index] = 0;
	player->primary_color_index = NONE;
	player->icon_index = 0;
	player->machine_index = (char)(BOTS_FIRST_MACHINE + bot_index);
	player->controller_index = 0;
	player->team_index = 0;
	player->player_list_index = NONE;
}

/* ---------- small helpers */

static unsigned long bots_random(struct bots_bot *bot)
{
	/* (xorshift32) */
	unsigned long x = bot->random ? bot->random : 0x9E3779B9UL;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	x &= 0xFFFFFFFFUL;
	bot->random = x;
	return x;
}

/* 0 to 1 */
static real bots_random_real(struct bots_bot *bot)
{
	return (real)(bots_random(bot) & 0xFFFFFF) / (real)0x1000000;
}

/* -1 to 1 */
static real bots_random_signed(struct bots_bot *bot)
{
	return bots_random_real(bot) * 2.0f - 1.0f;
}

static real bots_angle_difference(real a, real b)
{
	real difference = (real)fmod(a - b, 2.0f * BOTS_PI);

	if (difference > BOTS_PI)
		difference -= 2.0f * BOTS_PI;
	else if (difference < -BOTS_PI)
		difference += 2.0f * BOTS_PI;
	return difference;
}

static real bots_distance2d(real_point3d const *a, real_point3d const *b)
{
	real dx = a->x - b->x, dy = a->y - b->y;

	return (real)sqrt(dx * dx + dy * dy);
}

static real bots_distance3d(real_point3d const *a, real_point3d const *b)
{
	real dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;

	return (real)sqrt(dx * dx + dy * dy + dz * dz);
}

/* the surface of the level under a point (a step up included), and the
ground point there; NONE over nothing */
static long bots_surface_below(real_point3d const *point, real_point3d *ground)
{
	struct collision_result collision;
	real_point3d start = *point;
	real_vector3d down = { 0.0f, 0.0f, -6.0f };

	start.z += 0.4f;
	if (collision_test_vector(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit),
		&start, &down, NONE, &collision) && collision.type == _collision_result_structure)
	{
		if (ground)
			*ground = collision.point;
		return collision.surface_index;
	}
	return NONE;
}

/* the surface a unit stands on (a biped's support surface), and its point */
static long bots_unit_surface(long unit_index, real_point3d *point)
{
	struct object_datum *object = object_get(unit_index);
	struct biped_datum *biped = biped_try_and_get(unit_index);

	*point = object->object.position;
	if (object->object.parent_object_index != NONE)
		return NONE;
	if (biped && biped->biped.support_surface_index != NONE)
		return biped->biped.support_surface_index;
	return bots_surface_below(&object->object.position, point);
}

/* the unit's current weapon, NONE without one */
static long bots_current_weapon(struct unit_datum *unit)
{
	if (unit->unit.current_weapon_index < 0 || unit->unit.current_weapon_index >= MAXIMUM_WEAPONS_PER_UNIT)
		return NONE;
	return unit->unit.weapon_object_indices[unit->unit.current_weapon_index];
}

/* whether a weapon has no rounds left (one with magazines) */
static boolean bots_weapon_empty(long weapon_index)
{
	struct weapon_datum *weapon = weapon_get(weapon_index);
	struct weapon_definition *definition = weapon_definition_get(weapon->definition_index);

	if (definition->weapon.magazines.count <= 0)
		return FALSE;
	return weapon->weapon.magazines[0].rounds_loaded + weapon->weapon.magazines[0].rounds_total <= 0;
}

/* how full the weapon's magazine is, 0 to 1 (1 for one without) */
static real bots_weapon_loaded(long weapon_index)
{
	struct weapon_datum *weapon = weapon_get(weapon_index);
	struct weapon_definition *definition = weapon_definition_get(weapon->definition_index);
	struct weapon_magazine_definition *magazine;

	if (definition->weapon.magazines.count <= 0)
		return 1.0f;
	magazine = TAG_BLOCK_GET_ELEMENT(&definition->weapon.magazines, 0, struct weapon_magazine_definition);
	if (magazine->rounds_loaded_maximum <= 0)
		return 1.0f;
	return (real)weapon->weapon.magazines[0].rounds_loaded / (real)magazine->rounds_loaded_maximum;
}

/* the speed of the weapon's projectile (world units a tick; 0 for one
that hits at once or none) */
static real bots_projectile_speed_compute(long definition_index)
{
	struct weapon_definition *definition = weapon_definition_get(definition_index);

	if (definition->weapon.triggers.count > 0)
	{
		struct weapon_trigger_definition *trigger =
			TAG_BLOCK_GET_ELEMENT(&definition->weapon.triggers, 0, struct weapon_trigger_definition);

		if (trigger->projectile.index != NONE)
		{
			struct projectile_definition *projectile = projectile_definition_get(trigger->projectile.index);

			return 0.5f * (projectile->projectile.initial_velocity + projectile->projectile.final_velocity);
		}
	}
	return 0.0f;
}

/* how much a bot wants a weapon (the stock weapons, by their tags' names),
-1 for the game's own (a flag, a ball) */
static short bots_weapon_value_compute(long definition_index)
{
	static struct
	{
		char const *name;
		short value;
	} const values[] =
	{
		{ "rocket", 9 }, { "sniper", 8 }, { "shotgun", 8 }, { "fuel rod", 7 }, { "plasma_cannon", 7 },
		{ "plasma pistol", 3 }, { "pistol", 7 }, { "assault rifle", 6 }, { "needler", 5 }, { "plasma rifle", 5 },
		{ "flamethrower", 4 }, { "flag", -1 }, { "ball", -1 }, { "skull", -1 },
	};
	char const *name = tag_get_name(definition_index);
	long index;

	if (!name)
		return 4;
	for (index = 0; index < (long)NUMBEROF(values); index++)
	{
		if (strstr(name, values[index].name))
			return values[index].value;
	}
	return 4;
}

/* how a bot fires the weapon: the ticks between presses of a weapon fired a
press at a time (a latched trigger: the pistols, the sniper rifle, the
rocket launcher; or one that charges while held: the plasma pistol), 0 for
one held down in bursts */
static short bots_weapon_tap_ticks_compute(long definition_index)
{
	struct weapon_definition *definition = weapon_definition_get(definition_index);
	struct weapon_trigger_definition *trigger;
	real rate;

	if (definition->weapon.triggers.count <= 0)
		return 0;
	trigger = TAG_BLOCK_GET_ELEMENT(&definition->weapon.triggers, 0, struct weapon_trigger_definition);
	if (!TEST_FLAG(trigger->flags, _weapon_trigger_latched_bit) && trigger->charging_time <= 0.0f)
		return 0;
	rate = trigger->initial_rate_of_fire > 0.0f ? trigger->initial_rate_of_fire : 3.0f;
	return (short)PIN((long)((real)TICKS_PER_SECOND / rate + 0.5f), 2, TICKS_PER_SECOND * 2);
}

/* the distance a bot keeps from its enemy with the weapon */
static real bots_weapon_range_compute(long definition_index)
{
	char const *name = tag_get_name(definition_index);

	if (!name)
		return 6.0f;
	if (strstr(name, "shotgun") || strstr(name, "flamethrower"))
		return 2.0f;
	if (strstr(name, "sniper"))
		return 18.0f;
	if (strstr(name, "rocket") || strstr(name, "fuel rod") || strstr(name, "plasma_cannon"))
		return 12.0f;
	if (strstr(name, "plasma pistol") || strstr(name, "needler"))
		return 5.0f;
	if (strstr(name, "pistol"))
		return 9.0f;
	return 7.0f;
}

/* the weapon's family (by its tag's name) */
static short bots_weapon_family_compute(long definition_index)
{
	char const *name = tag_get_name(definition_index);

	if (!name)
		return _bots_family_other;
	if (strstr(name, "assault rifle"))
		return _bots_family_assault_rifle;
	if ((strstr(name, "plasma") && !strstr(name, "plasma_cannon")) || strstr(name, "needler"))
		return _bots_family_plasma;
	if (strstr(name, "pistol"))
		return _bots_family_pistol;
	if (strstr(name, "sniper"))
		return _bots_family_sniper;
	if (strstr(name, "rocket") || strstr(name, "fuel rod") || strstr(name, "plasma_cannon"))
		return _bots_family_explosive;
	return _bots_family_other;
}

/* what the bots know of a weapon definition (its value, its family, the distance kept
with it, how it fires, its projectile's speed), made once a map: the
functions above read only its tag, its name and its trigger, which a map
never changes (a bot asked them of its weapon every tick: a dozen name
searches and a few tag lookups) */
#define BOTS_WEAPON_KINDS 32
struct bots_weapon_kind
{
	long definition_index;
	short value;
	short tap_ticks;
	short family;
	/* a blow may be struck with it (not one whose tag prevents it), and it
	only strikes (it fires nothing: a flag, a ball) */
	boolean can_strike;
	boolean strikes_only;
	real range;
	real projectile_speed;
};

static struct
{
	long count;
	struct bots_weapon_kind kinds[BOTS_WEAPON_KINDS];
	/* (the one made when the table is full) */
	struct bots_weapon_kind spare;
} bots_weapon_kinds;

static struct bots_weapon_kind const *bots_weapon_kind(long definition_index)
{
	struct bots_weapon_kind *kind;
	long index;

	for (index = 0; index < bots_weapon_kinds.count; index++)
	{
		if (bots_weapon_kinds.kinds[index].definition_index == definition_index)
			return &bots_weapon_kinds.kinds[index];
	}
	kind = bots_weapon_kinds.count < BOTS_WEAPON_KINDS ? &bots_weapon_kinds.kinds[bots_weapon_kinds.count++] :
		&bots_weapon_kinds.spare;
	kind->definition_index = definition_index;
	kind->value = bots_weapon_value_compute(definition_index);
	kind->tap_ticks = bots_weapon_tap_ticks_compute(definition_index);
	kind->family = bots_weapon_family_compute(definition_index);
	{
		struct weapon_definition *definition = weapon_definition_get(definition_index);
		struct weapon_trigger_definition *trigger = definition->weapon.triggers.count > 0 ?
			TAG_BLOCK_GET_ELEMENT(&definition->weapon.triggers, 0, struct weapon_trigger_definition) : NULL;

		kind->can_strike = !TEST_FLAG(definition->weapon.flags, _weapon_prevents_melee_attack_bit);
		kind->strikes_only = kind->can_strike && (!trigger || trigger->projectile.index == NONE);
	}
	kind->range = bots_weapon_range_compute(definition_index);
	kind->projectile_speed = bots_projectile_speed_compute(definition_index);
	return kind;
}

static short bots_weapon_value(long definition_index)
{
	return bots_weapon_kind(definition_index)->value;
}

static real bots_projectile_speed(long weapon_index)
{
	return bots_weapon_kind(weapon_get(weapon_index)->definition_index)->projectile_speed;
}

static short bots_weapon_tap_ticks(long weapon_index)
{
	return bots_weapon_kind(weapon_get(weapon_index)->definition_index)->tap_ticks;
}

static real bots_weapon_range(long weapon_index)
{
	return weapon_index != NONE ? bots_weapon_kind(weapon_get(weapon_index)->definition_index)->range : 6.0f;
}

/* the weapon the bot keeps (part of its tag's name), "" for none:
debug.bot_player_weapon's for the first player's bot, debug.bots_weapon's
for the others */
static char const *bots_weapon_wanted(struct bots_bot const *bot)
{
	return config_string(bot->smooth ? "debug.bot_player_weapon" : "debug.bots_weapon");
}

/* whether the bot found no way to the item a while ago (left be thirty
seconds), and the item so remembered (the oldest of those kept forgotten) */
static boolean bots_item_unreachable(struct bots_bot const *bot, long item_index)
{
	long index;

	for (index = 0; index < BOTS_UNREACHABLE_ITEMS; index++)
	{
		if (bot->unreachable_item_indices[index] == item_index && game_time_get() < bot->unreachable_item_until_ticks[index])
			return TRUE;
	}
	return FALSE;
}

static void bots_item_set_unreachable(struct bots_bot *bot, long item_index)
{
	long slot;

	/* (one remembered already: kept longer) */
	for (slot = 0; slot < BOTS_UNREACHABLE_ITEMS; slot++)
	{
		if (bot->unreachable_item_indices[slot] == item_index && game_time_get() < bot->unreachable_item_until_ticks[slot])
		{
			bot->unreachable_item_until_ticks[slot] = game_time_get() + TICKS_PER_SECOND * 30;
			return;
		}
	}
	slot = bot->unreachable_item_next;
	bot->unreachable_item_indices[slot] = item_index;
	bot->unreachable_item_until_ticks[slot] = game_time_get() + TICKS_PER_SECOND * 30;
	bot->unreachable_item_next = (slot + 1) % BOTS_UNREACHABLE_ITEMS;
}

/* a weapon lying near the bot worth fetching: one better than the one it
holds; NONE for none */
static long bots_find_item(struct bots_bot *bot, struct unit_datum *unit, real_point3d const *position,
	real maximum_distance)
{
	struct object_iterator iterator;
	long best_index = NONE;
	real best_score = REAL_MAX;
	long current = bots_current_weapon(unit);
	short current_value = current != NONE ? bots_weapon_value(weapon_get(current)->definition_index) : 0;
	/* (debug.bot_player_weapon: the first player's bot goes for that weapon
	wherever it lies, and keeps it; debug.bots_weapon every bot) */
	char const *preferred = bots_weapon_wanted(bot);
	char const *current_name = current != NONE ? tag_get_name(weapon_get(current)->definition_index) : NULL;

	short slot;

	if (*preferred && current_name && strstr(current_name, preferred))
		current_value += 20;
	/* (holding it, in either hand: no other; walking over ammunition
	takes it) */
	for (slot = 0; *preferred && slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long held = unit->unit.weapon_object_indices[slot];
		char const *held_name = held != NONE ? tag_get_name(weapon_get(held)->definition_index) : NULL;

		if (held_name && strstr(held_name, preferred))
			return NONE;
	}

	object_iterator_new(&iterator, _object_mask_weapon, 0);
	while (object_iterator_next(&iterator))
	{
		struct item_datum *item = item_get(iterator.index);
		real distance, score;
		short value;
		short slot;
		boolean held = FALSE;

		if (item->object.parent_object_index != NONE ||
			bots_item_unreachable(bot, iterator.index) ||
			!TEST_FLAG(item->object.flags, _object_connected_to_map_bit) ||
			TEST_FLAG(item->item.flags, _item_attached_to_unit_bit))
		{
			continue;
		}
		distance = bots_distance3d(&item->object.position, position);
		value = bots_weapon_value(item->definition_index);
		if (*preferred && tag_get_name(item->definition_index) && strstr(tag_get_name(item->definition_index), preferred))
			value += 20;
		else if (distance > maximum_distance)
			continue;
		/* (one it holds already: only its ammunition, which walking over
		takes) */
		for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
		{
			long other = unit->unit.weapon_object_indices[slot];

			if (other != NONE && weapon_get(other)->definition_index == item->definition_index)
				held = TRUE;
		}
		if (held || value <= current_value)
			continue;
		score = distance - (real)(value - current_value) * 3.0f;
		if (score < best_score)
		{
			best_score = score;
			best_index = iterator.index;
		}
	}
	return best_index;
}

/* ---------- the map */

static void bots_add_roam_point(real_point3d const *point)
{
	real_point3d ground;
	long surface_index;

	if (bots_globals.roam_point_count >= BOTS_MAXIMUM_ROAM_POINTS)
		return;
	surface_index = bots_surface_below(point, &ground);
	if (surface_index == NONE)
		return;
	bots_globals.roam_points[bots_globals.roam_point_count] = ground;
	bots_globals.roam_surfaces[bots_globals.roam_point_count] = surface_index;
	bots_globals.roam_point_count++;
}

/* the bots' walkable surfaces let go */
static void bots_paths_release(void)
{
	if (bots_paths.walkable)
		free(bots_paths.walkable);
	csmemset(&bots_paths, 0, sizeof(bots_paths));
}

/* the surface's corners (at most BOTS_SURFACE_CORNERS), from its ring of
edges; 0 for one not the bsp's */
#define BOTS_SURFACE_CORNERS 16
static long bots_surface_corners(struct collision_bsp const *bsp, long surface_index, real_point3d *corners)
{
	struct collision_surface const *surface;
	long edge_index, count = 0;

	if (surface_index < 0 || surface_index >= bsp->surfaces.count)
		return 0;
	surface = TAG_BLOCK_GET_ELEMENT(&bsp->surfaces, surface_index, struct collision_surface);
	edge_index = surface->first_edge_index;
	do
	{
		struct collision_edge const *edge;
		boolean right;

		if (edge_index < 0 || edge_index >= bsp->edges.count)
			return 0;
		edge = TAG_BLOCK_GET_ELEMENT(&bsp->edges, edge_index, struct collision_edge);
		right = edge->surface_indices[1] == surface_index;
		if (!VALID_INDEX(edge->vertex_indices[right], bsp->vertices.count))
			return 0;
		corners[count++] = TAG_BLOCK_GET_ELEMENT(&bsp->vertices, edge->vertex_indices[right], struct collision_vertex)->point;
		edge_index = edge->edge_indices[right];
	} while (edge_index != surface->first_edge_index && count < BOTS_SURFACE_CORNERS);
	return count;
}

/* whether the object stands across the surface: a line over it at a biped's
knees or chest, from its middle to a little past an edge's, meets it, or
its middle is under it */
static boolean bots_surface_blocked(long object_index, real_point3d const *corners, long corner_count,
	real_point3d const *middle)
{
	static real const heights[] = { 0.35f, 1.0f };
	unsigned long flags = FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_back_facing_surfaces_bit) |
		FLAG(_collision_test_objects_bit) | FLAG(_collision_test_objects_scenery_bit) |
		FLAG(_collision_test_objects_machines_bit);
	struct collision_result collision;
	real_point3d start = *middle;
	real_vector3d up = { 0.0f, 0.0f, 1.6f };
	long height, corner;

	start.z += 0.05f;
	if (collision_test_vector(flags, &start, &up, NONE, &collision) && collision.type == _collision_result_object &&
		collision.object_index == object_index)
	{
		return TRUE;
	}
	for (height = 0; height < (long)NUMBEROF(heights); height++)
	{
		start.z = middle->z + heights[height];
		for (corner = 0; corner < corner_count; corner++)
		{
			real_point3d const *a = &corners[corner], *b = &corners[(corner + 1) % corner_count];
			real_vector3d vector;
			real length, past;

			vector.i = 0.5f * (a->x + b->x) - start.x;
			vector.j = 0.5f * (a->y + b->y) - start.y;
			vector.k = 0.5f * (a->z + b->z) + heights[height] - start.z;
			/* (on a little past the edge: a door may stand on it) */
			length = (real)sqrt(vector.i * vector.i + vector.j * vector.j + vector.k * vector.k);
			if (length < 0.01f)
				continue;
			past = (length + 0.4f) / length;
			vector.i *= past;
			vector.j *= past;
			vector.k *= past;
			if (collision_test_vector(flags, &start, &vector, NONE, &collision) &&
				collision.type == _collision_result_object && collision.object_index == object_index)
			{
				return TRUE;
			}
		}
	}
	return FALSE;
}

/* the bots' copy of the structure's walkable surfaces, made (the searches
take it from then on); FALSE without one */
static boolean bots_paths_copy(void)
{
	struct structure_bsp *structure = global_structure_bsp_get();
	long count;

	if (!structure || structure->pathfinding_surfaces.count <= 0 || structure->collision_bsp.count <= 0 ||
		(bots_paths.source && bots_paths.source != structure))
	{
		return FALSE;
	}
	if (bots_paths.walkable)
		return TRUE;
	count = structure->pathfinding_surfaces.count;
	bots_paths.walkable = (byte *)malloc((size_t)count);
	if (!bots_paths.walkable)
		return FALSE;
	csmemcpy(bots_paths.walkable, structure->pathfinding_surfaces.address, (size_t)count);
	bots_paths.structure = *structure;
	bots_paths.structure.pathfinding_surfaces.address = bots_paths.walkable;
	bots_paths.source = structure;
	return TRUE;
}

/* a walkable surface of the bots' copy: its corners, its middle and how far
its corners are from it; FALSE for one not walkable (or not the bsp's) */
static boolean bots_paths_walkable_surface(struct collision_bsp const *bsp, long surface_index,
	real_point3d *corners, long *corner_count, real_point3d *middle, real *reach)
{
	long corner;

	if (surface_index >= bots_paths.structure.pathfinding_surfaces.count ||
		!TEST_FLAG(bots_paths.walkable[surface_index], BOTS_PATHFINDING_WALKABLE_BIT))
	{
		return FALSE;
	}
	*corner_count = bots_surface_corners(bsp, surface_index, corners);
	if (*corner_count < 3)
		return FALSE;
	middle->x = middle->y = middle->z = 0.0f;
	for (corner = 0; corner < *corner_count; corner++)
	{
		middle->x += corners[corner].x / (real)*corner_count;
		middle->y += corners[corner].y / (real)*corner_count;
		middle->z += corners[corner].z / (real)*corner_count;
	}
	*reach = 0.0f;
	for (corner = 0; corner < *corner_count; corner++)
		*reach = MAX(*reach, bots_distance3d(middle, &corners[corner]));
	return TRUE;
}

/* the walkable surfaces an object stands across closed to the bots' searches */
static void bots_paths_close_object(long object_index, char const *why)
{
	struct object_datum *object = object_get(object_index);
	struct collision_bsp const *bsp;
	long surface_index, closed = 0;

	if (bots_paths.learned_count >= (long)NUMBEROF(bots_paths.learned) || !bots_paths_copy())
		return;
	bots_paths.learned[bots_paths.learned_count++] = object_index;
	bsp = TAG_BLOCK_GET_ELEMENT(&bots_paths.structure.collision_bsp, 0, struct collision_bsp);
	for (surface_index = 0; surface_index < bsp->surfaces.count; surface_index++)
	{
		real_point3d corners[BOTS_SURFACE_CORNERS], middle;
		real reach;
		long corner_count;

		if (bots_paths_walkable_surface(bsp, surface_index, corners, &corner_count, &middle, &reach) &&
			bots_distance3d(&middle, &object->object.bounding_sphere_center) <
				object->object.bounding_sphere_radius + reach + 0.5f &&
			bots_surface_blocked(object_index, corners, corner_count, &middle))
		{
			bots_paths.walkable[surface_index] &= (byte)~FLAG(BOTS_PATHFINDING_WALKABLE_BIT);
			closed++;
		}
	}
	bots_paths.closed_count += closed;
	platform_log("bots: %s %s at (%.1f %.1f %.1f): %ld walkable surfaces closed", why,
		tag_get_name(object->definition_index), object->object.bounding_sphere_center.x,
		object->object.bounding_sphere_center.y, object->object.bounding_sphere_center.z, closed);
}

/* the walkable surfaces with no room above them closed, a few a tick from
the game's start: a floor that goes on under a wall (Longest's bases: the
floor's strips under the walls joined the rooms either side, and a team's
bots walked into the wall a whole game). A surface is closed whose middle,
or most of the points halfway from it to its corners, have the level less
than half a biped's height above them */
#define BOTS_PATHS_SCAN_A_TICK 32
static void bots_paths_scan(void)
{
	struct collision_bsp const *bsp;
	long scanned;

	if (bots_paths.scan_done || !bots_paths_copy())
		return;
	bots_paths.scan_ticks++;
	bsp = TAG_BLOCK_GET_ELEMENT(&bots_paths.structure.collision_bsp, 0, struct collision_bsp);
	for (scanned = 0; scanned < BOTS_PATHS_SCAN_A_TICK; scanned++)
	{
		long surface_index = bots_paths.scan_index++;
		real_point3d corners[BOTS_SURFACE_CORNERS], middle;
		real reach;
		long corner_count, sample, covered = 0;

		if (surface_index >= bsp->surfaces.count || surface_index >= bots_paths.structure.pathfinding_surfaces.count)
		{
			bots_paths.scan_done = TRUE;
			platform_log("bots: %ld walkable surfaces with no room above closed (%ld ticks, %d surfaces a tick)",
				bots_paths.roofed_count, bots_paths.scan_ticks, BOTS_PATHS_SCAN_A_TICK);
			return;
		}
		if (!bots_paths_walkable_surface(bsp, surface_index, corners, &corner_count, &middle, &reach))
			continue;
		/* (the middle first: covered, the surface is; else most of the
		points between it and the corners) */
		for (sample = -1; sample < corner_count; sample++)
		{
			unsigned long flags = FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit) |
				FLAG(_collision_test_back_facing_surfaces_bit);
			real_vector3d up = { 0.0f, 0.0f, 0.35f };
			struct collision_result collision;
			real_point3d point = middle;

			if (sample >= 0)
			{
				point.x = 0.5f * (middle.x + corners[sample].x);
				point.y = 0.5f * (middle.y + corners[sample].y);
				point.z = 0.5f * (middle.z + corners[sample].z);
			}
			point.z += 0.05f;
			if (collision_test_vector(flags, &point, &up, NONE, &collision))
			{
				if (sample < 0)
				{
					covered = corner_count;
					break;
				}
				covered++;
			}
		}
		if (covered * 2 > corner_count)
		{
			bots_paths.walkable[surface_index] &= (byte)~FLAG(BOTS_PATHFINDING_WALKABLE_BIT);
			bots_paths.roofed_count++;
		}
	}
}

static boolean bots_paths_learned(long object_index)
{
	long index;

	for (index = 0; index < bots_paths.learned_count; index++)
	{
		if (bots_paths.learned[index] == object_index)
			return TRUE;
	}
	return FALSE;
}

/* whether the point is on or in the object (by its bounding sphere) */
static boolean bots_object_holds(struct object_datum *object, real_point3d const *point)
{
	return bots_distance2d(&object->object.bounding_sphere_center, point) < object->object.bounding_sphere_radius + 0.4f &&
		fabs(object->object.bounding_sphere_center.z - point->z) < object->object.bounding_sphere_radius + 1.0f;
}

/* the scenery or machine (with a collision model) the point is on or in,
the nearest; NONE for none */
static long bots_object_at(real_point3d const *point)
{
	struct object_iterator iterator;
	long best_index = NONE;
	real best_distance = REAL_MAX;

	object_iterator_new(&iterator, _object_mask_scenery | _object_mask_machine, 0);
	while (object_iterator_next(&iterator))
	{
		struct object_datum *object = object_get(iterator.index);
		real distance;

		if (object_definition_get(object->definition_index)->object.collision_model.index == NONE ||
			!bots_object_holds(object, point))
		{
			continue;
		}
		distance = bots_distance3d(&object->object.bounding_sphere_center, point);
		if (distance < best_distance)
		{
			best_distance = distance;
			best_index = iterator.index;
		}
	}
	return best_index;
}

/* whether one of the map's game places (a flag's stand, a teleporter, a
hill's corner, a ball's spawn) is on or in the object: never in the way */
static boolean bots_object_marks_place(struct object_datum *object)
{
	struct scenario *scenario = global_scenario_get();
	long index;

	for (index = 0; scenario && index < scenario->netgame_flags.count; index++)
	{
		struct scenario_netgame_flag *flag =
			TAG_BLOCK_GET_ELEMENT(&scenario->netgame_flags, index, struct scenario_netgame_flag);

		if (bots_object_holds(object, &flag->position))
			return TRUE;
	}
	return FALSE;
}

/* whether the point stands on the object (the level or another object
under it first) */
static boolean bots_object_supports(long object_index, real_point3d const *point)
{
	struct collision_result collision;
	real_point3d start = *point;
	real_vector3d down = { 0.0f, 0.0f, -1.2f };

	start.z += 0.4f;
	return collision_test_vector(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit) |
		FLAG(_collision_test_objects_bit) | FLAG(_collision_test_objects_scenery_bit) |
		FLAG(_collision_test_objects_machines_bit), &start, &down, NONE, &collision) &&
		collision.type == _collision_result_object && collision.object_index == object_index;
}

/* a path's obstacle avoidance found no way past where the bot is: the
scenery and machines about it (not the bipeds and vehicles, which move) are
in the way, their surfaces closed to the searches from now on (Sidewinder's
blast door between the bases is scenery: the search, over the structure
alone, goes through it, the shortest way, and the avoidance then finds no
way past; more than half of the searches across that map failed so). A
rock or a tree the avoidance goes around is never in the way. How many
were learnt */
static long bots_paths_learn(real_point3d const *position, real_point3d const *goal)
{
	struct object_iterator iterator;
	long learned = 0;

	object_iterator_new(&iterator, _object_mask_scenery | _object_mask_machine, 0);
	while (object_iterator_next(&iterator))
	{
		struct object_datum *object = object_get(iterator.index);
		struct object_definition *definition = object_definition_get(object->definition_index);

		/* (not what the bot or its goal stands on or in: a flag's stand, a
		teleporter's base; what the bot stands beside, within its bounding
		sphere but on the level, is learnt: a bot pressed against
		Sidewinder's blast door before it was learnt never learnt it) */
		if (definition->object.collision_model.index == NONE ||
			bots_distance2d(&object->object.bounding_sphere_center, position) > object->object.bounding_sphere_radius + 4.5f ||
			fabs(object->object.bounding_sphere_center.z - position->z) > object->object.bounding_sphere_radius + 2.0f ||
			(bots_object_holds(object, position) && bots_object_supports(iterator.index, position)) ||
			bots_object_holds(object, goal) ||
			bots_paths_learned(iterator.index) || bots_object_marks_place(object))
		{
			continue;
		}
		bots_paths_close_object(iterator.index, "in the way:");
		learned++;
	}
	return learned;
}

/* the way off a closed surface: away from the nearest of what closed it */
static boolean bots_paths_escape_direction(real_point3d const *position, real *direction)
{
	long index, nearest = NONE;
	real nearest_distance = REAL_MAX;

	for (index = 0; index < bots_paths.learned_count; index++)
	{
		struct object_datum *object = object_try_and_get(bots_paths.learned[index]);
		real distance;

		if (!object)
			continue;
		distance = bots_distance2d(&object->object.bounding_sphere_center, position);
		if (distance < nearest_distance && distance < object->object.bounding_sphere_radius + 4.0f)
		{
			nearest_distance = distance;
			nearest = index;
		}
	}
	if (nearest == NONE)
		return FALSE;
	{
		struct object_datum *object = object_get(bots_paths.learned[nearest]);

		*direction = (real)atan2(position->y - object->object.bounding_sphere_center.y,
			position->x - object->object.bounding_sphere_center.x);
	}
	return TRUE;
}

/* the bots' searches' walkable surfaces, the structure's to begin with,
less those of a machine its definition calls a pathfinding obstacle (not an
elevator, nor a door open now that is none open) */
static void bots_prepare_paths(void)
{
	struct object_iterator iterator;

	bots_paths_release();
	object_iterator_new(&iterator, _object_mask_machine, 0);
	while (object_iterator_next(&iterator))
	{
		struct object_datum *object = object_get(iterator.index);
		struct machine_definition *definition = machine_definition_get(object->definition_index);

		if (!TEST_FLAG(definition->machine.flags, _machine_is_pathfinding_obstacle_bit) ||
			TEST_FLAG(definition->machine.flags, _machine_is_elevator_bit) ||
			(device_get_position(iterator.index) > 0.5f &&
				TEST_FLAG(definition->machine.flags, _machine_is_not_pathfinding_obstacle_when_open_bit)))
		{
			continue;
		}
		bots_paths_close_object(iterator.index, "a machine in the way:");
	}
}

/* the places a bot roams to: the item spawns and the player starting
locations (once the level's BSP is in) */
static void bots_prepare_map(void)
{
	struct scenario *scenario = global_scenario_get();
	long index;

	bots_globals.map_ready = TRUE;
	bots_globals.roam_point_count = 0;
	bots_prepare_paths();
	if (!scenario)
		return;
	for (index = 0; index < scenario->netgame_equipment.count; index++)
	{
		struct scenario_netgame_equipment *equipment =
			TAG_BLOCK_GET_ELEMENT(&scenario->netgame_equipment, index, struct scenario_netgame_equipment);

		bots_add_roam_point(&equipment->position);
	}
	for (index = 0; index < scenario->players.count; index += 2)
	{
		struct player_starting_location *location =
			TAG_BLOCK_GET_ELEMENT(&scenario->players, index, struct player_starting_location);

		bots_add_roam_point(&location->position);
	}
	{
		real_point3d low = { REAL_MAX, REAL_MAX, REAL_MAX }, high = { -REAL_MAX, -REAL_MAX, -REAL_MAX };

		for (index = 0; index < bots_globals.roam_point_count; index++)
		{
			low.x = MIN(low.x, bots_globals.roam_points[index].x);
			low.y = MIN(low.y, bots_globals.roam_points[index].y);
			high.x = MAX(high.x, bots_globals.roam_points[index].x);
			high.y = MAX(high.y, bots_globals.roam_points[index].y);
		}
		platform_log("bots: %ld places to roam on this map, over %.0f units", bots_globals.roam_point_count,
			bots_globals.roam_point_count > 0 ? (double)bots_distance2d(&low, &high) : 0.0);
	}
}

void bots_initialize_for_new_map(void)
{
	long index;

	csmemset(bots_globals.bots, 0, sizeof(bots_globals.bots));
	for (index = 0; index < BOTS_MAXIMUM; index++)
		bots_globals.bots[index].player_index = NONE;
	csmemset(&bots_globals.player_bot, 0, sizeof(bots_globals.player_bot));
	bots_globals.player_bot.player_index = NONE;
	bots_globals.map_ready = FALSE;
	bots_globals.roam_point_count = 0;
	bots_globals.ticks = 0;
	bots_globals.searches = 0;
	bots_globals.search_failures = 0;
	bots_globals.search_partials = 0;
	bots_globals.search_overflows = 0;
	bots_globals.search_unavoided = 0;
	bots_globals.search_unsmoothed = 0;
	bots_globals.stuck_events = 0;
	bots_globals.stuck_giveups = 0;
	csmemset(bots_globals.aim_shots, 0, sizeof(bots_globals.aim_shots));
	csmemset(bots_globals.aim_hits, 0, sizeof(bots_globals.aim_hits));
	bots_globals.aim_head_shots = 0;
	bots_globals.aim_scan_tick = 0;
	csmemset(bots_globals.combos, 0, sizeof(bots_globals.combos));
	bots_globals.blows = 0;
	bots_globals.dodges = 0;
	bots_globals.dodge_jumps = 0;
	bots_globals.crouches = 0;
	bots_globals.recharges = 0;
	bots_globals.health_fetches = 0;
	bots_globals.power_fetches = 0;
	bots_globals.power_waits = 0;
	bots_globals.regroups = 0;
	csmemset(bots_globals.power_claims, 0, sizeof(bots_globals.power_claims));
	csmemset(bots_globals.power_claim_ticks, 0, sizeof(bots_globals.power_claim_ticks));
	/* (what the bots saw and their teams knew is the last map's) */
	bots_knowledge_reset();
	bots_tactics_reset();
	/* (the first line a minute into the game) */
	bots_globals.last_report_tick = 0;
	/* (the weapon definitions are the new map's) */
	bots_weapon_kinds.count = 0;
}

void bots_dispose_from_old_map(void)
{
	bots_knowledge_reset();
	bots_tactics_reset();
	bots_paths_release();
	bots_globals.map_ready = FALSE;
}

/* ---------- path searches */

/* the search's way to its destination (or as near as it came): each
surface's entry point (path_state_build_path's raw steps, at most 64), the
endpoint last. TRUE with steps */
static boolean bots_path_raw_steps(struct path_state *state, struct path_step *raw_steps, short *raw_step_count,
	boolean *steps_finish_path, real_point3d *endpoint)
{
	short node_index, child_index = NONE;

	*steps_finish_path = TRUE;
	*raw_step_count = 0;
	if (!state->destination_valid || state->node_count <= 0)
		return FALSE;
	node_index = path_node_from_hash_table(state, state->destination.surface_index);
	*endpoint = state->destination.point;
	if (node_index == NONE)
	{
		node_index = state->closest_node_index;
		*endpoint = state->closest_point;
	}
	if (node_index < 0 || node_index >= state->node_count)
		return FALSE;
	*raw_step_count = (short)MIN(state->node_list[node_index].depth + 1, 64);
	while (node_index != NONE)
	{
		struct path_node *node;

		if (node_index < 0 || node_index >= state->node_count)
			return FALSE;
		node = &state->node_list[node_index];
		if (node->depth >= 64)
			*steps_finish_path = FALSE;
		else if (node->depth >= 0 && node->depth < *raw_step_count)
		{
			raw_steps[node->depth].surface_index = node->surface_index;
			raw_steps[node->depth].point = child_index == NONE ? *endpoint : state->node_list[child_index].entry_point;
		}
		else
			return FALSE;
		child_index = node_index;
		node_index = node->parent_node_index;
	}
	return *raw_step_count > 0;
}

/* the path as path_state_build_path makes it, the steps smoothed, but not
taken round the objects about (the obstacle avoidance found no way round:
the other bots crowding a narrow way out of a base, Longest's, all waiting
on each other); the bots push past each other, and one that stops moving
sidesteps and jumps. TRUE with a path */
static boolean bots_build_path_unavoided(struct path_state *state, struct path_result *path)
{
	struct path_step raw_steps[64];
	struct path_step smoothed_steps[MAXIMUM_SMOOTHED_PATH_STEPS];
	short raw_step_count, smoothed_step_count = 0;
	boolean steps_finish_path;
	real_point3d endpoint;

	csmemset(path, 0, sizeof(*path));
	if (!bots_path_raw_steps(state, raw_steps, &raw_step_count, &steps_finish_path, &endpoint))
		return FALSE;
	path_smooth(state, raw_step_count, raw_steps, &smoothed_step_count, smoothed_steps, &steps_finish_path);
	if (smoothed_step_count <= 0)
		return FALSE;
	smoothed_step_count = MIN(smoothed_step_count, MAXIMUM_SMOOTHED_PATH_STEPS);
	path->valid = TRUE;
	path->endpoint.point = endpoint;
	path->endpoint.surface_index = smoothed_steps[smoothed_step_count - 1].surface_index;
	path->steps_finish_path = steps_finish_path;
	path->step_count = (char)smoothed_step_count;
	path->step_index = 0;
	csmemcpy(path->steps, smoothed_steps, smoothed_step_count * sizeof(struct path_step));
	return TRUE;
}

/* a path that goes nowhere: short of its end, every step where the bot
stands. The smoothing (path_smooth) gave one at the foot of a steep bank:
all four of its steps the same point a few centimetres from the bot, which
it took as reached at once and searched again, the same, while it slid
about (Sidewinder's banks up to the side ledges) */
static boolean bots_path_stalls(struct path_result const *path, real_point3d const *start)
{
	return path->valid && !path->steps_finish_path && path->step_count > 0 &&
		bots_distance2d(&path->steps[path->step_count - 1].point, start) <= BOTS_STEP_REACHED;
}

/* a stalling path's steps instead the search's own, unsmoothed: the next
surfaces' entry points beyond where the bot stands (up the bank), four at
most. TRUE when made */
static boolean bots_path_unsmoothed(struct path_state *state, real_point3d const *start, struct path_result *path)
{
	struct path_step raw_steps[64];
	short raw_step_count, step, count = 0;
	boolean steps_finish_path;
	real_point3d endpoint;
	struct path_result result;

	if (!bots_path_raw_steps(state, raw_steps, &raw_step_count, &steps_finish_path, &endpoint))
		return FALSE;
	csmemset(&result, 0, sizeof(result));
	for (step = 0; step < raw_step_count && count < (short)NUMBEROF(result.steps); step++)
	{
		if (!count && bots_distance2d(&raw_steps[step].point, start) <= BOTS_STEP_REACHED)
			continue;
		result.steps[count++] = raw_steps[step];
	}
	if (!count)
		return FALSE;
	result.valid = TRUE;
	result.steps_finish_path = steps_finish_path && step >= raw_step_count;
	result.step_count = (char)count;
	result.endpoint.point = endpoint;
	result.endpoint.surface_index = result.steps[count - 1].surface_index;
	*path = result;
	return TRUE;
}

/* a search's answer made a path: the way to the goal (or as near as the
search came); when the path's obstacle avoidance finds no way, again with
what the goal or the bot stands on (a flag's stand, a teleporter's base) no
obstacle, and failing that, the scenery and machines about the bot are
learnt to be in the way (the next searches go round them) and this once the
path is taken without the avoidance. TRUE with a path */
static boolean bots_search_build(struct path_state *state, real_point3d const *start, real_point3d const *goal,
	struct path_result *path)
{
	long object_index;

	if (state->node_count <= 0)
		return FALSE;
	if (path_state_build_path(state, path) && path->step_count > 0)
		return TRUE;
	object_index = bots_object_at(goal);
	if (object_index == NONE)
		object_index = bots_object_at(start);
	if (object_index != NONE)
	{
		state->input.ignore_target_object_index = object_index;
		csmemset(path, 0, sizeof(*path));
		if (path_state_build_path(state, path) && path->step_count > 0)
			return TRUE;
	}
	/* (something new learnt: a new search, round it) */
	if (bots_paths_learn(start, goal) > 0)
		return FALSE;
	if (!bots_build_path_unavoided(state, path))
		return FALSE;
	bots_globals.search_unavoided++;
	return TRUE;
}

/* a path from where the bot's unit is to its goal, searched now (a few a
tick: a bot past them asks again next tick), into the bot's path */
static void bots_search(struct bots_bot *bot, long unit_index)
{
	struct path_state *state;
	struct path_input input;
	struct path_result path;
	real_point3d start;
	long start_surface_index;
	boolean reached;

	if (bots_globals.tick_searches >= BOTS_SEARCHES_A_TICK || bot->goal_surface_index == NONE)
		return;
	if (!bots_globals.search)
	{
		bots_globals.search = (struct path_state *)malloc(sizeof(struct path_state));
		if (!bots_globals.search)
			return;
	}
	state = bots_globals.search;
	start_surface_index = bots_unit_surface(unit_index, &start);
	if (start_surface_index == NONE)
		return;
	/* (on a surface closed to the searches, whose way out may be closed
	too: away from what closed it first, a second) */
	if (bots_paths.walkable && start_surface_index < bots_paths.structure.pathfinding_surfaces.count &&
		!TEST_FLAG(bots_paths.walkable[start_surface_index], BOTS_PATHFINDING_WALKABLE_BIT) &&
		bots_paths_escape_direction(&start, &bot->escape_direction))
	{
		bot->escape_until_tick = game_time_get() + TICKS_PER_SECOND;
		bot->path.valid = FALSE;
		bot->path_tick = game_time_get();
		return;
	}
	bots_globals.tick_searches++;
	bots_globals.searches++;
	bot->searches++;
	path_input_new(&input, BOTS_PATH_RADIUS, FALSE, unit_index);
	path_input_set_start(&input, &start, start_surface_index);
	path_state_new(&input, state, NULL);
	/* (the walkable surfaces less those closed by what is in the way) */
	if (bots_paths_copy() && state->structure == bots_paths.source)
		state->structure = &bots_paths.structure;
	/* (a path to as near the goal as the search reaches: a long way across a
	large level is more surfaces than a search holds, and the bot searches
	again from there) */
	path_state_destination(state, &bot->goal_point, bot->goal_surface_index, 1000.0f);
	path_state_find(state);
	reached = state->node_count > 0 && path_node_from_hash_table(state, bot->goal_surface_index) != NONE;
	if (state->node_count >= PATH_NODE_LIST_SIZE)
		bots_globals.search_overflows++;
	if (!reached)
		bots_globals.search_partials++;
	csmemset(&path, 0, sizeof(path));
	if (bots_search_build(state, &start, &bot->goal_point, &path))
	{
		if (bots_path_stalls(&path, &start) && bots_path_unsmoothed(state, &start, &path))
			bots_globals.search_unsmoothed++;
		bot->path = path;
		bot->path.step_index = 0;
		bot->path_failures = 0;
		bot->path_short = !reached;
		if (!bot->path_short)
			bot->climb_until_tick = 0;
	}
	else
	{
		bot->path.valid = FALSE;
		bot->path_failures++;
		bots_globals.search_failures++;
	}
	/* (an item it finds no way to: another, and not this one a while) */
	if (bot->goal_kind == _bots_goal_item && !reached)
	{
		bots_item_set_unreachable(bot, bot->item_index);
		bot->goal_kind = _bots_goal_none;
		bot->path.valid = FALSE;
	}
	bot->path_tick = game_time_get();
}

/* ---------- perception */

/* whether the bot sees the point from its eyes (the AI's sight test) */
static boolean bots_line_of_sight(long unit_index, real_point3d const *eye, real_point3d const *point, long target_unit_index)
{
	struct collision_result collision;
	real_vector3d vector;

	vector.i = point->x - eye->x;
	vector.j = point->y - eye->y;
	vector.k = point->z - eye->z;
	if (!collision_test_vector(_collision_test_for_line_of_sight_flags, eye, &vector, unit_index, &collision))
		return TRUE;
	/* (the target's vehicle: seen) */
	if (collision.type == _collision_result_object && target_unit_index != NONE &&
		(collision.object_index == target_unit_index ||
			collision.object_index == object_get(target_unit_index)->object.parent_object_index))
	{
		return TRUE;
	}
	return FALSE;
}

static boolean bots_is_enemy(struct player_datum *player, struct player_datum *other)
{
	if (player == other || other->unit_index == NONE)
		return FALSE;
	if (game_engine_has_teams() && player->team_index == other->team_index)
		return FALSE;
	return TRUE;
}

/* the family of the weapon in the player's unit's hands (bots_weapon_kind),
NONE for none */
static short bots_holding(struct player_datum *player)
{
	long weapon_index;

	if (player->unit_index == NONE)
		return NONE;
	weapon_index = bots_current_weapon(unit_get(player->unit_index));
	return weapon_index != NONE ? bots_weapon_kind(weapon_get(weapon_index)->definition_index)->family : (short)NONE;
}

/* the enemy the bot sees best now, NONE for none: of the enemies in its
sight's reach and view, the one of the least score (nearest, most ahead, the
one it fights favoured) a line of sight reaches, the first in the players'
order of those of that score. The enemies are scored first and their lines
of sight cast from the least score up, until one reaches its enemy, which is
the answer. A score that is not a number (none is: the players' positions
are), or more enemies than kept, take the players' order */
struct bots_target_candidate
{
	long player_index;
	long unit_index;
	real score;
	real_point3d point;
};

/* an enemy's score as a target (its point the bounding sphere's centre);
FALSE for one out of reach or view, or not an enemy */
static boolean bots_target_score(struct bots_bot *bot, struct player_datum *player, struct player_datum *other,
	long player_index, real_point3d const *eye, struct bots_skill_definition const *skill, boolean hurt,
	struct bots_target_candidate *candidate)
{
	struct object_datum *object;
	struct unit_datum *other_unit;
	real distance, reach, bearing;
	real half_view = 0.5f * skill->field_of_view_degrees * BOTS_DEGREES;

	if (!bots_is_enemy(player, other))
		return FALSE;
	object = object_get(other->unit_index);
	other_unit = unit_get(other->unit_index);
	if (TEST_FLAG(object->object.damage_flags, _object_dead_bit))
		return FALSE;
	candidate->point = object->object.bounding_sphere_center;
	distance = bots_distance3d(eye, &candidate->point);
	reach = skill->sight_distance;
	/* (active camouflage: seen only close) */
	if (TEST_FLAG(other_unit->unit.flags, _unit_active_camouflaged_bit))
		reach *= 0.3f;
	/* (the one it fights: a little further) */
	if (player_index == bot->target_player_index)
		reach *= 1.25f;
	if (distance > reach)
		return FALSE;
	bearing = (real)fabs(bots_angle_difference((real)atan2(candidate->point.y - eye->y, candidate->point.x - eye->x),
		bot->yaw));
	/* (out of view: unless close by, it shot the bot, or it is the one
	the bot fights) */
	if (bearing > half_view && distance > 3.0f && !hurt && player_index != bot->target_player_index)
		return FALSE;
	candidate->player_index = player_index;
	candidate->unit_index = other->unit_index;
	candidate->score = distance + bearing * 4.0f - (player_index == bot->target_player_index ? 6.0f : 0.0f);
	/* (its team's: the enemy a teammate hurts, a teammate's killer) */
	if (bot != &bots_globals.player_bot)
		candidate->score -= bots_tactics_target_bonus(bot->index, player_index);
	/* (debug.bot_player's rocket launcher: the enemy with the most others
	about them, a group for one rocket) */
	if (bot->smooth && bots_holding(player) == _bots_family_explosive)
	{
		struct data_iterator others;
		struct player_datum *near;

		data_iterator_new(&others, player_data);
		while ((near = (struct player_datum *)data_iterator_next(&others)) != NULL)
		{
			if (near != other && bots_is_enemy(player, near) &&
				!TEST_FLAG(object_get(near->unit_index)->object.damage_flags, _object_dead_bit) &&
				bots_distance3d(&object_get(near->unit_index)->object.position, &candidate->point) < 4.0f)
			{
				candidate->score -= 8.0f;
			}
		}
	}
	return TRUE;
}

static long bots_find_target(struct bots_bot *bot, struct player_datum *player, real_point3d const *eye)
{
	struct bots_skill_definition const *skill = &bots_skills[bots_globals.skill];
	struct data_iterator iterator;
	struct player_datum *other;
	boolean hurt = game_time_get() - bot->hurt_tick < TICKS_PER_SECOND * 2;
	struct bots_target_candidate candidates[BOTS_MAXIMUM + MAXIMUM_LOCAL_PLAYERS];
	long candidate_count = 0, index;
	boolean ordered = TRUE;

	data_iterator_new(&iterator, player_data);
	while ((other = (struct player_datum *)data_iterator_next(&iterator)) != NULL && ordered)
	{
		if (candidate_count >= (long)NUMBEROF(candidates))
			ordered = FALSE;
		else if (bots_target_score(bot, player, other, iterator.datum_index, eye, skill, hurt, &candidates[candidate_count]))
		{
			real score = candidates[candidate_count].score;

			if (score != score)
				ordered = FALSE;
			else if (score < REAL_MAX)
				candidate_count++;
		}
	}
	if (!ordered)
	{
		/* (the players' order, each scoring less than the best so far) */
		struct bots_target_candidate candidate;
		long best_index = NONE;
		real best_score = REAL_MAX;

		data_iterator_new(&iterator, player_data);
		while ((other = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		{
			if (!bots_target_score(bot, player, other, iterator.datum_index, eye, skill, hurt, &candidate) ||
				candidate.score >= best_score ||
				!bots_line_of_sight(player->unit_index, eye, &candidate.point, candidate.unit_index))
			{
				continue;
			}
			best_score = candidate.score;
			best_index = candidate.player_index;
		}
		return best_index;
	}
	while (candidate_count > 0)
	{
		long least = 0;

		/* (the least score, the first of it in the players' order) */
		for (index = 1; index < candidate_count; index++)
		{
			if (candidates[index].score < candidates[least].score)
				least = index;
		}
		if (bots_line_of_sight(player->unit_index, eye, &candidates[least].point, candidates[least].unit_index))
			return candidates[least].player_index;
		/* (out, the others kept in their order) */
		candidate_count--;
		for (index = least; index < candidate_count; index++)
			candidates[index] = candidates[index + 1];
	}
	return NONE;
}

/* ---------- the scripted player's aim (debug.test_input's "hunt")

For footage of the campaign: the first local player's view turns toward the
nearest enemy unit (a biped or vehicle of a team the player's is at war
with, alive) within 40 world units and 50 degrees of where it looks, whom a
line of sight reaches, at most 300 degrees a second; TRUE (fire) once it is
within 3 degrees. It moves the player too (forward and strafe, -1 to 1):
closing on a target far off with a weave, strafing one at middle range,
backing from one close, and on ahead with none; a move in the script
overrides it. Nothing is random, so a script plays the same again. */
boolean bots_hunt(float *forward, float *strafe)
{
	long player_index = local_player_get_player_index(0);
	struct player_datum *player;
	struct object_iterator iterator;
	struct unit_datum *other;
	real_euler_angles2d const *facing;
	real_point3d eye, best_point;
	real best_score = REAL_MAX;
	short team;

	*forward = 0.6f;
	*strafe = 0.0f;
	if (player_index == NONE)
		return FALSE;
	player = player_get(player_index);
	if (player->unit_index == NONE)
		return FALSE;
	team = object_get(player->unit_index)->object.owner_team_index;
	facing = player_control_get_facing_angles(0);
	unit_get_head_position(player->unit_index, &eye);
	object_iterator_new(&iterator, _object_mask_biped|_object_mask_vehicle, 0);
	while ((other = (struct unit_datum *)object_iterator_next(&iterator)) != NULL)
	{
		real_point3d point = other->object.bounding_sphere_center;
		real distance = bots_distance3d(&eye, &point);
		real bearing;

		if (iterator.index == player->unit_index || TEST_FLAG(other->object.damage_flags, _object_dead_bit) ||
			!game_team_is_enemy(team, other->object.owner_team_index) || distance > 40.0f)
		{
			continue;
		}
		bearing = (real)fabs(bots_angle_difference((real)atan2(point.y - eye.y, point.x - eye.x), facing->yaw));
		if (bearing > 50.0f * BOTS_DEGREES || distance + bearing * 8.0f >= best_score ||
			!bots_line_of_sight(player->unit_index, &eye, &point, iterator.index))
		{
			continue;
		}
		best_score = distance + bearing * 8.0f;
		best_point = point;
	}
	if (best_score < REAL_MAX)
	{
		real_euler_angles2d angles;
		real_vector3d direction;
		real yaw = (real)atan2(best_point.y - eye.y, best_point.x - eye.x);
		real pitch = (real)atan2(best_point.z - eye.z, bots_distance2d(&eye, &best_point));
		real turn = 300.0f * BOTS_DEGREES / TICKS_PER_SECOND;
		real dyaw = bots_angle_difference(yaw, facing->yaw), dpitch = pitch - facing->pitch;

		real distance = bots_distance3d(&eye, &best_point);
		/* (a strafe's side, changing every second and a half) */
		real side = (game_time_get() / (TICKS_PER_SECOND * 3 / 2)) % 2 ? 1.0f : -1.0f;

		*forward = distance > 14.0f ? 1.0f : distance < 6.0f ? -0.4f : 0.2f;
		*strafe = distance > 14.0f ? side * 0.35f : side;
		angles.yaw = facing->yaw + PIN(dyaw, -turn, turn);
		angles.pitch = facing->pitch + PIN(dpitch, -turn, turn);
		vector3d_from_euler_angles2d(&direction, &angles);
		player_control_set_facing(0, &direction);
		return (real)fabs(dyaw) < 3.0f * BOTS_DEGREES && (real)fabs(dpitch) < 3.0f * BOTS_DEGREES;
	}
	return FALSE;
}

/* ---------- moving */

/* the power item the bot went for (bots_choose_power's claim) left to its
teammates */
static void bots_release_power_claim(struct bots_bot *bot)
{
	long item_index = bot->power_item_index;

	if (item_index >= 0 && item_index < BOTS_KNOWLEDGE_MAXIMUM_ITEMS &&
		bots_globals.power_claims[item_index] == bot->index + 1)
	{
		bots_globals.power_claims[item_index] = 0;
	}
	bot->power_item_index = NONE;
}

static void bots_set_goal(struct bots_bot *bot, short kind, real_point3d const *point, long surface_index)
{
	/* (the same goal, near enough: the path stands) */
	if (bot->goal_kind == kind && bot->goal_surface_index == surface_index &&
		bots_distance3d(&bot->goal_point, point) < 1.5f)
	{
		return;
	}
	/* (a goal other than the power item it went for: the item left to its
	teammates) */
	if (kind != _bots_goal_item && kind != _bots_goal_power)
		bots_release_power_claim(bot);
	bot->goal_kind = kind;
	bot->goal_point = *point;
	bot->goal_surface_index = surface_index;
	bot->goal_tick = game_time_get();
	bot->path.valid = FALSE;
	bot->path_failures = 0;
	bot->climb_until_tick = 0;
}

/* the next place to roam to: now and then near a teammate; most often
towards an enemy it knows of (the roaming place nearest where the one seen
last was, by itself or, bots.share_sightings, its team: bots_knowledge.h;
never where an enemy it does not know of is, which no motion tracker would
show of a crouched or walking player); else anywhere */
static void bots_pick_roam_goal(struct bots_bot *bot, real_point3d const *position)
{
	long tries;

	if (bots_globals.roam_point_count <= 0)
		return;
	bot->hunting = FALSE;
	/* (now and then near a teammate, a player's most: a team that keeps
	together) */
	if (game_engine_has_teams() && bots_random_real(bot) < 0.3f)
	{
		struct player_datum *player = player_get(bot->player_index);
		struct data_iterator iterator;
		struct player_datum *other;
		struct object_datum *mate = NULL;
		real mate_score = REAL_MAX;

		data_iterator_new(&iterator, player_data);
		while ((other = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		{
			struct object_datum *object;
			real score;

			if (other == player || other->unit_index == NONE || other->team_index != player->team_index)
				continue;
			object = object_get(other->unit_index);
			score = bots_distance2d(&object->object.position, position) *
				(bots_player_is_bot(iterator.datum_index) ? 1.0f : 0.5f);
			if (score < mate_score)
			{
				mate_score = score;
				mate = object;
			}
		}
		if (mate)
		{
			long index, nearest = NONE;
			real nearest_distance = REAL_MAX;

			for (index = 0; index < bots_globals.roam_point_count; index++)
			{
				real distance = bots_distance2d(&bots_globals.roam_points[index], &mate->object.position) +
					bots_random_real(bot) * 4.0f;

				if (distance < nearest_distance && bots_distance2d(&bots_globals.roam_points[index], position) > 4.0f)
				{
					nearest_distance = distance;
					nearest = index;
				}
			}
			if (nearest != NONE)
			{
				bots_globals.regroups++;
				bot->roam_index = (short)nearest;
				bots_set_goal(bot, _bots_goal_roam, &bots_globals.roam_points[nearest], bots_globals.roam_surfaces[nearest]);
				return;
			}
		}
	}
	if (bots_random_real(bot) < 0.65f && bot != &bots_globals.player_bot)
	{
		struct bots_known_enemy known[BOTS_KNOWLEDGE_MAXIMUM_KNOWN];
		short count = bots_knowledge_known(bot->index, known, BOTS_KNOWLEDGE_MAXIMUM_KNOWN), index, latest = NONE;

		for (index = 0; index < count; index++)
		{
			if (latest == NONE || known[index].tick > known[latest].tick)
				latest = index;
		}
		if (latest != NONE)
		{
			real_point3d const *enemy = &known[latest].position;
			long nearest = NONE;
			real nearest_distance = REAL_MAX;
			long point;

			for (point = 0; point < bots_globals.roam_point_count; point++)
			{
				real distance = bots_distance2d(&bots_globals.roam_points[point], enemy) +
					bots_random_real(bot) * 6.0f;

				if (distance < nearest_distance && bots_distance2d(&bots_globals.roam_points[point], position) > 4.0f)
				{
					nearest_distance = distance;
					nearest = point;
				}
			}
			if (nearest != NONE)
			{
				bot->roam_index = (short)nearest;
				bot->hunting = TRUE;
				bots_set_goal(bot, _bots_goal_roam, &bots_globals.roam_points[nearest], bots_globals.roam_surfaces[nearest]);
				return;
			}
		}
	}
	for (tries = 0; tries < 6; tries++)
	{
		short index = (short)(bots_random(bot) % (unsigned long)bots_globals.roam_point_count);

		/* (not where it is) */
		if (bots_distance2d(&bots_globals.roam_points[index], position) < 6.0f && tries < 5)
			continue;
		bot->roam_index = index;
		bots_set_goal(bot, _bots_goal_roam, &bots_globals.roam_points[index], bots_globals.roam_surfaces[index]);
		return;
	}
}

/* where the bot goes: the enemy last seen, a better weapon near, or about
the map */
/* debug.bot_player_vehicle: the nearest vehicle of that kind (part of its
tag's name) not destroyed and no one's (no unit in it), NONE for none */
static long bots_find_vehicle(real_point3d const *position)
{
	char const *wanted = config_string("debug.bot_player_vehicle");
	struct object_iterator iterator;
	struct object_datum *vehicle;
	long best_index = NONE;
	real best_distance = REAL_MAX;

	if (!*wanted)
		return NONE;
	object_iterator_new(&iterator, _object_mask_vehicle, 0);
	while ((vehicle = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
	{
		char const *name = tag_get_name(vehicle->definition_index);
		real distance = bots_distance3d(&vehicle->object.position, position);
		long child;
		boolean taken = FALSE;

		if (!name || !strstr(name, wanted) || TEST_FLAG(vehicle->object.damage_flags, _object_dead_bit) ||
			distance >= best_distance)
		{
			continue;
		}
		for (child = vehicle->object.first_child_object_index; child != NONE; child = object_get(child)->object.next_object_index)
		{
			if (TEST_FLAG(_object_mask_unit, object_get(child)->object.type))
				taken = TRUE;
		}
		if (taken)
			continue;
		best_distance = distance;
		best_index = iterator.index;
	}
	return best_index;
}

/* whether the unit holds a weapon whose tag's name has that in it */
static boolean bots_unit_holds_named(struct unit_datum *unit, char const *part)
{
	short slot;

	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long held = unit->unit.weapon_object_indices[slot];
		char const *name = held != NONE ? tag_get_name(weapon_get(held)->definition_index) : NULL;

		if (name && *part && strstr(name, part))
			return TRUE;
	}
	return FALSE;
}

/* the item at a spawn of the map as the bot's goal (the last bit straight
onto it, the action button for a weapon); FALSE for none under it */
static boolean bots_set_item_goal(struct bots_bot *bot, long object_index)
{
	real_point3d ground;
	long surface_index = bots_surface_below(&object_get(object_index)->object.position, &ground);

	if (surface_index == NONE)
		return FALSE;
	bots_set_goal(bot, _bots_goal_item, &ground, surface_index);
	bot->item_index = object_index;
	return TRUE;
}

/* an item's spawn as the bot's goal: the item, if it lies there (the bot
going there believes it does, and will see it), else the spawn */
static boolean bots_set_spawn_goal(struct bots_bot *bot, long item_index, short kind)
{
	struct bots_item item;
	long object_index;
	real_point3d ground;
	long surface_index;

	if (!bots_knowledge_item(item_index, &item, &object_index))
		return FALSE;
	if (object_index != NONE && kind == _bots_goal_item)
		return bots_set_item_goal(bot, object_index);
	surface_index = bots_surface_below(&item.position, &ground);
	if (surface_index == NONE)
		return FALSE;
	bots_set_goal(bot, kind, &ground, surface_index);
	return TRUE;
}

/* hurt: the nearest health pack within reach the bot believes lies at its
spawn (bots_knowledge_item_believed), as its goal; FALSE for none */
static boolean bots_choose_health(struct bots_bot *bot, real_point3d const *position, real maximum_distance)
{
	short count = bots_knowledge_item_count(), index, best = NONE;
	real best_distance = maximum_distance;

	for (index = 0; index < count; index++)
	{
		struct bots_item item;
		long object_index;
		real distance;

		if (!bots_knowledge_item(index, &item, &object_index) || item.kind != _bots_item_health ||
			!bots_knowledge_item_believed(bot->index, index, NULL) ||
			(object_index != NONE && bots_item_unreachable(bot, object_index)))
		{
			continue;
		}
		distance = bots_distance3d(&item.position, position);
		if (distance < best_distance)
		{
			best_distance = distance;
			best = index;
		}
	}
	if (best == NONE || !bots_set_spawn_goal(bot, best, _bots_goal_item))
		return FALSE;
	/* (gone from it as it comes: _bots_goal_power waits on there no
	longer than a health pack is worth) */
	bot->power_item_index = NONE;
	bots_globals.health_fetches++;
	return TRUE;
}

/* whether a teammate of the bot goes for the power item now */
static boolean bots_power_claimed(struct bots_bot *bot, long item_index, long now)
{
	long claimant = bots_globals.power_claims[item_index] - 1;
	struct player_datum *player, *other;

	if (claimant < 0 || claimant >= BOTS_MAXIMUM || claimant == bot->index ||
		now >= bots_globals.power_claim_ticks[item_index])
	{
		return FALSE;
	}
	player = player_try_and_get(bot->player_index);
	other = bots_globals.bots[claimant].player_index != NONE ? player_try_and_get(bots_globals.bots[claimant].player_index) : NULL;
	return player && other && other->unit_index != NONE &&
		(!game_engine_has_teams() || other->team_index == player->team_index);
}

/* a power weapon or powerup (bots_knowledge_item's power) the bot goes for:
within 50 units, one it believes lies there (it saw it there, or the spawn
has turned since it saw it gone: bots_knowledge_item_believed), or coming
back by the time the bot is there (at the spawn's next turn, which a player
times too), one it does not hold already, that a teammate does not go for;
the nearest, the one there favoured. Its goal the item's spawn (the item
once it sees it there), to take it or wait there; FALSE for none */
static boolean bots_choose_power(struct bots_bot *bot, struct unit_datum *unit, real_point3d const *position,
	boolean fighting, long now)
{
	short count = bots_knowledge_item_count(), index;
	long best_index = NONE, best_ticks = 0;
	boolean best_there = FALSE;
	real best_score = REAL_MAX;

	for (index = 0; index < count && index < BOTS_KNOWLEDGE_MAXIMUM_ITEMS; index++)
	{
		struct bots_item item;
		long object_index, ticks_until;
		real distance, score;
		boolean there;

		if (!bots_knowledge_item(index, &item, &object_index) || !item.power || bots_power_claimed(bot, index, now))
			continue;
		distance = bots_distance3d(&item.position, position);
		if (distance > (fighting ? 8.0f : 50.0f) ||
			(item.kind == _bots_item_weapon && bots_unit_holds_named(unit, item.name)))
		{
			continue;
		}
		there = bots_knowledge_item_believed(bot->index, index, &ticks_until);
		if (there)
		{
			if (object_index != NONE && bots_item_unreachable(bot, object_index))
				continue;
			score = distance - 20.0f;
			ticks_until = 0;
		}
		else
		{
			/* (about 2 units a second there, and 5 seconds to spare) */
			long travel_ticks = (long)(distance / 2.0f * TICKS_PER_SECOND);

			if (fighting || ticks_until == NONE || ticks_until > travel_ticks + TICKS_PER_SECOND * 5)
				continue;
			score = distance + (real)MAX(ticks_until - travel_ticks, 0) / TICKS_PER_SECOND;
		}
		/* (a teammate better placed for it takes it) */
		if (score >= best_score || !bots_tactics_may_claim(bot->index, index))
			continue;
		best_score = score;
		best_index = index;
		best_ticks = ticks_until;
		best_there = there;
	}
	if (best_index == NONE || !bots_set_spawn_goal(bot, best_index, best_there ? _bots_goal_item : _bots_goal_power))
		return FALSE;
	if (bot->goal_kind == _bots_goal_item)
		bots_globals.power_fetches++;
	else
		bots_globals.power_waits++;
	bot->power_item_index = best_index;
	bots_globals.power_claims[best_index] = bot->index + 1;
	bots_globals.power_claim_ticks[best_index] = now + MAX(best_ticks, 0) + TICKS_PER_SECOND * 20;
	return TRUE;
}

/* the team's point (bots_tactics_goal's) as the bot's goal: the ground
under it, else the roaming place nearest it within 10 units; FALSE for
none */
static boolean bots_set_tactic_goal(struct bots_bot *bot, real_point3d const *point)
{
	real_point3d ground;
	long surface_index;

	/* (the same point, near enough: the goal stands) */
	if (bot->goal_kind == _bots_goal_tactic && bots_distance3d(&bot->tactic_point, point) < 1.5f)
		return TRUE;
	surface_index = bots_surface_below(point, &ground);
	if (surface_index == NONE)
	{
		long index, nearest = NONE;
		real nearest_distance = 10.0f;

		for (index = 0; index < bots_globals.roam_point_count; index++)
		{
			real distance = bots_distance3d(&bots_globals.roam_points[index], point);

			if (distance < nearest_distance)
			{
				nearest_distance = distance;
				nearest = index;
			}
		}
		if (nearest == NONE)
			return FALSE;
		ground = bots_globals.roam_points[nearest];
		surface_index = bots_globals.roam_surfaces[nearest];
	}
	bots_set_goal(bot, _bots_goal_tactic, &ground, surface_index);
	bot->tactic_point = *point;
	return TRUE;
}

static void bots_choose_goal(struct bots_bot *bot, long player_index, real_point3d const *position, boolean fighting)
{
	struct unit_datum *own_unit = unit_get(player_get(player_index)->unit_index);
	long now = game_time_get();
	short tactic;
	real_point3d tactic_point;

	/* (debug.bot_player_vehicle: the player's bot boards one first, and
	drives it after) */
	if (bot->smooth && *config_string("debug.bot_player_vehicle") && own_unit->object.parent_object_index == NONE &&
		now >= bot->item_check_tick)
	{
		long vehicle_index = bots_find_vehicle(position);

		bot->item_check_tick = now + TICKS_PER_SECOND;
		if (vehicle_index != NONE)
		{
			real_point3d ground;
			long surface_index = bots_surface_below(&object_get(vehicle_index)->object.position, &ground);

			if (surface_index != NONE)
			{
				bots_set_goal(bot, _bots_goal_vehicle, &ground, surface_index);
				bot->vehicle_index = vehicle_index;
				return;
			}
		}
	}
	if (bot->goal_kind == _bots_goal_vehicle && own_unit->object.parent_object_index == NONE)
		return;
	/* (its team's ask, bots_tactics.h: none, a goal of it left) */
	tactic = bot->smooth ? (short)_bots_tactic_none : bots_tactics_goal(bot->index, position, &tactic_point);
	if (tactic == _bots_tactic_none && bot->goal_kind == _bots_goal_tactic)
		bot->goal_kind = _bots_goal_none;
	/* (badly hurt: a health pack first, before a chase) */
	if (!bot->smooth && own_unit->object.body_vitality < 0.35f && bot->goal_kind != _bots_goal_item &&
		now >= bot->item_check_tick)
	{
		bot->item_check_tick = now + TICKS_PER_SECOND;
		if (bots_choose_health(bot, position, fighting ? 8.0f : 30.0f))
			return;
	}
	/* (after a teammate's killer, falling back with the team: before its
	own chase) */
	if ((tactic == _bots_tactic_trade || tactic == _bots_tactic_fall_back) && bots_set_tactic_goal(bot, &tactic_point))
		return;
	if (!fighting && bot->target_player_index != NONE && bot->target_last_surface_index != NONE &&
		now - bot->target_seen_tick < TICKS_PER_SECOND * 8)
	{
		bots_set_goal(bot, _bots_goal_enemy, &bot->target_last_position, bot->target_last_surface_index);
		return;
	}
	/* a better weapon near (looked for once a second; given up after a
	while) */
	if (bot->goal_kind == _bots_goal_item)
	{
		struct item_datum *item = (struct item_datum *)object_try_and_get_and_verify_type(bot->item_index,
			_object_mask_item);

		if (item && item->object.parent_object_index == NONE && now - bot->goal_tick < TICKS_PER_SECOND * 15 &&
			(bot->item_first_press_tick < bot->goal_tick || now - bot->item_first_press_tick < TICKS_PER_SECOND * 3))
		{
			return;
		}
		/* (one it did not get, standing on it or not: left be a while) */
		if (item && item->object.parent_object_index == NONE)
			bots_item_set_unreachable(bot, bot->item_index);
		bot->goal_kind = _bots_goal_none;
		bot->item_check_tick = now + TICKS_PER_SECOND * 2;
	}
	/* (at a power item's spawn: on to it once it sees it there; waiting
	there a while for it to come back) */
	if (bot->goal_kind == _bots_goal_power)
	{
		long object_index;

		if (bots_knowledge_item_believed(bot->index, bot->power_item_index, NULL) &&
			bots_knowledge_item(bot->power_item_index, NULL, &object_index) && object_index != NONE &&
			bots_set_item_goal(bot, object_index))
		{
			return;
		}
		if (!fighting && now - bot->goal_tick < TICKS_PER_SECOND * 45)
			return;
		bot->goal_kind = _bots_goal_none;
	}
	if (now >= bot->item_check_tick)
	{
		long item_index;

		bot->item_check_tick = now + TICKS_PER_SECOND;
		/* (hurt: a health pack near; a power item, looked for every two
		seconds) */
		if (!bot->smooth && own_unit->object.body_vitality < 0.5f &&
			bots_choose_health(bot, position, fighting ? 8.0f : 25.0f))
		{
			return;
		}
		if (!bot->smooth && now >= bot->power_check_tick)
		{
			bot->power_check_tick = now + TICKS_PER_SECOND * 2;
			if (bots_choose_power(bot, own_unit, position, fighting, now))
				return;
		}
		item_index = bots_find_item(bot, unit_get(player_get(player_index)->unit_index), position, fighting ? 8.0f : 20.0f);
		if (item_index != NONE)
		{
			real_point3d ground;
			long surface_index = bots_surface_below(&object_get(item_index)->object.position, &ground);

			if (surface_index != NONE)
			{
				bots_set_goal(bot, _bots_goal_item, &ground, surface_index);
				bot->item_index = item_index;
				return;
			}
		}
	}
	/* (closing in on the team's kill, with a human; too far from every
	teammate, back to one: after a power item, health or a better weapon,
	which an escort or a bot closing in still takes) */
	if ((tactic == _bots_tactic_collapse || tactic == _bots_tactic_escort || tactic == _bots_tactic_support) &&
		bots_set_tactic_goal(bot, &tactic_point))
	{
		return;
	}
	if (bot->goal_kind != _bots_goal_roam || bots_distance2d(&bot->goal_point, position) < 1.5f ||
		now - bot->goal_tick > TICKS_PER_SECOND * (bot->hunting ? 12 : 40))
	{
		bots_pick_roam_goal(bot, position);
	}
}

/* the direction the bot walks (radians), following its path; FALSE when
it has nowhere to go */
static boolean bots_follow_path(struct bots_bot *bot, long unit_index, real_point3d const *position, real *direction)
{
	long now = game_time_get();

	if (bot->goal_kind == _bots_goal_none)
		return FALSE;
	if (now < bot->escape_until_tick)
	{
		*direction = bot->escape_direction;
		return TRUE;
	}
	/* (at the end of a path short of its goal: straight on, jumping, while
	it comes nearer (the walkable surfaces miss ramps a biped walks up:
	Prisoner's, Wizard's); then somewhere else) */
	if (bot->climb_until_tick)
	{
		real distance = bots_distance3d(&bot->goal_point, position);

		if (distance < bot->climb_best_distance - 0.5f && now - bot->climb_start_tick < TICKS_PER_SECOND * 20)
		{
			bot->climb_best_distance = distance;
			bot->climb_until_tick = now + TICKS_PER_SECOND * 3;
		}
		if (now < bot->climb_until_tick && distance > 1.0f)
		{
			*direction = (real)atan2(bot->goal_point.y - position->y, bot->goal_point.x - position->x);
			return TRUE;
		}
		bot->climb_until_tick = 0;
		if (distance > 1.0f)
		{
			bot->goal_kind = _bots_goal_none;
			return FALSE;
		}
	}
	/* (a new path now and then: the objects about move, and the goal) */
	if ((!bot->path.valid || now - bot->path_tick > TICKS_PER_SECOND * 4) &&
		now - bot->path_tick > TICKS_PER_SECOND / 3)
	{
		bots_search(bot, unit_index);
	}
	if (bot->path.valid)
	{
		while (bot->path.step_index < bot->path.step_count)
		{
			struct path_step *step = &bot->path.steps[(long)bot->path.step_index];

			if (bots_distance2d(&step->point, position) > BOTS_STEP_REACHED)
				break;
			bot->path.step_index++;
		}
		if (bot->path.step_index >= bot->path.step_count)
		{
			bot->path.valid = FALSE;
			/* (as near as the search came, the goal on above or beyond:
			on towards it) */
			if (bot->path.steps_finish_path && bot->path_short && bots_distance3d(&bot->goal_point, position) > 1.5f &&
				!bot->climb_until_tick)
			{
				bot->climb_start_tick = now;
				bot->climb_until_tick = now + TICKS_PER_SECOND * 3;
				bot->climb_best_distance = bots_distance3d(&bot->goal_point, position);
				*direction = (real)atan2(bot->goal_point.y - position->y, bot->goal_point.x - position->x);
				return TRUE;
			}
			/* (the whole way walked: there) */
			if (bot->path.steps_finish_path)
				return FALSE;
			bots_search(bot, unit_index);
		}
		else
		{
			struct path_step *step = &bot->path.steps[(long)bot->path.step_index];

			*direction = (real)atan2(step->point.y - position->y, step->point.x - position->x);
			return TRUE;
		}
	}
	/* (no path yet, or none found: straight there) */
	if (bots_distance2d(&bot->goal_point, position) < 1.0f)
		return FALSE;
	*direction = (real)atan2(bot->goal_point.y - position->y, bot->goal_point.x - position->x);
	return TRUE;
}

/* ---------- a bot's tick */

static void bots_bot_reset(struct bots_bot *bot, long player_index, long bot_index)
{
	csmemset(bot, 0, sizeof(*bot));
	bot->player_index = player_index;
	bot->unit_index = NONE;
	bot->target_player_index = NONE;
	bot->target_last_surface_index = NONE;
	bot->goal_surface_index = NONE;
	bot->random = 0x2545F491UL * (unsigned long)(bot_index + 1) ^ (unsigned long)network_game_get_random_seed();
	bot->strafe_sign = 1.0f;
	bot->index = bot_index;
	bot->power_item_index = NONE;
	bot->cover_surface_index = NONE;
	bot->survey_tick = bot_index % 10;
	/* (never hurt yet: not 0, which is "hurt at the game's start" for its
	first two seconds) */
	bot->hurt_tick = -TICKS_PER_SECOND * 10;
	bot->motion_change_tick = NONE;
	bot->aimed_since_tick = NONE;
	bot->opened_player_index = NONE;
	bot->tally_weapon_index = NONE;
	bot->throw_tick = bot->blow_tick = -TICKS_PER_SECOND * 10;
	bot->form = 1.0f + bots_random_signed(bot) * 0.2f;
	bot->smooth = bot_index == BOTS_MAXIMUM;
	if (bot->smooth)
	{
		platform_log("bots: the first player played by a bot (debug.bot_player): weapon \"%s\", vehicle \"%s\"",
			config_string("debug.bot_player_weapon"), config_string("debug.bot_player_vehicle"));
	}
}

/* the bot's aim turned toward an offset (radians) as a player's hand turns
it: a spring at that frequency (radians a second) and damping (1, critical:
it speeds into the turn and settles on the mark; less: it flicks past the
mark and comes back, the less the further), no faster than turn a tick. The
track rates (radians a second: a target crossing the view) are matched as a
player's hand follows a target, so that the aim does not trail a moving
one */
static void bots_turn(struct bots_bot *bot, real yaw_off, real pitch_off, real turn, real frequency, real damping,
	real yaw_track, real pitch_track)
{
	real dt = 1.0f / TICKS_PER_SECOND, most = turn * TICKS_PER_SECOND;
	real stiffness = frequency * frequency, friction = 2.0f * damping * frequency;

	/* (no faster a change than a hand's, 90 radians a second squared:
	a half turn in about 0.4 seconds) */
	bot->yaw_rate += PIN((stiffness * yaw_off - friction * (bot->yaw_rate - yaw_track)) * dt,
		-90.0f * dt, 90.0f * dt);
	bot->pitch_rate += PIN((stiffness * pitch_off - friction * (bot->pitch_rate - pitch_track)) * dt,
		-90.0f * dt, 90.0f * dt);
	bot->yaw_rate = PIN(bot->yaw_rate, -most, most);
	bot->pitch_rate = PIN(bot->pitch_rate, -most, most);
	bot->yaw += bot->yaw_rate * dt;
	bot->pitch += bot->pitch_rate * dt;
}

/* ---------- the aim's hand

A bot misses as a player does: its aim error on taking a target settles
while it keeps the target in sight, but never below a slow wander of its
skill's size (scaled by its form, a slow streak of good and bad stretches,
and by its own moves: walking, a jump); a hit knocks its aim off (the
flinch, more for its shield popped); a change of the target's motion (a
strafe the other way, a jump, a stop) is reacted to only after two fifths of
its reaction time, its aim going on where the old motion led until then, and the error
grows as it takes the target in again. Its hand (bots_turn) flicks past a
target and comes back, trails one crossing its view faster than it follows,
and it fires while its aim still closes on the target (the fire cone). All
of it is the bot's own random numbers and the game state at the tick. */

/* a tick of the aim's error while the bot fights: the error it took the
target with (and its flinches) settles in its skill's time; its wander
never does: a point going round the mark, never nearer than 0.4 of the
floor's size (its form's and disturbance's times: 1 standing still), whose
way turns smoothly, about a turn a few seconds, so that the aim wobbles
slowly as a hand's does */
static void bots_aim_settle(struct bots_bot *bot, struct bots_skill_definition const *skill, real disturbance)
{
	real settle = (real)exp(-1.1f / (skill->aim_settle_seconds * TICKS_PER_SECOND));
	real size = skill->aim_floor_degrees * BOTS_DEGREES * bot->form * disturbance;
	real radius;

	bot->aim_error_yaw *= settle;
	bot->aim_error_pitch *= settle;
	bot->wander_spin = bot->wander_spin * 0.9f + bots_random_signed(bot) * 0.05f;
	bot->wander_angle = bots_angle_difference(bot->wander_angle + bot->wander_spin, 0.0f);
	/* (PIN reads its value more than once: the random number taken first) */
	radius = 1.0f + (bot->wander_radius - 1.0f) * 0.95f + bots_random_signed(bot) * 0.1f;
	bot->wander_radius = PIN(radius, 0.4f, 1.6f);
	bot->wander_yaw = (real)cos(bot->wander_angle) * bot->wander_radius * size;
	bot->wander_pitch = (real)sin(bot->wander_angle) * bot->wander_radius * size * 0.6f;
}

/* the bot's form, changed once a second: about 1 (0.6 to 1.6), wandering
over some twenty seconds */
static void bots_aim_form(struct bots_bot *bot, long now)
{
	if (now % TICKS_PER_SECOND)
		return;
	bot->form = 1.0f + (bot->form - 1.0f) * 0.95f + bots_random_signed(bot) * 0.1f;
	bot->form = PIN(bot->form, 0.6f, 1.6f);
}

/* the bot hit, losing that much of its health and shield (each 1 whole):
its aim knocked off, sideways either way and up, the more the harder the
hit, and more for its shield popped */
static void bots_aim_flinch(struct bots_bot *bot, struct bots_skill_definition const *skill, real loss, boolean popped)
{
	real kick = skill->flinch_degrees * BOTS_DEGREES * (MIN(loss * 2.0f, 1.5f) + (popped ? 0.5f : 0.0f));
	real side = bots_random_real(bot) < 0.5f ? -1.0f : 1.0f;

	bot->aim_error_yaw += side * kick * (0.6f + 0.4f * bots_random_real(bot));
	bot->aim_error_pitch += kick * (0.3f + 0.4f * bots_random_real(bot));
}

/* the target's motion (its velocity, world units a tick) as the bot takes
it in: a change of it is seen at once and reacted to after two fifths of the
bot's reaction time (a target in sight is followed, not found); until then the bot aims where the old motion would have the
target (motion_drift: the target's way from there, which the aim point is
off by), and then, its place taken in again, the error grows by a part of
the error the bot takes a target with. new_target: the motion taken as it
is */
static void bots_aim_target_motion(struct bots_bot *bot, struct bots_skill_definition const *skill,
	real_vector3d const *velocity, long now, boolean new_target)
{
	real di = velocity->i - bot->seen_velocity.i;
	real dj = velocity->j - bot->seen_velocity.j;
	real dk = velocity->k - bot->seen_velocity.k;
	real length;

	if (new_target)
	{
		bot->seen_velocity = *velocity;
		bot->motion_change_tick = NONE;
		bot->motion_drift.i = bot->motion_drift.j = bot->motion_drift.k = 0.0f;
		return;
	}
	/* (a change of more than 0.02 units a tick: a walking player's turn
	about is 0.15) */
	if (bot->motion_change_tick == NONE && di * di + dj * dj + dk * dk > 0.02f * 0.02f)
		bot->motion_change_tick = now;
	if (bot->motion_change_tick != NONE &&
		now - bot->motion_change_tick >= (long)(skill->reaction_seconds * 0.4f * TICKS_PER_SECOND))
	{
		real error = skill->aim_error_degrees * BOTS_DEGREES * 0.2f * (0.5f + 0.5f * bots_random_real(bot));

		bot->seen_velocity = *velocity;
		bot->motion_change_tick = NONE;
		bot->motion_drift.i = bot->motion_drift.j = bot->motion_drift.k = 0.0f;
		bot->aim_error_yaw += (bots_random_real(bot) < 0.5f ? -1.0f : 1.0f) * error;
		bot->aim_error_pitch += bots_random_signed(bot) * error * 0.4f;
		return;
	}
	if (bot->motion_change_tick == NONE)
		return;
	bot->motion_drift.i -= di;
	bot->motion_drift.j -= dj;
	bot->motion_drift.k -= dk;
	/* (no further than 0.6 units off) */
	length = (real)sqrt(bot->motion_drift.i * bot->motion_drift.i + bot->motion_drift.j * bot->motion_drift.j +
		bot->motion_drift.k * bot->motion_drift.k);
	if (length > 0.6f)
	{
		bot->motion_drift.i *= 0.6f / length;
		bot->motion_drift.j *= 0.6f / length;
		bot->motion_drift.k *= 0.6f / length;
	}
}

/* where on the target the bot's weapon (its family) is best aimed: the
sniper rifle and the pistol at the head once the target's shield is down
(shield: its vitality, 0 to 1, more with an overshield), when a headshot
kills, and at the body, the larger target, to bring the shield down (a
sniper going for the head through a whole shield missed four times as many
kills: measured); the rocket launcher and the fuel rod gun at the feet, for
the blast's reach on the ground; anything else at the body. The head only
while the bot goes for it (head_wanted), and not on a target in a
vehicle */
static short bots_aim_point_kind(short family, real target_shield, boolean in_vehicle, boolean head_wanted)
{
	if (in_vehicle)
		return _bots_aim_body;
	switch (family)
	{
	case _bots_family_sniper:
	case _bots_family_pistol:
		return head_wanted && target_shield <= 0.05f ? _bots_aim_head : _bots_aim_body;
	case _bots_family_explosive:
		return _bots_aim_feet;
	}
	return _bots_aim_body;
}

/* whether the bot goes for the head now: its skill's chance, taken for
each new target and every two seconds, so that a less skilled bot aims at
the body more often */
static boolean bots_aim_head_wanted(struct bots_bot *bot, struct bots_skill_definition const *skill, long now,
	boolean new_target)
{
	if (new_target || now >= bot->head_tick)
	{
		bot->head_wanted = bots_random_real(bot) < skill->head_chance;
		bot->head_tick = now + TICKS_PER_SECOND * 2;
	}
	return bot->head_wanted;
}

/* ---------- the fight's habits */

/* whether an aim from an eye (along the vector, any length) passes within
radius of a point ahead of it */
static boolean bots_aimed_at(real_point3d const *eye, real_vector3d const *aim, real_point3d const *point, real radius)
{
	real dx = point->x - eye->x, dy = point->y - eye->y, dz = point->z - eye->z;
	real along = dx * aim->i + dy * aim->j + dz * aim->k;
	real length = aim->i * aim->i + aim->j * aim->j + aim->k * aim->k;

	if (along <= 0.0f || length <= 0.0f)
		return FALSE;
	return dx * dx + dy * dy + dz * dz - along * along / length < radius * radius;
}

/* the side a fighting bot strafes to (1 its left, -1 its right): kept while
its target does not aim at it (another way, by its random numbers, after
two to three and a half seconds: a player tires of one way); aimed at, the
other way once it reacts (its skill's dodge reaction, if it dodges this
aim at all: its dodge chance), and again every half second or so while the
aim stays on it. TRUE on a dodge */
static boolean bots_strafe(struct bots_bot *bot, struct bots_skill_definition const *skill, boolean aimed, long now)
{
	if (!aimed)
	{
		bot->aimed_since_tick = NONE;
		if (now >= bot->strafe_until_tick)
		{
			bot->strafe_sign = bots_random_real(bot) < 0.5f ? -1.0f : 1.0f;
			bot->strafe_until_tick = now + TICKS_PER_SECOND * 2 + (long)(bots_random(bot) % (TICKS_PER_SECOND * 3 / 2));
		}
		return FALSE;
	}
	if (bot->aimed_since_tick == NONE)
	{
		bot->aimed_since_tick = now;
		bot->dodge_tick = now + (long)(skill->dodge_reaction_seconds * TICKS_PER_SECOND);
		bot->dodging = bots_random_real(bot) < skill->dodge_chance;
	}
	if (!bot->dodging || now < bot->dodge_tick)
		return FALSE;
	bot->strafe_sign = -bot->strafe_sign;
	bot->dodge_tick = now + TICKS_PER_SECOND * 2 / 5 + (long)(bots_random(bot) % (TICKS_PER_SECOND * 3 / 5));
	bot->strafe_until_tick = now + TICKS_PER_SECOND * 2;
	return TRUE;
}

/* how near (eye to the target's middle) a blow reaches: the blow's reach
to the target's surface, its bounding sphere's radius in (a biped's
surface within it), and further while the bot charges in (the blow lands
a few ticks after the press) */
static real bots_strike_reach(real target_radius, boolean charging)
{
	return BOTS_MELEE_REACH + MIN(target_radius, 0.4f) + (charging ? 0.3f : 0.0f);
}

/* (the aim's tally) the rounds the bot's weapon fired since the last tick,
by its family, and its sniper's with the aim on the head */
static void bots_tally_shots(struct bots_bot *bot, long weapon_index)
{
	struct weapon_datum *weapon;
	short family;

	if (weapon_index == NONE)
	{
		bot->tally_weapon_index = NONE;
		return;
	}
	weapon = weapon_get(weapon_index);
	family = bots_weapon_kind(weapon->definition_index)->family;
	/* (a shot a tick at most: the weapons fire no faster) */
	if (weapon_index == bot->tally_weapon_index && weapon->weapon.game_time_last_fired != bot->tally_fired_tick)
	{
		bots_globals.aim_shots[family]++;
		if (family == _bots_family_sniper && bot->tally_head)
			bots_globals.aim_head_shots++;
	}
	bot->tally_fired_tick = weapon->weapon.game_time_last_fired;
	bot->tally_weapon_index = weapon_index;
	bot->tally_family = family;
}

/* (the aim's tally, at the tick's start) the bots whose rounds struck a
biped since the last look, by the game's record of who hurt each unit (a
tick once for each bot and unit; not its own unit, nor what a grenade or a
blow of it did) */
static void bots_tally_hits(long now)
{
	struct object_iterator iterator;
	struct unit_datum *unit;

	object_iterator_new(&iterator, _object_mask_biped, 0);
	while ((unit = (struct unit_datum *)object_iterator_next(&iterator)) != NULL)
	{
		short index;

		for (index = 0; index < MAXIMUM_ATTACKERS_PER_UNIT; index++)
		{
			struct unit_attacker const *attacker = &unit->unit.attackers[index];
			long bot_index;

			if (attacker->game_time_stamp == (unsigned long)NONE || attacker->player_index == NONE ||
				(long)attacker->game_time_stamp <= bots_globals.aim_scan_tick)
			{
				continue;
			}
			for (bot_index = 0; bot_index < BOTS_MAXIMUM; bot_index++)
			{
				struct bots_bot *bot = &bots_globals.bots[bot_index];

				if (bot->player_index != attacker->player_index || bot->unit_index == iterator.index ||
					now - bot->throw_tick < TICKS_PER_SECOND * 3 || now - bot->blow_tick < TICKS_PER_SECOND)
				{
					continue;
				}
				bots_globals.aim_hits[bot->tally_family]++;
			}
		}
	}
	bots_globals.aim_scan_tick = now - 1;
}

/* the weapon whose tag's name has that in it put in the unit's hands, as
the game engine gives a loadout (game_engine_give_loadout) */
static void bots_give_weapon(long unit_index, char const *part)
{
	struct tag_iterator iterator;
	long tag_index;

	tag_iterator_new(&iterator, WEAPON_DEFINITION_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		char const *name = tag_get_name(tag_index);
		struct object_placement_data data;
		long weapon_index;

		if (!name || !strstr(name, part))
			continue;
		object_placement_data_new(&data, tag_index, NONE);
		weapon_index = object_new(&data);
		if (weapon_index != NONE && !unit_add_weapon_to_inventory(unit_index, weapon_index, _unit_add_weapon_replace))
			object_delete(weapon_index);
		return;
	}
}

/* a bot's tick: what it knows of itself, and the controls it builds */
struct bots_tick
{
	struct bots_skill_definition const *skill;
	struct player_datum *player;
	struct unit_datum *unit;
	struct object_datum *object;
	long now;
	/* the unit's eyes (its head marker), found when first needed: the look
	(every third tick) and the fight (bots_tick_eye) */
	boolean eye_found;
	real_point3d eye;
	real_point3d position;
	long weapon_index;
	/* the controls: buttons, where it walks, how hard */
	unsigned long flags;
	boolean moving;
	real move_direction;
	real throttle_scale;
	/* it has a target in sight, this far away */
	boolean fighting;
	real target_distance;
	struct player_action *action;
	/* it crouches in its fight this tick (bots_bot_fight's) */
	boolean crouching;
	/* it stays where it is, looking about (its fall back's cover reached);
	it keeps to its goal's path while it fights (cover, its team's falling
	back) */
	boolean stay;
	boolean goal_in_fight;
};

static void bots_bot_watch(struct bots_bot *bot, struct bots_tick *tick);

/* the unit's eyes: its head marker, as unit_get_head_position finds it
(the marker's matrix from the object's node matrices, which nothing in the
bot's tick changes, so it is the same found now or at the tick's start) */
static real_point3d const *bots_tick_eye(struct bots_tick *tick)
{
	if (!tick->eye_found)
	{
		unit_get_head_position(tick->player->unit_index, &tick->eye);
		tick->eye_found = TRUE;
	}
	return &tick->eye;
}

/* what the bot sees, a look every third of a second (bots_knowledge.h):
every enemy in its sight's reach and view whom its sight test reaches (its
view's cone always, even hurt: a hurt player sees no more behind them than
the one who shot them, which its look finds), and the item spawns there
whose place its sight test reaches, the item there or not */
static void bots_bot_survey(struct bots_bot *bot, struct bots_tick *tick)
{
	real_point3d const *eye = bots_tick_eye(tick);
	real half_view = 0.5f * tick->skill->field_of_view_degrees * BOTS_DEGREES;
	short count = bots_knowledge_item_count(), index;
	struct data_iterator iterator;
	struct player_datum *other;

	data_iterator_new(&iterator, player_data);
	while ((other = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct bots_target_candidate candidate;

		if (iterator.datum_index != bot->target_player_index &&
			bots_target_score(bot, tick->player, other, iterator.datum_index, eye, tick->skill, FALSE, &candidate) &&
			bots_line_of_sight(tick->player->unit_index, eye, &candidate.point, candidate.unit_index))
		{
			bots_knowledge_saw(bot->index, iterator.datum_index, &object_get(other->unit_index)->object.position);
		}
	}
	for (index = 0; index < count; index++)
	{
		struct bots_item item;
		long object_index;
		real_point3d point;
		real distance, bearing;

		if (!bots_knowledge_item(index, &item, &object_index))
			continue;
		point = item.position;
		point.z += 0.25f;
		distance = bots_distance3d(eye, &point);
		if (distance > tick->skill->sight_distance)
			continue;
		bearing = (real)fabs(bots_angle_difference((real)atan2(point.y - eye->y, point.x - eye->x), bot->yaw));
		if (bearing > half_view && distance > 3.0f)
			continue;
		if (bots_line_of_sight(tick->player->unit_index, eye, &point, object_index))
			bots_knowledge_saw_item(bot->index, index, object_index != NONE);
	}
}

/* looking for enemies (every third tick, the bots in turn), and forgetting
the one gone a while */
static void bots_bot_look(struct bots_bot *bot, struct bots_tick *tick)
{
	if (tick->now >= bot->next_look_tick)
	{
		long target = bots_find_target(bot, tick->player, bots_tick_eye(tick));

		bot->next_look_tick = tick->now + 3;
		if (target != NONE)
		{
			struct player_datum *target_player = player_get(target);
			struct object_datum *target_object = object_get(target_player->unit_index);
			real_point3d ground;
			long surface_index;

			/* (a new target: the aim's error as it first turns to it) */
			if (target != bot->target_player_index || tick->now - bot->target_seen_tick > TICKS_PER_SECOND * 2)
			{
				real error = tick->skill->aim_error_degrees * BOTS_DEGREES;

				bot->target_first_seen_tick = tick->now;
				bot->aim_error_yaw = bots_random_signed(bot) * error;
				bot->aim_error_pitch = bots_random_signed(bot) * error * 0.6f;
			}
			bot->target_player_index = target;
			bot->target_seen_tick = tick->now;
			bot->target_last_position = target_object->object.position;
			surface_index = bots_unit_surface(target_player->unit_index, &ground);
			if (surface_index != NONE)
				bot->target_last_surface_index = surface_index;
			if (bot != &bots_globals.player_bot)
				bots_knowledge_saw(bot->index, target, &target_object->object.position);
		}
	}
	if (bot != &bots_globals.player_bot && tick->now >= bot->survey_tick)
	{
		bot->survey_tick = tick->now + 10;
		bots_bot_survey(bot, tick);
	}
	if (bot->target_player_index != NONE)
	{
		struct player_datum *target_player = player_try_and_get(bot->target_player_index);

		if (!target_player || target_player->unit_index == NONE ||
			tick->now - bot->target_seen_tick > TICKS_PER_SECOND * 10)
		{
			bot->target_player_index = NONE;
		}
	}
}

/* the way to where the enemy it knows of that was seen last
(bots_knowledge_known: by itself, or by its team, a callout's while late)
was, seen within four seconds and within its sight's reach, while it has
none in sight: it looks that way, as a player turns to a callout. Its aim
is its own sight's: nothing here fires. FALSE for none */
static boolean bots_known_threat(struct bots_bot *bot, struct bots_tick *tick, real *yaw)
{
	struct bots_known_enemy known[BOTS_KNOWLEDGE_MAXIMUM_KNOWN];
	short count, index, latest = NONE;

	if (bot == &bots_globals.player_bot ||
		(bot->target_player_index != NONE && tick->now - bot->target_seen_tick <= TICKS_PER_SECOND))
	{
		return FALSE;
	}
	count = bots_knowledge_known(bot->index, known, BOTS_KNOWLEDGE_MAXIMUM_KNOWN);
	for (index = 0; index < count; index++)
	{
		if (tick->now - known[index].tick > TICKS_PER_SECOND * 4 ||
			bots_distance3d(&known[index].position, &tick->position) > tick->skill->sight_distance ||
			bots_distance2d(&known[index].position, &tick->position) < 1.0f)
		{
			continue;
		}
		if (latest == NONE || known[index].tick > known[latest].tick)
			latest = index;
	}
	if (latest == NONE)
		return FALSE;
	*yaw = (real)atan2(known[latest].position.y - tick->position.y, known[latest].position.x - tick->position.x);
	return TRUE;
}

/* close up, and the grenades: the plays a player makes, each started by
the fight's situation and the bot's random numbers, and the blow at a
blow's reach (bots_strike_reach) with a weapon that strikes:
- a grenade, then shooting: a target newly fought at middle range (6 to 20
  units) whose shield is whole, now and then (the skill's grenade chance);
- a grenade, then in for a blow: a target 2 to 5.5 units off whose shield
  is up (the same chance, every six seconds at most);
- shooting, then in for a blow: the pistol on a target within 3 units,
  in once the shield is down; a weapon that only strikes (a flag, a ball),
  in at once.
debug.bot_player_grenades' plasma: a stick now and then, closer and often */
static void bots_bot_fight_close(struct bots_bot *bot, struct bots_tick *tick, struct bots_weapon_kind const *kind,
	real close, long reaction_ticks)
{
	struct bots_skill_definition const *skill = tick->skill;
	struct object_datum *target_object = object_get(player_get(bot->target_player_index)->unit_index);
	real shield = target_object->object.shield_vitality;
	long now = tick->now;
	boolean reacted = now - bot->target_first_seen_tick >= reaction_ticks;
	boolean grenades = tick->unit->unit.grenade_counts[0] > 0 || tick->unit->unit.grenade_counts[1] > 0;
	boolean on_foot = tick->object->object.parent_object_index == NONE;
	boolean charging;
	short combo = _bots_combo_none;

	if (now >= bot->combo_until_tick)
		bot->combo = _bots_combo_none;
	if (bot->smooth)
	{
		boolean sticking = !strcmp(config_string("debug.bot_player_grenades"), "plasma");
		real nearest = sticking ? 4.0f : 6.0f, furthest = sticking ? 18.0f : 22.0f;
		real chance = (sticking ? 0.6f : 0.15f) / TICKS_PER_SECOND;

		if (close > nearest && close < furthest && now >= bot->grenade_tick && grenades && reacted &&
			bots_random_real(bot) < chance)
		{
			tick->flags |= FLAG(_unit_control_throw_grenade_bit);
			if (sticking && tick->unit->unit.grenade_counts[1] > 0)
				tick->action->desired_grenade_index = 1;
			else if (tick->unit->unit.grenade_counts[(long)tick->unit->unit.current_grenade_index] <= 0)
				tick->action->desired_grenade_index = tick->unit->unit.grenade_counts[0] > 0 ? 0 : 1;
			bot->grenade_tick = now + (sticking ? TICKS_PER_SECOND * 5 / 2 : TICKS_PER_SECOND * 4);
		}
	}
	else if (bot->combo == _bots_combo_none && reacted && on_foot)
	{
		/* (an opener: once a target, at middle range, its shield whole) */
		if (bot->opened_player_index != bot->target_player_index && close > 6.0f && close < 20.0f && shield > 0.9f &&
			grenades && now >= bot->grenade_tick)
		{
			bot->opened_player_index = bot->target_player_index;
			if (bots_random_real(bot) < skill->grenade_chance)
				combo = _bots_combo_grenade_shoot;
		}
		/* (in close, its shield up: a grenade and the blow) */
		else if (close > 2.0f && close < 5.5f && shield > 0.5f && grenades && now >= bot->combo_next_tick &&
			kind && kind->can_strike)
		{
			bot->combo_next_tick = now + TICKS_PER_SECOND * 6;
			if (bots_random_real(bot) < skill->grenade_chance)
				combo = _bots_combo_grenade_blow;
		}
		/* (the pistol's shot then the blow; a weapon that only strikes) */
		else if (kind && kind->can_strike && close < 3.0f &&
			(kind->strikes_only || (kind->family == _bots_family_pistol && shield <= 0.05f)))
		{
			combo = _bots_combo_shoot_blow;
		}
		if (combo != _bots_combo_none)
		{
			bot->combo = combo;
			bot->combo_until_tick = now + TICKS_PER_SECOND * 3 / 2;
			bots_globals.combos[combo]++;
			if (combo != _bots_combo_shoot_blow)
			{
				tick->flags |= FLAG(_unit_control_throw_grenade_bit);
				tick->flags &= ~FLAG(_unit_control_weapon_primary_trigger_bit);
				if (tick->unit->unit.grenade_counts[(long)tick->unit->unit.current_grenade_index] <= 0)
					tick->action->desired_grenade_index = tick->unit->unit.grenade_counts[0] > 0 ? 0 : 1;
				bot->grenade_tick = now + TICKS_PER_SECOND * 4;
			}
		}
	}
	/* the blow, at its reach (further charging in) */
	charging = now < bot->combo_until_tick &&
		(bot->combo == _bots_combo_grenade_blow || bot->combo == _bots_combo_shoot_blow);
	if (kind && kind->can_strike && on_foot && now >= bot->melee_tick &&
		close < bots_strike_reach(target_object->object.bounding_sphere_radius, charging))
	{
		tick->flags |= FLAG(_unit_control_use_equipment_bit);
		tick->flags &= ~FLAG(_unit_control_weapon_primary_trigger_bit);
		bot->melee_tick = now + TICKS_PER_SECOND;
		bots_globals.blows++;
		if (bot->combo == _bots_combo_grenade_blow || bot->combo == _bots_combo_shoot_blow)
			bot->combo_until_tick = now;
	}
}

/* fighting the target seen in the last half second: aim (turning at its
speed, leading, an error that settles), fire, a grenade, a blow, strafing,
a crouch */
static void bots_bot_fight(struct bots_bot *bot, struct bots_tick *tick)
{
	struct bots_skill_definition const *skill = tick->skill;
	struct player_datum *target_player = player_get(bot->target_player_index);
	struct object_datum *target_object = object_get(target_player->unit_index);
	real_point3d aim_point = target_object->object.bounding_sphere_center;
	long reaction_ticks = (long)(skill->reaction_seconds * TICKS_PER_SECOND);
	long now = tick->now;
	real desired_yaw, desired_pitch, dx, dy, dz, horizontal, turn, yaw_off, pitch_off, cone;
	boolean new_target = bot->target_aim_tick != now - 1 || bot->target_first_seen_tick == now;
	short part;
	struct bots_weapon_kind const *kind = tick->weapon_index != NONE ?
		bots_weapon_kind(weapon_get(tick->weapon_index)->definition_index) : NULL;
	struct unit_datum *target_unit = unit_get(target_player->unit_index);
	real close;
	boolean aimed;
	short family = tick->weapon_index != NONE ?
		bots_weapon_kind(weapon_get(tick->weapon_index)->definition_index)->family : (short)_bots_family_other;

	tick->fighting = TRUE;
	bots_tick_eye(tick);
	if (new_target)
		bot->aimed_since_tick = NONE;
	tick->target_distance = bots_distance3d(&tick->eye, &aim_point);
	/* (how near its middle is, for a blow; whether it aims at the bot: its
	aim passing within half a unit of the bot's middle, more far off) */
	close = bots_distance3d(&tick->eye, &target_object->object.bounding_sphere_center);
	{
		real_point3d eye;

		unit_get_head_position(target_player->unit_index, &eye);
		aimed = bots_aimed_at(&eye, &target_unit->unit.aiming_vector, &tick->object->object.bounding_sphere_center,
			0.5f + close * 0.03f);
	}
	/* (a crouch at range, its target's aim not on it, with a weapon that
	reaches: a steadier aim, smaller to be seen, slower to move) */
	tick->crouching = !bot->smooth && !aimed && kind && tick->object->object.parent_object_index == NONE &&
		(kind->family == _bots_family_sniper || kind->family == _bots_family_pistol ||
			kind->family == _bots_family_assault_rifle) &&
		close > kind->range * 0.9f && now >= bot->combo_until_tick;
	if (tick->crouching)
	{
		if (!TEST_FLAG(bot->previous_control_flags, _unit_control_crouch_modifier_bit))
			bots_globals.crouches++;
		tick->flags |= FLAG(_unit_control_crouch_modifier_bit);
	}
	/* (the target's motion as the bot has taken it in: aimed where that
	would have it) */
	bots_aim_target_motion(bot, skill, &target_object->object.translational_velocity, now, new_target);
	/* (where its weapon is best aimed: the head, the feet, the body;
	debug.bot_player's goes for the head always) */
	part = bots_aim_point_kind(family, target_object->object.shield_vitality,
		target_object->object.parent_object_index != NONE, bot->smooth || bots_aim_head_wanted(bot, skill, now, new_target));
	if (part == _bots_aim_head)
		unit_get_head_position(target_player->unit_index, &aim_point);
	else if (part == _bots_aim_feet)
	{
		aim_point = target_object->object.position;
		aim_point.z += 0.05f;
	}
	/* (leading a moving target by the projectile's flight) */
	if (tick->weapon_index != NONE && skill->lead > 0.0f)
	{
		real speed = bots_projectile_speed(tick->weapon_index);

		if (speed > 0.01f)
		{
			real ticks = MIN(tick->target_distance / speed, 30.0f);

			aim_point.x += bot->seen_velocity.i * ticks * skill->lead;
			aim_point.y += bot->seen_velocity.j * ticks * skill->lead;
			aim_point.z += bot->seen_velocity.k * ticks * skill->lead;
		}
	}
	aim_point.x += bot->motion_drift.i;
	aim_point.y += bot->motion_drift.j;
	aim_point.z += bot->motion_drift.k;
	dx = aim_point.x - tick->eye.x;
	dy = aim_point.y - tick->eye.y;
	dz = aim_point.z - tick->eye.z;
	horizontal = (real)sqrt(dx * dx + dy * dy);
	/* the aim error settles while the target stays in sight, but not its
	wander, which its own moves make larger (walking, a jump) */
	{
		real_vector3d const *own = &tick->object->object.translational_velocity;
		struct biped_datum *biped = biped_try_and_get(tick->player->unit_index);
		real disturbance = (1.0f + (real)sqrt(own->i * own->i + own->j * own->j) * 3.0f +
			(biped && TEST_FLAG(biped->biped.flags, _biped_airborne_bit) ? 1.5f : 0.0f)) * (tick->crouching ? 0.75f : 1.0f);

		bots_aim_settle(bot, skill, disturbance);
	}
	desired_yaw = (real)atan2(dy, dx) + bot->aim_error_yaw + bot->wander_yaw;
	desired_pitch = (real)atan2(dz, horizontal) + bot->aim_error_pitch + bot->wander_pitch;
	turn = skill->turn_degrees_per_tick * BOTS_DEGREES;
	yaw_off = bots_angle_difference(desired_yaw, bot->yaw);
	pitch_off = desired_pitch - bot->pitch;
	/* (a player's hand on a target, its skill's: quick, following it as
	it moves; debug.bot_player's a steady one, about a quarter second to
	settle) */
	{
		real target_yaw = (real)atan2(dy, dx), target_pitch = (real)atan2(dz, horizontal);
		real yaw_track = 0.0f, pitch_track = 0.0f;

		if (bot->target_aim_tick == now - 1)
		{
			/* (as much and as fast as its hand follows: a faster target is
			trailed) */
			real gain = bot->smooth ? 1.0f : skill->track_gain, most = bot->smooth ? 6.0f : skill->track_most;

			yaw_track = PIN(bots_angle_difference(target_yaw, bot->target_yaw) * TICKS_PER_SECOND * gain, -most, most);
			pitch_track = PIN((target_pitch - bot->target_pitch) * TICKS_PER_SECOND * gain, -most, most);
		}
		bot->target_yaw = target_yaw;
		bot->target_pitch = target_pitch;
		bot->target_aim_tick = now;
		if (bot->smooth)
			bots_turn(bot, yaw_off, pitch_off, turn, 14.0f, 1.0f, yaw_track, pitch_track);
		else
			bots_turn(bot, yaw_off, pitch_off, turn, skill->aim_frequency, skill->aim_damping, yaw_track, pitch_track);
	}

	/* (debug.bot_player's sniper: through the scope, as a player snipes,
	2x past 6 world units and 8x past 30) */
	if (bot->smooth && family == _bots_family_sniper && tick->target_distance > 6.0f)
	{
		tick->action->desired_zoom_level = tick->target_distance > 30.0f ? 1 : 0;
	}

	/* fire: once it has reacted, while its aim is on the target (its size
	at that distance; going for the head, about the head and shoulders, as a
	player fires when the reticle is near the head) */
	yaw_off = (real)fabs(bots_angle_difference((real)atan2(dy, dx), bot->yaw));
	pitch_off = (real)fabs((real)atan2(dz, horizontal) - bot->pitch);
	cone = (real)atan((part == _bots_aim_head ? 0.3f : 0.35f) / MAX(tick->target_distance, 0.5f)) * skill->fire_cone +
		0.01f;
	/* (a weapon fired a shot a press, as a player fires a precise one: a
	tighter cone, the aim let settle on the mark) */
	if (tick->weapon_index != NONE && bots_weapon_tap_ticks(tick->weapon_index) > 0)
		cone *= 0.7f;
	bot->tally_head = FALSE;
	if (now - bot->target_first_seen_tick >= reaction_ticks && now - bot->target_seen_tick <= 3 &&
		tick->weapon_index != NONE &&
		((yaw_off < cone && pitch_off < cone * 1.5f) ||
			/* (the sniper rifle, now and then, as the flick crosses near
			the target: early) */
			(family == _bots_family_sniper && !bot->smooth && yaw_off < cone * 3.0f && pitch_off < cone * 4.5f &&
				bots_random_real(bot) < skill->early_shot_chance / TICKS_PER_SECOND)))
	{
		short tap_ticks = bots_weapon_tap_ticks(tick->weapon_index);

		if (tap_ticks > 0)
		{
			/* (a press a shot, released between: as fast as the weapon fires,
			a little slower for the less skilled) */
			if (now >= bot->burst_until_tick)
			{
				tick->flags |= FLAG(_unit_control_weapon_primary_trigger_bit);
				bot->burst_until_tick = now + tap_ticks + (long)(bots_random(bot) % (1 + skill->pause_ticks / 3));
				/* (the aim's tally: the sniper's round with its aim on the
				head, a tenth of a unit across) */
				if (family == _bots_family_sniper)
				{
					real_point3d head;
					real head_yaw, head_pitch, reach;

					unit_get_head_position(target_player->unit_index, &head);
					head_yaw = (real)atan2(head.y - tick->eye.y, head.x - tick->eye.x);
					head_pitch = (real)atan2(head.z - tick->eye.z, bots_distance2d(&head, &tick->eye));
					reach = (real)atan(0.1f / MAX(bots_distance3d(&head, &tick->eye), 0.5f));
					bot->tally_head = (real)fabs(bots_angle_difference(head_yaw, bot->yaw)) < reach &&
						(real)fabs(head_pitch - bot->pitch) < reach;
				}
			}
		}
		else if (now < bot->burst_until_tick)
			tick->flags |= FLAG(_unit_control_weapon_primary_trigger_bit);
		else if (now >= bot->pause_until_tick)
		{
			/* (bursts, with pauses between) */
			bot->burst_until_tick = now + skill->burst_ticks + (long)(bots_random(bot) % 4);
			bot->pause_until_tick = bot->burst_until_tick + skill->pause_ticks + (long)(bots_random(bot) % 4);
			tick->flags |= FLAG(_unit_control_weapon_primary_trigger_bit);
		}
	}
	/* the plays close up, and the blow */
	bots_bot_fight_close(bot, tick, kind, close, reaction_ticks);
	/* strafing: one way while the target does not aim at the bot, the
	other once it reacts to its aim (and a jump with it now and then);
	nearer or further to keep its weapon's distance, straight in for a
	blow; crouched, slowly */
	/* (no one aiming at it: its team's side, away from a teammate close
	by or round the enemy from a teammate on it too) */
	if (!aimed && !bot->smooth)
	{
		real side = bots_tactics_strafe_side(bot->index, bot->target_player_index, &tick->position,
			&target_object->object.position);

		if (side != 0.0f)
			bot->strafe_sign = side;
	}
	if (bots_strafe(bot, skill, aimed, now) && !bot->smooth)
	{
		bots_globals.dodges++;
		if (!tick->crouching && bots_random_real(bot) < skill->jump_chance)
		{
			tick->flags |= FLAG(_unit_control_jump_bit);
			bots_globals.dodge_jumps++;
		}
	}
	{
		real to_target = (real)atan2(dy, dx);
		real range = bots_weapon_range(tick->weapon_index);
		real forward = tick->target_distance > range * 1.3f ? 0.8f : tick->target_distance < range * 0.6f ? -0.5f : 0.0f;
		real side = bot->strafe_sign;

		if (now < bot->combo_until_tick &&
			(bot->combo == _bots_combo_grenade_blow || bot->combo == _bots_combo_shoot_blow))
		{
			forward = 1.0f;
			side *= 0.3f;
		}
		tick->move_direction = (real)atan2((real)sin(to_target) * forward + (real)sin(to_target + BOTS_PI * 0.5f) * side,
			(real)cos(to_target) * forward + (real)cos(to_target + BOTS_PI * 0.5f) * side);
		tick->moving = TRUE;
		tick->throttle_scale = tick->crouching ? 0.5f : 1.0f;
	}
}

/* walking to the goal, looking where it goes; there, a look about (and the
last bit straight onto an item); a reload when no one is about */
static void bots_bot_walk(struct bots_bot *bot, struct bots_tick *tick)
{
	real direction;

	if (bots_follow_path(bot, tick->player->unit_index, &tick->position, &direction))
	{
		real turn = 12.0f * BOTS_DEGREES;
		real off = bots_angle_difference(direction, bot->yaw);
		real threat_yaw;

		/* (an enemy it knows of, not behind it: looking that way as it
		walks) */
		if (!bot->smooth && bots_known_threat(bot, tick, &threat_yaw) &&
			(real)fabs(bots_angle_difference(threat_yaw, direction)) < 100.0f * BOTS_DEGREES)
		{
			off = bots_angle_difference(threat_yaw, bot->yaw);
		}
		tick->move_direction = direction;
		tick->moving = TRUE;
		/* (not on a teammate's heels) */
		if (!bot->smooth)
			bots_tactics_steer(bot->index, &tick->position, &tick->move_direction, &tick->throttle_scale);
		/* (looking where it walks: an easier hand than fighting's) */
		bots_turn(bot, off, -bot->pitch, turn, 6.0f, 1.0f, 0.0f, 0.0f);
		if (bot->climb_until_tick && tick->now < bot->climb_until_tick && (bot->climb_until_tick - tick->now) % 20 == 0)
			tick->flags |= FLAG(_unit_control_jump_bit);
	}
	else if (bot->goal_kind == _bots_goal_power || bot->goal_kind == _bots_goal_cover ||
		bot->goal_kind == _bots_goal_tactic)
	{
		/* (at a power item's spawn, its cover, its team's place: looking
		about there) */
		bots_bot_watch(bot, tick);
		return;
	}
	else
	{
		bot->yaw += 2.0f * BOTS_DEGREES;
		bot->yaw_rate = 2.0f * BOTS_DEGREES * TICKS_PER_SECOND;
		bot->pitch_rate = 0.0f;
		if (bot->goal_kind == _bots_goal_roam || bot->goal_kind == _bots_goal_enemy)
			bot->goal_kind = _bots_goal_none;
		/* (at an item: on to it) */
		if (bot->goal_kind == _bots_goal_item)
		{
			struct object_datum *item = object_try_and_get_and_verify_type(bot->item_index, _object_mask_item);

			if (item && bots_distance2d(&item->object.position, &tick->position) > 0.3f)
			{
				tick->move_direction = (real)atan2(item->object.position.y - tick->position.y,
					item->object.position.x - tick->position.x);
				tick->moving = TRUE;
				tick->throttle_scale = 0.6f;
			}
		}
	}
	if (tick->weapon_index != NONE && bots_weapon_loaded(tick->weapon_index) < 0.4f &&
		tick->now - bot->target_seen_tick > TICKS_PER_SECOND * 2)
	{
		tick->flags |= FLAG(_unit_control_weapon_reload_bit);
	}
}

/* the bots in each other's way (a team's bots leaving their base by the same
door, Longest's, jammed in it a whole game, each going round the others):
the one with the higher index steps back from the other, while the other
moves, so that the first goes through */
static void bots_bot_yield(long bot_index, struct bots_tick *tick)
{
	long index;

	if (!tick->moving || tick->fighting || tick->object->object.parent_object_index != NONE)
		return;
	for (index = 0; index < bot_index; index++)
	{
		struct bots_bot *other = &bots_globals.bots[index];
		struct object_datum *object;
		real dx, dy, distance, velocity;

		if (other->player_index == NONE || other->unit_index == NONE)
			continue;
		object = (struct object_datum *)object_try_and_get_and_verify_type(other->unit_index, _object_mask_unit);
		if (!object || object->object.parent_object_index != NONE)
			continue;
		dx = object->object.position.x - tick->position.x;
		dy = object->object.position.y - tick->position.y;
		distance = (real)sqrt(dx * dx + dy * dy);
		velocity = (real)sqrt(object->object.translational_velocity.i * object->object.translational_velocity.i +
			object->object.translational_velocity.j * object->object.translational_velocity.j);
		if (distance > 1.1f || distance < 0.01f || fabs(object->object.position.z - tick->position.z) > 1.5f ||
			velocity < 0.02f ||
			(dx * (real)cos(tick->move_direction) + dy * (real)sin(tick->move_direction)) < 0.3f * distance)
		{
			continue;
		}
		tick->move_direction = (real)atan2(-dy, -dx);
		tick->throttle_scale = 0.6f;
		return;
	}
}

/* weapons: the other one when this one is empty; at the weapon it fetches,
once the game offers it (the player's action, as the HUD shows it), the
action button held, as a player holds it to swap a weapon (the swap:
players_update_before_game's player_handle_weapon_swap), let go now and
then, so that a second swap can follow a first */
static void bots_bot_weapons(struct bots_bot *bot, struct bots_tick *tick)
{
	/* (debug.bot_player_weapon, debug.bots_weapon: in hand while it has
	rounds) */
	if (*bots_weapon_wanted(bot))
	{
		char const *preferred = bots_weapon_wanted(bot);
		short slot;

		for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
		{
			long held = tick->unit->unit.weapon_object_indices[slot];
			char const *name = held != NONE ? tag_get_name(weapon_get(held)->definition_index) : NULL;

			struct weapon_datum *weapon = held != NONE ? weapon_get(held) : NULL;

			/* (out of rounds only when its reserve is out too: an empty
			magazine is reloaded) */
			if (name && strstr(name, preferred) &&
				weapon->weapon.magazines[0].rounds_loaded + weapon->weapon.magazines[0].rounds_total > 0)
			{
				if (slot != tick->unit->unit.current_weapon_index)
					tick->action->desired_weapon_index = slot;
				else if (weapon->weapon.magazines[0].rounds_loaded == 0)
					tick->flags |= FLAG(_unit_control_weapon_reload_bit);
				return;
			}
		}
	}
	/* fighting: the weapon it holds better for the range (the pistol far
	off, the shotgun close up), at most every second and a half */
	if (!bot->smooth && tick->fighting && tick->weapon_index != NONE && !bots_weapon_empty(tick->weapon_index) &&
		tick->now - bot->range_swap_tick > TICKS_PER_SECOND * 3 / 2 && tick->now >= bot->combo_until_tick)
	{
		struct bots_weapon_kind const *held = bots_weapon_kind(weapon_get(tick->weapon_index)->definition_index);
		real best = (real)held->value - (real)fabs(tick->target_distance - held->range) * 0.4f + 1.5f;
		short slot, best_slot = NONE;

		for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
		{
			long other = tick->unit->unit.weapon_object_indices[slot];
			struct bots_weapon_kind const *kind;
			real fit;

			if (slot == tick->unit->unit.current_weapon_index || other == NONE || bots_weapon_empty(other))
				continue;
			kind = bots_weapon_kind(weapon_get(other)->definition_index);
			if (kind->value < 0)
				continue;
			fit = (real)kind->value - (real)fabs(tick->target_distance - kind->range) * 0.4f;
			if (fit > best)
			{
				best = fit;
				best_slot = slot;
			}
		}
		if (best_slot != NONE)
		{
			tick->action->desired_weapon_index = best_slot;
			bot->range_swap_tick = tick->now;
		}
	}
	if (tick->weapon_index != NONE && bots_weapon_empty(tick->weapon_index))
	{
		short slot;

		for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
		{
			long other = tick->unit->unit.weapon_object_indices[slot];

			if (slot != tick->unit->unit.current_weapon_index && other != NONE && !bots_weapon_empty(other))
			{
				tick->action->desired_weapon_index = slot;
				break;
			}
		}
	}
	/* (boarding: the action button while it would enter the vehicle) */
	if (bot->goal_kind == _bots_goal_vehicle && tick->object->object.parent_object_index == NONE &&
		tick->player->action_result == _player_action_result_enter_vehicle &&
		tick->player->action_object_index == bot->vehicle_index && (tick->now / (TICKS_PER_SECOND / 2)) % 2 == 0)
	{
		tick->flags |= FLAG(_unit_control_action_bit);
	}
	if (bot->goal_kind == _bots_goal_item)
	{
		struct object_datum *item = object_try_and_get_and_verify_type(bot->item_index, _object_mask_weapon);

		if (item && item->object.parent_object_index == NONE &&
			bots_distance2d(&item->object.position, &tick->position) < 0.8f &&
			fabs(item->object.position.z - tick->position.z) < 1.2f)
		{
			if (tick->now - bot->item_press_tick > TICKS_PER_SECOND)
				bot->item_press_tick = tick->now;
			if (bot->item_first_press_tick < bot->goal_tick)
				bot->item_first_press_tick = tick->now;
			if (tick->now - bot->item_press_tick < TICKS_PER_SECOND / 2 &&
				tick->player->action_object_index == bot->item_index &&
				(tick->player->action_result == _player_action_result_pickup_weapon ||
					tick->player->action_result == _player_action_result_swap_for_weapon ||
					tick->player->action_result == _player_action_result_add_weapon_to_inventory))
			{
				tick->flags |= FLAG(_unit_control_swap_weapons_bit);
			}
		}
	}
}

/* stuck: not moving for a second while it walks - a jump and a sidestep,
then another path; somewhere else after three */
static void bots_bot_unstick(struct bots_bot *bot, struct bots_tick *tick)
{
	long now = tick->now;

	if (now - bot->stuck_tick >= TICKS_PER_SECOND)
	{
		if (tick->moving && bots_distance2d(&tick->position, &bot->stuck_position) < 0.4f &&
			tick->object->object.parent_object_index == NONE)
		{
			bots_globals.stuck_events++;
			bot->stuck_count++;
			bot->unstick_until_tick = now + TICKS_PER_SECOND / 2;
			bot->unstick_direction = tick->move_direction + (bots_random_real(bot) < 0.5f ? -1.0f : 1.0f) * BOTS_PI * 0.5f;
			bot->path.valid = FALSE;
			if (bot->stuck_count >= 3)
			{
				bots_globals.stuck_giveups++;
				bot->goal_kind = _bots_goal_none;
				bot->stuck_count = 0;
				bots_pick_roam_goal(bot, &tick->position);
			}
		}
		else
			bot->stuck_count = 0;
		bot->stuck_position = tick->position;
		bot->stuck_tick = now;
	}
	if (now < bot->unstick_until_tick)
	{
		tick->move_direction = bot->unstick_direction;
		tick->moving = TRUE;
		if (now == bot->unstick_until_tick - TICKS_PER_SECOND / 2)
			tick->flags |= FLAG(_unit_control_jump_bit);
	}
}

/* ---------- falling back */

/* staying where it is: looking about (another way every second or two, by
its random numbers), or where an enemy it knows of was; a reload when no
one is about */
static void bots_bot_watch(struct bots_bot *bot, struct bots_tick *tick)
{
	real threat_yaw;

	if (bots_known_threat(bot, tick, &threat_yaw))
		bot->watch_yaw = threat_yaw;
	else if (tick->now >= bot->watch_tick)
	{
		bot->watch_yaw = bot->yaw + bots_random_signed(bot) * BOTS_PI * 0.75f;
		bot->watch_tick = tick->now + TICKS_PER_SECOND + (long)(bots_random(bot) % TICKS_PER_SECOND);
	}
	bots_turn(bot, bots_angle_difference(bot->watch_yaw, bot->yaw), -bot->pitch, 6.0f * BOTS_DEGREES, 5.0f, 1.0f,
		0.0f, 0.0f);
	if (tick->weapon_index != NONE && bots_weapon_loaded(tick->weapon_index) < 0.4f &&
		tick->now - bot->target_seen_tick > TICKS_PER_SECOND * 2)
	{
		tick->flags |= FLAG(_unit_control_weapon_reload_bit);
	}
}

/* a place to fall back to from a threat (an enemy's last place), of the
roaming places and the health packs it believes are there, 4 to 45 units
off and not towards the threat: the one of the least of its way there less
how far it is from the threat, less more for being out of the threat's
sight (the best three looked at) and for a health pack when hurt. FALSE for
none */
#define BOTS_COVER_CANDIDATES 3
static boolean bots_find_cover(struct bots_bot *bot, struct bots_tick *tick, real_point3d const *threat,
	real_point3d *point, long *surface_index)
{
	struct
	{
		real score;
		real_point3d point;
		long surface_index;
	} best[BOTS_COVER_CANDIDATES];
	long best_count = 0, index, count, chosen;
	short item_count = bots_knowledge_item_count();
	boolean hurt = tick->object->object.body_vitality < 0.7f;
	real threat_distance = bots_distance2d(threat, &tick->position);

	count = bots_globals.roam_point_count + item_count;
	for (index = 0; index < count; index++)
	{
		real_point3d candidate;
		long candidate_surface, at;
		real distance, score, away;
		boolean health = FALSE;

		if (index < bots_globals.roam_point_count)
		{
			candidate = bots_globals.roam_points[index];
			candidate_surface = bots_globals.roam_surfaces[index];
		}
		else
		{
			struct bots_item item;

			if (!bots_knowledge_item(index - bots_globals.roam_point_count, &item, NULL) ||
				item.kind != _bots_item_health ||
				!bots_knowledge_item_believed(bot->index, index - bots_globals.roam_point_count, NULL))
			{
				continue;
			}
			candidate_surface = bots_surface_below(&item.position, &candidate);
			if (candidate_surface == NONE)
				continue;
			health = TRUE;
		}
		distance = bots_distance2d(&candidate, &tick->position);
		if (distance < 4.0f || distance > 45.0f)
			continue;
		away = bots_distance2d(&candidate, threat);
		if (away < threat_distance + 2.0f)
			continue;
		score = distance * 0.4f - MIN(away, 40.0f) * 0.6f;
		if (health && hurt)
			score -= 10.0f;
		/* (in its place among the best, by score) */
		for (at = best_count; at > 0 && best[at - 1].score > score; at--)
		{
			if (at < BOTS_COVER_CANDIDATES)
				best[at] = best[at - 1];
		}
		if (at < BOTS_COVER_CANDIDATES)
		{
			best[at].score = score;
			best[at].point = candidate;
			best[at].surface_index = candidate_surface;
			if (best_count < BOTS_COVER_CANDIDATES)
				best_count++;
		}
	}
	if (!best_count)
		return FALSE;
	{
		real_point3d eye = *threat;

		eye.z += 0.6f;
		for (index = 0; index < best_count; index++)
		{
			real_point3d target = best[index].point;

			target.z += 0.6f;
			if (!bots_line_of_sight(NONE, &eye, &target, NONE))
				best[index].score -= 12.0f;
		}
	}
	chosen = 0;
	for (index = 1; index < best_count; index++)
	{
		if (best[index].score < best[chosen].score)
			chosen = index;
	}
	*point = best[chosen].point;
	*surface_index = best[chosen].surface_index;
	return TRUE;
}

/* fighting, its team falling back (bots_tactics.h): towards the rally
point, still facing the enemy, as the bots near the fight fall back
together */
static void bots_bot_fall_back(struct bots_bot *bot, struct bots_tick *tick)
{
	real_point3d point;
	short tactic = bot->smooth ? (short)_bots_tactic_none : bots_tactics_goal(bot->index, &tick->position, &point);

	if (tactic == _bots_tactic_fall_back && bots_set_tactic_goal(bot, &point))
		tick->goal_in_fight = TRUE;
	else if (bot->goal_kind == _bots_goal_tactic)
		bot->goal_kind = _bots_goal_none;
}

/* its shield down in a fight with no kill near (the enemy not low itself):
back to cover to let it come back, four seconds at most and not again for
ten; TRUE while it does (its goal the cover) */
static boolean bots_bot_recharge(struct bots_bot *bot, struct bots_tick *tick)
{
	struct player_datum *target_player;
	struct object_datum *target_object;
	long now = tick->now;

	if (now < bot->recharge_until_tick)
	{
		if (tick->object->object.shield_vitality < 0.9f &&
			bots_distance2d(&bot->cover_point, &tick->position) > 1.0f)
		{
			bots_set_goal(bot, _bots_goal_cover, &bot->cover_point, bot->cover_surface_index);
			tick->goal_in_fight = TRUE;
			return TRUE;
		}
		/* (there, or back up: on) */
		if (tick->object->object.shield_vitality >= 0.9f)
			bot->recharge_until_tick = 0;
		else
		{
			tick->stay = TRUE;
			return TRUE;
		}
	}
	if (bot->smooth || now < bot->recharge_next_tick || bot->target_player_index == NONE ||
		now - bot->target_seen_tick > TICKS_PER_SECOND / 2 || tick->object->object.shield_vitality > 0.15f ||
		tick->object->object.parent_object_index != NONE || !game_engine_has_shield(bot->player_index))
	{
		return FALSE;
	}
	target_player = player_try_and_get(bot->target_player_index);
	if (!target_player || target_player->unit_index == NONE)
		return FALSE;
	target_object = object_get(target_player->unit_index);
	/* (a kill near: on with the fight) */
	if (target_object->object.shield_vitality < 0.25f && target_object->object.body_vitality < 0.6f)
		return FALSE;
	bot->recharge_next_tick = now + TICKS_PER_SECOND * 2;
	if (!bots_find_cover(bot, tick, &bot->target_last_position, &bot->cover_point, &bot->cover_surface_index))
		return FALSE;
	bot->recharge_until_tick = now + TICKS_PER_SECOND * 4;
	bot->recharge_next_tick = now + TICKS_PER_SECOND * 10;
	bots_globals.recharges++;
	bots_set_goal(bot, _bots_goal_cover, &bot->cover_point, bot->cover_surface_index);
	tick->goal_in_fight = TRUE;
	return TRUE;
}

/* (debug.bot_trace = <seconds>) each bot's state that often */
static void bots_bot_trace(struct bots_bot *bot, long bot_index, struct bots_tick *tick)
{
	static long trace = -1;
	long now = tick->now;

	if (trace < 0)
		trace = MAX(config_integer("debug.bot_trace"), 0) * TICKS_PER_SECOND;
	if (trace <= 0 || now % trace != bot_index)
		return;
	platform_log("bots: trace %ld bot %ld at (%.1f %.1f %.1f) goal %d (%.1f %.1f %.1f) s%ld path %s %d/%d%s "
		"target %ld seen %ld ago%s stuck %d yaw %.0f throttle %.2f/%.2f flags %lx searches %ld failures %ld",
		now, bot_index + 1, tick->position.x, tick->position.y, tick->position.z, (int)bot->goal_kind,
		bot->goal_point.x, bot->goal_point.y, bot->goal_point.z, bot->goal_surface_index,
		bot->path.valid ? "yes" : "no", (int)bot->path.step_index, (int)bot->path.step_count,
		bot->path.steps_finish_path ? " (all)" : "",
		bot->target_player_index == NONE ? -1L : (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(bot->target_player_index),
		now - bot->target_seen_tick, tick->fighting ? " fighting" : "", (int)bot->stuck_count,
		bot->yaw / BOTS_DEGREES, tick->action->throttle.i, tick->action->throttle.j, tick->action->control_flags,
		bot->searches, bot->path_failures);
	if (tick->weapon_index != NONE)
	{
		struct weapon_datum *weapon = weapon_get(tick->weapon_index);

		platform_log("bots: trace weapon %s loaded %d total %d, aim off %.1f/%.1f degrees, distance %.1f",
			tag_get_name(weapon->definition_index), (int)weapon->weapon.magazines[0].rounds_loaded,
			(int)weapon->weapon.magazines[0].rounds_total, bot->aim_error_yaw / BOTS_DEGREES,
			bot->aim_error_pitch / BOTS_DEGREES, tick->target_distance);
	}
}

static void bots_bot_update(struct bots_bot *bot, long bot_index, struct player_action *action)
{
	struct bots_tick tick;
	real vitality;

	csmemset(&tick, 0, sizeof(tick));
	tick.skill = &bots_skills[bots_globals.skill];
	tick.player = player_get(bot->player_index);
	tick.now = game_time_get();
	tick.throttle_scale = 1.0f;
	tick.action = action;
	csmemset(action, 0, sizeof(*action));
	action->desired_weapon_index = NONE;
	action->desired_grenade_index = NONE;
	action->desired_zoom_level = NONE;
	action->desired_facing.yaw = bot->yaw;
	action->desired_facing.pitch = bot->pitch;

	if (tick.player->unit_index == NONE)
	{
		/* (dead: it forgets its fight and its path; the game respawns it) */
		bot->unit_index = NONE;
		bot->target_player_index = NONE;
		bot->path.valid = FALSE;
		bot->goal_kind = _bots_goal_none;
		bot->previous_control_flags = 0;
		bot->fighting = FALSE;
		bot->recharge_until_tick = 0;
		bot->combo_until_tick = 0;
		bots_release_power_claim(bot);
		return;
	}
	tick.unit = unit_get(tick.player->unit_index);
	tick.object = object_get(tick.player->unit_index);
	tick.position = tick.object->object.position;
	if (bot->unit_index != tick.player->unit_index)
	{
		/* (spawned: it looks the way its unit faces) */
		bot->unit_index = tick.player->unit_index;
		/* (debug.bot_player_weapon: the player's bot spawns with it in its
		hands, the trailer's snipes and rockets not a race to the map's
		few; debug.bots_weapon: every bot, to measure the bots' aim with
		one weapon) */
		if (*bots_weapon_wanted(bot))
			bots_give_weapon(tick.player->unit_index, bots_weapon_wanted(bot));
		/* (debug.bot_player_grenades: plasma grenades, four, for sticks) */
		if (bot->smooth && !strcmp(config_string("debug.bot_player_grenades"), "plasma"))
		{
			tick.unit = unit_get(tick.player->unit_index);
			tick.unit->unit.grenade_counts[0] = 0;
			tick.unit->unit.grenade_counts[1] = 4;
			tick.unit->unit.current_grenade_index = 1;
		}
		bot->yaw = (real)atan2(tick.object->object.forward.j, tick.object->object.forward.i);
		bot->pitch = 0.0f;
		bot->yaw_rate = 0.0f;
		bot->pitch_rate = 0.0f;
		bot->last_vitality = tick.object->object.body_vitality + tick.object->object.shield_vitality;
		bot->last_shield = tick.object->object.shield_vitality;
		bot->wander_yaw = bot->wander_pitch = bot->wander_spin = 0.0f;
		bot->wander_radius = 1.0f;
		bot->stuck_position = tick.position;
		bot->stuck_tick = tick.now;
		bot->stuck_count = 0;
		bot->goal_kind = _bots_goal_none;
		bot->next_look_tick = tick.now + (bot_index % 3);
		bot->climb_until_tick = 0;
		bot->path_short = FALSE;
		bot->escape_until_tick = 0;
		bot->last_position = tick.position;
	}
	/* (sent on by a teleporter it walked onto: a new path from where it came
	out) */
	if (bots_distance3d(&tick.position, &bot->last_position) > 3.0f && bot->goal_kind != _bots_goal_none &&
		tick.object->object.parent_object_index == NONE)
	{
		bot->path.valid = FALSE;
		bot->path_tick = tick.now - TICKS_PER_SECOND;
		bot->stuck_position = tick.position;
	}
	bot->last_position = tick.position;

	/* hurt: it turns to look for who did it, its aim knocked off */
	vitality = tick.object->object.body_vitality + tick.object->object.shield_vitality;
	if (vitality < bot->last_vitality - 0.01f)
	{
		bot->hurt_tick = tick.now;
		if (!bot->smooth)
		{
			bots_aim_flinch(bot, tick.skill, bot->last_vitality - vitality,
				bot->last_shield > 0.05f && tick.object->object.shield_vitality <= 0.0f);
		}
	}
	bot->last_vitality = vitality;
	bot->last_shield = tick.object->object.shield_vitality;
	if (!bot->smooth)
		bots_aim_form(bot, tick.now);

	bots_bot_look(bot, &tick);
	tick.weapon_index = bots_current_weapon(tick.unit);
	bots_tally_shots(bot, tick.weapon_index);
	if (bot->target_player_index != NONE && tick.now - bot->target_seen_tick <= TICKS_PER_SECOND / 2)
		bots_bot_fight(bot, &tick);
	/* the goal, and the path to it: its fall back to recharge, else its own
	(while fighting, only a weapon close by or its team's falling back:
	towards it, still facing the enemy) */
	if (!bots_bot_recharge(bot, &tick))
	{
		if (!tick.fighting || bot->goal_kind == _bots_goal_item || bot->smooth)
			bots_choose_goal(bot, bot->player_index, &tick.position, tick.fighting);
		else
			bots_bot_fall_back(bot, &tick);
	}
	if (tick.stay && !tick.fighting)
		bots_bot_watch(bot, &tick);
	else if (!tick.fighting)
	{
		bots_bot_walk(bot, &tick);
		bots_bot_yield(bot_index, &tick);
	}
	/* (a weapon close by, debug.bot_player_vehicle's vehicle, its cover or
	its team's falling back: towards it, still facing the enemy) */
	else if (!tick.stay && (bot->goal_kind == _bots_goal_item || bot->goal_kind == _bots_goal_vehicle ||
		(tick.goal_in_fight && bot->goal_kind != _bots_goal_none)))
	{
		real direction;

		if (bots_follow_path(bot, tick.player->unit_index, &tick.position, &direction))
			tick.move_direction = direction;
	}
	bot->fighting = tick.fighting;
	bots_bot_weapons(bot, &tick);
	bots_bot_unstick(bot, &tick);

	/* (driving, fighting: at the enemy, full ahead, as a Ghost's or a
	Warthog's driver charges) */
	if (bot->smooth && tick.fighting && tick.object->object.parent_object_index != NONE)
	{
		tick.moving = TRUE;
		tick.move_direction = bot->yaw;
		tick.throttle_scale = 1.0f;
	}
	if (tick.moving)
	{
		real relative = bots_angle_difference(tick.move_direction, bot->yaw);

		action->throttle.i = (real)cos(relative) * tick.throttle_scale;
		action->throttle.j = (real)sin(relative) * tick.throttle_scale;
	}
	bot->pitch = PIN(bot->pitch, -1.4f, 1.4f);
	bot->yaw = bots_angle_difference(bot->yaw, 0.0f);
	action->desired_facing.yaw = bot->yaw;
	action->desired_facing.pitch = bot->pitch;

	/* a press is one tick held and the next released (the jump, the grenade,
	the blow, the reload), as a player's buttons are; the trigger and the
	swap button may be held */
	{
		unsigned long pressed = FLAG(_unit_control_jump_bit) | FLAG(_unit_control_throw_grenade_bit) |
			FLAG(_unit_control_use_equipment_bit) | FLAG(_unit_control_weapon_reload_bit);

		tick.flags &= ~(bot->previous_control_flags & pressed);
		bot->previous_control_flags = tick.flags;
		if (TEST_FLAG(tick.flags, _unit_control_throw_grenade_bit))
			bot->throw_tick = tick.now;
		if (TEST_FLAG(tick.flags, _unit_control_use_equipment_bit))
			bot->blow_tick = tick.now;
	}
	action->control_flags = tick.flags;
	action->primary_trigger = TEST_FLAG(tick.flags, _unit_control_weapon_primary_trigger_bit) ? 1.0f : 0.0f;
	bots_bot_trace(bot, bot_index, &tick);
}

/* halo.log's line, a minute into the game and each minute on */
static void bots_report(void)
{
	long now = game_time_get();
	long count = 0, index;
	char line[400];
	char knowledge[80];
	char tactics[400];
	int length = 0;

	if (bots_globals.last_report_tick != NONE && now - bots_globals.last_report_tick < TICKS_PER_SECOND * 60)
		return;
	bots_globals.last_report_tick = now;
	line[0] = 0;
	for (index = 0; index < BOTS_MAXIMUM; index++)
	{
		struct bots_bot *bot = &bots_globals.bots[index];
		struct player_datum *player = bot->player_index != NONE ? player_try_and_get(bot->player_index) : NULL;

		if (!player)
			continue;
		count++;
		if (length < (int)sizeof(line) - 40)
			length += snprintf(line + length, sizeof(line) - (size_t)length, " %ld:%d/%d", index + 1,
				(int)player->statistics.kills[0], (int)player->statistics.deaths);
	}
	if (!count)
		return;
	bots_knowledge_report(knowledge, sizeof(knowledge));
	bots_tactics_report(tactics, sizeof(tactics));
	platform_log("bots: tick %ld, %ld bots (%s): recharges %ld, health %ld, power %ld (waited %ld), regroups %ld%s%s",
		now, count, bots_skills[bots_globals.skill].name, bots_globals.recharges, bots_globals.health_fetches,
		bots_globals.power_fetches, bots_globals.power_waits, bots_globals.regroups, knowledge, tactics);
	platform_log("bots: tick %ld, %ld bots (%s): searches %ld (%ld failed, %ld short, %ld full, %ld unavoided, "
		"%ld unsmoothed), stuck %ld (goals given up %ld), kills/deaths%s", now, count,
		bots_skills[bots_globals.skill].name, bots_globals.searches, bots_globals.search_failures,
		bots_globals.search_partials, bots_globals.search_overflows, bots_globals.search_unavoided,
		bots_globals.search_unsmoothed, bots_globals.stuck_events, bots_globals.stuck_giveups, line);
	/* the aim's tally: each family's rounds that struck of those fired (a
	round of a shotgun's pellets counts once) */
	{
		static char const *const names[NUMBER_OF_BOTS_FAMILIES] =
		{
			"other", "assault rifle", "pistol", "sniper", "explosive", "plasma",
		};
		char aim[400];
		int aim_length = 0;
		short family;

		aim[0] = 0;
		for (family = 0; family < NUMBER_OF_BOTS_FAMILIES; family++)
		{
			long shots = bots_globals.aim_shots[family], hits = bots_globals.aim_hits[family];

			if ((!shots && !hits) || aim_length >= (int)sizeof(aim) - 60)
				continue;
			aim_length += snprintf(aim + aim_length, sizeof(aim) - (size_t)aim_length, ", %s %ld/%ld (%ld%%)",
				names[family], hits, shots, shots ? hits * 100 / shots : 0L);
		}
		if (bots_globals.aim_shots[_bots_family_sniper] && aim_length < (int)sizeof(aim) - 60)
		{
			snprintf(aim + aim_length, sizeof(aim) - (size_t)aim_length, ", sniper on the head %ld/%ld",
				bots_globals.aim_head_shots, bots_globals.aim_shots[_bots_family_sniper]);
		}
		platform_log("bots: tick %ld, aim (%s): hits/rounds%s", now, bots_skills[bots_globals.skill].name,
			aim[0] ? aim : ", none");
		platform_log("bots: tick %ld, habits: combos %ld (grenade-shoot %ld, grenade-blow %ld, shoot-blow %ld), "
			"blows %ld, dodges %ld (jumps %ld), crouches %ld", now,
			bots_globals.combos[_bots_combo_grenade_shoot] + bots_globals.combos[_bots_combo_grenade_blow] +
				bots_globals.combos[_bots_combo_shoot_blow],
			bots_globals.combos[_bots_combo_grenade_shoot], bots_globals.combos[_bots_combo_grenade_blow],
			bots_globals.combos[_bots_combo_shoot_blow], bots_globals.blows, bots_globals.dodges,
			bots_globals.dodge_jumps, bots_globals.crouches);
	}
}

void bots_update_actions(struct player_action *actions)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long count = 0;

	/* (the host's alone: a client replays the bots from the host's input) */
	if (!game_engine_running() || !global_network_game_server_get() ||
		game_connection() != _game_connection_network_server)
	{
		return;
	}
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		long bot_index;
		long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);
		struct bots_bot *bot;

		if (player->local_player_index == 0 && config_boolean("debug.bot_player"))
		{
			bot_index = BOTS_MAXIMUM;
			bot = &bots_globals.player_bot;
		}
		/* (and not a bot that left the game in progress, whose datum stays
		for the scores: bots_network_player's machine may be another's now) */
		else if (player->local_player_index != NONE || player->quit_out_of_game ||
			!bots_machine_is_bot(player->network_player_data.machine_index))
		{
			continue;
		}
		else
		{
			bot_index = player->network_player_data.machine_index - BOTS_FIRST_MACHINE;
			bot = &bots_globals.bots[bot_index];
		}
		/* (the roaming places and the walkable surfaces once a game has
		bots; the skill once a tick) */
		if (!count)
		{
			if (!bots_globals.map_ready)
				bots_prepare_map();
			bots_globals.skill = bots_skill();
			bots_globals.tick_searches = 0;
			bots_paths_scan();
			bots_tally_hits(game_time_get());
			/* (what the bots know: their players, the reports on their
			way, the items at their spawns; what the teams know: the
			deaths since the last tick) */
			bots_knowledge_tick_begin();
			bots_tactics_tick_begin();
		}
		if (bot->player_index != iterator.datum_index)
			bots_bot_reset(bot, iterator.datum_index, bot_index);
		if (absolute_index >= 0 && absolute_index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS)
		{
			bots_bot_update(bot, bot_index, &actions[absolute_index]);
			if (bot != &bots_globals.player_bot)
			{
				bots_tactics_bot_done(bot_index, iterator.datum_index, bot->fighting ? bot->target_player_index : NONE,
					TEST_FLAG(actions[absolute_index].control_flags, _unit_control_weapon_primary_trigger_bit));
			}
			/* (a network game's: the clients are given it as the input the
			host's tick ran, which they replay the bot by) */
			if (bot != &bots_globals.player_bot)
				update_server_take_bot_action(absolute_index, &actions[absolute_index]);
			/* (debug.bot_player: the view turned with the bot's aim, which
			the unit follows, as a player's view leads its unit's; the
			camera is the player's control's facing, not the action's) */
			if (bot == &bots_globals.player_bot)
			{
				real_vector3d facing;

				vector3d_from_euler_angles2d(&facing, &actions[absolute_index].desired_facing);
				player_control_set_facing(0, &facing);
			}
		}
		count++;
	}
	if (count)
	{
		bots_globals.ticks++;
		bots_report();
	}
}
