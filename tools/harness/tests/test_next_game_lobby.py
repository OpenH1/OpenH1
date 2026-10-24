"""A network game's end and the next game (next_game_lobby.c): after the host's PICK GAME on the scores every
machine loads the main menu's map for the next game's lobby, behind the loading screen; the lobby's clock runs, so it
is drawn and the loading screen ends (else it stayed over the lobby for good, on the host and on every client), no
tick of the ended game is left open, and the next game plays at the rate agreed for it. For the host and a client,
of an OpenH1 game at 30, 60 and 120 and of a Legacy one."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, function, mutated, read, run, structure  # noqa: E402

CASES = [f"{role}:{rate}" for role in ("host", "client") for rate in ("30", "60", "120", "legacy")]

# a fault in the code, and the case that must catch it
NEGATIVE_CONTROLS = {
    # (the lobby's clock stopped, as it was: the loading screen stayed over the lobby)
    "lobby-clock-stopped": (("\tif (!main_menu_is_loaded())\n\t\tgame_time_end();", "\tgame_time_end();"),
                            "client:120"),
    "host-lobby-clock-stopped": (("\tif (!main_menu_is_loaded())\n\t\tgame_time_end();", "\tgame_time_end();"),
                                 "host:60"),
    "ended-game-clock-runs": (("\tif (!main_menu_is_loaded())\n\t\tgame_time_end();", ""), "host:30"),
    "tick-left-open": (("\tsim_rate_tick_drop();\n\tcsmemset(&game_time_substeps", "\tcsmemset(&game_time_substeps"),
                       "client:120"),
    "rate-of-the-last-game": (("\tif (in_lobby)\n\t\tnetwork_standard_globals.hosting_rate", "\tif (FALSE)\n\t\t"
                               "network_standard_globals.hosting_rate"), "host:60"),
}


def tail(source, marker):
    """the source from a marker on: a file's globals and its code"""
    return source[source.index(marker):]


def generated(fault=None):
    sim_header = read("port/linux/game/sim_rate.h")
    config = enum_with(read("source/game/game.h"), "_game_connection_network_client") + "\n"
    config += enum_with(sim_header, "SIM_RATE_CLASSIC") + "\n" + enum_with(sim_header, "NUMBER_OF_SIM_SUBSTEP_SLOTS") + "\n"
    start = sim_header.index("typedef void (*sim_substep_procedure)")
    config += sim_header[start:sim_header.index(";", start) + 1] + "\n"
    limits = read("port/linux/include/halo_port_limits.h")
    config += "#define HALO_PORT_STANDARD_NETWORK_VERSION %d\n" % constant(limits, "HALO_PORT_STANDARD_NETWORK_VERSION")

    code = tail(read("port/linux/game/sim_rate.c"), "/* ---------- globals */")
    code += "\n" + tail(read("port/linux/game/network_standard.c"), "/* ---------- globals */")
    clock = read("source/game/game_time.c")
    code += "\n" + structure(clock, "game_time_globals_struct")
    code += "\nstatic struct game_time_globals_struct *game_time_globals;\n"
    end = clock.index("} game_time_substeps;") + len("} game_time_substeps;")
    code += clock[clock.rindex("static struct", 0, end):end] + "\n"
    for name in ("game_time_port_drop_substeps", "game_time_initialize_for_new_map", "game_time_dispose_from_old_map",
                 "game_time_end", "game_time_start", "game_in_progress"):
        code += "\n" + function(clock, name)
    main = read("source/main/main.c")
    for name in ("main_menu_is_loaded", "main_new_map", "main_load_ui_scenario"):
        code += "\n" + function(main, name)
    code += "\n" + function(read("source/networking/network_game_manager.c"), "network_game_reset_for_next_round")
    if fault:
        code = mutated(code, *fault)
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("next_game_lobby", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("next_game_lobby", generated(fault)), case)
    assert status == CHECK_FAILED, f"{control} was not caught: {output}"
