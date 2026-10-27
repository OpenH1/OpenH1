#include "harness.h"

/* Matchmaking's decisions (port/linux/game/matchmaking.c): which public
game a party takes (matchmaking_game_score, matchmaking_pick: its playlist's,
an OpenH1 game of this build, open, no password, room for the whole party,
not failed; by ping in steps, then the fullest, a lobby first), who may
search (matchmaking_search_problem), how a hosted game starts
(matchmaking_start_choice: full at once, else after a wait with bots or with
its least players), and a search's steps (matchmaking_step: looked for,
reached, joined, failed and looked again, the lead passed or hosted when
none fits, the new leader followed) */

#define NUMBEROF(array) ((long)(sizeof(array) / sizeof((array)[0])))

#include "config.inc"
#include "under_test.inc"

static struct p2p_listing game(short playlist, short players, short most, short ping)
{
	struct p2p_listing listing;

	memset(&listing, 0, sizeof(listing));
	strcpy(listing.invite, "openh1://join/00");
	listing.playlist = (unsigned char)playlist;
	listing.player_count = (unsigned char)players;
	listing.maximum_player_count = (unsigned char)most;
	listing.ping = ping;
	listing.standard = 1;
	listing.open = 1;
	listing.holds_party = 1;
	return listing;
}

static void case_pick(void)
{
	struct p2p_listing games[8];

	games[0] = game(1, 2, 8, 40);
	CHECK(matchmaking_game_score(&games[0], 1, 3) != NONE, "a Team Slayer game with room for 3 takes the party");
	CHECK(matchmaking_game_score(&games[0], 2, 1) == NONE, "not one looking for another playlist");
	CHECK(matchmaking_game_score(&games[0], 0, 1) == NONE, "nor one looking for no playlist");
	games[1] = game(0, 2, 8, 40);
	CHECK(matchmaking_game_score(&games[1], 1, 1) == NONE, "a game of no playlist is never picked");
	games[1] = game(1, 6, 8, 40);
	CHECK(matchmaking_game_score(&games[1], 1, 2) != NONE && matchmaking_game_score(&games[1], 1, 3) == NONE,
		"room for 2, not 3");
	games[1].standard = 0;
	CHECK(matchmaking_game_score(&games[1], 1, 1) == NONE, "a Legacy game (or another protocol's) is never picked");
	games[1] = game(1, 2, 8, 40);
	games[1].open = 0;
	CHECK(matchmaking_game_score(&games[1], 1, 1) == NONE, "nor a closed one");
	games[1] = game(1, 2, 8, 40);
	games[1].locked = 1;
	CHECK(matchmaking_game_score(&games[1], 1, 1) == NONE, "nor one with a password");
	games[1] = game(1, 2, 8, 40);
	games[1].failed = 1;
	CHECK(matchmaking_game_score(&games[1], 1, 1) == NONE, "nor one this machine failed to join");
	games[1] = game(1, 2, 8, 40);
	games[1].invite[0] = 0;
	CHECK(matchmaking_game_score(&games[1], 1, 1) == NONE, "nor one with no invite");
	/* by ping, then the fullest */
	games[0] = game(1, 2, 8, 120);
	games[1] = game(1, 1, 8, 30);
	games[2] = game(2, 7, 8, 5);
	games[3] = game(1, 6, 8, 20);
	CHECK(matchmaking_pick(games, 4, 1, 2) == 3, "the nearest with room (not another playlist's)");
	CHECK(matchmaking_pick(games, 4, 1, 3) == 1, "the nearest with room for 3");
	games[3] = game(1, 3, 8, 35);
	CHECK(matchmaking_pick(games, 4, 1, 2) == 3, "of pings in one step, the fuller");
	games[3].in_progress = 1;
	games[1] = game(1, 3, 8, 30);
	CHECK(matchmaking_pick(games, 4, 1, 2) == 1, "and a lobby before a game under way");
	games[1].ping = -1;
	games[3].ping = -1;
	games[0].ping = 300;
	CHECK(matchmaking_pick(games, 4, 1, 2) == 1, "a ping not measured yet is taken as MATCHMAKING_UNKNOWN_PING");
	games[0] = game(1, 2, 8, 100);
	games[1] = game(1, 2, 8, -1);
	CHECK(matchmaking_pick(games, 2, 1, 2) == 0, "a game measured at 100 ms before one not measured");
	CHECK(matchmaking_pick(games, 4, 5, 1) == NONE, "none for a playlist no game has");
	CHECK(matchmaking_pick(games, 0, 1, 1) == NONE, "none of none");
}

