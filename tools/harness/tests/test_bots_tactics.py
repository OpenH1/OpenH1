"""The geometry and rules a team's bots move by (port/linux/game/bots_tactics.c): crossfire (two bots on one enemy
strafe round it apart), spacing (a fighting bot strafes away from a teammate close by, a walking one turns away from
it), and when a team loses the fight and falls back."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, function, mutated, read, run  # noqa: E402

CASES = ["crossfire", "spacing-strafe", "steer", "losing"]

FUNCTIONS = ("tactics_angle_difference", "tactics_recent", "tactics_losing", "tactics_side_away",
             "tactics_angle_round", "tactics_crossfire_side", "tactics_steer_away")

# a fault in bots_tactics.c, the function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    "crossfire-closes-in": (("\t\treturn -1.0f;\n\treturn 1.0f;", "\t\treturn 1.0f;\n\treturn -1.0f;"),
                            "tactics_crossfire_side", "crossfire"),
    "crossfire-tie-together": (("(difference == 0.0f && !first_of_two)", "(difference == 0.0f)"),
                               "tactics_crossfire_side", "crossfire"),
    "strafes-into-teammate": (("> 0.0f ? -1.0f : 1.0f", "> 0.0f ? 1.0f : -1.0f"), "tactics_side_away",
                              "spacing-strafe"),
    "steers-into-teammate": (("(across > 0.0f ? -turn : turn)", "(across > 0.0f ? turn : -turn)"),
                             "tactics_steer_away", "steer"),
    "no-easing": (("*throttle *= 0.6f;", "*throttle *= 1.0f;"), "tactics_steer_away", "steer"),
    "loses-late": ((">= 2;", ">= 3;"), "tactics_losing", "losing"),
    "remembers-forever": (("now - ticks[index] <= seconds * TICKS_PER_SECOND", "TRUE"), "tactics_recent", "losing"),
}


def _define(source, name):
    match = re.search(r"^#define\s+" + re.escape(name) + r"\s+(.+)$", source, re.M)
    if not match:
        raise LookupError(f"#define not found in the sources: {name}")
    return f"#define {name} {match.group(1)}\n"


def generated(fault=None, faulty_function=None):
    source = read("port/linux/game/bots_tactics.c")
    config = "".join(_define(source, name) for name in ("TACTICS_PI", "TACTICS_DEGREES", "TACTICS_SPACING"))
    config += f"#define TACTICS_EVENTS {constant(source, 'TACTICS_EVENTS')}\n"
    config += f"#define TICKS_PER_SECOND {constant(read('source/cseries/cseries.h'), 'TICKS_PER_SECOND')}\n"
    text = ""
    for name in FUNCTIONS:
        code = function(source, name)
        if fault and name == faulty_function:
            code = mutated(code, *fault)
        text += code + "\n"
    return (("config.inc", config), ("under_test.inc", text))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("bots_tactics", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, faulty_function, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("bots_tactics", generated(fault, faulty_function)), case)
    assert status == CHECK_FAILED, f"bots_tactics.c with a fault ({control}) passed '{case}': the test cannot see it"
