"""What a host's tick takes of a client's player's predictions as it begins (#303): in the phase where each prediction
comes before its tick's input, it holds once and then runs each input from the prediction before it, not the client's
tick after the input it has; with the prediction of the input heard lost, it takes none rather than one a tick behind."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run

SOURCE = "port/linux/game/network_distributed.c"
CASES = ["caught-up", "no-input", "far", "earlier", "lost", "earlier-ahead", "hold", "held-once", "late-new-input",
         "out-of-range", "steady-late"]

# a fault in distributed_host_take_choice and the case that must catch it
NEGATIVE_CONTROLS = {
    # the take before the fix: never held, the late phase kept for the whole game
    "never-hold": (("if (distributed_host_takes.last_run[player_index] == key && "
                    "!distributed_host_takes.held[player_index])", "if (FALSE)"), "steady-late"),
    # held every tick: the player's input never run
    "always-hold": (("&& !distributed_host_takes.held[player_index])", ")"), "held-once"),
    # a lost prediction's one before it taken, a tick behind the input run
    "lost-taken": (("earlier_time == key ? _distributed_take_earlier : _distributed_take_lost",
                    "_distributed_take_earlier"), "lost"),
    # a prediction ahead of the input taken though its input has not come
    "no-ahead-check": (("!(time > key && time - key <= TICKS_PER_SECOND)", "!(time - key <= TICKS_PER_SECOND)"),
                       "caught-up"),
}


def generated(fault=None):
    code = function(read(SOURCE), "distributed_host_take_choice")
    if fault:
        code = mutated(code, *fault)
    header = read("port/linux/game/network_distributed.h")
    config = re.search(r"(enum\s*\{\s*_distributed_take_none.*?\};)", header, re.S)[1] + "\n"
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("host_take_choice", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("host_take_choice", generated(fault)), case)
    assert status == CHECK_FAILED, output
