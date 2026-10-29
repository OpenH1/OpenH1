"""The host's sub-step controls of a client's player in a tick that runs the client's tick after the input it took
(#303): none of that tick come yet, the latest of the input taken is held, not that input's tick-start actions,
which would step the player back by the sub-steps its tick before ran."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, function, mutated, read, run, structure

SOURCE = "port/linux/game/network_substeps.c"
CASES = ["late-input", "later-came", "lost-substeps", "fresh", "taken", "older", "no-input", "record-came",
         "other-count"]

# a fault in host_live_apply and the case that must catch it
NEGATIVE_CONTROLS = {
    # the take before the fix: nothing of the tick run come, nothing given
    "no-fallback": (("for (earlier = key - 1; input_key != NONE", "for (earlier = key - 1; FALSE"), "late-input"),
    # older than the input taken: its own actions overridden by the tick before's
    "past-input": (("earlier >= input_key &&", "earlier >= input_key - 1 &&"), "older"),
    # the first come of the earlier tick, not its latest
    "earliest": (("for (substep = MAXIMUM_SIM_SUBSTEPS - 1; tick->key == earlier && substep >= 0 && !live; substep--)",
                  "for (substep = 0; tick->key == earlier && substep < MAXIMUM_SIM_SUBSTEPS && !live; substep++)"),
                 "late-input"),
}


def generated(fault=None):
    source = read(SOURCE)
    code = function(source, "host_live_apply")
    if fault:
        code = mutated(code, *fault)
    substeps = constant(read("port/linux/game/sim_rate.h"), "MAXIMUM_SIM_SUBSTEPS")
    config = f"enum {{ MAXIMUM_SIM_SUBSTEPS = {substeps}, MAXIMUM_LATER_SUBSTEPS = MAXIMUM_SIM_SUBSTEPS - 1, " \
             f"HOST_LIVE_TICKS = {constant(source, 'HOST_LIVE_TICKS')} }};\n"
    for name in ("substep_control", "substep_record", "live_control"):
        config += structure(source, name) + "\n"
    config += re.search(r"(struct host_live_tick\s*\{.*?\n\t\})", source, re.S)[1] + ";\n"
    config += function(source, "live_control_later") + "\n"
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("host_live_take", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("host_live_take", generated(fault)), case)
    assert status == CHECK_FAILED, output
