#!/usr/bin/env python3
"""The scorecard (README.md): plays each scenario at each simulation rate, measures what it did in real units, and
compares the rates two by two (30 and 60, 30 and 120, 60 and 120 by default), each measure within its tolerance.

    python tools/scorecard/scorecard.py                   every scenario, 30:60, 30:120 and 60:120
    python tools/scorecard/scorecard.py --tags U4         a unit's own (any of the tags)
    python tools/scorecard/scorecard.py --pairs 30:30     the comparisons themselves (two runs at 30)
    python tools/scorecard/scorecard.py --mutation-checks the known bugs, each of which must fail
    python tools/scorecard/scorecard.py --list            the scenarios and their tags

Exit status: 0 when every comparison passed (skips allowed), 1 when one failed (or, with --mutation-checks, when a
mutation got through), 2 when the scorecard could not run.
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
import random
import shutil
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "stagekit/tests"))

import metrics  # noqa: E402
import runner  # noqa: E402
import scenarios as builtin  # noqa: E402

ROOT = runner.ROOT
# the known bugs (debug.trace_mutation, sim_trace.c) and the tags of the scenarios that must catch each
MUTATIONS = {
    "head_start": ["first_shot"],
    "jump": ["jump"],
    "speed": ["first_shot", "impact", "grenade"],
}
# how long past its seconds a run may take before it is stopped
RUN_TIMEOUT = 60


def load_holdouts(folder, seed):
    """the hidden scenarios: every *.json in folder, each a list of scenarios (scenarios.py's fields; a parameter
    given as [low, high] is drawn from that range with the seed, and the script may name it as {name})"""
    found = []
    for path in sorted(Path(folder).glob("*.json")):
        for number, entry in enumerate(json.loads(path.read_text())):
            draw = random.Random(f"{seed}/{path.name}/{number}")
            values = {name: round(draw.uniform(*value), 4) if isinstance(value, list) else value
                      for name, value in entry.get("params", {}).items()}
            values.update({name: round(draw.uniform(*value), 4)
                           for name, value in entry.get("draw", {}).items()})
            item = builtin.scenario(
                name=f"holdout:{path.stem}:{number}", map=entry["map"], metric=entry["metric"],
                tags=entry.get("tags", []) + ["holdout"], script=entry.get("script", "").format(**values),
                seconds=entry.get("seconds", 3.0),
                params={name: values[name] for name in entry.get("params", {})},
                arguments=entry.get("arguments"), cues=[tuple(cue) for cue in entry.get("cues", [])],
                needs=entry.get("needs"))
            found.append(item)
    return found


class Runs:
    """the scenarios' runs, each played once: the generator built once, each map written once"""

    def __init__(self, work, jobs):
        self.work = work
        self.jobs = jobs
        self.program = None
        self.maps = {}
        self.slot = 0

    def generator(self):
        if self.program is None:
            folder = self.work / "stagekit"
            folder.mkdir(parents=True, exist_ok=True)
            self.program = runner.build(folder)
        return self.program

    def map_for(self, item):
        key = json.dumps([item["map"], item["params"]], sort_keys=True)
        if key not in self.maps:
            folder = self.work / "maps" / hashlib.sha1(key.encode()).hexdigest()[:12]
            self.maps[key] = runner.generate(self.generator(), item["map"], folder, item["params"])
        return self.maps[key]

    def play(self, item, rate, variant, mutation, slot):
        """one run: its trace's measures, or the reason it has none"""
        folder = self.work / "runs" / f"{item['name'].replace(':', '_')}-{rate}-{variant}{'-' + mutation if mutation else ''}"
        if folder.exists():
            shutil.rmtree(folder)
        (folder / "custom_maps").mkdir(parents=True)
        shutil.copy(self.map_for(item), folder / "custom_maps" / "void.map")
        runner.prepare(folder)
        settings = dict(HALO_FIXED_STEP="1", HALO_SIM_RATE=str(rate), HALO_TRACE="trace.txt",
                        HALO_TRACE_SECONDS=str(item["seconds"]), **runner.cue_settings(item["cues"], 1))
        if mutation:
            settings["HALO_TRACE_MUTATION"] = mutation
        game = runner.launch(folder, f"script:{item['script']}", *runner.addresses(slot % 250), "log",
                             init="map_name custom_maps\\void\n", **settings)
        try:
            game.wait(timeout=RUN_TIMEOUT + item["seconds"])
        except Exception:
            game.kill()
            game.wait()
            return None, f"timed out ({folder})"
        trace_path = folder / "trace.txt"
        if not trace_path.exists():
            return None, f"no trace, the game exited {game.returncode} ({folder}/game.log)"
        trace = metrics.Trace(trace_path)
        if trace.rate != rate:
            return None, f"the build runs at {trace.rate} (no game.sim_rate {rate})"
        if not trace.steps or trace.steps[-1]["t"] < item["seconds"] - 1e-6:
            return None, f"the trace stops short ({folder})"
        return metrics.measure(trace, item["metric"], item["arguments"]), None

    def play_all(self, wanted):
        """{(name, rate, variant, mutation): (measures, reason)} for each run wanted, side by side"""
        for item, _, _, _ in wanted:
            self.map_for(item)
        results = {}
        with concurrent.futures.ThreadPoolExecutor(self.jobs) as pool:
            futures = {}
            for item, rate, variant, mutation in wanted:
                self.slot += 1
                futures[(item["name"], rate, variant, mutation)] = pool.submit(self.play, item, rate, variant,
                                                                                mutation, self.slot)
            for key, future in futures.items():
                try:
                    results[key] = future.result()
                except Exception as error:  # (a run that could not start fails its comparisons)
                    results[key] = (None, f"{type(error).__name__}: {error}")
        return results


