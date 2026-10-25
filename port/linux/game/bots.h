/* bots.h: computer players for the multiplayer games this computer hosts
(port/linux/game/bots.c)

A bot is a player of a game this machine hosts (a local split screen game,
or a system link or internet one) whose controls come from code instead of
a controller: it is in the lobby, the scores and the game's rules as any
player is, spawns, respawns and scores. Each bot is the one player of a
machine of its own at the top of the machine indices (BOTS_FIRST_MACHINE up),
which no real machine takes (network_server_manager.c numbers the machines
that join below them): a machine with no connection, whose player the host
plays as it plays a remote player whose input comes at once. The other
machines of a network game see an ordinary player of an ordinary machine.
The bots have no view and no controller: nothing is drawn for them. */
#ifndef HALO_BOTS_H
#define HALO_BOTS_H

struct player_action;
struct network_player;

/* the most bots in one game (with one player, the Xbox's 16) */
#define BOTS_MAXIMUM 15
/* the first bot's machine index (the last machine indices of the session) */
#define BOTS_FIRST_MACHINE (HALO_PORT_MAXIMUM_NETWORK_MACHINES - BOTS_MAXIMUM)

enum
{
	/* bots.teams: in a team game, the bots on both teams (evening them
	with the players), or all on the team the players are not on */
	_bots_teams_even = 0,
	_bots_teams_against,
};

/* the settings (bots.count, bots.skill, bots.teams, bots.make_room,
bots.fill_to) */
long bots_wanted_count(void);
short bots_skill(void);
short bots_teams(void);
int bots_make_room(void);
long bots_fill_to(void);
/* the bots play the game engine's games (Slayer's, and so SWAT, Infection
and Gun Game) */
int bots_play_game_engine(long game_engine_index);

/* a machine is a bot's */
int bots_machine_is_bot(long machine_index);
/* a player (its datum) is a bot: on any machine of the game, as its machine
says (an OpenH1 machine's scores tag it) */
int bots_player_is_bot(long player_index);
/* the network player for bot number `bot_index` (0 up): its machine,
controller, name and no team or colour yet */
void bots_network_player(long bot_index, struct network_player *player);

/* the game tick (players_update_before_game): every bot's controls this
tick, into the actions of the players by their index, on the machine that
hosts the game (a network game's host gives them to its clients as it gives
a remote player's: player_queues_new.c's update_server_take_bot_action);
nothing on a client */
void bots_update_actions(struct player_action *actions);
/* the first local player's view turned toward the nearest enemy in sight
(debug.test_input's "hunt"), and how it moves (forward and strafe, -1 to
1); TRUE once on it, to fire */
boolean bots_hunt(float *forward, float *strafe);

/* a map's start and end (players.c) */
void bots_initialize_for_new_map(void);
void bots_dispose_from_old_map(void);

#endif
