/*
MATCHMAKING.C

Matchmaking ("find me a Team Slayer game") without a server of OpenH1's
own: the public listings (port/linux/src/p2p_lobby.c) and the signalling
brokers are all it uses. A playlist (matchmaking_playlists: its gametypes,
maps, team size, the most players and the most a party may bring) is
searched by a party's leader, or a player in no party, from the Matchmaking
screen (matchmaking_menu.inc), for the whole party; the members follow their
leader as always (party_menu.inc), and the game's host holds room for them
(network_party.c).

- Looking: the public games are read (p2p_lobby_browse) for
  MATCHMAKING_LOOK_MILLISECONDS, while their hosts' pings are measured, then
  each second the best is picked (matchmaking_pick): an OpenH1 game of this
  build hosted for the playlist (the listing's stamp says which), open, with
  no password, with room for the whole party, not one this machine failed to
  join; of those the lowest ping (in steps of MATCHMAKING_PING_STEP, an
  unknown one taken as MATCHMAKING_UNKNOWN_PING), then the fullest, a lobby
  before a game under way. Nothing about skill is weighed.
- Reaching and joining: the game's invite reached (p2p_join_listing), its
  game joined as the system link list joins one
  (network_game_client_join_available_game), the lobby opened. A host not
  reached, a join refused, is marked failed (p2p_lobby_mark_failed) and the
  search looks again.
- Hosting: when no game fits for MATCHMAKING_SEARCH_MILLISECONDS (and a
  little more, at random, so that two parties searching at once do not both
  host), the party's leader passes the lead to the member that can host best
  (p2p_party_pass_host: a member in the menus whose network is not strict,
  when the leader's is), which then hosts as below and the others follow
  (MATCHMAKING_HANDOFF_MILLISECONDS for it to take the lead, else the leader
  hosts after all); else it hosts itself: an OpenH1 internet game, public,
  listed for the playlist (p2p_set_game_listing_playlist), named after it, of
  its most players, on one of its maps at random with its gametype. Those
  searching find it as above. It starts by itself MATCHMAKING_FULL_MILLISECONDS
  after it is full, or once it has waited MATCHMAKING_FILL_MILLISECONDS: with
  bots filling it (bots_set_matchmaking) in a gametype they play, else with
  the players there are if they are its playlist's least; else it waits on.
  A member asked to host by its leader (p2p_party_take_host_request) hosts
  the same way.
- Following: a leader that passed the lead follows its new leader's game as
  a member does, by its invite.
A Legacy game, or one of an older OpenH1, is never picked (the listing says
no playlist), so a search never pulls in a machine of a Legacy game; a game
hosted here takes OpenCE's players as any OpenH1 game does
(network.allow_opence). Everything the search decides is
matchmaking_step's, matchmaking_pick's and matchmaking_start_choice's,
tools/harness/tests/test_matchmaking.py's.
*/

#include "cseries.h"
#include "game_modes.h"
#include "main/main.h"
#include "interface/player_ui.h"
#include "interface/ui_widget.h"
#include "saved games/player_profile.h"
#include "networking/network_game_globals.h"
#include "networking/network_client_manager.h"
#include "networking/network_server_manager.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "text/unicode.h"
#include "bots.h"
#include "matchmaking.h"
#include "../src/p2p.h" /* port: port/linux/src/p2p.c, p2p_lobby.c, p2p_party.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the platform layer's (port_config.c, sdl_platform.c) */
char const *config_string(char const *name);
int config_boolean(char const *name);
void platform_log(char const *format, ...);
unsigned long system_milliseconds(void);
/* ui_widget_event_handler_functions.c's: a profile's player in a
multiplayer game */
boolean ui_widget_port_multiplayer_player(short controller_index, long profile_index);

/* ---------- constants */

enum
{
	MATCHMAKING_MAXIMUM_MAPS = 9,
	MATCHMAKING_MAXIMUM_VARIANTS = 2,
	/* the public games read at once */
	MATCHMAKING_MAXIMUM_GAMES = 64,

