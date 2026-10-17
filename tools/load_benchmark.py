#!/usr/bin/env python3
"""Measure offline map loading with explicit file, game and shader cache states.

The default offscreen driver measures work and submission, not physical display
cadence. HALO_LOAD_TRACE instrumentation is required. Private game caches live
only for this invocation; logs and JSON survive, run folders and BMPs do not.
Linux cold runs evict only the tested map/cache files and verify their residency.
No system-wide caches are dropped.
"""
import argparse
import ctypes
import hashlib
import json
import math
import mmap
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

from render_enumerate_sweep import STOCK_MAPS


MARKER = re.compile(r"\b(LOAD|LOAD_IO) ")
FIELDS = re.compile(r"(\w+)=(.*?)(?= \w+=|$)")
PRESENT_PHASES = ("gl.swap.return", "vulkan.present.return", "metal.presented", "xr.submit.end")
SAFE_SETTINGS = {"HALO_VR", "HALO_PREPARE_SHADERS", "HALO_RANDOM_SEED", "HALO_NO_AUDIO",
                 "HALO_SKIP_CINEMATICS", "HALO_GPU_STATS", "HALO_VULKAN_STATS", "HALO_VULKAN_VALIDATION",
                 "HALO_NO_VSYNC", "HALO_RESOLUTION", "HALO_RESOLUTION_SCALING", "HALO_VR_TIMING",
                 "HALO_VR_GPU_TIME", "HALO_VR_FOVEATION", "HALO_VR_RESOLUTION"}


def parse_trace(text):
    events, operations = [], []
    for number, line in enumerate(text.splitlines(), 1):
        marker = MARKER.search(line)
        if not marker:
            continue
        if MARKER.search(line, marker.end()):
            raise ValueError(f"interleaved load records at line {number}")
        kind = marker[1]
        payload = line[marker.end():]
        # detail and path are terminal free text, and may themselves contain '='.
        tail = "detail" if kind == "LOAD" else "path"
        head, separator, value = payload.partition(" " + tail + "=")
        pairs = FIELDS.findall(head)
        fields = dict(pairs)
        try:
            if not separator or len(fields) != len(pairs):
                raise ValueError("missing terminal field or duplicate field")
            fields[tail] = value
            required = ({"phase", "wall", "thread", "process", "detail"} if kind == "LOAD" else
                        {"op", "wall", "elapsed", "requested", "actual", "offset", "path"})
            if not required <= fields.keys():
                raise ValueError("missing required field")
            for key in ("wall", "thread", "process", "elapsed", "syscall"):
                if key in fields:
                    fields[key] = float(fields[key])
                    if not math.isfinite(fields[key]) or (fields[key] < 0 and
                            not (key in ("thread", "process") and fields[key] == -1)):
                        raise ValueError("invalid clock or duration")
            for key in ("requested", "actual", "offset", "bounce"):
                if key in fields:
                    fields[key] = int(fields[key])
                    if fields[key] < 0:
                        raise ValueError("negative byte count or offset")
            if kind == "LOAD":
                if not fields["phase"] or any(c.isspace() for c in fields["phase"]):
                    raise ValueError("invalid phase")
                events.append(fields)
            else:
                if fields["op"] not in ("read", "write") or fields["actual"] > fields["requested"]:
                    raise ValueError("invalid I/O operation or byte count")
                operations.append(fields)
        except (ValueError, TypeError) as error:
            raise ValueError(f"malformed {kind} at line {number}: {error}") from error
    return events, operations


