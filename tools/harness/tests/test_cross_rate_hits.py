"""Shots between players of different rates in a mixed game (an OpenCE player at 30, OpenH1 players at 60 and 120:
network_standard.c): each shooter's machine decides its hit at its own rate, and the host takes the report if the
target is near where the host had it at a tick as far back as the report was made (network_damage.c). A 30 Hz
shooter's target seen at a tick's pose and a 60 or 120 Hz shooter's seen between two ticks' poses are taken alike;
a target where the host never had it is refused at either rate."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, function, mutated, read, run  # noqa: E402

CASES = ["opence-shooter", "openh1-shooter", "copy-off", "never-there"]

# a fault in the host's check, and the case that must catch it
NEGATIVE_CONTROLS = {
    # (no look back: the tick the report was made at alone)
    "no-look-back": (("back <= ticks && back < TARGET_HISTORY_TICKS", "back <= 0 && back < TARGET_HISTORY_TICKS"),
                     "opence-shooter"),
    # (none of the ticks' movement: a sub-step's pose a vehicle's tick from the ticks' refused)
    "tolerance-too-tight": (("real reach = tolerance + REPORT_HISTORY_LEAD_TICKS * speed;", "real reach = 0.1f;"),
                            "openh1-shooter"),
    # (anything taken)
    "anything-taken": (("return distributed_distance_squared(position, history_position) <= reach * reach;",
                        "return TRUE;"), "never-there"),
}


def generated(fault=None):
    source = read("port/linux/game/network_damage.c")
    config = ""
    for name in ("TARGET_HISTORY_TICKS", "TARGET_HISTORY_SLACK_TICKS"):
        config += f"#define {name} {constant(source, name)}\n"
    for name in ("REPORT_HISTORY_TOLERANCE", "REPORT_HISTORY_LEAD_TICKS"):
        config += f"#define {name} {re.search(rf'#define {name} ([0-9.]+f)', source).group(1)}\n"
    code = "\n".join(function(source, name) for name in (
        "distributed_distance_squared", "distributed_history_near", "distributed_target_seen"))
    if fault:
        code = mutated(code, *fault)
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("cross_rate_hits", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("cross_rate_hits", generated(fault)), case)
    assert status == CHECK_FAILED, f"network_damage.c with a fault ({control}) passed '{case}': the test cannot see it"


def test_the_host_checks_every_report_so():
    """Every client's report goes through the one check, whatever the shooter's rate: none of it reads a rate."""
    valid = function(read("port/linux/game/network_damage.c"), "distributed_report_valid")
    assert "seen = distributed_target_seen(report->object_index, &report->target_position, ticks);" in valid
    assert "ticks = MIN(now - report->host_time + TARGET_HISTORY_SLACK_TICKS, TARGET_HISTORY_TICKS - 1);" in valid
    assert "sim_" not in valid and "substep" not in valid
