/*
NETWORK_HOST_CONTROLS.H

The host's controls of a network game (network_host_controls.c): what the
pause menu's HOST CONTROLS (menu_functions.c's host_controls_menu.inc), the
scoreboard's player menu (game_engine.c) and the console (hs.c) ask of it.
*/

#ifndef __NETWORK_HOST_CONTROLS_H
#define __NETWORK_HOST_CONTROLS_H
#pragma once

/* ---------- constants */

/* what the host may do to a player (network_host_controls_player_actions:
a bit each) */
enum
{
	_host_controls_kick,
	_host_controls_ban,
	_host_controls_move_team,
	_host_controls_mute,
	NUMBER_OF_HOST_CONTROLS_ACTIONS
};

/* a player's tag on Host Controls' rows */
enum
{
	_host_controls_tag_none,
	_host_controls_tag_host,
	_host_controls_tag_bot,
	_host_controls_tag_openh1,
	_host_controls_tag_opence,
	NUMBER_OF_HOST_CONTROLS_TAGS
};

/* a restart's steps (network_host_controls_restart_step) and what each asks */
enum
{
	_host_controls_restart_none,
	/* the game ended, its scores not yet shown */
	_host_controls_restart_ending,
	/* back to the lobby, which is not yet loaded */
	_host_controls_restart_lobby,
};
enum
{
	_host_controls_restart_wait,
	/* the scores' A, the host's: every machine to the lobby */
	_host_controls_restart_to_lobby,
	/* the lobby shown in the map choice's place, its countdown let go */
	_host_controls_restart_start,
};

enum
{
	/* a line of the host's bans (bans.txt), at most, and its parts */
	HOST_CONTROLS_BAN_LINE_SIZE = 512,
	HOST_CONTROLS_BAN_WHEN_SIZE = 20,
	HOST_CONTROLS_BAN_ADDRESS_SIZE = 32,
	HOST_CONTROLS_BAN_HARDWARE_ID_SIZE = 40,
	HOST_CONTROLS_BAN_TEXT_SIZE = 96,
	/* the bans Host Controls lists, at most */
	MAXIMUM_HOST_CONTROLS_BANS = 128,
};

/* the world behind Host Controls, dimmed (pixel32: black, three quarters
over it) */
#define NETWORK_HOST_CONTROLS_BACKDROP 0xC0000000

/* ---------- structures */

/* who looks: whether Host Controls is this machine's */
struct host_controls_seat
{
	/* this machine runs the game's server */
	boolean server;
	/* game_connection() */
	short connection;
	/* a game's map is loaded and its game under way (not the menus) */
	boolean playing;
};

/* what the host knows of a player, for what it may do to them */
struct host_controls_player_view
{
	/* the host's machine's (its own players) */
	boolean own;
	/* a bot (bots.c's machines) */
	boolean bot;
	/* ... which the host may take out (its network game's: bots.c) */
	boolean bot_removable;
	/* the game has teams, and teams may move in it
	(network_host_controls_team_moves_for) */
	boolean teams;
	boolean team_moves;
};

/* what a game is, for moving its players between teams */
struct host_controls_team_view
{
	/* an OpenH1 game (network_standard_hosting): a Legacy game's machines
	hear nothing new */
	boolean standard;
	/* the gametype has teams */
	boolean teams;
	/* OpenCE machines in it, which would not hear of the move */
	short opence_machines;
};

/* a restart's world */
struct host_controls_restart_view
{
	boolean server;
	/* the game's scores shown (game_engine_showing_postgame) */
	boolean postgame;
	/* the main menu's map loaded, the server in its lobby */
	boolean menu_loaded;
	boolean pregame;
};

/* a line of bans.txt, as distributed_write_player_record writes it (when,
then "key=value" parts separated by tabs), or one a host wrote by hand */
struct host_controls_ban
{
	char when[HOST_CONTROLS_BAN_WHEN_SIZE];
	char address[HOST_CONTROLS_BAN_ADDRESS_SIZE];
	char hardware_id[HOST_CONTROLS_BAN_HARDWARE_ID_SIZE];
	char players[HOST_CONTROLS_BAN_TEXT_SIZE];
	char reason[HOST_CONTROLS_BAN_TEXT_SIZE];
};

/* a player of the game as Host Controls lists them */
struct host_controls_player
{
	/* its datum index */
	long player_index;
	wchar_t name[12];
	short team;
	long ping;
	short tag;
	unsigned long actions;
};