static void case_problem(void)
{
	CHECK(matchmaking_search_problem(FALSE, FALSE, 1, 4) == _matchmaking_problem_offline, "internet play off");
	CHECK(matchmaking_search_problem(TRUE, TRUE, 3, 4) == _matchmaking_problem_member, "a member: its leader searches");
	CHECK(matchmaking_search_problem(TRUE, FALSE, 5, 4) == _matchmaking_problem_party_too_big, "a party of 5 for 4");
	CHECK(matchmaking_search_problem(TRUE, FALSE, 4, 4) == _matchmaking_problem_none, "a party of 4 for 4");
	CHECK(matchmaking_search_problem(TRUE, FALSE, 1, 2) == _matchmaking_problem_none, "a player in no party");
}

static void case_start(void)
{
	CHECK(matchmaking_start_choice(8, 8, 2, 0, TRUE) == _matchmaking_start_now, "full: at once");
	CHECK(matchmaking_start_choice(3, 8, 2, MATCHMAKING_FILL_MILLISECONDS - 1, TRUE) == _matchmaking_start_wait,
		"waiting for players");
	CHECK(matchmaking_start_choice(3, 8, 2, MATCHMAKING_FILL_MILLISECONDS, TRUE) == _matchmaking_start_bots,
		"then bots fill it");
	CHECK(matchmaking_start_choice(1, 8, 2, MATCHMAKING_FILL_MILLISECONDS, TRUE) == _matchmaking_start_bots,
		"one player and bots");
	CHECK(matchmaking_start_choice(3, 8, 2, MATCHMAKING_FILL_MILLISECONDS, FALSE) == _matchmaking_start_now,
		"a gametype bots do not play: with the players there are");
	CHECK(matchmaking_start_choice(1, 8, 2, MATCHMAKING_FILL_MILLISECONDS * 3, FALSE) == _matchmaking_start_wait,
		"but not fewer than its least");
	CHECK(matchmaking_start_choice(0, 0, 2, 0, FALSE) == _matchmaking_start_wait, "no most known: not full");
}

static short step(struct matchmaking_view *view, short *action)
{
	return matchmaking_step(view, action);
}

static struct matchmaking_view looking(unsigned long searched, short best)
{
	struct matchmaking_view view;

	memset(&view, 0, sizeof(view));
	view.stage = _matchmaking_looking;
	view.searched = searched;
	view.search_time = 30000;
	view.best = best;
	view.leading = TRUE;
	view.join_stage = _p2p_join_idle;
	return view;
}

static void case_look(void)
{
	struct matchmaking_view view;
	short action;

	view = looking(1000, 2);
	CHECK(step(&view, &action) == _matchmaking_looking && action == _matchmaking_action_none,
		"a game found at once waits for the others' listings and pings");
	view = looking(MATCHMAKING_LOOK_MILLISECONDS, 2);
	CHECK(step(&view, &action) == _matchmaking_reaching && action == _matchmaking_action_reach, "then reached");
	view = looking(20000, NONE);
	CHECK(step(&view, &action) == _matchmaking_looking && action == _matchmaking_action_none, "none yet: looking");
	view = looking(30000, NONE);
	CHECK(step(&view, &action) == _matchmaking_handing && action == _matchmaking_action_pass,
		"none in the search's time: the lead passed to a better host, or hosted");
	view = looking(30000, 1);
	CHECK(step(&view, &action) == _matchmaking_reaching, "a game found as the time runs out is taken");
	view = looking(1000, NONE);
	view.leading = FALSE;
	CHECK(step(&view, &action) == _matchmaking_following, "the lead taken by another: its game followed");
}