	/* milliseconds: the listings and their pings gathered before a game is
	picked; then games picked each MATCHMAKING_PICK_MILLISECONDS; no game
	found, hosting after this and up to MATCHMAKING_SEARCH_JITTER more */
	MATCHMAKING_LOOK_MILLISECONDS = 6000,
	MATCHMAKING_PICK_MILLISECONDS = 1000,
	MATCHMAKING_SEARCH_MILLISECONDS = 20000,
	MATCHMAKING_SEARCH_JITTER = 15000,
	/* a host reached and its game found in this long, joined in this long */
	MATCHMAKING_REACH_MILLISECONDS = 15000,
	MATCHMAKING_JOIN_MILLISECONDS = 20000,
	/* the member asked to host leads in this long, else this machine hosts */
	MATCHMAKING_HANDOFF_MILLISECONDS = 10000,
	/* a hosted game: set up this long after it is made (fast setup clears
	its map), its own player added this long after; it starts this long
	after it is full, or after waiting this long, with bots after this long
	more */
	MATCHMAKING_SETUP_MILLISECONDS = 1000,
	MATCHMAKING_PLAYER_MILLISECONDS = 2000,
	MATCHMAKING_FULL_MILLISECONDS = 3000,
	MATCHMAKING_FILL_MILLISECONDS = 90000,
	MATCHMAKING_BOTS_MILLISECONDS = 5000,
	/* a server that never came */
	MATCHMAKING_HOST_MILLISECONDS = 10000,

	/* pings compared in steps of this many milliseconds; one not measured
	taken as this */
	MATCHMAKING_PING_STEP = 50,
	MATCHMAKING_UNKNOWN_PING = 250,
};

/* what a step does as it goes on (matchmaking_step) */
enum
{
	_matchmaking_action_none,
	/* the game picked: its host reached */
	_matchmaking_action_reach,
	/* the host reached: its game joined */
	_matchmaking_action_join,
	/* the game not reached or joined: marked failed, others looked at */
	_matchmaking_action_failed,
	/* no game found: the lead passed to a better host, else hosted here */
	_matchmaking_action_pass,
	_matchmaking_action_host,
	/* (following) the new leader's game reached */
	_matchmaking_action_follow,
	/* in the game: the search over */
	_matchmaking_action_done,
	/* the search given up (a server that never came) */
	_matchmaking_action_give_up,
};

/* how a hosted game starts (matchmaking_start_choice) */
enum
{
	_matchmaking_start_wait,
	_matchmaking_start_now,
	_matchmaking_start_bots,
};

/* the client's states (network_client_manager.c's) */
enum
{
	_matchmaking_client_searching,
	_matchmaking_client_joining,
	_matchmaking_client_pregame,
};

/* ---------- structures */

struct matchmaking_playlist
{
	/* the listing's name (ASCII), the menus' name and what it is */
	char const *name;
	wchar_t const *title;
	wchar_t const *about;
	/* its gametypes (debug.network_test's names: game_modes.c's, else the
	built-in ones', game_engine_get_variant_by_name), one at random */
	char const *variants[MATCHMAKING_MAXIMUM_VARIANTS];
	char const *maps[MATCHMAKING_MAXIMUM_MAPS];
	/* a team's players (0: everyone for themselves), the game's most, the
	least it starts with without bots, and the most a party brings */
	short team_size;
	short players;
	short minimum;
	short party_most;
};

/* what a step is made from (matchmaking_step) */
struct matchmaking_view
{
	short stage;
	/* milliseconds in this stage, and since the search began; the search's
	time before hosting */
	unsigned long waited;
	unsigned long searched;
	unsigned long search_time;
	/* the best game, NONE for none (matchmaking_pick) */
	short best;
	/* p2p_join_status's stage; the game joined (its join begun, the join
	call taken); this machine in a game (the lobby or later); its client
	back to searching */
	short join_stage;
	boolean entered;
	boolean in_game;
	boolean in_none;
	/* this machine leads its party (or is in none), and the party's leader's
	game is an internet game to follow (not this machine's); this machine
	hosts */
	boolean leading;
	boolean leader_game;
	boolean hosting;
	/* (reaching, joining) the game is the new leader's, followed */
	boolean following;
};

/* ---------- globals */

static struct matchmaking_playlist const matchmaking_playlists[] =
{
	{ "Team Slayer", L"TEAM SLAYER", L"4 against 4. The team with the most kills wins.",
		{ "team_slayer", NULL },
		{ "beavercreek", "carousel", "chillout", "damnation", "hangemhigh", "longest", "prisoner", "ratrace",
			"wizard" }, 4, 8, 2, 4 },
	{ "Slayer", L"SLAYER", L"Every player for themselves, 8 at most.",
		{ "slayer", NULL },
		{ "chillout", "damnation", "hangemhigh", "longest", "prisoner", "ratrace", "wizard", "carousel", NULL },
		0, 8, 2, 2 },
	{ "CTF", L"CAPTURE THE FLAG", L"4 against 4: take their flag home.",
		{ "ctf", NULL },
		{ "beavercreek", "bloodgulch", "boardingaction", "damnation", "sidewinder", "carousel", NULL, NULL, NULL },
		4, 8, 2, 4 },
	{ "SWAT", L"SWAT", L"Pistols and rifles, no shields, one shot to the head.",
		{ "swat", NULL },
		{ "beavercreek", "chillout", "damnation", "hangemhigh", "longest", "prisoner", "wizard", NULL, NULL },
		4, 8, 2, 4 },
	{ "Big Team", L"BIG TEAM BATTLE", L"8 against 8 on the big maps, vehicles and all.",
		{ "team_slayer", "ctf" },
		{ "bloodgulch", "sidewinder", "boardingaction", NULL, NULL, NULL, NULL, NULL, NULL }, 8, 16, 4, 8 },
};

