"""The host's tick runs the client's tick after the prediction it took as it began (#287, stage 2b), not the
input heard before it, which follows that prediction by a sub-step."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run

SOURCE = "port/linux/game/network_distributed.c"
CASES = ["prediction-ahead", "input-ahead", "same", "no-input", "no-prediction", "far", "at-tick-end"]

# a fault in distributed_host_run_time and the case that must catch it
NEGATIVE_CONTROLS = {
    # the input heard alone: the host runs the client's tick before, a tick behind
    "input-only": (("? time : key;", "? key : key;"), "prediction-ahead"),
    # a prediction of a game begun again taken for the latest
    "no-far-check": (("time > key && time - key <= TICKS_PER_SECOND", "time > key"), "far"),
    # every player's, whatever its take point
    "every-player": (("!distributed_prediction_at_tick_start(player_index) || time == NONE", "time == NONE"),
                     "at-tick-end"),
}


def generated(fault=None):
    code = function(read(SOURCE), "distributed_host_run_time")
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("substep_take", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("substep_take", generated(fault)), case)
    assert status == CHECK_FAILED, output