def parse_pairs(text):
    pairs = []
    for word in text.split(","):
        a, b = word.split(":")
        pairs.append((int(a), int(b)))
    return pairs


def wanted_runs(selected, pairs, mutation):
    """each run the comparisons need: (scenario, rate, variant, mutation); a pair's second run is the candidate,
    which carries the mutation; a pair of one rate plays it twice"""
    runs = []
    for item in selected:
        if item["needs"]:
            continue
        for a, b in pairs:
            for run in ((item, a, 0, None), (item, b, 1 if a == b else 0, mutation)):
                if run not in runs:
                    runs.append(run)
    return runs


def compare_all(selected, pairs, results, mutation, quiet_holdouts):
    """the table's rows and the verdict: [(scenario, measure, {pair: text}, verdict)], failures, skips"""
    rows = []
    failures = skips = 0
    for item in selected:
        if item["needs"]:
            rows.append((item, "-", {}, f"SKIP needs {item['needs']}"))
            skips += 1
            continue
        names = []
        per_pair = {}
        for a, b in pairs:
            first = results[(item["name"], a, 0, None)]
            second = results[(item["name"], b, 1 if a == b else 0, mutation)]
            reason = first[1] or second[1]
            if reason:
                per_pair[(a, b)] = reason
                continue
            per_pair[(a, b)] = (first[0], second[0])
            names = list(dict.fromkeys(names + list(first[0]) + list(second[0])))
        if not names:
            reasons = sorted({value for value in per_pair.values() if isinstance(value, str)})
            skipped = all("no game.sim_rate" in reason for reason in reasons)
            rows.append((item, "-", {}, ("SKIP " if skipped else "FAIL ") + "; ".join(reasons)))
            skips += skipped
            failures += not skipped
            continue
        for name in names:
            cells = {}
            verdict = "ok"
            for pair, value in per_pair.items():
                if isinstance(value, str):
                    cells[pair] = "skip" if "no game.sim_rate" in value else "FAIL"
                    if cells[pair] == "FAIL":
                        verdict = "FAIL"
                    elif verdict == "ok":
                        verdict = "ok (some skipped)"
                    continue
                first, second = value
                if name not in first or name not in second:
                    cells[pair] = "missing"
                    verdict = "FAIL"
                    continue
                kind = first[name][1]
                gap, limit, passed = metrics.compare(first[name][0], second[name][0], kind, pair)
                cells[pair] = f"{gap:.3g}/{limit:.3g}{'' if passed else ' FAIL'}"
                if not passed:
                    verdict = "FAIL"
            failures += verdict == "FAIL"
            rows.append((item, name, cells, verdict))
    return rows, failures, skips


