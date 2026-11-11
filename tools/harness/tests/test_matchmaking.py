"""Matchmaking's decisions (port/linux/game/matchmaking.c): the game a party takes from the public listings (its
playlist's, an OpenH1 game of this build, open, no password, room for the whole party, by ping and then the fullest),
who may search, how a hosted game starts (full at once, else after a wait with bots or its least players), and a
search's steps (looked for, reached, joined, failed and looked again, the lead passed or hosted when none fits, the new
leader followed), and the playlists themselves (each listed by its number, which builds must agree on; teams that a
party fits on; a row of the Matchmaking screen for each), and 2V2's kinds (Classic and Lowest Latency: each its own
playlist and tick rate, never matched with the other's games). The party's host election is tools/p2p_party_check.c's "host" case; the listing's playlist
p2p_lobby_check.c's."""

import re

import pytest

from tools import port_settings
from tools.harness import build, constant, enum_with, function, mutated, read, run, structure

CASES = ["pick", "problem", "start", "look", "join", "host", "playlists", "doubles"]
FUNCTIONS = ["matchmaking_game_score", "matchmaking_pick", "matchmaking_search_problem", "matchmaking_start_choice",
             "matchmaking_step"]


def generated(fault=None):
    p2p = read("port/linux/src/p2p.h")
    header = read("port/linux/game/matchmaking.h")
    source = read("port/linux/game/matchmaking.c")
    config = enum_with(p2p, "P2P_LISTING_NAME_SIZE") + "\n" + structure(p2p, "p2p_listing") + "\n"
    config += enum_with(p2p, "_p2p_join_reached") + "\n"
    config += enum_with(header, "_matchmaking_looking") + "\n" + enum_with(header, "_matchmaking_problem_member") + "\n"
    config += enum_with(header, "_matchmaking_doubles_classic") + "\n"
    for member in ("MATCHMAKING_MAXIMUM_MAPS = ", "_matchmaking_action_reach,", "_matchmaking_start_bots,"):
        config += enum_with(source, member) + "\n"
    config += structure(source, "matchmaking_view") + "\n"
    config += enum_with(header, "MATCHMAKING_PLAYLIST_NAME_SIZE") + "\n" + structure(source, "matchmaking_playlist")
    config += "\n" + re.search(r"^static struct matchmaking_playlist const matchmaking_playlists\[\] =\n\{.*?^\};",
                                source, re.M | re.S).group(0) + "\n"
    config += re.search(r"^static short const matchmaking_doubles_playlists\[.*?;", source, re.M | re.S).group(0) + "\n"
    config += f"#define MATCHMAKING_ROWS {rows()}\n"
    code = "\n\n".join(function(source, name) for name in FUNCTIONS)
    if fault and fault[0] in code:
        code = mutated(code, *fault)
    elif fault:
        config = mutated(config, *fault)
    return (("config.inc", config), ("under_test.inc", code))


def rows():
    """The Matchmaking screen's rows (matchmaking_menu.inc's)."""
    return constant(read("port/linux/game/matchmaking_menu.inc"), "MATCHMAKING_ROWS")


def test_screen_rows():
    """The screen's rows as the menus' generator writes them (tools/port_settings.py) and as the game fills them."""
    assert port_settings.MATCHMAKING_ROWS == rows()


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("matchmaking", generated()), case)
    assert status == 0, output