static struct
{
	short stage;
	short playlist;
	short controller;
	unsigned long begin_time;
	unsigned long stage_time;
	unsigned long search_time;
	unsigned long pick_time;
	short problem;
	/* the game picked (reaching, joining): its listing (or, following, its
	host's identifier and the order of the leader's game) */
	struct p2p_listing game;
	byte host[6];
	unsigned long order;
	boolean entered;
	/* the last read: the playlist's games and those with room */
	short games, games_with_room;
	/* hosting: when the server was asked for, whether it was set up and its
	player added, the bots asked for, when it was full, the variant's game
	engine, and its listing's playlist (0: none set) */
	unsigned long host_time;
	boolean set_up;
	boolean player_added;
	boolean bots_asked;
	unsigned long bots_time;
	unsigned long full_time;
	long game_engine;
	/* its map's name (a multiplayer map's: "carousel") */
	char map[32];
	short listed_playlist;
	/* debug.matchmaking taken */
	boolean debug_checked;
} matchmaking = { _matchmaking_idle, NONE };

/* ---------- the playlists */

short matchmaking_playlist_count(
	void)
{
	return (short)NUMBEROF(matchmaking_playlists);
}

void matchmaking_playlist_text(
	short playlist,
	wchar_t *name,
	short name_size,
	wchar_t *about,
	short about_size)
{
	struct matchmaking_playlist const *entry = VALID_INDEX(playlist, NUMBEROF(matchmaking_playlists)) ?
		&matchmaking_playlists[playlist] : NULL;

	if (name && name_size > 0)
	{
		ustrncpy(name, entry ? entry->title : L"", name_size - 1);
		name[name_size - 1] = 0;
	}
	if (about && about_size > 0)
	{
		ustrncpy(about, entry ? entry->about : L"", about_size - 1);
		about[about_size - 1] = 0;
	}
}

/* ---------- what the search decides */

/* a public game's worth to a party of party_size looking for a game of the
playlist (its listing's number: 1 up): NONE if it cannot take the party (a
game of no playlist or another's, not an OpenH1 game this build joins as
one, closed, with a password, no room for the whole party, its join failed
this run), else the higher the better: the lowest ping (in steps of
MATCHMAKING_PING_STEP, an unknown one MATCHMAKING_UNKNOWN_PING), then the
fullest, a lobby before a game under way */
static long matchmaking_game_score(
	struct p2p_listing const *game,
	short playlist,
	short party_size)
{
	long ping;

	if (game->playlist != playlist || !playlist || !game->standard || !game->open || game->locked || game->failed ||
		!game->invite[0])
	{
		return NONE;
	}
	if (game->maximum_player_count > 0 && game->player_count + party_size > game->maximum_player_count)
		return NONE;
	ping = game->ping >= 0 ? game->ping : MATCHMAKING_UNKNOWN_PING;
	return 100000L - (ping / MATCHMAKING_PING_STEP) * 1000L + game->player_count * 10L + (game->in_progress ? 0 : 5);
}

/* the best of the games for the party (matchmaking_game_score), NONE if
none can take it */
static short matchmaking_pick(
	struct p2p_listing const *games,
	short count,
	short playlist,
	short party_size)
{
	short best = NONE;
	long best_score = NONE;
	short index;

	for (index = 0; index < count; index++)
	{
		long score = matchmaking_game_score(&games[index], playlist, party_size);

		if (score != NONE && score > best_score)
		{
			best = index;
			best_score = score;
		}
	}
	return best;
}

/* why this machine may not search a playlist (_matchmaking_problem_none: it
may): internet play off, a party's member (its leader searches), a party
bigger than the playlist takes */
static short matchmaking_search_problem(
	boolean online,
	boolean member,
	short party_size,
	short party_most)
{
	if (!online)
		return _matchmaking_problem_offline;
	if (member)
		return _matchmaking_problem_member;
	if (party_size > party_most)
		return _matchmaking_problem_party_too_big;
	return _matchmaking_problem_none;
}

