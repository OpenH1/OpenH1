"""The simulation's two clocks (game.sim_rate, port/linux/game/sim_rate.c): the rate a tick takes from the setting
and the connection (30 in any network game), the fast phase's sub-steps and their slots, the conversions of per-tick
quantities, each exactly its argument at 30, the frames' blend between sub-steps, and the sub-steps run one at a
time as the clock spreads them over the frames (which is next, the first of a frame, the open tick's rate)."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run  # noqa: E402

CASES = ["classic-identity", "rates", "network-classic", "exact-scaling", "decay-and-chance", "substeps-run", "blend",
         "spread"]

# a fault in the code, and the case that must catch it
NEGATIVE_CONTROLS = {
    "network-not-classic": (("\t\treturn sim_rate_network();\n", "\t\treturn sim_rate_setting();\n"),
                            "network-classic"),
    "film-not-classic": (("\tdefault:\n\t\treturn SIM_RATE_CLASSIC;\n", "\tdefault:\n\t\treturn sim_rate_setting();\n"),
                         "network-classic"),
    "any-rate": (("(setting == 60 || setting == 120) ? setting", "setting > 0 ? setting"), "rates"),
    "setting-never-read-again": (("|| sim_rate_globals.read_at != config_changes()", ""), "rates"),
    "decay-at-30-rounded": (("\tif (count >= 2)\n\t\tfactor = square_root(factor);",
                             "\tfactor = square_root(factor * factor);\n\tif (count >= 2)\n\t\tfactor = square_root(factor);"),
                            "classic-identity"),
    "decay-one-root-at-120": (("\tif (count >= 4)\n\t\tfactor = square_root(factor);\n", ""), "decay-and-chance"),
    "quadratic-as-linear": (("per_tick_squared / (real)(count * count)", "per_tick_squared / (real)count"),
                            "exact-scaling"),
    "index-left-after-phase": (("\t}\n\tsim_rate_globals.substep_index = NONE;\n", "\t}\n"), "substeps-run"),
    "tick-never-open": (("substep_index + 1 < count ? substep_index + 1 : 0", "0"), "spread"),
    "next-rate-not-held": (("sim_rate_globals.substep_next ? sim_rate_globals.rate : ", ""), "spread"),
    "every-substep-first-of-frame": (("\tsim_rate_globals.frame_fresh = FALSE;\n\tsim_rate_globals.substep_index",
                                      "\tsim_rate_globals.substep_index"), "spread"),
    "drop-leaves-tick-open": (("\tsim_rate_globals.substep_next = 0;\n}\n\nvoid sim_rate_tick_begin",
                               "}\n\nvoid sim_rate_tick_begin"), "spread"),
    "blend-past-the-last": (("\tif (index > count - 1)\n\t\tindex = count - 1;\n", ""), "blend"),
}


def generated(fault=None):
    header = read("port/linux/game/sim_rate.h")
    code = read("port/linux/game/sim_rate.c")
    config = "#define NUMBEROF(array) (sizeof(array) / sizeof((array)[0]))\n"
    config += enum_with(header, "SIM_RATE_CLASSIC") + "\n" + enum_with(header, "NUMBER_OF_SIM_SUBSTEP_SLOTS") + "\n"
    start = header.index("typedef void (*sim_substep_procedure)")
    config += header[start:header.index(";", start) + 1] + "\n"
    # the globals and every function, as sim_rate.c has them
    text = code[code.index("/* ---------- globals */"):]
    if fault:
        text = mutated(text, *fault)
    return (("config.inc", config), ("under_test.inc", text))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("sim_rate", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("sim_rate", generated(fault)), case)
    assert status == CHECK_FAILED, f"the code with a fault ({control}) passed '{case}': the test cannot see it"


def test_default_and_tick_loop():
    # 30 unless asked (port_config.c), the rate taken at each tick's start and the fast phase after the objects'
    # update (game.c: the tick's macro step with its first sub-step, what follows the fast phase with its last); the
    # determinism check writes the rate (determinism_log.c)
    assert '{ "game.sim_rate", _config_integer, "30", "HALO_SIM_RATE"' in read("port/linux/src/port_config.c")
    game = read("source/game/game.c")
    macro = function(game, "game_tick_macro_step")
    assert macro.index("sim_rate_tick_begin();") < macro.index("units_update();") < macro.index("objects_update();")
    substep = function(game, "game_tick_substep")
    assert substep.index("if (substep_index == 0)\n\t\tgame_tick_macro_step();") < \
        substep.index("sim_substep_run(substep_index);") < substep.index("game_tick_after_substeps();")
    assert "players_update_after_game();" in function(game, "game_tick_after_substeps")
    assert 'fprintf(file, "rate %ld\\n", determinism_log.rate);' in read("port/linux/game/determinism_log.c")
