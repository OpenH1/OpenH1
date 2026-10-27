"""The controller's rumble (source/game/player_rumble.c) starts at the end of the step that made it, not at the next
tick's update, lasts the impulse's duration from then, and a vehicle's starts and stops in the frame that sets it."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, mutated, read, run, structure  # noqa: E402

CASES = ["starts_in_its_step", "lasts_its_duration", "before_the_update", "vehicle_in_its_frame"]

# a fault in player_rumble.c, and the case that must catch it
NEGATIVE_CONTROLS = {
    # the motors written only by the next tick's update
    "written-next-tick": (("void rumble_port_step_end(\n\tvoid)\n{\n\trumble_port_write_motors();",
                           "void rumble_port_step_end(\n\tvoid)\n{"), "starts_in_its_step"),
    # an impulse made in a step at its start again at the next update: a tick too long
    "a-tick-long": (("? 0.0f : -1.0f / 30.0f;", "? -1.0f / 30.0f : -1.0f / 30.0f;"), "lasts_its_duration"),
    # one made before the update a tick old at it: a tick short, and not from full
    "a-tick-short": (("? 0.0f : -1.0f / 30.0f;", "? 0.0f : 0.0f;"), "before_the_update"),
    # a vehicle's left for the next tick's update
    "vehicle-next-tick": (("\tif (changed)\n\t\trumble_port_write_motors();\n", ""), "vehicle_in_its_frame"),
}


def generated(fault=None):
    header = read("source/game/player_rumble.h")
    rumble = read("source/game/player_rumble.c")
    config = enum_with(header, "NUMBER_OF_RUMBLE_MOTORS") + "\n"
    config += structure(header, "rumble_motor") + "\n" + structure(header, "rumble_definition") + "\n"
    code = rumble[rumble.index("/* ---------- constants */"):]
    if fault:
        code = mutated(code, *fault)
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("rumble_timing", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("rumble_timing", generated(fault)), case)
    assert status == CHECK_FAILED, f"player_rumble.c with a fault ({control}) passed '{case}': the test cannot see it"
