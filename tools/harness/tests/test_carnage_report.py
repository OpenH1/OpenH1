"""The carnage report's and the in-game scoreboard's tables (carnage_report.c): the report's columns (betrayals in a
team game only, the numbers right, across the table), its rows (each player's statistics, this machine's players
lit), the teams' totals, the rows' height (filling the table), kills per death, and the scoreboard's ping (only in a
network game with other machines), with game_engine.c's own code; and both on a wide screen (4:3, 16:10, 16:9, 21:9:
menu_tags.c's wide layout), the report's columns spread across it and the scoreboard's names wider, on the screen."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure  # noqa: E402

CASES = ["columns-teams", "columns-free-for-all", "rows", "totals", "row-height", "ratio", "ping", "columns-wide",
         "scoreboard-wide"]
FUNCTIONS = ("carnage_report_sides", "carnage_report_columns", "carnage_report_rows", "carnage_report_team_totals",
             "carnage_report_row_height", "carnage_report_ratio_tenths", "scoreboard_ping_shown",
             "scoreboard_name_width", "scoreboard_column_width")

# a fault in the code, the function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    # betrayals in a free for all too
    "betrayals-always": (("short count = teams ? NUMBER_OF_CARNAGE_COLUMNS : _carnage_column_betrayals;",
                          "short count = NUMBER_OF_CARNAGE_COLUMNS;"), "carnage_report_columns",
                         "columns-free-for-all"),
    # the numbers left of their headings
    "numbers-left": (("columns[kind].justification = kind > _carnage_column_player ? 1 : 0;",
                      "columns[kind].justification = 0;"), "carnage_report_columns", "columns-teams"),
    # the player's column as wide as nothing, the table short of its right
    "player-unsized": (("? (short)MAX(right - left - fixed, 0) : spread[kind]", "? 0 : spread[kind]"),
                       "carnage_report_columns", "columns-teams"),
    # only the first local player's row lit
    "second-player-unlit": (("row->local = player->local_player_index != NONE;",
                             "row->local = player->local_player_index == 0;"), "carnage_report_rows", "rows"),
    # betrayals read as suicides
    "betrayals-as-suicides": (("row->betrayals = player->statistics.friendly_fire_kills;",
                               "row->betrayals = player->statistics.suicides;"), "carnage_report_rows", "rows"),
    # a team's assists counted as its kills
    "totals-assists": (("total->assists += player->statistics.assists[0];",
                        "total->assists += player->statistics.kills[0];"), "carnage_report_team_totals", "totals"),
    # the totals added to what was there before
    "totals-not-cleared": (("csmemset(&totals[team], 0, sizeof(totals[team]));", ""), "carnage_report_team_totals",
                           "totals"),
    # the rows' height without the tallest's limit
    "rows-uncapped": (("MIN(CARNAGE_MAXIMUM_ROW_HEIGHT, (height - CARNAGE_HEADING_HEIGHT) / MAX(row_count, 1))",
                       "((height - CARNAGE_HEADING_HEIGHT) / MAX(row_count, 1))"), "carnage_report_row_height",
                      "row-height"),
    # the rows' height forgetting the totals' two rows
    "rows-overflow": (("/ MAX(row_count, 1)", "/ MAX(row_count - 2, 1)"), "carnage_report_row_height", "row-height"),
    # kills per death truncated
    "ratio-truncated": (("(kills * 10 + divisor / 2) / divisor", "(kills * 10) / divisor"),
                        "carnage_report_ratio_tenths", "ratio"),
    # the pings of a game of this machine alone (all 0)
    "ping-local": (("return network && !network_game_is_splitscreen_local();", "return network;"),
                   "scoreboard_ping_shown", "ping"),
    # the column as wide with no ping
    "ping-width": (("(ping_shown ? SCOREBOARD_PING_WIDTH : 0)", "SCOREBOARD_PING_WIDTH"), "scoreboard_column_width",
                   "ping"),
    # the report the 640's on a wide screen
    "report-narrow": (("short half = (short)(ui_widget_port_wide_extra_of(screen_width) / 2);", "short half = 0;"),
                      "carnage_report_sides", "columns-wide"),
    # the wider table's room all the players' column's
    "columns-unspread": (("(short)(widths[kind] * MAX(table, CARNAGE_TABLE_WIDTH) / CARNAGE_TABLE_WIDTH)",
                          "widths[kind]"), "carnage_report_columns", "columns-wide"),
    # the names the whole extra width wider: the team columns off the screen
    "names-too-wide": (("ui_widget_port_wide_extra_of(window_width) / 2 / SCOREBOARD_SCALE",
                        "ui_widget_port_wide_extra_of(window_width) * 2 / SCOREBOARD_SCALE"), "scoreboard_name_width",
                       "scoreboard-wide"),
}


def generated(fault=None, faulty_function=None):
    engine = read("source/game/game_engine.c")
    tags = read("port/linux/game/menu_tags.c")
    config = enum_with(engine, "_carnage_column_place") + "\n" + enum_with(engine, "SCOREBOARD_PLACE_WIDTH") + "\n"
    config += re.search(r"^#define SCOREBOARD_SCALE [^\n]+", engine, re.M).group(0) + "\n"
    config += "\n".join(re.findall(r"^#define WIDE_LAYOUT_\w+ [^\n]+", tags, re.M)) + "\n"
    config += function(tags, "ui_widget_port_wide_extra_of") + "\n"
    for name in ("statistic_buffer", "carnage_column", "carnage_row"):
        config += structure(engine, name) + "\n"
    text = ""
    for name in FUNCTIONS:
        code = function(engine, name)
        if fault and name == faulty_function:
            code = mutated(code, *fault)
        text += code + "\n"
    return (("config.inc", config), ("under_test.inc", text))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("carnage_report", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, name, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("carnage_report", generated(fault, name)), case)
    assert status == CHECK_FAILED, f"{control} was not caught: {output}"