static void case_join(void)
{
	struct matchmaking_view view;
	short action;

	memset(&view, 0, sizeof(view));
	view.stage = _matchmaking_reaching;
	view.leading = TRUE;
	view.waited = 500;
	view.join_stage = _p2p_join_finding;
	CHECK(step(&view, &action) == _matchmaking_reaching && action == _matchmaking_action_none, "finding the host");
	view.join_stage = _p2p_join_reached;
	CHECK(step(&view, &action) == _matchmaking_reaching && action == _matchmaking_action_join,
		"reached: its game joined (once it is among the client's)");
	view.entered = TRUE;
	CHECK(step(&view, &action) == _matchmaking_joining, "joined: waiting to be in it");
	view.entered = FALSE;
	view.waited = MATCHMAKING_REACH_MILLISECONDS + 1;
	CHECK(step(&view, &action) == _matchmaking_looking && action == _matchmaking_action_failed,
		"its game never found: failed, looked for again");
	view.join_stage = _p2p_join_idle;
	view.waited = 1500;
	CHECK(step(&view, &action) == _matchmaking_looking && action == _matchmaking_action_failed,
		"the join given up: failed");
	view.join_stage = _p2p_join_opening;
	view.waited = MATCHMAKING_REACH_MILLISECONDS + 1;
	view.following = TRUE;
	CHECK(step(&view, &action) == _matchmaking_following && action == _matchmaking_action_failed,
		"a leader's game not reached: back to following");
	memset(&view, 0, sizeof(view));
	view.stage = _matchmaking_joining;
	view.waited = 1000;
	view.in_none = TRUE;
	CHECK(step(&view, &action) == _matchmaking_joining, "between the join and the lobby a moment");
	view.in_none = FALSE;
	view.in_game = TRUE;
	CHECK(step(&view, &action) == _matchmaking_idle && action == _matchmaking_action_done, "in the game: done");
	view.in_game = FALSE;
	view.in_none = TRUE;
	view.waited = 2500;
	CHECK(step(&view, &action) == _matchmaking_looking && action == _matchmaking_action_failed,
		"turned away: looked for again");
	view.in_none = FALSE;
	view.waited = MATCHMAKING_JOIN_MILLISECONDS + 1;
	CHECK(step(&view, &action) == _matchmaking_looking && action == _matchmaking_action_failed, "never in it");
}

static void case_host(void)
{
	struct matchmaking_view view;
	short action;

	memset(&view, 0, sizeof(view));
	view.stage = _matchmaking_handing;
	view.leading = TRUE;
	view.waited = 1000;
	CHECK(step(&view, &action) == _matchmaking_handing, "waiting for the member asked to host to lead");
	view.leading = FALSE;
	CHECK(step(&view, &action) == _matchmaking_following, "it leads: its game followed");
	view.leading = TRUE;
	view.waited = MATCHMAKING_HANDOFF_MILLISECONDS + 1;
	CHECK(step(&view, &action) == _matchmaking_hosting && action == _matchmaking_action_host,
		"it never did: hosted here");
	memset(&view, 0, sizeof(view));
	view.stage = _matchmaking_following;
	CHECK(step(&view, &action) == _matchmaking_following, "following: the new leader's game not there yet");
	view.leader_game = TRUE;
	CHECK(step(&view, &action) == _matchmaking_reaching && action == _matchmaking_action_follow, "then reached");
	view.leader_game = FALSE;
	view.waited = MATCHMAKING_FOLLOW_MILLISECONDS + 1;
	CHECK(step(&view, &action) == _matchmaking_idle && action == _matchmaking_action_give_up,
		"the new leader hosting nothing: the search given up");
	view.waited = 0;
	view.leading = TRUE;
	CHECK(step(&view, &action) == _matchmaking_looking, "the lead back here: looked for again");
	view.leading = FALSE;
	view.in_game = TRUE;
	CHECK(step(&view, &action) == _matchmaking_idle && action == _matchmaking_action_done, "in its game: done");
	memset(&view, 0, sizeof(view));
	view.stage = _matchmaking_hosting;
	view.hosting = TRUE;
	view.waited = MATCHMAKING_HOST_MILLISECONDS * 10;
	CHECK(step(&view, &action) == _matchmaking_hosting, "hosting until the game starts");
	view.hosting = FALSE;
	view.waited = 1000;
	CHECK(step(&view, &action) == _matchmaking_hosting, "the server a moment coming");
	view.waited = MATCHMAKING_HOST_MILLISECONDS + 1;
	CHECK(step(&view, &action) == _matchmaking_idle && action == _matchmaking_action_give_up,
		"a server that never came: given up");
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	if (!strcmp(case_name, "pick"))
		case_pick();
	else if (!strcmp(case_name, "problem"))
		case_problem();
	else if (!strcmp(case_name, "start"))
		case_start();
	else if (!strcmp(case_name, "look"))
		case_look();
	else if (!strcmp(case_name, "join"))
		case_join();
	else if (!strcmp(case_name, "host"))
		case_host();
	else
		return 2;
	return 0;
}
