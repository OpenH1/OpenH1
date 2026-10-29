"""A player host's next game and players (port/linux/game/network_host_game.c's pure parts: host_game.c): the
playlist's next entry that can be played, round from its end; a ban's and a mute's duration as the host's console and
Host Controls take it; and the teams shuffled (as many on each, drawn from the seed) or balanced (as few moved as can
be; by score where there are scores), the teams' colours kept for the most players. Each check has a fault that it must
catch."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, function, mutated, read, run  # noqa: E402

SOURCE = "port/linux/game/network_host_game.c"
CASES = ["playlist", "duration", "shuffle", "balance"]
PURE = ("network_host_game_playlist_after", "network_host_game_duration_parse", "network_host_game_teams_plan")

# a fault in the code, and the case that must catch it
NEGATIVE_CONTROLS = {
    "playlist-no-wrap": ("long candidate = (index + step) % playlist->count;", "long candidate = index + step;",
                         "playlist"),
    "playlist-plays-rejected": ("if (!playlist->entries[candidate].rejected[0])", "if (TRUE)", "playlist"),
    "playlist-shorter-kept": ("if (index < 0 || index >= playlist->count)", "if (index < 0)", "playlist"),
    "duration-hours-as-minutes": ("case 'h': unit = 60 * 60; break;", "case 'h': unit = 60; break;", "duration"),
    "duration-no-forever": ('!csstrcmp(text, "forever")', "FALSE", "duration"),
    "duration-trailing": ("if ((*end && end[1]) || number <= 0)", "if (number <= 0)", "duration"),
    "shuffle-one-team": ("teams[order[index]] = (char)(index % HOST_GAME_TEAMS);", "teams[order[index]] = 0;",
                         "shuffle"),
    "shuffle-no-seed": ("seed = seed * 1664525UL + 1013904223UL;", "seed = 7;", "shuffle"),
    "balance-moves-everyone": ("if (teams[index] == larger)", "if (TRUE)", "balance"),
    "balance-scores-ignored": ("\telse if (scored)", "\telse if (scored && FALSE)", "balance"),
    "colours-not-kept": ("if (kept * 2 < count)", "if (FALSE)", "balance"),
}


def generated(fault=None):
    source = read(SOURCE)
    rotation = read("port/linux/src/server_rotation.h")
    limits = read("port/linux/include/halo_port_limits.h")
    config = rotation[rotation.index("enum\n{\n\tSERVER_ROTATION_MAXIMUM_ENTRIES"):
                      rotation.index("/* one entry's text read")]
    config += enum_with(read("port/linux/game/network_host_game.h"), "_host_game_shuffle,") + ";\n"
    config += f"#define MAXIMUM_HOST_GAME_PLAYERS {constant(limits, 'HALO_PORT_MAXIMUM_NETWORK_PLAYERS')}\n"
    config += f"#define HOST_GAME_TEAMS {constant(source, 'HOST_GAME_TEAMS')}\n"
    text = ""
    for name in PURE:
        code = function(source, name)
        if fault:
            try:
                code = mutated(code, fault[0], fault[1])
            except LookupError:
                pass
        text += code + "\n"
    if fault:
        assert fault[0] not in text, f"the fault {fault[0]!r} is in none of the pure parts"
    return (("config.inc", config), ("under_test.inc", text))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("host_game", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    before, after, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("host_game", generated((before, after))), case)
    assert status == CHECK_FAILED, f"network_host_game.c with a fault ({control}) passed '{case}': the test cannot see it"
