/*
CARNAGE_REPORT.C (test)

The carnage report's and the in-game scoreboard's tables
(source/game/game_engine.c): the report's columns (a team game's with
betrayals, a free for all's without; the numbers right, under their
headings; the player's column taking what the others leave, all of them
across the table), its rows (each player's statistics, this machine's
players lit), the teams' totals (every player's of the game, shown or not),
the rows' height (the players' and the totals' rows filling the table, none
taller than the tallest), kills per death, and the scoreboard's ping (shown
only in a network game with other machines, its column then wider).
test_carnage_report.py takes the enums, structures and code.
*/

#include "harness.h"

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define csmemset memset

/* the fake world: players, with what the code reads of them */
struct player_statistics
{
	short kills[4];
	short assists[4];
	short friendly_fire_kills;
	short deaths;
	short suicides;
};

struct player_datum
{
	short local_player_index;
	long team_index;
	struct player_statistics statistics;
};

static struct player_datum players[8];
static boolean splitscreen_local = FALSE;

#define player_get(index) (&players[(index)])
#define network_game_is_splitscreen_local() (splitscreen_local)

#include "config.inc"
#include "under_test.inc"

static void player_set(long index, long team, short local, short kills, short assists, short deaths, short suicides,
	short betrayals)
{
	struct player_datum *player = &players[index];

	memset(player, 0, sizeof(*player));
	player->team_index = team;
	player->local_player_index = local;
	player->statistics.kills[0] = kills;
	player->statistics.assists[0] = assists;
	player->statistics.deaths = deaths;
	player->statistics.suicides = suicides;
	player->statistics.friendly_fire_kills = betrayals;
}

/* the report's table across its panel's inner width */
#define TABLE_LEFT (CARNAGE_LEFT + CARNAGE_TABLE_PADDING)
#define TABLE_RIGHT (CARNAGE_RIGHT - CARNAGE_TABLE_PADDING)

