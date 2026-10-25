#include "harness.h"

/* A party member following its leader into its game
(port/linux/game/party_menu.inc's party_game_verdict and party_follow_step):
what a game found allows (joined, waited for while it is not open, or why
not: full, another version, a Legacy game this machine cannot join), and the
follow's steps: the host reached, its game found, joined (the A press
posted), in it; given up when the host is not reached, the game not found
or never open, the join refused, or the Party screen left (but not once the
lobby has taken its place) */

#include "config.inc"
#include "under_test.inc"

static short step(short stage, unsigned long waited, short join_stage, boolean found, short verdict, boolean screen,
	boolean in_game, boolean in_none, short *result)
{
	struct party_follow_view view;

	memset(&view, 0, sizeof(view));
	view.stage = stage;
	view.waited = waited;
	view.join_stage = join_stage;
	view.found = found;
	view.verdict = verdict;
	view.screen = screen;
	view.in_game = in_game;
	view.in_none = in_none;
	return party_follow_step(&view, result);
}

static void case_verdict(void)
{
	/* (open, under way, players, most, compatible, Standard, mixed) */
	CHECK(party_game_verdict(TRUE, FALSE, 3, 16, TRUE, TRUE, FALSE) == _p2p_party_follow_joined, "an open game is joined");
	CHECK(party_game_verdict(FALSE, TRUE, 3, 16, TRUE, FALSE, FALSE) == _p2p_party_follow_joined,
		"a game under way is joined (its lobby first)");
	CHECK(party_game_verdict(FALSE, FALSE, 3, 16, TRUE, TRUE, FALSE) == _p2p_party_follow_none,
		"a game not open yet is waited for");
	CHECK(party_game_verdict(TRUE, FALSE, 16, 16, TRUE, TRUE, FALSE) == _p2p_party_follow_full, "a full game is not");
	CHECK(party_game_verdict(FALSE, TRUE, 16, 16, TRUE, TRUE, FALSE) == _p2p_party_follow_full,
		"nor one under way that is full");
	CHECK(party_game_verdict(TRUE, FALSE, 3, 16, FALSE, TRUE, FALSE) == _p2p_party_follow_version,
		"an OpenH1 game of another version is not");
	CHECK(party_game_verdict(TRUE, FALSE, 3, 16, FALSE, FALSE, TRUE) == _p2p_party_follow_version,
		"nor a mixed one of another version");
	CHECK(party_game_verdict(TRUE, FALSE, 3, 16, FALSE, FALSE, FALSE) == _p2p_party_follow_legacy,
		"a Legacy game this machine cannot join is said so");
}

static void case_reach(void)
{
	short result;

	CHECK(step(_party_follow_reaching, 500, _p2p_join_finding, FALSE, 0, TRUE, FALSE, TRUE, &result) ==
		_party_follow_reaching && result == _p2p_party_follow_none, "finding the host");
	CHECK(step(_party_follow_reaching, 500, _p2p_join_idle, FALSE, 0, TRUE, FALSE, TRUE, &result) ==
		_party_follow_reaching, "the join just asked for");
	CHECK(step(_party_follow_reaching, 4000, _p2p_join_reached, FALSE, 0, TRUE, FALSE, TRUE, &result) ==
		_party_follow_finding, "the host reached: its game is looked for");
	CHECK(step(_party_follow_reaching, 1500, _p2p_join_idle, FALSE, 0, TRUE, FALSE, TRUE, &result) ==
		_party_follow_idle && result == _p2p_party_follow_unreachable, "a join given up: the host not reached");
	CHECK(step(_party_follow_reaching, PARTY_REACH_TIME + 1, _p2p_join_opening, FALSE, 0, TRUE, FALSE, TRUE, &result) ==
		_party_follow_idle && result == _p2p_party_follow_unreachable, "nor in time");
	CHECK(step(_party_follow_reaching, 500, _p2p_join_opening, FALSE, 0, FALSE, FALSE, TRUE, &result) ==
		_party_follow_idle && result == _p2p_party_follow_none, "the screen left: given up, no answer");
}

