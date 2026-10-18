#!/usr/bin/env python3
"""Exercise VR transitions on the simulated headset, checking the events occurred.

    python tools/vr_test/scenarios.py --game build/linux_vulkan --maps /path/to/maps --out /path/to/evidence

Uses vr_run.sh (Docker/Monado/NVIDIA). Requires Pillow for lossless PNG evidence.
Runs one case at a time, offline, loading maps directly. Logs, commands, PNGs and
summary.json are retained in --out; temporary run folders and BMPs are removed.
Timing runs should be separate: these runs read images back from the GPU.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BLOODGULCH = r"levels\test\bloodgulch\bloodgulch"


def pose(*, left=(-.15, -.30, .35), right=(.18, -.25, .35), grip=0,
         buttons=0, trigger=-1, frames=72, down=0, move=0, right_grip=0):
    """debug.vr_test_hands: positions, orientations, then its documented controls."""
    return " ".join(str(value) for value in
        (*left, 0, 0, 0, *right, 0, 0, 0, grip, buttons, trigger, 0, frames,
         move, down, 0, 0, 0, 0, 0, right_grip))


def hands(*poses):
    return ";".join(poses)


def eye_content(picture):
    """Reject blank or single-eye captures; this is not a geometry comparison."""
    width, height = picture.size
    result = []
    for left, right in ((0, width // 2), (width // 2, width)):
        eye = picture.crop((left, 0, right, height)).convert("RGB")
        extrema = eye.getextrema()
        result.append(dict(extrema=[list(pair) for pair in extrema],
                           has_content=bool(eye.width and eye.height)
                           and any(high - low > 8 for low, high in extrema)))
    return result


def cases():
    # A reach is held before squeezing: a fast swing through a holster must
    # not draw. Coordinates are the holster and pouch centres in vr.c.
    holsters = hands(pose(frames=288), pose(right=(.24, -.68, .02)),
                     pose(right=(.24, -.68, .02), right_grip=1), pose(frames=144),
                     pose(right=(.14, -.08, -.12)), pose(right=(.14, -.08, -.12), right_grip=1),
                     pose(frames=2880))
    # The AR's well at this right-hand pose is logged as (.14, -.29, .22).
    # The held magazine's top is 8 cm above the left grip (reload_held).
    reload = hands(pose(trigger=1, frames=288), pose(left=(-.22, -.66, .06)),
                   pose(left=(-.22, -.66, .06), grip=1),
                   pose(left=(.14, -.37, .22), grip=1, move=24),
                   pose(frames=144), pose(trigger=1, frames=144), pose(frames=2880))
    crouch = hands(pose(frames=288), pose(down=.50, frames=144, move=36),
                   pose(frames=2880, move=36))
    menu = hands(pose(frames=288), pose(buttons=16), pose(buttons=128),
                 pose(buttons=128), pose(buttons=1), pose(frames=2880))
    return {
        "holsters": (BLOODGULCH, 40, {"HALO_VR_TEST_HANDS": holsters},
                     [r"hip's holster: drawing its weapon", r"shoulder's holster: drawing its weapon"]),
        "return_menu": (BLOODGULCH, 24, {"HALO_VR_AIM": "gamepad", "HALO_VR_TEST_HANDS": menu},
                        [r"starting precaching of map 'ui'", r"starting main menu music"]),
        "reload": (BLOODGULCH, 36, {"HALO_VR_TEST_HANDS": reload},
                   [r"a magazine taken from the left hip", r"the magazine pushed home", r"loaded at once:", r"vr reload: tick \d+: fired"]),
        "crouch": (BLOODGULCH, 30, {"HALO_VR_TEST_HANDS": crouch},
                   [r"vr: crouch: on", r"vr: crouch: off"]),
        "death": (BLOODGULCH, 30, {"HALO_VR_TEST_DEATH": "3 kill", "HALO_VR_TEST_HANDS": pose(frames=2880)},
                  [r"vr death test: frame \d+: dead, the eye held", r"back: the helmet's view switches on"]),
        "cinematic": ("a10", 60, {"HALO_SKIP_CINEMATICS": "false", "HALO_VR_TEST_HANDS": pose(frames=2880)},
                      [r"vr cinematic: tick \d+: from (?:the player's head|a movie camera)"]),
    }


def run_case(name, case, args):
    from PIL import Image
    destination = args.out / name
    destination.mkdir(parents=True, exist_ok=False)
    map_name, seconds, settings, expected = case
    settings = dict(HALO_NET_ONLINE="false", HALO_VR_TEST_HEAD="0", HALO_PREPARE_SHADERS="true",
                    HALO_SCREENSHOT_DIR="/out/shots", HALO_SCREENSHOT_EVERY="144", **settings)
    settings.update(dict(value.split("=", 1) for value in args.env))
    settings["HALO_NET_ONLINE"] = "false"
    evidence = {"name": name, "map": map_name, "seconds": seconds, "settings": settings,
                "game": str(args.game), "binary_sha256": hashlib.sha256((args.game / "halo").read_bytes()).hexdigest()}
    guest = args.game / "halo_guest.elf"
    if guest.exists():
        evidence["guest_sha256"] = hashlib.sha256(guest.read_bytes()).hexdigest()
    environment = {key: value for key, value in os.environ.items() if not key.startswith("HALO_")}
    environment.update(HALO_GAME=str(args.game), HALO_MAPS=str(args.maps))
    with tempfile.TemporaryDirectory(prefix=f"halo-vr-{name}-") as temporary:
        run = Path(temporary)
        command = [str(ROOT / "tools/vr_test/vr_run.sh"), str(run), map_name, str(seconds)]
        command += [f"{key}={value}" for key, value in settings.items()]
        with (destination / "runner.log").open("w") as log:
            try:
                result = subprocess.run(command, env=environment, stdout=log, stderr=subprocess.STDOUT,
                                        timeout=seconds + 150)
                status = result.returncode
            except subprocess.TimeoutExpired:
                status = 124
                log.write("VR scenario exceeded its timeout\n")
        evidence["exit_status"] = status
        log_text = ""
        for filename in ("game.log", "debug.txt", "monado.log", "manifest.txt"):
            path = run / filename
            if path.exists():
                shutil.copy2(path, destination / filename)
                if filename in ("game.log", "debug.txt"):
                    log_text += path.read_text(errors="replace")
        evidence["events"] = {pattern: bool(re.search(pattern, log_text)) for pattern in expected}
        evidence["exceptions"] = [line for line in log_text.splitlines() if "EXCEPTION" in line]
        evidence["validation"] = [line for line in log_text.splitlines() if "vulkan validation:" in line]
        evidence["pipeline_misses"] = [line for line in log_text.splitlines()
                                       if "pipeline missed map preparation" in line]
        images = destination / "images"
        images.mkdir()
        evidence["images"] = []
        for path in sorted((run / "shots").glob("*.bmp")):
            with Image.open(path) as original:
                picture = original.convert("RGBA")
                picture.save(images / (path.stem + ".png"))
                evidence["images"].append({"name": path.stem, "size": list(picture.size),
                                           "eyes": eye_content(picture) if path.stem.startswith("eyes") else [],
                                           "sha256": hashlib.sha256(picture.tobytes()).hexdigest()})
        evidence["passed"] = (status == 0 and not evidence["exceptions"]
                              and not evidence["validation"] and not evidence["pipeline_misses"]
                              and all(evidence["events"].values())
                              and any(item["eyes"] and all(eye["has_content"] for eye in item["eyes"])
                                      for item in evidence["images"]))
    (destination / "result.json").write_text(json.dumps(evidence, indent=2) + "\n")
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--maps", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", action="append", choices=list(cases()))
    parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE")
    args = parser.parse_args()
    args.game, args.maps, args.out = (path.resolve() for path in (args.game, args.maps, args.out))
    args.out.mkdir(parents=True, exist_ok=True)
    results = []
    for name in args.case or cases():
        result = run_case(name, cases()[name], args)
        results.append(result)
        print(f"{'PASS' if result['passed'] else 'FAIL'} {name}: "
              f"{sum(result['events'].values())}/{len(result['events'])} events, "
              f"{len(result['images'])} images, exit {result['exit_status']}", flush=True)
        summary = [json.loads(path.read_text()) for path in sorted(args.out.glob("*/result.json"))]
        (args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    return 0 if all(result["passed"] for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
