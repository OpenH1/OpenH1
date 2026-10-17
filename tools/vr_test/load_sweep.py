#!/usr/bin/env python3
"""Measure all stock maps on simulated VR with verified file cache states.

Uses the existing NVIDIA/Monado runner. Each map primes private decompressed and
shader caches, then measures OS-cold and OS-warm loads in separate processes.
No screenshots or global cache eviction. Failures remain failures while the
remaining maps run. Only logs, settings, hashes and residency proofs survive.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from load_benchmark import FIELDS, completed_load, parse_trace, prepare_files, scenario_name, summarize
from render_enumerate_sweep import STOCK_MAPS


def runtime_evidence(events, scenario, text):
    """Reject flat fallback and require a rendered XR frame for the loaded map."""
    errors = [line for line in text.splitlines() if any(marker in line for marker in
              ("XR_ERROR_", "OpenXR is not available; playing flat", "halo-host: error: vr:"))]
    scenario = r"levels\ui\ui" if scenario == "ui" else scenario
    activation = next((event["wall"] for event in events
                       if event["phase"] == "main_new_map.begin" and event["detail"] == scenario), None)
    next_map = min((event["wall"] for event in events if event["phase"] == "main_new_map.begin"
                    and activation is not None and event["wall"] > activation), default=float("inf"))
    first = next((event["wall"] for event in events
                  if event["phase"] == "first_game_frame.begin" and activation is not None
                  and activation <= event["wall"] < next_map), None)
    waits, frames = {}, 0
    for event in sorted(events, key=lambda event: event["wall"]):
        fields = dict(FIELDS.findall(event["detail"]))
        predicted = fields.get("predicted")
        if event["phase"] == "xr.wait.end" and predicted:
            waits[predicted] = fields.get("render")
        elif event["phase"] == "xr.submit.end" and first is not None and first <= event["wall"] < next_map:
            try:
                layers = int(fields.get("layers", "0"))
            except ValueError:
                continue
            # This sweep uses the default projection plus HUD fixture. Loading
            # is one world-space quad; scope/panels can add normal layers.
            if predicted and waits.get(predicted) == "1" and layers >= 2:
                frames += 1
    if not frames:
        errors.append("no rendered OpenXR gameplay submission after the requested map began rendering")
    return dict(valid=not errors, errors=errors, normal_submissions=frames)


def run(args, scenario, work, destination):
    destination.mkdir(parents=True)
    for name in ("game.log", "debug.txt", "monado.log", "config.toml"):
        (work / name).unlink(missing_ok=True)
    settings = dict(HALO_NET_ONLINE="false", HALO_VR="true", HALO_LOAD_TRACE="1",
                    HALO_SAVE_ROOT="/out/save-live", HALO_VULKAN_PIPELINE_CACHE="/out/shaders/pipelines.bin",
                    XDG_CACHE_HOME="/out/shaders", __GL_SHADER_DISK_CACHE_PATH="/out/shaders/nvidia",
                    VK_DRIVER_FILES="/usr/share/vulkan/icd.d/nvidia_icd.json",
                    VK_ICD_FILENAMES="/usr/share/vulkan/icd.d/nvidia_icd.json",
                    HALO_SCREENSHOT_EVERY="0", HALO_SCREENSHOT_DIR="", HALO_PREPARE_SHADERS="true")
    environment = {k: v for k, v in os.environ.items() if not k.startswith("HALO_")}
    environment.update(HALO_GAME=str(args.game), HALO_MAPS=str(args.maps))
    command = [str(TOOLS / "vr_test/vr_run.sh"), str(work), "menu" if scenario == "ui" else scenario,
               str(args.seconds)] + [f"{k}={v}" for k, v in settings.items()]
    with (destination / "runner.log").open("w") as output:
        try:
            status = subprocess.run(command, env=environment, stdout=output, stderr=subprocess.STDOUT,
                                    timeout=args.seconds + 150).returncode
        except subprocess.TimeoutExpired:
            status = 124
    for name in ("game.log", "debug.txt", "monado.log", "config.toml"):
        if (work / name).exists():
            shutil.copy2(work / name, destination / name)
    text = (destination / "game.log").read_text(errors="replace") if (destination / "game.log").exists() else ""
    debug = (destination / "debug.txt").read_text(errors="replace") if (destination / "debug.txt").exists() else ""
    errors = [line for line in (text + "\n" + debug).splitlines()
              if any(marker in line for marker in ("EXCEPTION", "vulkan validation:", "pipeline missed map preparation"))]
    try:
        events, operations = parse_trace(text)
    except ValueError as error:
        events, operations = [], []
        errors.append(str(error))
    runtime = runtime_evidence(events, scenario, text + "\n" + debug)
    errors.extend(runtime["errors"])
    result = summarize(events, operations, scenario)
    result.update(exit_status=status, errors=errors, settings=settings, command=command,
                  vr_runtime=runtime,
                  load_completed=completed_load(events, scenario),
                  passed=status == 0 and not errors and completed_load(events, scenario))
    (destination / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--maps", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--map", action="append")
    parser.add_argument("--seconds", type=int, default=15)
    parser.add_argument("--stop-file", type=Path, help="stop between maps when this file exists; record pending maps")
    args = parser.parse_args()
    if args.seconds < 1:
        parser.error("--seconds must be positive")
    scenarios = [scenario_name(name) for name in args.map] if args.map else STOCK_MAPS
    if any(scenario not in STOCK_MAPS for scenario in scenarios):
        parser.error("only stock maps are supported")
    args.game, args.maps, args.out = (p.resolve() for p in (args.game, args.maps, args.out))
    args.out.mkdir(parents=True, exist_ok=False)
    manifest = dict(passed=False, game_cache="reused", shader_cache="reused",
                    presentation="simulated OpenXR submission; no physical display claim",
                    hashes={p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                            for p in args.game.iterdir() if p.name in ("halo", "halo_guest.elf", "libSDL3.so.0")},
                    runs=[])
    def save():
        (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    save()
    for index, scenario in enumerate(scenarios):
        if args.stop_file and args.stop_file.exists():
            manifest.update(interrupted=True, pending_maps=list(scenarios[index:]))
            break
        name = scenario.rsplit("\\", 1)[-1]
        with tempfile.TemporaryDirectory(prefix=".work-", dir=args.out) as temporary:
            work = Path(temporary)
            (work / "shaders").mkdir()
            prime_passed = False
            for state in ("prime", "cold", "warm"):
                destination = args.out / name / state
                record = dict(map=scenario, state=state, passed=False)
                manifest["runs"].append(record)
                save()
                try:
                    if state != "prime":
                        caches = list((work / "save-live").rglob("*.map"))
                        shaders = [p for p in (work / "shaders").rglob("*") if p.is_file() and p.stat().st_size]
                        if not caches or not shaders:
                            raise RuntimeError("priming produced no reusable game or shader artifacts")
                        record["cache_proof"] = prepare_files([args.maps / f"{name}.map", args.maps / "ui.map", *caches], state)
                        record["shader_inventory"] = [dict(path=str(p.relative_to(work)), bytes=p.stat().st_size,
                                                           sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in shaders]
                    result = run(args, scenario, work, destination)
                    if state == "prime":
                        prime_passed = result["passed"]
                    else:
                        record["priming_valid"] = prime_passed
                        if not prime_passed:
                            result["run_passed"] = result["passed"]
                            result["priming_valid"] = False
                            result.setdefault("errors", []).append("initial VR cache priming failed")
                            result["passed"] = False
                            (destination / "result.json").write_text(json.dumps(result, indent=2) + "\n")
                    record.update(passed=result["passed"], result=str(destination / "result.json"))
                except Exception as error:
                    record["error"] = str(error)
                save()
                print(f"{'PASS' if record['passed'] else 'FAIL'} {name} {state}", flush=True)
    manifest["passed"] = not manifest.get("interrupted") and all(record["passed"] for record in manifest["runs"])
    save()
    if manifest.get("interrupted"):
        return 2
    return 0 if manifest["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