# a fault, and the case that must catch it
FAULTS = {
    "any-playlist": (("if (game->playlist != playlist || !playlist || !game->standard",
                      "if (!playlist || !game->standard"), "pick"),
    "legacy-picked": (("|| !playlist || !game->standard || !game->open", "|| !playlist || !game->open"), "pick"),
    "password-picked": ((" || game->locked || game->failed", " || game->failed"), "pick"),
    "failed-picked": ((" || game->failed ||\n\t\t!game->invite[0])", " ||\n\t\t!game->invite[0])"), "pick"),
    "party-split": (("game->player_count + party_size > game->maximum_player_count",
                     "game->player_count + 1 > game->maximum_player_count"), "pick"),
    "emptiest-first": (("+ game->player_count * 10L", "- game->player_count * 10L"), "pick"),
    "ping-ignored": (("- (ping / MATCHMAKING_PING_STEP) * 1000L", "- (ping / MATCHMAKING_PING_STEP) * 0L"), "pick"),
    "unknown-ping-best": (("game->ping >= 0 ? game->ping : MATCHMAKING_UNKNOWN_PING", "game->ping >= 0 ? game->ping : 0"),
                          "pick"),
    "member-searches": (("\tif (member)\n\t\treturn _matchmaking_problem_member;\n", ""), "problem"),
    "party-too-big": (("if (party_size > party_most)", "if (0)"), "problem"),
    "never-starts-full": (("if (most > 0 && humans >= most)", "if (0)"), "start"),
    "no-wait": (("if (waited < MATCHMAKING_FILL_MILLISECONDS)\n\t\treturn _matchmaking_start_wait;", ""), "start"),
    "starts-too-few": (("return humans >= minimum ? _matchmaking_start_now : _matchmaking_start_wait;",
                        "return _matchmaking_start_now;"), "start"),
    "picked-at-once": (("view->best != NONE && view->searched >= MATCHMAKING_LOOK_MILLISECONDS",
                        "view->best != NONE"), "look"),
    "never-hosts": (("if (view->searched >= view->search_time)", "if (0)"), "look"),
    "member-searches-on": (("\t\tif (!view->leading)\n\t\t\treturn _matchmaking_following;\n\t\tif (view->best",
                            "\t\tif (view->best"), "look"),
    "reach-forever": ((" || view->waited > MATCHMAKING_REACH_MILLISECONDS)", ")"), "join"),
    "joined-at-reach": (("if (view->join_stage == _p2p_join_reached && view->entered)",
                         "if (view->join_stage == _p2p_join_reached)"), "join"),
    "refused-as-joined": (("\t\tif (view->in_game)\n\t\t{\n\t\t\t*action = _matchmaking_action_done;\n"
                           "\t\t\treturn _matchmaking_idle;\n\t\t}\n\t\tif ((view->in_none",
                           "\t\tif (view->in_none)\n\t\t{\n\t\t\t*action = _matchmaking_action_done;\n"
                           "\t\t\treturn _matchmaking_idle;\n\t\t}\n\t\tif ((view->in_none"), "join"),
    "follow-failure-searches": (("return view->following ? _matchmaking_following : _matchmaking_looking;\n\t\t}\n"
                                 "\t\treturn _matchmaking_reaching;",
                                 "return _matchmaking_looking;\n\t\t}\n\t\treturn _matchmaking_reaching;"), "join"),
    "handoff-forever": (("if (view->waited > MATCHMAKING_HANDOFF_MILLISECONDS)", "if (0)"), "host"),
    "new-leader-not-followed": (("\t\tif (view->leader_game)\n", "\t\tif (0)\n"), "host"),
    "followed-for-ever": (("if (view->waited > MATCHMAKING_FOLLOW_MILLISECONDS)", "if (0)"), "host"),
    "playlist-inserted": (('\t{ "Slayer", L"SLAYER"', '\t{ "Doubles", L"DOUBLES", L"", { "team_slayer", NULL },\n'
                           '\t\t{ "wizard", NULL }, 2, 4, 2, 2 },\n\t{ "Slayer", L"SLAYER"'), "playlists"),
    "party-split-teams": (("\t\t2, 4, 2, 2, 30, TRUE },", "\t\t2, 4, 2, 4, 30, TRUE },"), "playlists"),
    "rate-ignored": (("\tif (rate && game->rate != rate)\n\t\treturn NONE;", ""), "doubles"),
    "kinds-swapped": (("matchmaking_doubles_playlists[NUMBER_OF_MATCHMAKING_DOUBLES_KINDS] = { 6, 5 };",
                       "matchmaking_doubles_playlists[NUMBER_OF_MATCHMAKING_DOUBLES_KINDS] = { 5, 6 };"), "doubles"),
    "classic-at-120": (("\t\t2, 4, 2, 2, 30, TRUE },", "\t\t2, 4, 2, 2, 120, TRUE },"), "doubles"),
    "classic-instant-aim": (("\t\t2, 4, 2, 2, 30, TRUE },", "\t\t2, 4, 2, 2, 30, FALSE },"), "doubles"),
    "kinds-maps-differ": (('{ "chillout", "hangemhigh", "prisoner", "damnation", "carousel", NULL, NULL, NULL, NULL },\n'
                           '\t\t2, 4, 2, 2, 30',
                           '{ "chillout", "hangemhigh", "prisoner", "damnation", "wizard", NULL, NULL, NULL, NULL },\n'
                           '\t\t2, 4, 2, 2, 30'), "doubles"),
    "server-waited-for-ever": (("if (!view->hosting && view->waited > MATCHMAKING_HOST_MILLISECONDS)", "if (0)"),
                               "host"),
}


@pytest.mark.parametrize("fault", sorted(FAULTS))
def test_negative_control(fault):
    change, case = FAULTS[fault]
    status, output = run(build("matchmaking", generated(change)), case)
    assert status == 1, f"{fault} was not caught by {case}: {output}"
