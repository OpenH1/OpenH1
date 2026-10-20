"""The simulation's two clocks (game.sim_rate, port/linux/game/sim_rate.c): the rate a tick takes from the setting
and the connection (30 in any network game), the fast phase's sub-steps and their slots, the conversions of per-tick
quantities, each exactly its argument at 30, and the frames' blend between sub-steps."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run  # noqa: E402

CASES = ["classic-identity", "rates", "network-classic", "exact-scaling", "decay-and-chance", "substeps-run", "blend"]

# a fault in the code, and the case that must catch it
NEGATIVE_CONTROLS = {
    "network-not-classic": (("\t\trate = sim_rate_network();\n", ""), "network-classic"),
    "film-not-classic": (("\tdefault:\n\t\trate = SIM_RATE_CLASSIC;\n", "\tdefault:\n"), "network-classic"),
    "any-rate": (("(setting == 60 || setting == 120) ? setting", "setting > 0 ? setting"), "rates"),
    "setting-never-read-again": (("|| sim_rate_globals.read_at != config_changes()", ""), "rates"),
    "decay-at-30-rounded": (("\tif (count >= 2)\n\t\tfactor = square_root(factor);",
                             "\tfactor = square_root(factor * factor);\n\tif (count >= 2)\n\t\tfactor = square_root(factor);"),
                            "classic-identity"),
    "decay-one-root-at-120": (("\tif (count >= 4)\n\t\tfactor = square_root(factor);\n", ""), "decay-and-chance"),
    "quadratic-as-linear": (("per_tick_squared / (real)(count * count)", "per_tick_squared / (real)count"),
                            "exact-scaling"),
    "index-left-after-phase": (("\t}\n\tsim_rate_globals.substep_index = NONE;\n}", "\t}\n}"), "substeps-run"),
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
    # update (game.c); the determinism check writes the rate (determinism_log.c)
    assert '{ "game.sim_rate", _config_integer, "30", "HALO_SIM_RATE"' in read("port/linux/src/port_config.c")
    tick = function(read("source/game/game.c"), "game_tick")
    assert tick.index("sim_rate_tick_begin();") < tick.index("units_update();")
    assert tick.index("objects_update();") < tick.index("sim_substeps_run();") < tick.index("players_update_after_game();")
    assert 'fprintf(file, "rate %ld\\n", determinism_log.rate);' in read("port/linux/game/determinism_log.c")
