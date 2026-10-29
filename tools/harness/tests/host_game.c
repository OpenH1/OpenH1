#include "harness.h"

/* A player host's next game and players (port/linux/game/network_host_game.c's
pure parts): the playlist's next entry that can be played, round from its
end, the entry played last's place kept; a ban's and a mute's duration as
the console takes it ("30m", "2h", "7d", "1w", "forever"); and the teams
shuffled (as many on each, drawn from the seed) or balanced (as few moved as
can be; by score where there are scores, each team's total near the
other's), the teams' colours kept for the most players */

#define PIN(value, low, high) ((value) < (low) ? (low) : (value) > (high) ? (high) : (value))
#define csstrcmp strcmp

#include "config.inc"
#include "under_test.inc"

/* a playlist of so many entries, those marked rejected */
static void playlist_of(struct server_rotation *playlist, int count, char const *rejected)
{
	int index;

	memset(playlist, 0, sizeof(*playlist));
	playlist->count = count;
	for (index = 0; index < count; index++)
	{
		if (rejected && rejected[index] == 'x')
			strcpy(playlist->entries[index].rejected, "missing");
	}
}

static void case_playlist(void)
{
	static struct server_rotation playlist;

	playlist_of(&playlist, 0, NULL);
	CHECK(network_host_game_playlist_after(&playlist, NONE) == NONE, "an empty playlist: nothing");
	playlist_of(&playlist, 3, NULL);
	CHECK(network_host_game_playlist_after(&playlist, NONE) == 0, "none played: the first");
	CHECK(network_host_game_playlist_after(&playlist, 0) == 1, "the one after");
	CHECK(network_host_game_playlist_after(&playlist, 2) == 0, "round from the end");
	CHECK(network_host_game_playlist_after(&playlist, 7) == 0, "past a shorter playlist: its first");
	playlist_of(&playlist, 3, ".x.");
	CHECK(network_host_game_playlist_after(&playlist, 0) == 2, "one that can't be played passed over");
	playlist_of(&playlist, 1, NULL);
	CHECK(network_host_game_playlist_after(&playlist, 0) == 0, "one entry: itself again");
	playlist_of(&playlist, 2, "xx");
	CHECK(network_host_game_playlist_after(&playlist, NONE) == NONE, "none can be played: nothing");
}

static void case_duration(void)
{
	CHECK(network_host_game_duration_parse("30m") == 30 * 60, "minutes");
	CHECK(network_host_game_duration_parse("45") == 45 * 60, "a number alone: minutes");
	CHECK(network_host_game_duration_parse("2h") == 2 * 60 * 60, "hours");
	CHECK(network_host_game_duration_parse("7d") == 7 * 24 * 60 * 60, "days");
	CHECK(network_host_game_duration_parse("1w") == 7 * 24 * 60 * 60, "weeks");
	CHECK(network_host_game_duration_parse("forever") == 0, "for ever");
	CHECK(network_host_game_duration_parse("0") == 0, "0: for ever");
	CHECK(network_host_game_duration_parse("griefing") == NONE, "a word is no duration");
	CHECK(network_host_game_duration_parse("2x") == NONE, "an unknown unit");
	CHECK(network_host_game_duration_parse("2hh") == NONE, "a unit and more");
	CHECK(network_host_game_duration_parse("Bob") == NONE, "a name is no duration");
	CHECK(network_host_game_duration_parse("99999w") == 0, "past ten years: for ever");
}

static short count_on(char const *teams, short count, char team)
{
	short index, on = 0;

	for (index = 0; index < count; index++)
		on += teams[index] == team;
	return on;
}

static short moved(char const *teams, char const *current, short count)
{
	short index, changed = 0;

	for (index = 0; index < count; index++)
		changed += teams[index] != current[index];
	return changed;
}

static void case_shuffle(void)
{
	char current[8] = { 0, 0, 0, 0, 0, 0, 1, 1 };
	char teams[8], other[8];
	unsigned long seed;
	boolean differed = FALSE;

	network_host_game_teams_plan(8, NULL, current, _host_game_shuffle, 1234, teams);
	CHECK(count_on(teams, 8, 0) == 4 && count_on(teams, 8, 1) == 4, "as many on each");
	CHECK(moved(teams, current, 8) <= 4, "the colours kept for the most players");
	for (seed = 1; seed < 20; seed++)
	{
		network_host_game_teams_plan(8, NULL, current, _host_game_shuffle, seed, other);
		CHECK(count_on(other, 8, 0) == 4, "as many on each, every draw");
		differed |= memcmp(teams, other, 8) != 0;
	}
	CHECK(differed, "drawn from the seed");
	network_host_game_teams_plan(5, NULL, current, _host_game_shuffle, 99, teams);
	CHECK(count_on(teams, 5, 0) + count_on(teams, 5, 1) == 5 && count_on(teams, 5, 0) >= 2 && count_on(teams, 5, 1) >= 2,
		"an odd count: one more on one");
}

static void case_balance(void)
{
	char current[7] = { 0, 0, 0, 0, 0, 1, 1 };
	char even[6] = { 0, 1, 0, 1, 0, 1 };
	char unteamed[4] = { NONE, NONE, 0, 0 };
	char teams[8];
	long scores[6] = { 30, 25, 20, 2, 1, 0 };
	char stacked[6] = { 0, 0, 0, 1, 1, 1 };
	long totals[2] = { 0, 0 };
	short index;

	network_host_game_teams_plan(7, NULL, current, _host_game_balance, 0, teams);
	CHECK(count_on(teams, 7, 0) == 4 && count_on(teams, 7, 1) == 3, "5 against 2: 4 against 3");
	CHECK(moved(teams, current, 7) == 1, "one moved, no more");
	network_host_game_teams_plan(6, NULL, even, _host_game_balance, 0, teams);
	CHECK(moved(teams, even, 6) == 0, "even teams: nobody moved");
	network_host_game_teams_plan(4, NULL, unteamed, _host_game_balance, 0, teams);
	CHECK(count_on(teams, 4, 0) == 2 && count_on(teams, 4, 1) == 2 && teams[2] == 0 && teams[3] == 0,
		"those on no team to the smaller");
	/* (by score: the best three were together) */
	network_host_game_teams_plan(6, scores, stacked, _host_game_balance, 0, teams);
	CHECK(count_on(teams, 6, 0) == 3, "as many on each, by score too");
	for (index = 0; index < 6; index++)
		totals[(short)teams[index]] += scores[index];
	CHECK(totals[0] - totals[1] <= 15 && totals[1] - totals[0] <= 15, "the scores near even (%ld, %ld)", totals[0],
		totals[1]);
	CHECK(moved(teams, stacked, 6) <= 3, "the colours kept for the most players");
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	if (!strcmp(case_name, "playlist"))
		case_playlist();
	else if (!strcmp(case_name, "duration"))
		case_duration();
	else if (!strcmp(case_name, "shuffle"))
		case_shuffle();
	else if (!strcmp(case_name, "balance"))
		case_balance();
	else
		return 2;
	return 0;
}
