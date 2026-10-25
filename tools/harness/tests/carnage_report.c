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
only in a network game with other machines, its column then wider); on a
wide screen (the menus' wide layout: 16:10, 16:9, and 21:9 as 16:9) the
report as wide as the layout, on the screen, its columns spread across it
and the players' widest, and the scoreboard's names wider, its two team
columns still on the screen.
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

static void check_columns_between(boolean teams, short expected_count, short table_left, short table_right,
	struct carnage_column *columns)
{
	short count;
	short index;

	memset(columns, 0, (NUMBER_OF_CARNAGE_COLUMNS + 4) * sizeof(*columns));
	count = carnage_report_columns(teams, table_left, table_right, columns);
	CHECK(count == expected_count, "%s: %d columns, not %d", teams ? "teams" : "free for all", count, expected_count);
	CHECK(columns[0].x0 == table_left, "the first column starts at %d, not the table's left %d", columns[0].x0,
		table_left);
	CHECK(columns[count - 1].x1 == table_right, "the last column ends at %d, not the table's right %d",
		columns[count - 1].x1, table_right);
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

static void check_columns(boolean teams, short expected_count)
{
	struct carnage_column columns[NUMBER_OF_CARNAGE_COLUMNS + 4];

	check_columns_between(teams, expected_count, TABLE_LEFT, TABLE_RIGHT, columns);
}

/* the screens' widths (in the menus' units at 480 lines): 4:3, 16:10,
16:9 and 21:9 */
static long const screen_widths[] = { 640, 768, 852, 1138 };

/* the report on a screen this wide: on it, as wide as the menus' wide
layout, its columns spread across it (each at least as wide as at 4:3, the
player's the widest) */
static void check_wide_report(long screen_width, boolean teams)
{
	struct carnage_column columns[NUMBER_OF_CARNAGE_COLUMNS + 4];
	struct carnage_column narrow[NUMBER_OF_CARNAGE_COLUMNS + 4];
	short side = (short)(ui_widget_port_wide_extra_of(screen_width) / 2);
	short screen_side = (short)((screen_width - 640) / 2);
	short left, right, count, index;

	carnage_report_sides(screen_width, &left, &right);
	CHECK(left == CARNAGE_LEFT - side && right == CARNAGE_RIGHT + side, "at %ld wide: %d to %d", screen_width, left,
		right);
	CHECK(left >= -screen_side && right <= 640 + screen_side, "at %ld wide, %d to %d is off the screen", screen_width,
		left, right);
	check_columns_between(teams, teams ? NUMBER_OF_CARNAGE_COLUMNS : _carnage_column_betrayals,
		(short)(left + CARNAGE_TABLE_PADDING), (short)(right - CARNAGE_TABLE_PADDING), columns);
	count = carnage_report_columns(teams, TABLE_LEFT, TABLE_RIGHT, narrow);
	for (index = 0; index < count; index++)
	{
		short width = (short)(columns[index].x1 - columns[index].x0);
		short narrow_width = (short)(narrow[index].x1 - narrow[index].x0);

		CHECK(width >= narrow_width, "at %ld wide, column %d is %d wide, %d at 4:3", screen_width, index, width,
			narrow_width);
		CHECK(side == 0 || width > narrow_width, "at %ld wide, column %d is no wider than at 4:3", screen_width,
			index);
		CHECK(index == _carnage_column_player ||
			width < columns[_carnage_column_player].x1 - columns[_carnage_column_player].x0,
			"at %ld wide, column %d is wider than the players'", screen_width, index);
	}
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
	CASE("columns-wide")
	{
		size_t index;

		for (index = 0; index < sizeof(screen_widths) / sizeof(*screen_widths); index++)
		{
			check_wide_report(screen_widths[index], TRUE);
			check_wide_report(screen_widths[index], FALSE);
		}
		return 0;
	}
	/* (the scoreboard's names wider on a wider screen, to 16:9's; its two
	team columns, with their pings, on the screen, scaled as they are drawn) */
	CASE("scoreboard-wide")
	{
		size_t index;
		short previous = 0;

		CHECK(scoreboard_name_width(640) == SCOREBOARD_NAME_WIDTH, "4:3's names are %d wide",
			scoreboard_name_width(640));
		for (index = 0; index < sizeof(screen_widths) / sizeof(*screen_widths); index++)
		{
			short width = (short)screen_widths[index];
			short name_width = scoreboard_name_width(width);
			short columns = (short)(2 * scoreboard_column_width(TRUE, name_width) + SCOREBOARD_COLUMN_GAP);

			CHECK(name_width >= previous, "the names narrower on a wider screen: %d", name_width);
			CHECK(columns * SCOREBOARD_SCALE <= width, "at %d wide the team columns are %d", width, columns);
			previous = name_width;
		}
		CHECK(scoreboard_name_width(1138) == scoreboard_name_width(852), "21:9's names %d wide, 16:9's %d",
			scoreboard_name_width(1138), scoreboard_name_width(852));
		CHECK(scoreboard_name_width(852) > SCOREBOARD_NAME_WIDTH, "16:9's names no wider");
		return 0;
	}
	CASE("ping")
	{
		splitscreen_local = FALSE;
		CHECK(scoreboard_ping_shown(TRUE), "no ping in a network game with other machines");
		CHECK(!scoreboard_ping_shown(FALSE), "a ping in a game with no network");
		splitscreen_local = TRUE;
		CHECK(!scoreboard_ping_shown(TRUE), "a ping in a game of this machine alone (split screen, bots)");
		CHECK(scoreboard_column_width(TRUE, SCOREBOARD_NAME_WIDTH) ==
			scoreboard_column_width(FALSE, SCOREBOARD_NAME_WIDTH) + SCOREBOARD_PING_WIDTH,
			"a column %d wide with its ping, %d without", scoreboard_column_width(TRUE, SCOREBOARD_NAME_WIDTH),
			scoreboard_column_width(FALSE, SCOREBOARD_NAME_WIDTH));
		CHECK(scoreboard_column_width(FALSE, SCOREBOARD_NAME_WIDTH) ==
			SCOREBOARD_PLACE_WIDTH + SCOREBOARD_NAME_WIDTH + SCOREBOARD_SCORE_WIDTH,
			"a column without its ping %d wide", scoreboard_column_width(FALSE, SCOREBOARD_NAME_WIDTH));
		return 0;
	}
	fprintf(stderr, "unknown case: %s\n", case_name);
	return 2;
}