# Milestones whose end is deliberately not named <phase>.end.
MILESTONES = {
    "enumerate.draws": ("enumerate.begin", "enumerate.draws.end"),
    "enumerate.total": ("enumerate.begin", "enumerate.prepared.end"),
    "enumerate.wait": ("enumerate.draws.end", "enumerate.prepared.end"),
    "tags.validate": ("tags.read.end", "tags.validate.end"),
    "bsp.validate": ("bsp.read.end", "bsp.validate.end"),
    "bsp.register": ("bsp.validate.end", "bsp.end"),
    "objects_and_remaining_initialize": ("objects.begin", "initialize.end"),
    "prepare.wait": ("prepare.wait.begin", "prepare.end"),
    "main_new_map.activation": ("main_new_map.begin", "main_new_map.initial_pulse.end"),
    "first_game_frame": ("first_game_frame.begin", "first_game_frame.submitted"),
}


def span_between(phase, begin, end):
    same_owner = begin.get("owner") is not None and begin.get("owner") == end.get("owner")
    span = dict(phase=phase, detail=begin["detail"], owner=begin.get("owner"),
                begin_wall=begin["wall"], end_wall=end["wall"])
    for key in ("wall", "thread", "process"):
        valid = end[key] >= begin[key] >= 0 and (key != "thread" or same_owner)
        span[key + "_ms"] = 1000 * (end[key] - begin[key]) if valid else None
    return span


def summarize(events, operations, scenario):
    # Owner IDs prevent interleaved threads from pairing each other's phases.
    # Legacy traces retain wall/process spans but cannot prove thread CPU time.
    starts, spans, milestones = {}, [], {}
    for event in events:
        phase, owner = event["phase"], event.get("owner")
        if phase.endswith(".begin"):
            starts.setdefault((phase[:-6], owner), []).append(event)
        elif phase.endswith(".end") and starts.get((phase[:-4], owner)):
            begin = starts[(phase[:-4], owner)].pop()
            spans.append(span_between(phase[:-4], begin, event))
        for name, (first, last) in MILESTONES.items():
            if phase == first:
                milestones.setdefault((name, owner), []).append(event)
            elif phase == last and milestones.get((name, owner)):
                begin = milestones[(name, owner)].pop()
                spans.append(span_between(name, begin, event))
    presentations = {}
    for phase in PRESENT_PHASES:
        times = sorted(e["wall"] for e in events if e["phase"] == phase)
        if times:
            gaps = [1000 * (b - a) for a, b in zip(times, times[1:])]
            presentations[phase] = dict(count=len(times), maximum_gap_ms=max(gaps) if gaps else None,
                                         first_wall=times[0], last_wall=times[-1])
    totals = {}
    for op in ("read", "write"):
        selected = [record for record in operations if record.get("op") == op]
        totals[op] = dict(calls=len(selected), actual_bytes=sum(r.get("actual", 0) for r in selected),
                         elapsed_ms=1000 * sum(r.get("elapsed", 0) for r in selected),
                         syscall_ms=1000 * sum(r["syscall"] for r in selected)
                         if selected and all("syscall" in r for r in selected) else None)
    return dict(scenario=scenario, spans=spans, presentations=presentations, io=totals,
                events=events, operations=operations)


def completed_load(events, scenario):
    """Require the requested map's ordered activation and first rendered frame."""
    scenario = r"levels\ui\ui" if scenario == "ui" else scenario
    phase, owner, last_wall = 0, None, -1
    for event in events:
        name = event["phase"]
        if name == "main_new_map.begin":
            phase = 1 if event["detail"] == scenario else 0
            owner, last_wall = event.get("owner"), event["wall"]
            continue
        if not phase or event.get("owner") != owner:
            continue
        if event["wall"] < last_wall:
            return False
        last_wall = event["wall"]
        if phase == 1 and name == "initialize.end":
            phase = 2
        elif phase == 2 and name == "main_new_map.initial_pulse.end" and event["detail"] == scenario:
            phase = 3
        elif phase == 3 and name == "first_game_frame.begin" and event["detail"] in ("", scenario):
            phase = 4
        elif phase == 4 and name == "first_game_frame.submitted" and event["detail"] in ("", scenario):
            return True
    return False


