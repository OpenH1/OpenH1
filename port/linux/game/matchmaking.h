/*
MATCHMAKING.H

Matchmaking (matchmaking.c): a game found, or hosted, for this machine's
party by playlist, through the public listings and the signalling brokers
alone. The Matchmaking screen (matchmaking_menu.inc) shows it.
*/

#ifndef __HALO_PORT_MATCHMAKING_H
#define __HALO_PORT_MATCHMAKING_H

/* ---------- constants */

enum
{
	MATCHMAKING_PLAYLIST_NAME_SIZE = 16,
	MATCHMAKING_ABOUT_SIZE = 48,
	MATCHMAKING_HOST_NAME_SIZE = 33,
};

/* where a search is */
enum
{
	_matchmaking_idle,
	/* the public games read, a game of the playlist with room looked for */
	_matchmaking_looking,
	/* the game picked: its host reached, its game found */
	_matchmaking_reaching,
	/* joined, waiting to be in it */
	_matchmaking_joining,
	/* the lead passed to the party's member that hosts best */
	_matchmaking_handing,
	/* hosting the playlist's game: waiting for players, then starting it */
	_matchmaking_hosting,
	/* (the lead passed) the party's new leader's game followed */
	_matchmaking_following,
	NUMBER_OF_MATCHMAKING_STAGES
};

/* why a search could not begin, or ended */
enum
{
	_matchmaking_problem_none,
	_matchmaking_problem_offline,
	/* a member of a party: its leader searches */
	_matchmaking_problem_member,
	/* the party has more than the playlist takes */
	_matchmaking_problem_party_too_big,
	_matchmaking_problem_no_profile,
	/* the game could not be hosted */
	_matchmaking_problem_hosting,
	/* cancelled */
	_matchmaking_problem_cancelled,
	/* (ended well) in the game found or hosted */
	_matchmaking_problem_joined,
};

/* ---------- structures */

struct matchmaking_status
{
	short stage;
	/* the playlist (0 up), NONE for none */
	short playlist;
	/* milliseconds since the search began */
	unsigned long elapsed;
	/* the party's size (this machine too) */
	short party_size;
	/* the games of the playlist seen, and of them those with room */
	short games, games_with_room;
	/* the game reached or joined: its name; hosting: its players (people)
	and its most */
	char host_name[MATCHMAKING_HOST_NAME_SIZE];
	short players, most_players;
	/* hosting: milliseconds until it starts with bots or whoever is there
	(0: it waits for players) */
	unsigned long start_in;
	/* why the last search ended (_matchmaking_problem_*) */
	short problem;
};

/* ---------- prototypes */

/* each frame (main.c): the search's step, the hosted game's start, and
what the party is told of how well this machine can host */
void matchmaking_update(void);
/* the playlists: how many, and each one's name and what it is */
short matchmaking_playlist_count(void);
void matchmaking_playlist_text(short playlist, wchar_t *name, short name_size, wchar_t *about, short about_size);
/* whether this machine may search the playlist (a party's leader, or in no
party; online), and why not (_matchmaking_problem_*) */
short matchmaking_can_search(short playlist);
/* a search of the playlist begun (its player the controller's): FALSE, and
why in the status, if it cannot be */
boolean matchmaking_search(short playlist, short controller);
void matchmaking_cancel(void);
void matchmaking_status(struct matchmaking_status *status);

#endif