/* ---------- prototypes/NETWORK_HOST_CONTROLS.C */

/* the pure parts (tools/harness/tests/test_host_controls.py) */
boolean network_host_controls_shown_for(struct host_controls_seat const *seat);
unsigned long network_host_controls_actions_for(struct host_controls_player_view const *view);
boolean network_host_controls_team_moves_for(struct host_controls_team_view const *view);
char network_host_controls_requested_team(boolean locked, boolean host_machine, char requested, char current);
short network_host_controls_restart_step(short stage, struct host_controls_restart_view const *view, short *action);
short network_host_controls_maximum_step(short const *values, short count, short current, short present,
	short step);
boolean network_host_controls_ban_parse(char const *line, struct host_controls_ban *ban);

/* whether this machine hosts a network game under way (Host Controls is
its, its pause menu's and scoreboard's) */
boolean network_host_controls_available(void);
/* the game's players (but those who quit), their count */
short network_host_controls_players(struct host_controls_player *players, short maximum);
/* what the host may do to the player (datum index): a bit of each
_host_controls_* */
unsigned long network_host_controls_player_actions(long player_index);
/* (the host) the player's machine kicked or banned, as the console's kick
and ban do (network_votekick_host_kick, network_server_manager.c) */
boolean network_host_controls_kick(long player_index, boolean ban);
/* (the host) the player put on the other team (a living one's unit killed
first, without statistics, as a quit player's is): every machine told, the
game's OpenH1 machines alone hear it (network_host_controls_team_moves) */
boolean network_host_controls_move_team(long player_index);
/* whether teams may move in this game */
boolean network_host_controls_team_moves(void);
/* (the host) the players' own team changes in the lobby refused (but the
host machine's) */
boolean network_host_controls_teams_locked(void);
void network_host_controls_lock_teams(boolean locked);
/* (the host: network_server_message_handler.c) the team a lobby's player
settings ask for, as the lock keeps it */
char network_host_controls_lobby_team(boolean host_machine, char requested, char current);
/* (the host) the game ended now, as its time limit ends it; or ended and
started again on its map and gametype, through the lobby */
boolean network_host_controls_end_game(void);
boolean network_host_controls_restart(void);
boolean network_host_controls_restarting(void);
/* (the host) the bans (bans.txt), read again; one taken out (its line),
and its address no longer kept out */
short network_host_controls_bans(struct host_controls_ban *bans, char (*lines)[HOST_CONTROLS_BAN_LINE_SIZE],
	short maximum);
boolean network_host_controls_unban(char const *line);
/* (the host) OpenCE players may join the game from now on (an OpenH1 game's) */
boolean network_host_controls_opence_joins(void);
void network_host_controls_set_opence_joins(boolean allowed);
/* the bots' rows of Game (bots.c's host-owned bots of a network game:
BOTS, BOT SKILL, BOT TEAMS, MAKE ROOM FOR PLAYERS and, with it on, FILL TO;
none in a gametype the bots do not play), their texts, and a row's value
stepped (left -1, right or A 1), written to config.toml */
short network_host_controls_bot_rows(void);
void network_host_controls_bot_row(short row, wchar_t *label, wchar_t *value, wchar_t *help, short size);
void network_host_controls_bot_row_step(short row, short step);
/* Host Controls opened on the Players tab, the player chosen (NONE: none),
for the local player (the scoreboard's "Host controls...", the console's
host_controls) */
boolean network_host_controls_open(long player_index, short tab);
/* the console's host_controls: its tab ("players", "game", "bans"; none:
Players) and a player chosen by their name's start; or "end" and "restart",
Game's End game now and Restart round; or "unban <players or address>" */
boolean network_host_controls_open_command(char const *text);
/* whether Host Controls is shown now (its update ran a moment ago): the
HUD hidden under it, the world dimmed (interface.c) */
boolean network_host_controls_shown(void);
/* (the screen's update, each frame) the player (NONE: none) and tab an
opening asked for, taken once; FALSE: none asked */
boolean network_host_controls_take_opening(long *player_index, short *tab);
/* each frame (main.c): a restart's steps, the lock forgotten once the
server goes */
void network_host_controls_update(void);
/* the team-move message (an OpenH1 game's: network_distributed.c) */
boolean network_host_controls_handles_message(word const *message, word size);
void network_host_controls_handle_message(long machine_index, word const *message, word size, boolean from_stream);

#endif // __NETWORK_HOST_CONTROLS_H