/* how a hosted game of the playlist starts, with humans of its most in it
(people, the bots apart), having waited so long since it was made: now
once it is full; after MATCHMAKING_FILL_MILLISECONDS with bots filling it,
if they play its gametype (bots_play), else now if it has its least players;
else it waits */
static short matchmaking_start_choice(
	short humans,
	short most,
	short minimum,
	unsigned long waited,
	boolean bots_play)
{
	if (most > 0 && humans >= most)
		return _matchmaking_start_now;
	if (waited < MATCHMAKING_FILL_MILLISECONDS)
		return _matchmaking_start_wait;
	if (bots_play)
		return _matchmaking_start_bots;
	return humans >= minimum ? _matchmaking_start_now : _matchmaking_start_wait;
}

/* a search's next stage, and what to do as it goes on to it (action) */
static short matchmaking_step(
	struct matchmaking_view const *view,
	short *action)
{
	*action = _matchmaking_action_none;
	switch (view->stage)
	{
	case _matchmaking_looking:
		/* (the lead taken by another: its game followed) */
		if (!view->leading)
			return _matchmaking_following;
		if (view->best != NONE && view->searched >= MATCHMAKING_LOOK_MILLISECONDS)
		{
			*action = _matchmaking_action_reach;
			return _matchmaking_reaching;
		}
		if (view->searched >= view->search_time)
		{
			*action = _matchmaking_action_pass;
			return _matchmaking_handing;
		}
		return _matchmaking_looking;
	case _matchmaking_reaching:
		if (view->join_stage == _p2p_join_reached && view->entered)
			return _matchmaking_joining;
		if (view->join_stage == _p2p_join_reached && !view->entered && view->waited <= MATCHMAKING_REACH_MILLISECONDS)
		{
			*action = _matchmaking_action_join;
			return _matchmaking_reaching;
		}
		if ((view->join_stage == _p2p_join_idle && view->waited > 1000) || view->waited > MATCHMAKING_REACH_MILLISECONDS)
		{
			*action = _matchmaking_action_failed;
			return view->following ? _matchmaking_following : _matchmaking_looking;
		}
		return _matchmaking_reaching;
	case _matchmaking_joining:
		if (view->in_game)
		{
			*action = _matchmaking_action_done;
			return _matchmaking_idle;
		}
		if ((view->in_none && view->waited > 2000) || view->waited > MATCHMAKING_JOIN_MILLISECONDS)
		{
			*action = _matchmaking_action_failed;
			return view->following ? _matchmaking_following : _matchmaking_looking;
		}
		return _matchmaking_joining;
	case _matchmaking_handing:
		/* (the member asked leads: its game followed) */
		if (!view->leading)
			return _matchmaking_following;
		if (view->waited > MATCHMAKING_HANDOFF_MILLISECONDS)
		{
			*action = _matchmaking_action_host;
			return _matchmaking_hosting;
		}
		return _matchmaking_handing;
	case _matchmaking_following:
		if (view->in_game)
		{
			*action = _matchmaking_action_done;
			return _matchmaking_idle;
		}
		if (view->leader_game)
		{
			*action = _matchmaking_action_follow;
			return _matchmaking_reaching;
		}
		/* (the lead back here: looked for again) */
		if (view->leading)
			return _matchmaking_looking;
		return _matchmaking_following;
	case _matchmaking_hosting:
		if (!view->hosting && view->waited > MATCHMAKING_HOST_MILLISECONDS)
		{
			*action = _matchmaking_action_give_up;
			return _matchmaking_idle;
		}
		return _matchmaking_hosting;
	}
	return _matchmaking_idle;
}

/* ---------- this machine */

/* the client's state, NONE without a client */
static short matchmaking_client_state(
	void)
{
	struct network_game_client *client = global_network_game_client_get();

	return client ? network_game_client_get_state(client, NULL) : NONE;
}

/* in the menus, in no game (a client looking for games at most) */
static boolean matchmaking_in_menus(
	void)
{
	short state = matchmaking_client_state();

	return main_menu_is_active() && !global_network_game_server_get() &&
		(state == NONE || state == _matchmaking_client_searching);
}

/* this machine's party, as the search counts it: its size (its members of
this version, itself too; 1 in no party), and whether this machine is a
member of it, not its leader */
static short matchmaking_party(
	boolean *member)
{
	struct p2p_party_status status;
	short count = 0;
	short index;

	p2p_party_status(&status);
	*member = status.state != _p2p_party_none && !(status.state == _p2p_party_member && status.leader);
	if (status.state != _p2p_party_member)
		return 1;
	for (index = 0; index < status.count; index++)
	{
		if (!status.members[index].other_version)
			count++;
	}
	return (short)MAX(count, 1);
}