def residency(path):
    """Count resident file pages without faulting them into memory (Linux)."""
    if sys.platform != "linux":
        raise RuntimeError("file-cache verification requires Linux mincore and posix_fadvise")
    size = path.stat().st_size
    if not size:
        return dict(bytes=0, pages=0, resident_pages=0)
    pages = (size + mmap.PAGESIZE - 1) // mmap.PAGESIZE
    library = ctypes.CDLL(None, use_errno=True)
    library.mincore.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p)
    library.mincore.restype = ctypes.c_int
    with path.open("rb") as file, mmap.mmap(file.fileno(), size, access=mmap.ACCESS_COPY) as mapped:
        address = ctypes.addressof(ctypes.c_char.from_buffer(mapped))
        vector = (ctypes.c_ubyte * pages)()
        if library.mincore(address, size, vector):
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error), str(path))
        resident = sum(bool(value & 1) for value in vector)
    return dict(bytes=size, pages=pages, resident_pages=resident)


def prepare_files(paths, state):
    if state not in ("cold", "warm"):
        raise ValueError("unknown file-cache state: " + state)
    proof = []
    for path in sorted(set(paths)):
        if not path.is_file():
            raise FileNotFoundError(path)
        before = residency(path)
        with path.open("rb") as file:
            if state == "cold":
                os.fsync(file.fileno())
                os.posix_fadvise(file.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
            else:
                while file.read(1024 * 1024):
                    pass
        after = residency(path)
        proof.append(dict(path=str(path), before=before, after=after))
    # mincore can conservatively report pages resident without permission to
    # inspect their cache state. Such a result is not proof of a cold run.
    if not proof:
        raise RuntimeError("no files supplied for cache-state verification")
    if state == "cold" and any(item["after"]["resident_pages"] for item in proof):
        raise RuntimeError("file-cache eviction not verified: " + json.dumps(proof))
    if state == "warm" and any(item["after"]["resident_pages"] != item["after"]["pages"] for item in proof):
        raise RuntimeError("warm file-cache residency not verified: " + json.dumps(proof))
    return proof


def scenario_name(value):
    if value == "ui":
        return value
    if "\\" in value:
        return value
    campaign = {"a10", "a30", "a50", "b30", "b40", "c10", "c20", "c40", "d20", "d40"}
    return rf"levels\{value}\{value}" if value in campaign else rf"levels\test\{value}\{value}"


def run_game(args, scenario, work, saves, shaders, destination, cache_proof=None):
    if any(key not in SAFE_SETTINGS for key, value in args.env):
        raise ValueError("--env includes settings that can change the scenario or isolation")
    destination.mkdir(parents=True, exist_ok=False)
    if cache_proof is not None:
        (destination / "cache-proof.json").write_text(json.dumps(cache_proof, indent=2) + "\n")
    with tempfile.TemporaryDirectory(prefix="run-", dir=work) as temporary:
        run = Path(temporary)
        # Desktop settings are located from the executable, not HALO_DATA_ROOT.
        # A symlink still resolves to the original installation on Linux.
        executable = run / args.halo.name
        shutil.copy2(args.halo, executable)
        sdl = args.halo.parent / "libSDL3.so.0"
        if sdl.exists():
            shutil.copy2(sdl, run / sdl.name)
        (run / "maps").symlink_to(args.data / "maps")
        if scenario != "ui":
            (run / "init.txt").write_text(f"map_name {scenario}\n")
        environment = dict(os.environ)
        # Existing task-specific settings must not accidentally select a lobby,
        # screenshots, or another run's paths.
        for key in list(environment):
            if key.startswith(("HALO_", "MESA_SHADER_CACHE", "__GL_SHADER_DISK_CACHE")):
                del environment[key]
        environment.update(dict(args.env))
        environment.update(HALO_NET_ONLINE="false", HALO_DATA_ROOT=str(run), HALO_SAVE_ROOT=str(saves),
                           HALO_EXIT_AFTER=str(args.seconds), HALO_LOAD_TRACE="1",
                           HALO_UPDATE_ANSWER="no", HALO_SCREENSHOT_EVERY="0", HALO_SCREENSHOT_DIR="",
                           XDG_CACHE_HOME=str(shaders), __GL_SHADER_DISK_CACHE_PATH=str(shaders / "nvidia"),
                           MESA_SHADER_CACHE_DIR=str(shaders / "mesa"), MESA_SHADER_CACHE_DISABLE="false",
                           __GL_SHADER_DISK_CACHE="1",
                           HALO_VULKAN_PIPELINE_CACHE=str(shaders / "vulkan.bin"))
        environment["SDL_VIDEO_DRIVER"] = "offscreen"
        environment["SDL_AUDIO_DRIVER"] = "dummy"
        environment.pop("SDL_VIDEODRIVER", None)
        environment.pop("SDL_AUDIODRIVER", None)
        started = time.monotonic()
        with (destination / "stdout.txt").open("w") as output:
            try:
                status = subprocess.run([str(executable)], cwd=run, env=environment, stdout=output,
                                        stderr=subprocess.STDOUT, timeout=args.seconds + 120).returncode
            except subprocess.TimeoutExpired:
                status = 124
            except OSError as error:
                status = 127
                output.write(f"launch failed: {error}\n")
        elapsed = time.monotonic() - started
        for name in ("debug.txt", "config.toml"):
            if (run / name).exists():
                shutil.copy2(run / name, destination / name)
        text = (destination / "stdout.txt").read_text(errors="replace")
        debug = destination / "debug.txt"
        debug_text = debug.read_text(errors="replace") if debug.exists() else ""
        errors = [line for line in (text + "\n" + debug_text).splitlines()
                  if "EXCEPTION" in line or "vulkan validation:" in line]
        pipeline_misses = [line for line in (text + "\n" + debug_text).splitlines()
                           if "pipeline missed map preparation" in line]
        try:
            events, operations = parse_trace(text)
        except ValueError as error:
            errors.append(str(error))
            events, operations = [], []
        result = summarize(events, operations, scenario)
        result.update(exit_status=status, elapsed_seconds=elapsed, errors=errors,
                      passed=status == 0 and not errors and not pipeline_misses and completed_load(events, scenario),
                      pipeline_misses=pipeline_misses, load_completed=completed_load(events, scenario),
                      settings={k: v for k, v in environment.items()
                                if k.startswith(("HALO_", "SDL_", "XDG_CACHE", "__GL_SHADER", "MESA_SHADER"))})
        (destination / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        if not result["passed"]:
            raise RuntimeError(f"run failed: {destination} (exit {status}; errors {errors[:3]}; trace {len(events)})")
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--halo", type=Path, required=True)
    parser.add_argument("--data", type=Path, required=True, help="folder containing maps/")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--maps", nargs="+", default=["bloodgulch", "a10", "b30"])
    parser.add_argument("--all", action="store_true", help="all 24 stock maps, including ui")
    parser.add_argument("--seconds", type=float, default=15)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--os-cache", nargs="+", choices=("cold", "warm"), default=["cold", "warm"])
    parser.add_argument("--game-cache", choices=("fresh", "reused"), default="reused")
    parser.add_argument("--shader-cache", choices=("fresh", "reused"), default="reused")
    parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE")
    args = parser.parse_args()
    args.env = [value.split("=", 1) for value in args.env]
    if any(len(value) != 2 for value in args.env):
        parser.error("--env must be NAME=VALUE")
    if any(key not in SAFE_SETTINGS for key, value in args.env):
        parser.error("--env permits only documented rendering/timing settings: " + ", ".join(sorted(SAFE_SETTINGS)))
    if args.repeat < 1 or not math.isfinite(args.seconds) or args.seconds <= 0:
        parser.error("--repeat and --seconds must be positive")
    if sys.platform != "linux":
        parser.error("cache-state benchmarks require Linux")
    scenarios = STOCK_MAPS if args.all else [scenario_name(value) for value in args.maps]
    if any(scenario not in STOCK_MAPS for scenario in scenarios):
        parser.error("--maps must name stock maps")
    if len(set(scenarios)) != len(scenarios) or len(set(args.os_cache)) != len(args.os_cache):
        parser.error("duplicate maps or OS cache states; use --repeat for repetitions")
    args.halo, args.data, args.out = (path.resolve() for path in (args.halo, args.data, args.out))
    args.out.mkdir(parents=True, exist_ok=False)
    manifest = dict(binary=str(args.halo), sha256=hashlib.sha256(args.halo.read_bytes()).hexdigest(),
                    kernel=os.uname().release, machine=os.uname().machine, game_cache=args.game_cache,
                    shader_cache=args.shader_cache, presentation="offscreen by default; submissions are not display proof",
                    file_cache_scope="requested source map, ui.map, and private decompressed map files only",
                    shader_cache_scope="private disk artifacts; every run starts a new process",
                    passed=False, runs=[])
    guest = args.halo.parent / "halo_guest.elf"
    if guest.exists():
        manifest["guest_sha256"] = hashlib.sha256(guest.read_bytes()).hexdigest()
    def save_manifest():
        (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    def recorded_run(scenario, work, saves, shaders, destination, **metadata):
        record = dict(map=scenario, result=str(destination / "result.json"), passed=False, **metadata)
        manifest["runs"].append(record)
        save_manifest()
        try:
            result = run_game(args, scenario, work, saves, shaders, destination,
                              cache_proof=metadata.get("cache_proof"))
            record["passed"] = result["passed"]
            return result
        except Exception as error:
            record["error"] = str(error)
            raise
        finally:
            save_manifest()

    save_manifest()
    try:
        # Keep decompressed maps on the evidence filesystem: /tmp is often
        # tmpfs, whose pages cannot be evicted for a cold disk-cache run.
        with tempfile.TemporaryDirectory(prefix=".work-", dir=args.out) as temporary:
            work = Path(temporary)
            for index, scenario in enumerate(scenarios):
                name = scenario.rsplit("\\", 1)[-1]
                saves, shaders = work / f"save-{index}", work / f"shaders-{index}"
                saves.mkdir()
                shaders.mkdir()
                if "reused" in (args.game_cache, args.shader_cache):
                    recorded_run(scenario, work, saves, shaders, args.out / name / "prime", kind="prime")
                for repeat in range(args.repeat):
                    for state in args.os_cache:
                        for folder, mode in ((saves, args.game_cache), (shaders, args.shader_cache)):
                            if mode == "fresh":
                                shutil.rmtree(folder)
                                folder.mkdir()
                        destination = args.out / name / f"{repeat}-{state}"
                        paths = [args.data / "maps" / f"{name}.map", args.data / "maps" / "ui.map"]
                        map_caches = list(saves.rglob("*.map"))
                        paths += map_caches
                        if args.game_cache == "reused" and not map_caches:
                            raise RuntimeError("priming run produced no reusable map cache")
                        shader_files = [p for p in shaders.rglob("*") if p.is_file() and p.stat().st_size]
                        if args.shader_cache == "reused" and not shader_files:
                            raise RuntimeError("priming run produced no reusable shader cache artifacts")
                        shader_inventory = [dict(path=str(p.relative_to(shaders)), bytes=p.stat().st_size)
                                            for p in sorted(shader_files)]
                        proof = prepare_files(paths, state)
                        result = recorded_run(scenario, work, saves, shaders, destination, kind="measured",
                                              repeat=repeat, os_cache=state, cache_proof=proof,
                                              shader_artifacts_before=shader_inventory)
                        print(f"PASS {name} {state} repeat {repeat}: {len(result['events'])} trace events", flush=True)
        manifest["passed"] = True
    except Exception as error:
        manifest["error"] = str(error)
        raise
    finally:
        save_manifest()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
