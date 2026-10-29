"""King of the Hill's Multi-Hill (source/game/game_engine_king.c, OpenCE's #467, by s3anyboy): the variant's
multi_hill gives the hills in play at once (2 to 5, else one), each a hill of the map with an area and none twice,
moved off the ones they were where the map has others; the host sends the hills after the first behind January's
state, in OpenCE's layout (a game of one hill sends January's state alone), and a client takes them, refusing a
state of another size or a count out of range."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import (CHECK_FAILED, build, constant, enum_with, function, mutated, read,  # noqa: E402
                     run, structure)

CASES = ["count-from-option", "map-has-fewer", "no-area-left-out", "moving", "one-hill-state", "state-round-trip",
         "state-refused"]

KING = "source/game/game_engine_king.c"
FUNCTIONS = ["king_hills_wanted", "king_hill_in_play", "king_pick_extra_hills", "king_hills_to_globals",
             "game_engine_king_write_network_state", "game_engine_king_read_network_state"]

# a fault in the code, and the case that must catch it
NEGATIVE_CONTROLS = {
    "one-hill-always": (("return wanted >= 2 ? MIN(wanted, MAXIMUM_HILLS_AT_ONCE) : 1;", "return 1;"),
                        "count-from-option"),
    "a-hill-twice": (("if (king_hill_in_play(hill_id, king_hill_count) || (pass == 0 && was_in_play))",
                      "if (pass == 0 && was_in_play)"), "count-from-option"),
    "no-area-taken": (("found = hill->hill_point_count > 0;", "found = TRUE;"), "no-area-left-out"),
    "moving-stays": (("for (pass = moving ? 0 : 1; pass < 2 && !found; pass++)",
                      "for (pass = 1; pass < 2 && !found; pass++)"), "moving"),
    "always-extra-state": (("if (king_hill_count <= 1)\n\t\treturn sizeof(king_globals);",
                            "if (king_hill_count < 1)\n\t\treturn sizeof(king_globals);"), "one-hill-state"),
    "count-off-by-one": (("extra_hills.hill_count = king_hill_count - 1;",
                          "extra_hills.hill_count = king_hill_count;"), "state-round-trip"),
    "count-unchecked": (("if (!VALID_INDEX(extra_hills.hill_count, MAXIMUM_HILLS_AT_ONCE))\n\t\treturn FALSE;",
                         ";"), "state-refused"),
}


def types():
    king = read(KING)
    header = read("source/game/game_engine_king.h")
    limits = read("port/linux/include/halo_port_limits.h")
    constants = {name: constant(king, name) for name in ("MAXIMUM_HILL_POINTS", "MAXIMUM_HILLS",
                                                           "MAXIMUM_HILLS_AT_ONCE", "_multiplayer_sound_hill_move")}
    return "\n".join([
        f"#define HALO_PORT_MAXIMUM_NETWORK_PLAYERS {constant(limits, 'HALO_PORT_MAXIMUM_NETWORK_PLAYERS')}",
        "enum { " + ", ".join(f"{name} = {value}" for name, value in constants.items()) + " };",
        enum_with(header, "king_hill_uncontrolled"),
        structure(header, "king_globals"),
        structure(king, "king_hill"),
        structure(king, "king_extra_hills_network_state"),
    ]) + "\n"


def generated(fault=None):
    king = read(KING)
    code = "\n\n".join(function(king, name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    return (("king_types.inc", types()), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("king_multi_hill", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("king_multi_hill", generated(fault)), case)
    assert status == CHECK_FAILED, f"Multi-Hill with a fault ({control}) passed '{case}': the test cannot see it"