/* the player of the search's controller in the multiplayer game, by its
profile (an automated test's machine, which has none, by none:
debug.matchmaking) */
static boolean matchmaking_player(
	void)
{
	long profile_index = player_ui_get_active_player_profile_index(0);
	short controller = matchmaking.controller >= 0 && matchmaking.controller < 4 ? matchmaking.controller : 0;

	if (profile_index == NONE)
		profile_index = player_ui_get_player1_last_used_profile_index();
	if (profile_index == NONE)
	{
		word count = 1;

		player_profiles_enumerate_available_to_local_player_index(NONE, &count, &profile_index, FALSE);
		if ((short)count <= 0)
			profile_index = NONE;
	}
	if (profile_index != NONE && ui_widget_port_multiplayer_player(controller, profile_index))
		return TRUE;
	if (config_string("debug.matchmaking")[0])
	{
		player_ui_local_player_joined_multiplayer_game(0);
		return TRUE;
	}
	return FALSE;
}

/* a client looking for games, for the host reached to be found by */
static void matchmaking_client(
	void)
{
	if (!global_network_game_client_get() && create_global_network_game_client())
		game_connection_set(_game_connection_network_client);
}

/* the stage set, its clock begun */
static void matchmaking_stage(
	short stage)
{
	matchmaking.stage = stage;
	matchmaking.stage_time = system_milliseconds();
}

/* the search over, why (_matchmaking_problem_*) */
static void matchmaking_end(
	short problem)
{
	if (matchmaking.stage == _matchmaking_reaching)
		p2p_join_cancel();
	matchmaking.problem = problem;
	matchmaking_stage(_matchmaking_idle);
}

/* the playlist's game hosted here: an OpenH1 internet game, public, named
after it, of its most players; its map, gametype and listing set once its
server is made (matchmaking_host_update) */
static boolean matchmaking_host(
	void)
{
	struct matchmaking_playlist const *playlist = &matchmaking_playlists[matchmaking.playlist];
	short maps = 0;
	unsigned long pick = system_milliseconds() ^ ((unsigned long)p2p_identifier()[5] << 8);

	if (!matchmaking_player())
	{
		matchmaking_end(_matchmaking_problem_no_profile);
		return FALSE;
	}
	while (maps < MATCHMAKING_MAXIMUM_MAPS && playlist->maps[maps])
		maps++;
	dispose_global_network_game_client();
	dispose_global_network_game_server();
	p2p_set_hosting_allowed(TRUE);
	p2p_set_hosting_public(TRUE);
	{
		wchar_t name[MATCHMAKING_PLAYLIST_NAME_SIZE];
		short index;

		for (index = 0; index < MATCHMAKING_PLAYLIST_NAME_SIZE - 1 && playlist->name[index]; index++)
			name[index] = (wchar_t)playlist->name[index];
		name[index] = 0;
		network_game_server_port_set_settings(name, playlist->players);
	}
	snprintf(matchmaking.map, sizeof(matchmaking.map), "%s", playlist->maps[pick % MAX(maps, 1)]);
	main_set_multiplayer_map_name(matchmaking.map);
	player_ui_fast_setup_network_server();
	matchmaking.host_time = system_milliseconds();
	matchmaking.set_up = matchmaking.player_added = matchmaking.bots_asked = FALSE;
	matchmaking.full_time = 0;
	matchmaking.game_engine = NONE;
	platform_log("matchmaking: hosting a %s game", playlist->name);
	return TRUE;
}

