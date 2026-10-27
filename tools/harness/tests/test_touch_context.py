"""What the Android touch controls' buttons show of player 1 (touch_game.c): the HUD's action prompt glows the action
button (not the vehicle's exit), a magazine under a quarter loaded with a reserve pulses reload (never a plasma
weapon, nor while reloading), none of the current grenade type dims the grenade, a vehicle's seat is told, and so is
a living player (a crouch tapped on stands up at death)."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure  # noqa: E402

CASES = ["on-foot", "action", "reload", "no-magazine", "grenades", "vehicle"]

# a fault in touch_game.c, the function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    "exit-glows": (("&&\n\t\tplayer->action_result != _player_action_result_exit_vehicle)", ")"),
                   "touch_game_context", "action"),
    "reload-at-half": (("magazine->rounds_loaded * 4 <", "magazine->rounds_loaded * 2 <"),
                       "touch_game_reload_due", "reload"),
    "reload-without-reserve": (("magazine->rounds_remaining > 0 &&", ""), "touch_game_reload_due", "reload"),
    "reload-while-reloading": (("&& !magazine->reloading", ""), "touch_game_reload_due", "reload"),
    "plasma-pulses": (("state.magazine_count > 0 && ", ""), "touch_game_context", "no-magazine"),
    "grenade-past-counts": (("grenade < NUMBER_OF_UNIT_GRENADE_TYPES && ", ""),
                            "touch_game_context", "grenades"),
    "carried-is-seated": ((" && unit->unit.parent_seat_index != NONE", ""), "touch_game_context", "vehicle"),
    "never-alive": (("\tcontext |= _touch_context_alive;\n", ""), "touch_game_context", "on-foot"),
}


def generated(fault=None):
    touch = read("port/linux/game/touch_game.c")
    config = "\n".join([
        enum_with(read("source/game/players.h"), "_player_action_result_reload"),
        enum_with(read("source/units/units.h"), "NUMBER_OF_UNIT_GRENADE_TYPES"),
        enum_with(touch, "_touch_context_action"),
        structure(touch, "weapon_interface_magazine_state"),
    ]) + "\n"
    functions = {name: function(touch, name) for name in ("touch_game_reload_due", "touch_game_context")}
    if fault:
        (before, after), name, _ = NEGATIVE_CONTROLS[fault]
        functions[name] = mutated(functions[name], before, after)
    code = "\n\n".join(functions.values())
    return (("config.inc", config), ("under_test.inc", code + "\n"))


@pytest.mark.parametrize("case", CASES)
def test_touch_context(case):
    status, output = run(build("touch_context", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("fault", sorted(NEGATIVE_CONTROLS))
def test_fault_detected(fault):
    status, output = run(build("touch_context", generated(fault)), NEGATIVE_CONTROLS[fault][2])
    assert status == CHECK_FAILED, output
