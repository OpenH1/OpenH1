#!/usr/bin/env python3
"""The hard gate (README.md): at 30 Hz a branch plays exactly as master does, tick for tick. StageKit's scenarios
are played at 30 Hz with the determinism log (debug.determinism_log), by this build and by master's, and the logs
compared (tools/determinism_compare.py: each tick's game state, allocation by allocation, and random seeds, the
seeds' takers by count, as two builds' call sites lie at other addresses; COMPARE).

    python tools/scorecard/identity.py --baseline-game <master's build>/halo
    python tools/scorecard/identity.py --record <folder>      this build's logs, as master's baseline
    python tools/scorecard/identity.py --baseline <folder>    this build against logs recorded so

The scenarios use only what master's game has had since the merge of OpenCE's StageKit (the scripted bot,
debug.test_input "bot:", on the game's clock; cues; debug.fixed_step), so that either build plays them; the maps
are this build's generator's, the same for both.

Exit status: 0 when every scenario's logs agree, 1 when one parts (the first tick where, and what), 2 when the gate
could not run (no baseline: a note, not a failure, with --missing-ok).
"""

import argparse
import concurrent.futures
import json
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "stagekit/tests"))

import runner  # noqa: E402

ROOT = runner.ROOT
# (StageKit map, input, cues, seconds): a biped that walks, turns, jumps and fires its weapon on its own, a lift
# ridden, shots at walls, grenades, a car driven (the script's input, on the game's clock at 30), turned and jumped
SCENARIOS = {
    "walk": ("walk", "bot:1", [], 12),
    "elevator": ("elevator", "look:1", [(1, "device_set_position elevator 1"), (5, "device_set_position elevator 0")],
                 8),
    "shoot": ("shoot", "bot:3", [], 12),
    "lob": ("lob", "bot:2", [], 12),
    "drive": ("drive", "script:0.5 aim=0,0; 1-12 move=0,1; 2-4 look=0.3,0; 4.5 aim=0,0",
              [(0.1, 'unit_enter_vehicle (unit (list_get (players) 0)) car "stand"')], 12),
}
LOG = "determinism.txt"
# what two builds' logs are compared by (tools/determinism_compare.py): the game state and the seeds, the seeds'
# takers by count (--builds), not the allocations the frames move (--simulation), and not the script
# threads, which keep an address in the guest image (a byte of a string's or a function's, found with
# debug.determinism_dump), which moves when any code does; the decal vertex cache, which keeps another, is one of
# the frames' allocations already
COMPARE = ["--builds", "--simulation", "--ignore", "hs thread"]


def play(name, folder, game, map_path, slot, trace=None):
    """the scenario played at 30 Hz in folder, its determinism log written there: for its seconds of the slice,
    then stopped"""
    map_name, input, cues, seconds = SCENARIOS[name]
    if folder.exists():
        shutil.rmtree(folder)
    (folder / "custom_maps").mkdir(parents=True)
    shutil.copy(map_path, folder / "custom_maps" / "void.map")
    runner.prepare(folder)
    process = runner.launch(folder, input, *runner.addresses(slot), "log", init="map_name custom_maps\\void\n",
                            game=game, HALO_FIXED_STEP="1", HALO_SIM_RATE="30", HALO_DETERMINISM_LOG=LOG,
                            **runner.cue_settings(cues, 1), **(trace or {}))
    try:
        start = runner.play_slice(folder / "game.log", seconds, [process])
    finally:
        process.kill()
        process.wait()
    return start


def play_all(game, work, maps, trace=None):
    with concurrent.futures.ThreadPoolExecutor(len(SCENARIOS)) as pool:
        futures = {name: pool.submit(play, name, work / name, game, maps[name], slot, trace)
                   for slot, name in enumerate(SCENARIOS, start=200)}
        return {name: future.result() for name, future in futures.items()}


def ticks_logged(path):
    return sum(1 for line in path.read_text(errors="replace").splitlines() if line.startswith("t "))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--baseline-game", type=Path, help="master's game, played now")
    source.add_argument("--baseline", type=Path, help="master's logs, recorded with --record")
    source.add_argument("--record", type=Path, help="record this build's logs here (master's baseline)")
    parser.add_argument("--missing-ok", action="store_true", help="no baseline is a note, exit 0 (CI's first run)")
    parser.add_argument("--work", type=Path, default=ROOT / "build/scorecard/identity")
    parser.add_argument("--trace", action="store_true",
                        help="this build traced (debug.trace), which must change nothing")
    parser.add_argument("--mutation", default="",
                        help="this build traced with a known bug (debug.trace_mutation), which the gate must catch")
    args = parser.parse_args()

    reason = runner.can_build()
    if reason:
        print(f"identity: {reason}", file=sys.stderr)
        return 2
    if args.baseline and not (args.baseline / "baseline.json").is_file():
        print(f"identity: no baseline in {args.baseline}" + (" (skipped)" if args.missing_ok else ""))
        return 0 if args.missing_ok else 2
    work = args.work
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    program = runner.build(work)
    maps = {name: runner.generate(program, SCENARIOS[name][0], work / "maps" / name) for name in SCENARIOS}
    trace = dict(HALO_TRACE="trace.txt", HALO_TRACE_MUTATION=args.mutation) if args.trace or args.mutation else None
    play_all(runner.GAME, work / "branch", maps, trace)
    if args.record:
        args.record.mkdir(parents=True, exist_ok=True)
        for name in SCENARIOS:
            shutil.copy(work / "branch" / name / LOG, args.record / f"{name}.txt")
        (args.record / "baseline.json").write_text(json.dumps(dict(scenarios=list(SCENARIOS)), indent=1))
        print(f"identity: {len(SCENARIOS)} scenarios' logs recorded in {args.record}")
        return 0
    if args.baseline_game:
        play_all(args.baseline_game, work / "baseline", maps)
        baseline = {name: work / "baseline" / name / LOG for name in SCENARIOS}
    else:
        baseline = {name: args.baseline / f"{name}.txt" for name in SCENARIOS}
    failed = 0
    # (a scenario new since the recorded baseline, which master records once it has it: a note)
    recorded = json.loads((args.baseline / "baseline.json").read_text())["scenarios"] if args.baseline else SCENARIOS
    for name in SCENARIOS:
        if name not in recorded:
            print(f"{name}: new since the baseline was recorded (master records it once it has it)")
            continue
        if not baseline[name].is_file():
            print(f"{name}: no baseline log ({baseline[name]})")
            failed += 1
            continue
        compared = subprocess.run([sys.executable, str(ROOT / "tools/determinism_compare.py"), str(baseline[name]),
                                   str(work / "branch" / name / LOG), *COMPARE],
                                  capture_output=True, text=True)
        least = SCENARIOS[name][3] * runner.TICKS_PER_SECOND
        alike = compared.returncode == 0 and min(ticks_logged(baseline[name]),
                                                 ticks_logged(work / "branch" / name / LOG)) >= least
        said = compared.stdout.strip().splitlines() or [compared.stderr.strip()]
        print(f"{name}: {'identical' if alike else 'DIFFERS'}: " +
              next((line for line in said if line.startswith("first difference")), said[-1]))
        if not alike:
            print("\n".join("    " + line for line in compared.stdout.strip().splitlines()))
            failed += 1
    checked = [name for name in SCENARIOS if name in recorded]
    print(f"identity: {len(checked) - failed} of {len(checked)} scenarios bit-identical at 30 Hz")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
