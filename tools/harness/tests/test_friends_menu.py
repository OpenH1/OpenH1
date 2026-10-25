"""The Friends screen's words and prompts (port/linux/game/friends_menu.inc): each row's note (green for a friend
online and a request), the prompts each row offers (JOIN only when a friend's presence offers its party or its invite
waits, with internet play on; ADD FRIEND only for a player of a code who is no friend yet, never an OpenCE player;
never two prompts of one key at once), the panel's words (a friend's game in UTF-8, a request's code, why an OpenCE
player cannot be added, appearing offline). The protocol under them is tools/p2p_friends_check.c's."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, function, mutated, read, run, structure  # noqa: E402

CASES = ["notes", "prompts", "panel"]
FUNCTIONS = ["friends_wide", "friends_wide_utf8", "friends_where", "friends_row_note", "friends_prompts_of",
             "friends_panel_text"]

# a fault in friends_menu.inc, and the case that must catch it
NEGATIVE_CONTROLS = {
    "join-without-link": (("if (online && (entry->joinable || entry->inviting))", "if (online)"), "prompts"),
    "invite-offline": (("\t\tif (online)\n\t\t\tSET_FLAG(prompts, _friends_prompt_invite, TRUE);",
                        "\t\tSET_FLAG(prompts, _friends_prompt_invite, TRUE);"), "prompts"),
    "add-a-friend-again": (("(entry->relation == _p2p_friend_none || entry->relation == _p2p_friend_request)", "1"),
                           "prompts"),
    "opence-addable": (("\t\tif (entry->has_code && (entry->relation == _p2p_friend_none ||",
                        "\t\tif ((entry->relation == _p2p_friend_none ||"), "prompts"),
    "hide-when-hidden": (("sharing ? _friends_prompt_hide : _friends_prompt_show",
                          "sharing ? _friends_prompt_show : _friends_prompt_hide"), "prompts"),
    "request-not-green": (("\t\t*live = TRUE;\n\t\tusnprintf(text, size - 1, L\"Wants to be friends\");",
                           "\t\tusnprintf(text, size - 1, L\"Wants to be friends\");"), "notes"),
    "invite-not-told": (("\tif (entry->inviting)\n\t\treturn L\"Invites you\";\n", ""), "notes"),
    "utf8-read-as-bytes": (("character = character << 6 | (*bytes++ & 0x3F);", "character = *bytes++;"), "panel"),
    "players-left-out": (("L\"%s (%d of %d).\", game, entry->players, entry->maximum_players);",
                          "L\"%s.\", game);"), "panel"),
}


def generated(fault=None):
    p2p = read("port/linux/src/p2p.h")
    menu = read("port/linux/game/friends_menu.inc")
    config = "".join(f"#define {name} {constant(p2p, name)}\n" for name in (
        "P2P_FRIEND_CODE_BYTES", "P2P_FRIEND_CODE_TEXT_SIZE", "P2P_FRIEND_NAME_SIZE", "P2P_FRIEND_GAME_SIZE"))
    config += enum_with(p2p, "\t_p2p_friend_request,") + "\n" + enum_with(p2p, "_p2p_party_place_menus") + "\n"
    config += structure(p2p, "p2p_friend") + "\n" + enum_with(menu, "_friends_prompt_copy") + "\n"
    code = "\n\n".join(function(menu, name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("friends_menu", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("friends_menu", generated(fault)), case)
    assert status == CHECK_FAILED, f"friends_menu.inc with a fault ({control}) passed '{case}': the test cannot see it"