static void case_find(void)
{
	short result;

	CHECK(step(_party_follow_finding, 1000, _p2p_join_reached, FALSE, 0, TRUE, FALSE, TRUE, &result) ==
		_party_follow_finding, "its game not listed yet");
	CHECK(step(_party_follow_finding, PARTY_FIND_TIME + 1, _p2p_join_reached, FALSE, 0, TRUE, FALSE, TRUE, &result) ==
		_party_follow_idle && result == _p2p_party_follow_unreachable, "never listed");
	CHECK(step(_party_follow_finding, 100, _p2p_join_reached, TRUE, _p2p_party_follow_joined, TRUE, FALSE, TRUE,
		&result) == _party_follow_ready, "found and open: joined");
	CHECK(step(_party_follow_finding, 100, _p2p_join_reached, TRUE, _p2p_party_follow_full, TRUE, FALSE, TRUE,
		&result) == _party_follow_idle && result == _p2p_party_follow_full, "found full: said so");
	CHECK(step(_party_follow_finding, 100, _p2p_join_reached, TRUE, _p2p_party_follow_legacy, TRUE, FALSE, TRUE,
		&result) == _party_follow_idle && result == _p2p_party_follow_legacy, "a Legacy game: said so");
	CHECK(step(_party_follow_finding, 60000, _p2p_join_reached, TRUE, _p2p_party_follow_none, TRUE, FALSE, TRUE,
		&result) == _party_follow_finding, "not open yet: waited for");
	CHECK(step(_party_follow_finding, PARTY_OPEN_WAIT + 1, _p2p_join_reached, TRUE, _p2p_party_follow_none, TRUE,
		FALSE, TRUE, &result) == _party_follow_idle && result == _p2p_party_follow_full, "never open: full or starting");
	CHECK(step(_party_follow_ready, 1000, _p2p_join_reached, TRUE, _p2p_party_follow_joined, TRUE, FALSE, TRUE,
		&result) == _party_follow_ready, "the press posted");
	CHECK(step(_party_follow_ready, PARTY_READY_TIME + 1, _p2p_join_reached, TRUE, _p2p_party_follow_joined, TRUE,
		FALSE, TRUE, &result) == _party_follow_finding, "not taken: posted again");
}

static void case_join(void)
{
	short result;

	CHECK(step(_party_follow_joining, 500, _p2p_join_reached, FALSE, 0, FALSE, FALSE, FALSE, &result) ==
		_party_follow_joining, "joining (the lobby has the screen's place)");
	CHECK(step(_party_follow_joining, 900, _p2p_join_reached, FALSE, 0, FALSE, TRUE, FALSE, &result) ==
		_party_follow_idle && result == _p2p_party_follow_joined, "in the leader's game");
	CHECK(step(_party_follow_joining, 1000, _p2p_join_reached, FALSE, 0, FALSE, FALSE, TRUE, &result) ==
		_party_follow_joining, "the client between games a moment");
	CHECK(step(_party_follow_joining, 3000, _p2p_join_reached, FALSE, 0, FALSE, FALSE, TRUE, &result) ==
		_party_follow_idle && result == _p2p_party_follow_refused, "back to looking for games: turned away");
	CHECK(step(_party_follow_joining, PARTY_JOIN_TIME + 1, _p2p_join_reached, FALSE, 0, TRUE, FALSE, FALSE, &result) ==
		_party_follow_idle && result == _p2p_party_follow_refused, "never in it");
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "verdict"))
		case_verdict();
	else if (!strcmp(name, "reach"))
		case_reach();
	else if (!strcmp(name, "find"))
		case_find();
	else if (!strcmp(name, "join"))
		case_join();
	else
		return 2;
	return 0;
}
