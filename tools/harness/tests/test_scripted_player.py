"""The automated tests' scripted player (port/linux/src/xinput_sdl.c's test_input_gamepad: debug.test_input "bot",
"look", "walk", and the network test's held action button) plays in a game and leaves the menus alone: in the
network test's lobby its stick and its A every few seconds chose GAME SETTINGS, whose Server Setup made the test's
public internet game private (out of the server browser) and whose START GAME set the gametype. The menus' own
scripts ("menu:") still play in the menus."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run  # noqa: E402

CASES = ["bot_menus", "bot_game", "look_menus", "walk_menus", "hold_menus", "menu_script"]

GATE = "\tif (main_menu_is_loaded())\n\t\treturn;\n"

# a fault in test_input_gamepad, and the case that must catch it
NEGATIVE_CONTROLS = {
    "plays-in-the-menus": ((GATE, ""), "bot_menus"),
    "holds-in-the-menus": ((GATE + "\tif (test_input_holding_action)", "\tif (test_input_holding_action)"),
                           "hold_menus"),
    "looks-in-the-menus": ((GATE + "\tif (test_input_holding_action)",
                            "\tif (looking)\n\t{\n\t\tpad->sThumbRX = 1;\n\t\treturn;\n\t}\n" + GATE +
                            "\tif (test_input_holding_action)"), "look_menus"),
    "never-plays": ((GATE, "\treturn;\n"), "bot_game"),
    "menu-scripts-stopped": (("\tif (test_script_on)\n", GATE + "\tif (test_script_on)\n"), "menu_script"),
}


def generated(fault=None):
    keyboard = read("port/linux/include/halo_keyboard.h")
    config = enum_with(keyboard, "HALO_CHAT_PAD_A,") + "\n"
    code = function(read("port/linux/src/xinput_sdl.c"), "test_input_gamepad")
    if fault:
        code = mutated(code, *fault)
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("scripted_player", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("scripted_player", generated(fault)), case)
    assert status == CHECK_FAILED, f"test_input_gamepad with a fault ({control}) passed '{case}': " \
        "the test cannot see it"