static void check_columns(boolean teams, short expected_count)
{
	struct carnage_column columns[NUMBER_OF_CARNAGE_COLUMNS + 4];
	short count;
	short index;

	memset(columns, 0, sizeof(columns));
	count = carnage_report_columns(teams, TABLE_LEFT, TABLE_RIGHT, columns);
	CHECK(count == expected_count, "%s: %d columns, not %d", teams ? "teams" : "free for all", count, expected_count);
	CHECK(columns[0].x0 == TABLE_LEFT, "the first column starts at %d, not the table's left %d", columns[0].x0,
		TABLE_LEFT);
	CHECK(columns[count - 1].x1 == TABLE_RIGHT, "the last column ends at %d, not the table's right %d",
		columns[count - 1].x1, TABLE_RIGHT);
	for (index = 0; index < count; index++)
	{
		CHECK(columns[index].kind == index, "column %d shows %d", index, columns[index].kind);
		CHECK(columns[index].x1 > columns[index].x0, "column %d is %d wide", index,
			columns[index].x1 - columns[index].x0);
		CHECK(index == 0 || columns[index].x0 == columns[index - 1].x1, "column %d starts at %d, not at %d", index,
			columns[index].x0, columns[index - 1].x1);
		/* (the place and name left, the numbers right under their headings) */
		CHECK(columns[index].justification == (index > _carnage_column_player ? 1 : 0),
			"column %d justified %d", index, columns[index].justification);
	}
	/* (a name of 11 characters, about 7 of the report's units each) */
	CHECK(columns[_carnage_column_player].x1 - columns[_carnage_column_player].x0 >= 100,
		"the player's column is %d wide", columns[_carnage_column_player].x1 - columns[_carnage_column_player].x0);
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	CASE("columns-teams")
	{
		check_columns(TRUE, NUMBER_OF_CARNAGE_COLUMNS);
		return 0;
	}
	CASE("columns-free-for-all")
	{
		/* (no betrayals, which a free for all has none of) */
		check_columns(FALSE, _carnage_column_betrayals);
		return 0;
	}
	CASE("rows")
	{
		struct statistic_buffer entries[3];
		struct carnage_row rows[3];
		long index;

		player_set(0, 0, 0, 5, 1, 2, 1, 0);
		player_set(1, 1, NONE, 7, 2, 3, 0, 1);
		player_set(2, 0, NONE, 1, 4, 6, 2, 2);
		player_set(3, 1, 1, 3, 0, 1, 0, 0);
		memset(entries, 0, sizeof(entries));
		/* (in order of place: players 1, 0, 3) */
		entries[0].player_index = 1;
		entries[1].player_index = 0;
		entries[2].player_index = 3;
		memset(rows, 0xAB, sizeof(rows));
		carnage_report_rows(entries, 3, rows);
		for (index = 0; index < 3; index++)
		{
			struct player_datum *player = &players[entries[index].player_index];

			CHECK(rows[index].player_index == entries[index].player_index, "row %ld is player %ld's", index,
				rows[index].player_index);
			CHECK(rows[index].team_index == player->team_index, "row %ld's team %ld", index, rows[index].team_index);
			CHECK(rows[index].kills == player->statistics.kills[0] &&
				rows[index].assists == player->statistics.assists[0] &&
				rows[index].deaths == player->statistics.deaths &&
				rows[index].suicides == player->statistics.suicides &&
				rows[index].betrayals == player->statistics.friendly_fire_kills,
				"row %ld's statistics: %ld %ld %ld %ld %ld", index, rows[index].kills, rows[index].assists,
				rows[index].deaths, rows[index].suicides, rows[index].betrayals);
		}
		/* (this machine's players lit: the first local player's and the
		second's, not another machine's) */
		CHECK(!rows[0].local, "another machine's player's row is lit");
		CHECK(rows[1].local, "the first local player's row is not lit");
		CHECK(rows[2].local, "the second local player's row is not lit");
		return 0;
	}
	CASE("totals")
	{
		struct statistic_buffer all[5];
		struct carnage_row totals[2];

		player_set(0, 0, 0, 5, 1, 2, 1, 0);
		player_set(1, 1, NONE, 7, 2, 3, 0, 1);
		player_set(2, 0, NONE, 1, 4, 6, 2, 2);
		player_set(3, 1, NONE, 3, 0, 1, 0, 0);
		/* (a player of no team, as a free for all's: in neither) */
		player_set(4, NONE, NONE, 9, 9, 9, 9, 9);
		memset(all, 0, sizeof(all));
		all[0].player_index = 0;
		all[1].player_index = 1;
		all[2].player_index = 2;
		all[3].player_index = 3;
		all[4].player_index = 4;
		memset(totals, 0xAB, sizeof(totals));
		carnage_report_team_totals(all, 5, totals);
		CHECK(totals[0].player_index == NONE && totals[1].player_index == NONE, "a total is a player's row");
		CHECK(totals[0].team_index == 0 && totals[1].team_index == 1, "the totals' teams: %ld %ld",
			totals[0].team_index, totals[1].team_index);
		CHECK(!totals[0].local && !totals[1].local, "a total is lit");
		CHECK(totals[0].kills == 6 && totals[0].assists == 5 && totals[0].deaths == 8 && totals[0].suicides == 3 &&
			totals[0].betrayals == 2, "red's totals: %ld %ld %ld %ld %ld", totals[0].kills, totals[0].assists,
			totals[0].deaths, totals[0].suicides, totals[0].betrayals);
		CHECK(totals[1].kills == 10 && totals[1].assists == 2 && totals[1].deaths == 4 && totals[1].suicides == 0 &&
			totals[1].betrayals == 1, "blue's totals: %ld %ld %ld %ld %ld", totals[1].kills, totals[1].assists,
			totals[1].deaths, totals[1].suicides, totals[1].betrayals);
		return 0;
	}
	CASE("row-height")
	{
		/* (the table's room, as game_engine_post_rasterize_post_game gives it) */
		short height = CARNAGE_TABLE_BOTTOM - CARNAGE_TABLE_TOP - 2 * 4;
		short available = height - CARNAGE_HEADING_HEIGHT;
		long count;

		for (count = 1; count <= CARNAGE_MAXIMUM_ROWS + 2; count++)
		{
			short row_height = carnage_report_row_height(count, height);

			CHECK(row_height <= CARNAGE_MAXIMUM_ROW_HEIGHT, "%ld rows %d high", count, row_height);
			CHECK(count * row_height <= available, "%ld rows of %d overflow the table's %d", count, row_height,
				available);
			/* (filling the table: as tall as they may be, else within a unit
			a row of its foot) */
			CHECK(row_height == CARNAGE_MAXIMUM_ROW_HEIGHT || available - count * row_height < count,
				"%ld rows of %d leave %ld of the table's %d empty", count, row_height, available - count * row_height,
				available);
		}
		/* (the most players and two teams' totals still readable) */
		CHECK(carnage_report_row_height(CARNAGE_MAXIMUM_ROWS + 2, height) >= 12, "the fullest table's rows are %d high",
			carnage_report_row_height(CARNAGE_MAXIMUM_ROWS + 2, height));
		return 0;
	}
	CASE("ratio")
	{
		CHECK(carnage_report_ratio_tenths(3, 2) == 15, "3 kills, 2 deaths: %ld", carnage_report_ratio_tenths(3, 2));
		CHECK(carnage_report_ratio_tenths(2, 3) == 7, "2 kills, 3 deaths: %ld", carnage_report_ratio_tenths(2, 3));
		CHECK(carnage_report_ratio_tenths(5, 0) == 50, "5 kills, no deaths: %ld", carnage_report_ratio_tenths(5, 0));
		CHECK(carnage_report_ratio_tenths(0, 0) == 0, "no kills or deaths: %ld", carnage_report_ratio_tenths(0, 0));
		return 0;
	}
	CASE("ping")
	{
		splitscreen_local = FALSE;
		CHECK(scoreboard_ping_shown(TRUE), "no ping in a network game with other machines");
		CHECK(!scoreboard_ping_shown(FALSE), "a ping in a game with no network");
		splitscreen_local = TRUE;
		CHECK(!scoreboard_ping_shown(TRUE), "a ping in a game of this machine alone (split screen, bots)");
		CHECK(scoreboard_column_width(TRUE) == scoreboard_column_width(FALSE) + SCOREBOARD_PING_WIDTH,
			"a column %d wide with its ping, %d without", scoreboard_column_width(TRUE), scoreboard_column_width(FALSE));
		CHECK(scoreboard_column_width(FALSE) ==
			SCOREBOARD_PLACE_WIDTH + SCOREBOARD_NAME_WIDTH + SCOREBOARD_SCORE_WIDTH,
			"a column without its ping %d wide", scoreboard_column_width(FALSE));
		return 0;
	}
	fprintf(stderr, "unknown case: %s\n", case_name);
	return 2;
}