def value_text(results, item, name, rate, mutation=None):
    measures = results.get((item["name"], rate, 0, mutation), (None, None))[0]
    if not measures or name not in measures:
        return "-"
    value = measures[name][0]
    if isinstance(value, tuple) and len(value) > 3:
        return f"{len(value)} values"
    if isinstance(value, tuple):
        return "(" + " ".join(f"{x:.3f}" for x in value) + ")"
    return f"{value:.4f}" if isinstance(value, float) else str(value)


def print_table(rows, pairs, results, quiet_holdouts):
    rates = sorted({rate for pair in pairs for rate in pair})
    header = ["scenario", "measure"] + [str(rate) for rate in rates] + [f"{a}:{b} gap/tol" for a, b in pairs] + \
        ["result"]
    lines = [header]
    hidden = {}
    for item, name, cells, verdict in rows:
        if quiet_holdouts and "holdout" in item["tags"]:
            hidden.setdefault(item["name"], []).append(verdict)
            continue
        lines.append([item["name"], name] + [value_text(results, item, name, rate) if name != "-" else ""
                                             for rate in rates] +
                     [cells.get(pair, "") for pair in pairs] + [verdict])
    for number, (name, verdicts) in enumerate(sorted(hidden.items())):
        failed = sum(v.startswith("FAIL") for v in verdicts)
        lines.append([f"holdout #{number + 1}", f"{len(verdicts)} measures", *[""] * (len(rates) + len(pairs)),
                      "FAIL" if failed else "ok"])
    widths = [max(len(line[column]) for line in lines) for column in range(len(header))]
    for index, line in enumerate(lines):
        print("  ".join(cell.ljust(width) for cell, width in zip(line, widths)).rstrip())
        if index == 0:
            print("  ".join("-" * width for width in widths))


def select(all_scenarios, tags, names):
    chosen = []
    for item in all_scenarios:
        if tags and not set(tags) & set(item["tags"]):
            continue
        if names and item["name"] not in names:
            continue
        chosen.append(item)
    return chosen


