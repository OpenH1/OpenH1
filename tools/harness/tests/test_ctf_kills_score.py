"""Capture the Flag's Kills Score (source/game/game_engine_ctf.c, OpenCE's #467, by s3anyboy): with it on, a kill
scores a point for the killer's team (a betrayal takes one away), a capture is worth the variant's points_per_capture
(25 where it keeps none), and a player's score is their kills less their betrayals and their captures' points; off,
Capture the Flag scores as January's. A distributed client, whose scores are the host's, adds nothing itself."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, function, mutated, read, run  # noqa: E402

CASES = ["off", "kills-score", "capture-points", "player-points"]

CTF = "source/game/game_engine_ctf.c"
FUNCTIONS = ["ctf_kills_score", "ctf_capture_points", "ctf_engine_player_killed_player", "ctf_player_points",
             "ctf_get_score"]

# a fault in the code, and the case that must catch it
NEGATIVE_CONTROLS = {
    "betrayal-scores": (("friendly_fire ? -CTF_KILL_POINTS : CTF_KILL_POINTS", "CTF_KILL_POINTS"), "kills-score"),
    "client-scores": (("!network_game_distributed_client() && ", ""), "kills-score"),
    "quitter-scores": (("!dead_player->quit_out_of_game && ", ""), "kills-score"),
    "kills-always-score": (("if (ctf_kills_score() && ", "if ("), "off"),
    "no-default-points": (("return points > 0 ? points : CTF_DEFAULT_CAPTURE_POINTS;", "return points;"),
                          "capture-points"),
    "points-when-off": (("if (!ctf_kills_score())\n\t\treturn 1;", ";"), "capture-points"),
    "betrayals-kept": (("-\n\t\t(long)player->statistics.friendly_fire_kills * CTF_KILL_POINTS +", "+"),
                       "player-points"),
    "captures-count-one": (("flag_scores * ctf_capture_points();", "flag_scores;"), "player-points"),
}


def types():
    ctf = read(CTF)
    return "enum { " + ", ".join(f"{name} = {constant(ctf, name)}" for name in (
        "NUMBER_OF_CTF_TEAMS", "CTF_KILL_POINTS", "CTF_DEFAULT_CAPTURE_POINTS")) + " };\n"


def generated(fault=None):
    ctf = read(CTF)
    code = "\n\n".join(function(ctf, name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    return (("ctf_types.inc", types()), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("ctf_kills_score", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("ctf_kills_score", generated(fault)), case)
    assert status == CHECK_FAILED, f"Kills Score with a fault ({control}) passed '{case}': the test cannot see it"