/* the hosted game's set up (its map and gametype), its own player, and its
start (matchmaking_start_choice) */
static void matchmaking_host_update(
	void)
{
	/* (the clock read now: the host may have been made this frame) */
	unsigned long now = system_milliseconds();
	struct matchmaking_playlist const *playlist = &matchmaking_playlists[matchmaking.playlist];
	struct network_game_server *server = global_network_game_server_get();
	struct network_game_client *client = global_network_game_client_get();
	long bots = 0, most = 0, humans;

	if (!server || !client)
		return;
	if (!matchmaking.set_up && now - matchmaking.host_time >= MATCHMAKING_SETUP_MILLISECONDS)
	{
		unsigned long pick = now ^ ((unsigned long)p2p_identifier()[4] << 8);
		char const *name = playlist->variants[playlist->variants[1] ? pick % MATCHMAKING_MAXIMUM_VARIANTS : 0];
		char path[128];
		struct game_variant variant;
		struct game_variant_options options;
		boolean opence = game_modes_gametype_by_name(name, &variant, &options);

		snprintf(path, sizeof(path), "levels\\test\\%s\\%s", matchmaking.map, matchmaking.map);
		network_game_server_change_map_name(server, path);
		if (!opence)
			variant = *game_engine_get_variant_by_name(&variant, name);
		player_ui_set_game_variant(&variant);
		if (opence)
			player_ui_set_game_variant_options(&options);
		network_game_server_change_game_variant(server, &variant);
		matchmaking.game_engine = variant.game_engine_index;
		matchmaking.listed_playlist = (short)(matchmaking.playlist + 1);
		p2p_set_game_listing_playlist(matchmaking.listed_playlist);
		matchmaking.set_up = TRUE;
		platform_log("matchmaking: %s on %s, listed for the playlist", name, matchmaking.map);
	}
	if (!matchmaking.player_added && now - matchmaking.host_time >= MATCHMAKING_PLAYER_MILLISECONDS)
	{
		matchmaking.player_added = TRUE;
		if (network_game_client_get_local_machine_index() != NONE && !network_game_server_port_bots_humans(server,
			NULL, NULL))
		{
			network_game_client_add_player(client, 0);
		}
	}
	if (!matchmaking.set_up)
		return;
	humans = network_game_server_port_bots_humans(server, &bots, &most);
	if (humans >= most && most > 0)
	{
		if (!matchmaking.full_time)
			matchmaking.full_time = now | 1;
	}
	else
		matchmaking.full_time = 0;
	switch (matchmaking_start_choice((short)humans, (short)most, playlist->minimum, now - matchmaking.host_time,
		bots_play_game_engine(matchmaking.game_engine)))
	{
	case _matchmaking_start_now:
		if (matchmaking.full_time && now - matchmaking.full_time < MATCHMAKING_FULL_MILLISECONDS)
			break;
		platform_log("matchmaking: starting the %s game (%ld players, %ld bots)", playlist->name, humans, bots);
		network_game_client_request_immediate_start();
		matchmaking_end(_matchmaking_problem_joined);
		break;
	case _matchmaking_start_bots:
		if (!matchmaking.bots_asked)
		{
			matchmaking.bots_asked = TRUE;
			matchmaking.bots_time = now;
			bots_set_matchmaking(MAX(most - humans, 0), most);
			platform_log("matchmaking: bots fill the %s game (%ld players of %ld)", playlist->name, humans, most);
		}
		else if (now - matchmaking.bots_time >= MATCHMAKING_BOTS_MILLISECONDS)
		{
			platform_log("matchmaking: starting the %s game (%ld players, %ld bots)", playlist->name, humans, bots);
			network_game_client_request_immediate_start();
			matchmaking_end(_matchmaking_problem_joined);
		}
		break;
	}
}

/* the identifier of the host of an invite link (its XNADDR's abEnet:
p2p.c's p2p_identifier_from_hash of the link's key hash) */
static boolean matchmaking_link_host(
	char const *link,
	byte *host)
{
	char const *start = strstr(link, "halo://join/");
	short index;

	if (!start)
		return FALSE;
	start += strlen("halo://join/");
	for (index = 0; index < 12; index++)
	{
		int digit = start[index];

		digit = digit >= '0' && digit <= '9' ? digit - '0' : (digit | 0x20) >= 'a' && (digit | 0x20) <= 'f' ?
			(digit | 0x20) - 'a' + 10 : -1;
		if (digit < 0)
			return FALSE;
		host[index / 2] = (byte)(index & 1 ? host[index / 2] | digit : digit << 4);
	}
	host[0] = (byte)((host[0] & 0xFC) | 0x02);
	return TRUE;
}

/* the playlist's games read (into games, their count), and the best for
the party picked */
static short matchmaking_read(
	struct p2p_listing *games,
	short *count,
	short party_size)
{
	short index;

	*count = (short)p2p_lobby_games(games, MATCHMAKING_MAXIMUM_GAMES);
	matchmaking.games = matchmaking.games_with_room = 0;
	for (index = 0; index < *count; index++)
	{
		if (games[index].playlist != matchmaking.playlist + 1)
			continue;
		matchmaking.games++;
		if (matchmaking_game_score(&games[index], (short)(matchmaking.playlist + 1), party_size) != NONE)
			matchmaking.games_with_room++;
	}
	return matchmaking_pick(games, *count, (short)(matchmaking.playlist + 1), party_size);
}

/* ---------- public code */

short matchmaking_can_search(
	short playlist)
{
	boolean member;
	short size = matchmaking_party(&member);

	if (!VALID_INDEX(playlist, NUMBEROF(matchmaking_playlists)))
		return _matchmaking_problem_cancelled;
	return matchmaking_search_problem(config_boolean("network.online") && config_boolean("network.public_lobby"),
		member, size, matchmaking_playlists[playlist].party_most);
}