def mutation_checks(all_scenarios, runs, args):
    """each known bug, put in the candidate of 30:30: the scenarios of its tags must fail"""
    print("mutation checks: each known bug in the second of two 30 Hz runs; the scenarios named must catch it\n")
    survived = 0
    lines = [("mutation", "scenarios", "failing measures", "caught")]
    for mutation, tags in MUTATIONS.items():
        selected = [item for item in select(all_scenarios, tags, args.scenario) if not item["needs"] and
                    "holdout" not in item["tags"]]
        results = runs.play_all(wanted_runs(selected, [(30, 30)], mutation))
        rows, failures, _ = compare_all(selected, [(30, 30)], results, mutation, False)
        failing = sorted({f"{item['name']}:{name}" for item, name, _, verdict in rows if verdict.startswith("FAIL")})
        caught = bool(failures)
        survived += not caught
        lines.append((mutation, ", ".join(item["name"] for item in selected), str(len(failing)),
                      "yes" if caught else "NO: got through"))
        if args.verbose:
            for entry in failing:
                print(f"  {mutation}: {entry}")
    widths = [max(len(line[column]) for line in lines) for column in range(4)]
    for index, line in enumerate(lines):
        print("  ".join(cell.ljust(width) for cell, width in zip(line, widths)).rstrip())
        if index == 0:
            print("  ".join("-" * width for width in widths))
    print(f"\n{len(MUTATIONS) - survived} of {len(MUTATIONS)} mutations caught")
    return 1 if survived else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tags", default="", help="only scenarios with any of these tags (comma-separated)")
    parser.add_argument("--scenario", action="append", default=[], help="only this scenario (repeatable)")
    parser.add_argument("--pairs", default="30:60,30:120,60:120", help="the rates compared, a:b,...")
    parser.add_argument("--mutation", default="", help="put this known bug (sim_trace.c) in each pair's second run")
    parser.add_argument("--mutation-checks", action="store_true", help="check that each known bug is caught")
    parser.add_argument("--holdouts", default=os.environ.get("SCORECARD_HOLDOUTS", ""),
                        help="a folder of hidden scenarios (*.json; default $SCORECARD_HOLDOUTS)")
    parser.add_argument("--seed", default=os.environ.get("SCORECARD_SEED", "0"),
                        help="what the hidden scenarios' drawn values are drawn with (default $SCORECARD_SEED)")
    parser.add_argument("--show-holdouts", action="store_true", help="show the hidden scenarios' measures")
    parser.add_argument("--no-retail", action="store_true", help="leave out the scenarios that need retail maps")
    parser.add_argument("--work", type=Path, default=ROOT / "build/scorecard", help="where the runs go")
    parser.add_argument("--jobs", type=int, default=int(os.environ.get("SCORECARD_JOBS") or 0) or
                        max(1, (os.cpu_count() or 4) // 2),
                        help="runs side by side (default $SCORECARD_JOBS, else half the processors)")
    parser.add_argument("--json", type=Path, help="write every run's measures and the verdicts here")
    parser.add_argument("--list", action="store_true", help="list the scenarios and their tags")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    all_scenarios = list(builtin.SCENARIOS)
    if args.holdouts:
        all_scenarios += load_holdouts(args.holdouts, args.seed)
    if args.no_retail:
        all_scenarios = [item for item in all_scenarios if item["needs"] != builtin.RETAIL]
    tags = [tag for tag in args.tags.split(",") if tag]
    selected = select(all_scenarios, tags, args.scenario)
    if args.list:
        for item in selected:
            if "holdout" in item["tags"] and not args.show_holdouts:
                continue
            print(f"{item['name']:34} {','.join(item['tags']):40} {'needs ' + item['needs'] if item['needs'] else ''}")
        return 0
    reason = runner.can_build()
    if reason:
        print(f"scorecard: {reason}", file=sys.stderr)
        return 2
    args.work.mkdir(parents=True, exist_ok=True)
    runs = Runs(args.work, args.jobs)
    started = time.monotonic()
    if args.mutation_checks:
        status = mutation_checks(all_scenarios, runs, args)
        print(f"({time.monotonic() - started:.1f} s)")
        return status
    if not selected:
        print("scorecard: no scenario has those tags", file=sys.stderr)
        return 2
    pairs = parse_pairs(args.pairs)
    results = runs.play_all(wanted_runs(selected, pairs, args.mutation or None))
    rows, failures, skips = compare_all(selected, pairs, results, args.mutation or None, not args.show_holdouts)
    print_table(rows, pairs, results, not args.show_holdouts)
    played = sum(1 for item in selected if not item["needs"])
    print(f"\n{played} scenarios played at {', '.join(str(r) for r in sorted({r for p in pairs for r in p}))} Hz "
          f"({len(results)} runs, {time.monotonic() - started:.1f} s): "
          f"{failures} failing, {skips} skipped" + (f", mutation {args.mutation}" if args.mutation else ""))
    if args.json:
        args.json.write_text(json.dumps(dict(
            pairs=pairs, mutation=args.mutation,
            runs={f"{name}@{rate}#{variant}": dict(reason=reason, measures=measures)
                  for (name, rate, variant, mutation), (measures, reason) in results.items()
                  if args.show_holdouts or not name.startswith("holdout:")},
            rows=[dict(scenario=item["name"], measure=name, verdict=verdict) for item, name, _, verdict in rows
                  if args.show_holdouts or "holdout" not in item["tags"]]), indent=1, default=list))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
