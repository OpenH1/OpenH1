"""A party member following its leader into its game (port/linux/game/party_menu.inc): what a game found allows,
and the follow's steps. The party's own state machine and messages are tools/p2p_party_check.c's
(tools/test_linux_port.py)."""

import pytest

from tools.harness import build, constant, enum_with, function, mutated, read, run, structure

CASES = ["verdict", "reach", "find", "join"]
TIMES = ["PARTY_REACH_TIME", "PARTY_FIND_TIME", "PARTY_OPEN_WAIT", "PARTY_JOIN_TIME", "PARTY_READY_TIME"]


def generated(fault=None):
    p2p = read("port/linux/src/p2p.h")
    menus = read("port/linux/game/party_menu.inc")
    config = enum_with(p2p, "_p2p_join_reached") + "\n" + enum_with(p2p, "_p2p_party_follow_joined") + "\n"
    config += enum_with(menus, "_party_follow_reaching") + "\n"
    config += "".join(f"#define {name} {constant(menus, name)}\n" for name in TIMES)
    config += structure(menus, "party_follow_view") + "\n"
    code = "\n\n".join(function(menus, name) for name in ("party_game_verdict", "party_follow_step"))
    if fault:
        code = mutated(code, *fault)
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("party_follow", generated()), case)
    assert status == 0, output


# a fault, and the case that must catch it
FAULTS = {
    "legacy-as-version": (("return standard || mixed ? _p2p_party_follow_version : _p2p_party_follow_legacy;",
                           "return _p2p_party_follow_version;"), "verdict"),
    "full-joined": (("if (maximum > 0 && players >= maximum && !standard && !mixed)", "if (0)"), "verdict"),
    "full-openh1-given-up": ((" && !standard && !mixed)", ")"), "verdict"),
    "closed-joined": (("if (!open && !in_progress)", "if (0)"), "verdict"),
    "screen-left-ignored": (("if (!view->screen)\n\t\t\treturn _party_follow_idle;", ""), "reach"),
    "reach-forever": ((" || view->waited > PARTY_REACH_TIME", ""), "reach"),
    "open-forever": (("view->waited <= PARTY_OPEN_WAIT", "1"), "find"),
    "refused-as-joined": (("\t\t\t*result = _p2p_party_follow_refused;", "\t\t\t*result = _p2p_party_follow_joined;"),
                          "join"),
}


@pytest.mark.parametrize("fault", sorted(FAULTS))
def test_negative_control(fault):
    change, case = FAULTS[fault]
    status, output = run(build("party_follow", generated(change)), case)
    assert status == 1, f"{fault} was not caught by {case}: {output}"