boolean matchmaking_search(
	short playlist,
	short controller)
{
	short problem = matchmaking_can_search(playlist);
	unsigned long jitter = (system_milliseconds() * 2654435761UL) ^ ((unsigned long)p2p_identifier()[5] << 16);

	if (matchmaking.stage != _matchmaking_idle && matchmaking.stage != _matchmaking_looking)
		return FALSE;
	matchmaking.problem = problem;
	if (problem != _matchmaking_problem_none)
		return FALSE;
	matchmaking.playlist = playlist;
	matchmaking.controller = controller;
	matchmaking.begin_time = system_milliseconds();
	matchmaking.search_time = MATCHMAKING_SEARCH_MILLISECONDS + (jitter >> 8) % MATCHMAKING_SEARCH_JITTER;
	matchmaking.pick_time = 0;
	matchmaking.games = matchmaking.games_with_room = 0;
	p2p_lobby_browse(TRUE);
	matchmaking_client();
	matchmaking_stage(_matchmaking_looking);
	platform_log("matchmaking: looking for a %s game (hosting one in %lu seconds if none is found)",
		matchmaking_playlists[playlist].name, matchmaking.search_time / 1000);
	return TRUE;
}

void matchmaking_cancel(
	void)
{
	if (matchmaking.stage == _matchmaking_idle)
		return;
	platform_log("matchmaking: cancelled");
	/* (a hosted game stays, not started by itself: its host's own now) */
	bots_set_matchmaking(NONE, 0);
	matchmaking_end(_matchmaking_problem_cancelled);
}

void matchmaking_status(
	struct matchmaking_status *status)
{
	boolean member;

	csmemset(status, 0, sizeof(*status));
	status->stage = matchmaking.stage;
	status->playlist = matchmaking.stage != _matchmaking_idle || matchmaking.problem ? matchmaking.playlist : NONE;
	status->elapsed = matchmaking.stage != _matchmaking_idle ? system_milliseconds() - matchmaking.begin_time : 0;
	status->party_size = matchmaking_party(&member);
	status->games = matchmaking.games;
	status->games_with_room = matchmaking.games_with_room;
	status->problem = matchmaking.problem;
	snprintf(status->host_name, sizeof(status->host_name), "%s", matchmaking.game.name);
	if (matchmaking.stage == _matchmaking_hosting && global_network_game_server_get())
	{
		long most = 0;
		unsigned long waited = system_milliseconds() - matchmaking.host_time;

		status->players = (short)network_game_server_port_bots_humans(global_network_game_server_get(), NULL, &most);
		status->most_players = (short)most;
		status->start_in = waited < MATCHMAKING_FILL_MILLISECONDS ? MATCHMAKING_FILL_MILLISECONDS - waited : 0;
	}
}

