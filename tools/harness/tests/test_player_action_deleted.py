"""A player's action let go with its object (source/game/players.c's players_handle_deleted_object): the action the HUD
draws a prompt for is chosen once a tick, and its object can be deleted before the next tick chooses again (an item
another unit takes); the HUD then read the deleted object (a null pointer in hud_show_action_response). The deleted
object's players' actions are cleared, the others' kept, and a deleted unit's player still dies."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["item", "unit"]

# a fault, and the case that must catch it
NEGATIVE_CONTROLS = {
    "action-kept": (("\t\t\tplayer->action_object_index = NONE;\n", ""), "item"),
    "every-action-let-go": (("if (player->action_object_index == deleted_object_index)", "if (1)"), "item"),
    "unit-player-lives": (("player_died(iterator.datum_index);", ";"), "unit"),
}


def generated(fault=None):
    code = function(read("source/game/players.c"), "players_handle_deleted_object")
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code + "\n"),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("player_action_deleted", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("player_action_deleted", generated(fault)), case)
    assert status == CHECK_FAILED, f"players.c with a fault ({control}) passed '{case}': the test cannot see it"
