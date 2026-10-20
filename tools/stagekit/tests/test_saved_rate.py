"""A game saved at one simulation rate and loaded at another (port/linux/game/sim_rate.h): a game saved is the game
state between two ticks, which carries no rate, so it loads at the rate the tick after it takes, the player's
setting (game.sim_rate) or a network game's 30. On scenarios/walk.c, the player walks and jumps; at 120 the game is
saved in the air (core_save_name), then loaded a second into games at 120 and at 30.

Loaded at 120 it plays on exactly as the game it was saved from did, step for step: nothing of a sub-step outlives
its tick, and nothing the sub-steps need is left out of the game saved. Loaded at 30 it plays on at 30, one step a
tick, from where the save left the player, as the game at 120 does within a jump's tolerance (the scorecard's)."""

import shutil

import pytest
import runner
from conftest import SLOTS_PER_WORKER, worker

SCRIPT = "script:0.2-4 move=0,1; 1.9 jump"
SAVED = 2.0  # (in the air: the jump at 1.9 lands near 2.5)
LOADED = 1.0
SECONDS = 3.5
CORE = "rate_core"
# the scorecard's jump tolerance (tools/scorecard/metrics.py: a jump's apex height, 0.003 world units)
TOLERANCE = 0.003


def played(folder, map_path, rate, cue, slot, core=None):
    """the walk played at the rate with cue, traced (debug.trace): [(t, tick, sub-step, (x, y, z), airborne)] and the
    rate the trace says"""
    (folder / "custom_maps").mkdir(parents=True)
    shutil.copy(map_path, folder / "custom_maps" / "void.map")
    runner.prepare(folder)
    if core:
        (folder / "core").mkdir()
        shutil.copy(core, folder / "core" / CORE)
    game = runner.launch(folder, SCRIPT, *runner.addresses(slot), "log", init="map_name custom_maps\\void\n",
                         HALO_FIXED_STEP="1", HALO_SIM_RATE=str(rate), HALO_TRACE="trace.txt",
                         HALO_TRACE_SECONDS=str(SECONDS), **runner.cue_settings([cue], 1))
    try:
        game.wait(timeout=runner.LOADING_SECONDS + SECONDS + 60)
    except Exception:
        game.kill()
        game.wait()
        raise
    lines = (folder / "trace.txt").read_text().splitlines()
    steps = []
    for line in lines[1:]:
        words = line.split()
        if words[0] == "s":
            steps.append([float(words[1]), int(words[2]), int(words[3]), None, None])
        elif words[0] == "p" and words[1] == "0":
            steps[-1][3] = tuple(float(word) for word in words[2:5])
            steps[-1][4] = int(words[11])
    return [tuple(step) for step in steps], int(lines[0].split()[5])


@pytest.fixture(scope="module")
def games(stagekit, tmp_path_factory):
    """the game saved at 120, and loaded at 120 and at 30: each one's steps and rate"""
    map_path = runner.generate(stagekit, "walk", tmp_path_factory.mktemp("saved-rate-map"))
    slot = SLOTS_PER_WORKER * worker() + SLOTS_PER_WORKER - 3
    saved_folder = tmp_path_factory.mktemp("saved-120")
    saved = played(saved_folder, map_path, 120, (SAVED, f"core_save_name {CORE}"), slot)
    core = saved_folder / "core" / CORE
    assert core.is_file(), runner.Session(saved_folder, 0, -1, [], {}, "", "").tails()
    return {"saved": saved, **{rate: played(tmp_path_factory.mktemp(f"loaded-{rate}"), map_path, rate,
                                            (LOADED, f"core_load_name {CORE}"), slot + index + 1, core)
                               for index, rate in enumerate((120, 30))}}


def after_save(steps):
    """the steps after the game saved (a game loaded at LOADED jumps on to them)"""
    return [step for step in steps if step[0] > SAVED + 1e-6]


def test_loaded_at_120_plays_on_as_saved(games):
    saved, _ = games["saved"]
    loaded, rate = games[120]
    assert rate == 120
    # (it was loaded: it went from LOADED to past SAVED at once)
    assert any(a[0] < LOADED + 0.1 and b[0] > SAVED for a, b in zip(loaded, loaded[1:])), loaded[:80]
    steps = after_save(loaded)
    assert steps and steps[-1][0] >= SECONDS - 1e-6
    # (from the first step after the load, every step of the game saved from, the same)
    assert steps == [step for step in saved if step[0] >= steps[0][0] - 1e-6], (steps[:8], saved[-len(steps) - 4:])


def test_loaded_at_30_plays_on_at_30(games):
    saved, _ = games["saved"]
    loaded, rate = games[30]
    assert rate == 30
    steps = after_save(loaded)
    assert any(a[0] < LOADED + 0.1 and b[0] > SAVED for a, b in zip(loaded, loaded[1:])), loaded[:80]
    assert steps and steps[-1][0] >= SECONDS - 1e-6
    # one step a tick, each a tick on from the last
    assert all(sub_step == 0 for _, _, sub_step, _, _ in steps)
    assert all(abs(b[0] - a[0] - 1 / 30) < 1e-4 for a, b in zip(steps, steps[1:]))
    # where the game at 120 was at each tick's end, the air and the landing too
    at_120 = {round(t * 30): place for t, _, _, place, _ in after_save(saved) if abs(t * 30 - round(t * 30)) < 1e-3}
    assert steps[0][4] == 1 and steps[-1][4] == 0, steps
    for t, _, _, place, _ in steps:
        other = at_120[round(t * 30)]
        assert max(abs(a - b) for a, b in zip(place, other)) < TOLERANCE, (t, place, other)
