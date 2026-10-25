"""Room held for a party joining a game (port/linux/game/network_party.c's holds: granted, lapsing, let go, taken
by their members alone, bots making room, a party's team) and what the server browser says of a game's room for
the party (menu_functions.c's browser_party_room). The leader's ask and the host's answer are messages of their
own (NETCODE.md, "Parties in public games"); a real run of several copies checks those."""

import pytest

from tools.harness import build, constant, enum_with, function, mutated, read, run, structure

CASES = ["grant", "expire", "release", "admit", "bots", "team", "room"]
HOLDS = ["party_holds_count", "party_holds_find", "party_holds_release", "party_holds_grant", "party_holds_expire",
         "party_holds_admit", "party_team_choice"]


def generated(fault=None):
    party = read("port/linux/game/network_party.c")
    menus = read("port/linux/game/menu_functions.c")
    p2p = read("port/linux/src/p2p.h")
    config = "".join(f"#define {name} {constant(p2p + party, name)}\n"
                     for name in ("MAXIMUM_PARTY_HOLDS", "PARTY_HOLD_MILLISECONDS", "PARTY_KEY_HASH_SIZE"))
    config += structure(party, "party_hold") + "\n"
    config += enum_with(menus, "_browser_party_held") + "\n"
    code = "\n\n".join(function(party, name) for name in HOLDS)
    code += "\n\n" + function(menus, "browser_party_room")
    if fault:
        code = mutated(code, *fault)
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("party_holds", generated()), case)
    assert status == 0, output


# a fault, and the case that must catch it
FAULTS = {
    "held-past-the-room": (("member < count && granted < room", "member < count && granted <= room"), "grant"),
    "earlier-holds-kept": (("\tparty_holds_release(holds, group);\n\troom", "\troom"), "grant"),
    "held-twice": (("\t\tif (party_holds_find(holds, key_hash) != NONE)\n\t\t\tcontinue;\n", ""), "grant"),
    "never-lapse": (("now - holds[index].time >= PARTY_HOLD_MILLISECONDS", "0"), "expire"),
    "every-party-let-go": (("holds[index].used && holds[index].group == group", "holds[index].used"), "release"),
    "stranger-takes-held": (("return players - bots + party_holds_count(holds) < maximum;",
                             "return players - bots < maximum;"), "admit"),
    "member-refused": (("\t\treturn players - bots < maximum;\n", "\t\treturn FALSE;\n"), "admit"),
    "bots-stay": (("room = (short)(maximum - (players - bots) - party_holds_count(holds));",
                   "room = (short)(maximum - players - party_holds_count(holds));"), "bots"),
    "team-overfilled": (("return team_counts[team] < (maximum + 1) / 2 ? team : NONE;", "return team;"), "team"),
    "team-of-the-fewest": (("team = (short)(party_counts[1] > party_counts[0] ? 1 : 0);",
                            "team = (short)(party_counts[1] > party_counts[0] ? 0 : 1);"), "team"),
    "room-for-one-less": (("if (maximum > 0 && players + size > maximum)",
                           "if (maximum > 0 && players + size > maximum + 1)"), "room"),
    "always-held": (("return holds ? _browser_party_held : _browser_party_unheld;", "return _browser_party_held;"),
                    "room"),
}


@pytest.mark.parametrize("fault", sorted(FAULTS))
def test_negative_control(fault):
    change, case = FAULTS[fault]
    status, output = run(build("party_holds", generated(change)), case)
    assert status == 1, f"{fault} was not caught by {case}: {output}"