void matchmaking_update(
	void)
{
	static struct p2p_listing games[MATCHMAKING_MAXIMUM_GAMES];
	unsigned long now = system_milliseconds();
	struct matchmaking_view view;
	boolean member;
	short party_size, count = 0, stage, action;
	int request;

	if (!game_time_initialized() || !config_boolean("network.online"))
		return;
	/* (what the party is told of how well this machine can host; a test's
	machine as debug.matchmaking_host says, in the menus) */
	{
		char const *forced = config_string("debug.matchmaking_host");

		p2p_party_set_host_quality(!matchmaking_in_menus() ? _p2p_party_host_unable :
			!strcmp(forced, "strict") ? _p2p_party_host_strict : !strcmp(forced, "open") ? _p2p_party_host_open :
			p2p_nat_strict() ? _p2p_party_host_strict : _p2p_party_host_open);
	}
	/* (the listing's playlist and the bots of a game no longer hosted) */
	if (matchmaking.listed_playlist && !global_network_game_server_get())
	{
		matchmaking.listed_playlist = 0;
		p2p_set_game_listing_playlist(0);
		bots_set_matchmaking(NONE, 0);
	}
	/* debug.matchmaking: an automated test's search, of a playlist by its
	name or number (1 up), once the main menu is up */
	if (!matchmaking.debug_checked && matchmaking_in_menus())
	{
		char const *text = config_string("debug.matchmaking");
		short index;

		matchmaking.debug_checked = TRUE;
		for (index = 0; *text && index < (short)NUMBEROF(matchmaking_playlists); index++)
		{
			if (!_stricmp(text, matchmaking_playlists[index].name) || atoi(text) == index + 1)
			{
				matchmaking_search(index, 0);
				break;
			}
		}
	}
	/* a member asked by its leader to host for the party */
	request = p2p_party_take_host_request();
	if (request > 0 && request <= (int)NUMBEROF(matchmaking_playlists) && matchmaking_in_menus())
	{
		matchmaking.playlist = (short)(request - 1);
		matchmaking.begin_time = now;
		matchmaking.controller = 0;
		platform_log("matchmaking: the party's leader asked this machine to host for the party");
		if (matchmaking_host())
			matchmaking_stage(_matchmaking_hosting);
		return;
	}
	if (matchmaking.stage == _matchmaking_idle)
		return;
	party_size = matchmaking_party(&member);
	csmemset(&view, 0, sizeof(view));
	view.stage = matchmaking.stage;
	view.waited = now - matchmaking.stage_time;
	view.searched = now - matchmaking.begin_time;
	view.search_time = matchmaking.search_time;
	view.best = NONE;
	view.leading = !member;
	view.entered = matchmaking.entered;
	view.hosting = global_network_game_server_get() != NULL;
	view.in_game = matchmaking_client_state() >= _matchmaking_client_pregame && !view.hosting;
	view.in_none = matchmaking_client_state() == NONE || matchmaking_client_state() == _matchmaking_client_searching;
	view.following = matchmaking.order != 0;
	{
		struct p2p_join_status join;

		p2p_join_status(&join);
		view.join_stage = (short)join.stage;
	}
	if (matchmaking.stage == _matchmaking_looking && (!matchmaking.pick_time ||
		now - matchmaking.pick_time >= MATCHMAKING_PICK_MILLISECONDS))
	{
		matchmaking.pick_time = now | 1;
		view.best = matchmaking_read(games, &count, party_size);
	}
	if (matchmaking.stage == _matchmaking_following)
	{
		char link[128];
		unsigned long order = 0;

		view.leader_game = p2p_party_follow(link, sizeof(link), &order) == _p2p_party_game_invite &&
			matchmaking_link_host(link, matchmaking.host);
		if (view.leader_game && !p2p_join_invite(link))
			view.leader_game = FALSE;
		if (view.leader_game)
		{
			matchmaking.order = order;
			snprintf(matchmaking.game.name, sizeof(matchmaking.game.name), "your party's leader");
		}
	}
	stage = matchmaking_step(&view, &action);
	switch (action)
	{
	case _matchmaking_action_reach:
		matchmaking.game = games[view.best];
		csmemcpy(matchmaking.host, matchmaking.game.identifier, sizeof(matchmaking.host));
		matchmaking.entered = FALSE;
		matchmaking.order = 0;
		matchmaking_client();
		if (!p2p_join_listing(&matchmaking.game))
		{
			p2p_lobby_mark_failed(matchmaking.game.identifier);
			stage = _matchmaking_looking;
			break;
		}
		platform_log("matchmaking: joining %s's game (%d of %d players, %d ms)", matchmaking.game.name,
			matchmaking.game.player_count, matchmaking.game.maximum_player_count, matchmaking.game.ping);
		break;
	case _matchmaking_action_join:
		/* (once the host's game is among the client's: joined, the lobby
		opened) */
		if (matchmaking_player() && network_game_client_join_available_game(matchmaking.host))
		{
			matchmaking.entered = TRUE;
			ui_widgets_close_all();
			ui_widget_load_by_name_or_tag(
				pc_menus_screen("ui\\shell\\main_menu\\multiplayer_type_select\\connected\\pregame\\connected_pregame_screen"),
				NONE, NULL, NONE, NONE, NONE, NONE);
		}
		break;
	case _matchmaking_action_failed:
		platform_log("matchmaking: %s's game was not joined", matchmaking.game.name);
		p2p_join_cancel();
		if (matchmaking.order)
			p2p_party_follow_result(matchmaking.order, _p2p_party_follow_refused);
		else
			p2p_lobby_mark_failed(matchmaking.host);
		matchmaking.entered = FALSE;
		break;
	case _matchmaking_action_pass:
		if (!p2p_party_pass_host(matchmaking.playlist + 1))
		{
			stage = _matchmaking_hosting;
			if (!matchmaking_host())
				return;
		}
		break;
	case _matchmaking_action_host:
		platform_log("matchmaking: the member asked to host did not; hosting here");
		if (!matchmaking_host())
			return;
		break;
	case _matchmaking_action_follow:
		matchmaking.entered = FALSE;
		break;
	case _matchmaking_action_done:
		platform_log("matchmaking: in the %s game", matchmaking_playlists[matchmaking.playlist].name);
		if (matchmaking.order)
			p2p_party_follow_result(matchmaking.order, _p2p_party_follow_joined);
		matchmaking.order = 0;
		matchmaking.problem = _matchmaking_problem_joined;
		break;
	case _matchmaking_action_give_up:
		platform_log("matchmaking: the %s game could not be hosted", matchmaking_playlists[matchmaking.playlist].name);
		matchmaking.problem = _matchmaking_problem_hosting;
		break;
	}
	if (stage != matchmaking.stage)
		matchmaking_stage(stage);
	if (matchmaking.stage == _matchmaking_hosting)
		matchmaking_host_update();
}
