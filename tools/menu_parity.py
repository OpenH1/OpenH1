#!/usr/bin/env python3
"""The menu engine's parity with the game's widgets (docs/menu_engine.md), proved by machine: every generated
screen run on each engine (display.menu_engine "classic" and "new"), in each aspect ratio and with each input,
and what each draws compared.

    python tools/menu_parity.py --layout              # every screen's debug.menu_layout, both engines, diffed
    python tools/menu_parity.py --layout --screens settings --variants 16x9-keyboard

--layout runs tools/menu_gallery.py's tour (debug.menu_tour: each screen opened alone, hidden, muted, no
renderer) once on each engine for each variant, and compares each screen's layout (port/linux/game/menu_layout.c:
every widget's box and clip, every string's text, box, clip and extent, every button's glyph and every pointer
target), item by item. The variants are the four aspect ratios (4:3, 16:10, 16:9, 21:9) with each input (a
controller, the keyboard and mouse, touch).

The runs reuse one map cache (a clone of --saves's, by default ~/.cache/openh1-bench/saves/z: APFS clones on a
Mac, so nothing is unpacked again), each with its own data and save folders, deleted afterwards. A difference
not listed in DELIBERATE fails it (exit status 1).
"""

import argparse
import concurrent.futures
import json
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import menu_gallery  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

# the aspect ratios, at 1080 rows (16:10's the Steam Deck's 1280x800), and the inputs
ASPECTS = {"4x3": (1440, 1080), "16x10": (1280, 800), "16x9": (1920, 1080), "21x9": (2520, 1080)}
INPUTS = {"controller": ("controller", False), "keyboard": ("keyboard", False), "touch": ("controller", True)}
VARIANTS = {f"{aspect}-{name}": menu_gallery.Variant(f"{name}, {aspect.replace('x', ':')}", size, prompts,
                                                     touch=touch)
            for aspect, size in ASPECTS.items() for name, (prompts, touch) in INPUTS.items()}

# the differences between the engines that are meant (docs/menu_engine.md, "Deliberate differences"): (a
# regex of the screen, a function of the two items that says whether their difference is that one)
def _shows_menu_engine(left, right) -> bool:
    """Profiles' MENU ENGINE row shows the engine that draws it"""
    return all(item and "menu_engine_spinner" in item.get("of", "") and item.get("text") in ("CLASSIC", "NEW")
               for item in (left, right))


DELIBERATE = [("profile_settings", _shows_menu_engine)]


def is_deliberate_frame(screen: str, classic: dict, new: dict) -> bool:
    """whether two frames' difference is a deliberate one: their layouts differ only so (the frames of a
    screen whose layouts are the same must be the same)"""
    if classic is None or new is None:
        return False
    a, b = layout_items(classic), layout_items(new)
    return len(a) == len(b) and a != b and all(
        left == right or any(re.search(pattern, screen) and meant(left, right) for pattern, meant in DELIBERATE)
        for left, right in zip(a, b))


# the engine each side runs (--baseline: the classic on both, to see what differs between two runs of one)
ENGINES = {}


def engine_variant(variant: menu_gallery.Variant, engine: str) -> menu_gallery.Variant:
    settings = dict(variant.settings)
    settings["HALO_MENU_ENGINE"] = engine
    return menu_gallery.Variant(variant.description, variant.size, variant.prompts, variant.touch, variant.phone,
                                variant.default, settings)


def layout_items(layout: dict) -> list:
    """a layout's items, without the frame's number and the widgets' addresses (each run's own): each widget
    named by its path of names from the screen, the others by their widget's"""
    widgets = {}
    for item in layout.get("items", []):
        if item.get("kind") == "widget":
            widgets[item.get("id")] = item
    paths = {}

    def path_of(identifier):
        if identifier in paths:
            return paths[identifier]
        item = widgets.get(identifier)
        if not item:
            return "?"
        parent = item.get("parent")
        path = (path_of(parent) + "/" if parent in widgets else "") + str(item.get("name"))
        paths[identifier] = path
        return path

    items = []
    for item in layout.get("items", []):
        copy = {key: value for key, value in item.items() if key not in ("id", "parent", "widget")}
        if item.get("kind") == "widget":
            copy["path"] = path_of(item.get("id"))
        elif "widget" in item:
            copy["of"] = path_of(item.get("widget"))
        items.append(copy)
    return items


def compare_layouts(screen: str, classic: dict, new: dict) -> list:
    """the differences of two layouts of a screen, as lines (none: the same)"""
    if classic is None or new is None:
        return [] if classic is None and new is None else \
            [f"layout written by {'classic' if classic is not None else 'new'} only"]
    lines = []
    for key in ("screen_name", "screen", "prompts", "touch_menus"):
        if classic.get(key) != new.get(key):
            lines.append(f"{key}: {classic.get(key)!r} | {new.get(key)!r}")
    a, b = layout_items(classic), layout_items(new)
    for index in range(max(len(a), len(b))):
        left = a[index] if index < len(a) else None
        right = b[index] if index < len(b) else None
        if left == right:
            continue
        if any(re.search(pattern, screen) and meant(left, right) for pattern, meant in DELIBERATE):
            continue
        lines.append(f"item {index}: {json.dumps(left, sort_keys=True)}\n      | {json.dumps(right, sort_keys=True)}")
        if len(lines) >= 6:
            lines.append(f"... ({abs(len(a) - len(b))} items more on one side)" if len(a) != len(b) else "...")
            break
    return lines


def raw_shot(path: Path, size):
    """a tour's frame as it was drawn (not scaled to the variant's size, as the gallery's are), in memory"""
    from PIL import Image
    with Image.open(path) as opened:
        return opened.convert("RGB")


def compare_pictures(classic, new) -> list:
    """the differences of two frames of a screen, as lines (none: the same pixels)"""
    if classic is None or new is None:
        return [] if classic is None and new is None else \
            [f"frame drawn by {'classic' if classic is not None else 'new'} only"]
    if classic.size != new.size:
        return [f"frame sizes {classic.size} | {new.size}"]
    from PIL import ImageChops
    difference = ImageChops.difference(classic, new)
    box = difference.getbbox()
    if not box:
        return []
    count = sum(1 for pixel in difference.crop(box).getdata() if pixel != (0, 0, 0))
    largest = max(max(pixel) for pixel in difference.crop(box).getdata())
    return [f"{count} pixels differ (by up to {largest}) in {list(box)} of {list(classic.size)}"]


def run_layouts(build: Path, screens: list, variant_names: list, maps: Path, template: Path, jobs: int,
                timeout: float, pictures: bool = False) -> dict:
    """{(variant, engine): {screen: (picture, layout)}}, each variant's screens split over parts run at once
    (pictures: each screen's frame too, read from the run's folder into memory, which is then deleted)"""
    parts = max(1, jobs // max(1, 2 * len(variant_names)))
    size = -(-len(screens) // parts)
    tasks = [(name, engine, screens[start:start + size]) for name in variant_names for engine in ("classic", "new")
             for start in range(0, len(screens), size)]
    results = {}

    def one(task):
        name, engine, part = task
        try:
            found = menu_gallery.tour(build, part, engine_variant(VARIANTS[name], ENGINES.get(engine, engine)), maps,
                                      None, pictures,
                                      False, timeout + len(part), template)
        except Exception as error:  # (reported as the screens' difference)
            return task, {screen.name: {"error": str(error)} for screen in part}
        return task, {screen.name: found[screen.name] for screen in part}

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(jobs, len(tasks)))) as pool:
        for (name, engine, part), made in pool.map(one, tasks):
            results.setdefault((name, engine), {}).update(made)
            print(f"menu_parity: {name} {engine}: {len(part)} screens", file=sys.stderr, flush=True)
    return results


# ---------- behaviour: scripted presses on each engine, what they leave compared

PE = "main_menu/settings_select/player_setup/player_profile_edit"
# (the 4:3 window's fractions of the buttons of a Settings tab or page: the menus' 640 x 480 fill it)
OK_CLICK, CANCEL_CLICK, DEFAULTS_CLICK = "click:0.659:0.889", "click:0.859:0.889", "click:0.138:0.889"
# the seconds a run waits for the menus before its first press (debug.test_input "menu:" times its presses
# from the input's first read, before the menus are up)
SETTLE = ["wait"] * 12

# each script: its name, the screen it starts on (debug.menu_open) and its presses (debug.test_input
# "menu:...", one a second: port/linux/src/xinput_sdl.c)
SCRIPTS = [
    ("spinners", f"{PE}/audio_settings/audio_settings_screen",
     ["down", "right", "down", "right", "down", "right", "down", "down", "down", "right", "right", "left",
      OK_CLICK]),
    ("toggle-and-cancel", f"{PE}/audio_settings/audio_settings_screen",
     ["down", "down", "right", "down", "down", "right", "b"]),
    ("slider-keys-and-typed", f"{PE}/video_settings/video_settings_screen",
     ["down", "down", "down", "right", "right", "left", "down", "up", "a", "type:118", "enter",
      OK_CLICK]),
    ("sub-page", f"{PE}/video_settings/video_settings_screen",
     ["down", "down", "down", "down", "a", "wait", "down", "right", "down", "right", "b", "wait", OK_CLICK]),
    ("sub-page-keep", f"{PE}/video_settings/video_settings_screen",
     ["down", "down", "down", "down", "a", "wait", "down", "right", "right", OK_CLICK, "wait", "down", "right",
      OK_CLICK]),
    ("tabs", f"{PE}/audio_settings/audio_settings_screen",
     ["rb", "wait", "down", "right", "rb", "wait", "rb", "wait", "down", "right", "lb", "wait", "lb", "wait",
      "down", "right", OK_CLICK]),
    ("defaults", f"{PE}/audio_settings/audio_settings_screen",
     ["down", "right", DEFAULTS_CLICK, "wait", OK_CLICK]),
    ("key-binding", f"{PE}/controls_setup/controls_settings_screen",
     ["down", "down", "a", "type:k", "wait", "down", "right", "a", "type:j", "wait", "up", "up", OK_CLICK]),
    ("mouse-and-cancel-button", f"{PE}/mouse_settings/mouse_settings_screen",
     ["down", "right", "right", "down", "right", "a", "type:3", "enter", CANCEL_CLICK]),
    ("profile-tab", f"{PE}/profile_settings/profile_settings_screen",
     ["down", "down", "down", "down", "down", "right", "up", "up", OK_CLICK]),
]
BEHAVIOUR_VARIANT = "4x3-keyboard"


def behaviour_run(build: Path, script: tuple, engine: str, maps: Path, template: Path, timeout: float) -> dict:
    """one script on one engine: {"config": config.toml's lines, "profile": the saved profiles' bytes'
    digests, "writes": the settings the log says were written, "layout": the last screen's layout, "log"}"""
    import hashlib
    import signal
    import subprocess
    import tempfile
    name, screen, presses = script
    folder = Path(tempfile.mkdtemp(prefix="menu-parity-"))
    process = None
    try:
        game = folder / "game"
        game.mkdir()
        for file in menu_gallery.capture.BUILD_FILES:
            if (build / file).is_file():
                menu_gallery.place(build / file, game / file)
        if sys.platform != "darwin":
            for library in build.glob("*.so*"):
                menu_gallery.place(library, game / library.name)
        variant = engine_variant(VARIANTS[BEHAVIOUR_VARIANT], ENGINES.get(engine, engine))
        environment = menu_gallery.run_environment(variant, folder, timeout, True)
        environment.pop("HALO_MENU_TOUR", None)
        presses = SETTLE + presses + ["wait"] * 3
        environment.update({
            "HALO_MENU_OPEN": screen, "HALO_TEST_INPUT": "menu:" + " ".join(presses),
            "HALO_EXIT_AFTER": str(len(presses) + 4), "HALO_MENU_LAYOUT": "layout.json",
        })
        if template:
            menu_gallery.map_cache_clone(template, folder / "saves" / "z")
        (folder / "data" / "maps").symlink_to(maps.resolve())
        if sys.platform != "darwin":
            environment["SDL_VIDEO_DRIVER"] = "offscreen"
        with open(folder / "halo.log", "w") as log:
            process = subprocess.Popen([str(game / "halo")], cwd=game, env=environment, stdin=subprocess.DEVNULL,
                                       stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                process.wait(timeout=len(presses) + timeout)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        text = (folder / "halo.log").read_text(errors="replace")
        # (the run's config.toml, beside its executable: never the computer's own)
        config = (game / "config.toml").read_text(errors="replace").splitlines() \
            if (game / "config.toml").is_file() else []
        profiles = {path.relative_to(folder / "saves").as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                    for path in sorted((folder / "saves").rglob("*")) if path.is_file() and
                    not path.relative_to(folder / "saves").as_posix().startswith("z/")}
        layout = None
        if (folder / "data" / "layout.json").is_file():
            try:
                layout = json.loads((folder / "data" / "layout.json").read_text(errors="replace"))
            except json.JSONDecodeError:
                layout = None
        writes = [line.split("menus: ", 1)[1] for line in text.splitlines()
                  if "menus: " in line and " = " in line and "could not" not in line]
        return {"config": config, "profile": profiles, "writes": writes, "layout": layout, "log": text[-3000:]}
    finally:
        if process and process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
        import shutil
        shutil.rmtree(folder, ignore_errors=True)


def behaviour(arguments, template: Path) -> dict:
    """{script: lines} of the scripts whose results differ between the engines"""
    scripts = [script for script in SCRIPTS if re.search(arguments.scripts, script[0])]
    tasks = [(script, engine) for script in scripts for engine in ("classic", "new")]
    results = {}

    def one(task):
        script, engine = task
        return task, behaviour_run(arguments.build, script, engine, arguments.data, template, arguments.timeout)

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(arguments.jobs, len(tasks)))) as pool:
        for (script, engine), result in pool.map(one, tasks):
            results[(script[0], engine)] = result
    differences = {}
    for script in scripts:
        a, b = results[(script[0], "classic")], results[(script[0], "new")]
        lines = []
        if not a["writes"] and not b["writes"] and script[0] not in ("toggle-and-cancel", "mouse-and-cancel-button"):
            lines.append("nothing written by either engine: the script did not reach its rows\n" + a["log"][-800:])
        for key in ("config", "profile", "writes"):
            if a[key] != b[key]:
                lines.append(f"{key}: {json.dumps(a[key])[:1500]}\n      | {json.dumps(b[key])[:1500]}")
        lines += compare_layouts(script[1], a["layout"], b["layout"])
        if lines:
            differences[script[0]] = lines
        print(f"menu_parity: behaviour {script[0]}: {'differs' if lines else 'the same'} "
              f"({len(a['writes'])} settings written: {', '.join(a['writes'])[:300]})", file=sys.stderr, flush=True)
    return differences


def compare_results(results: dict, screens: list, variant_names: list, arguments) -> dict:
    """{"<variant> <screen>": lines} of the screens whose layouts or frames differ between the engines"""
    differences = {}
    for name in variant_names:
        classic, new = results.get((name, "classic"), {}), results.get((name, "new"), {})
        for screen in screens:
            a, b = classic.get(screen.name), new.get(screen.name)
            if isinstance(a, dict) or isinstance(b, dict):
                lines = [f"run failed: {(a if isinstance(a, dict) else b)['error'][:300]}"]
            else:
                lines = compare_layouts(screen.name, a[1], b[1]) if arguments.layout else []
                if arguments.pixels and not lines:
                    lines = compare_pictures(a[0], b[0])
                    if lines and is_deliberate_frame(screen.name, a[1], b[1]):
                        lines = []
                    if lines and arguments.show:
                        # (the pair, to look at: in a folder of the caller's, deleted by them)
                        arguments.show.mkdir(parents=True, exist_ok=True)
                        stem = arguments.show / f"{name}-{screen.slug}"
                        a[0].save(f"{stem}-classic.png")
                        b[0].save(f"{stem}-new.png")
            if lines:
                differences[f"{name} {screen.name}"] = lines
    return differences


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--layout", action="store_true", help="compare the screens' layouts (debug.menu_layout)")
    parser.add_argument("--pixels", action="store_true", help="compare the screens' frames, pixel for pixel")
    parser.add_argument("--behaviour", action="store_true",
                        help="run the scripts of presses on each engine and compare what they leave")
    parser.add_argument("--scripts", default="", help="only the scripts whose name has this in it (a regex)")
    parser.add_argument("--build", type=Path, default=ROOT / "build" / ("macos" if sys.platform == "darwin" else
                                                                         "linux_x86_64"))
    parser.add_argument("--data", type=Path, default=Path.home() / "dev" / "halo-macos-data" / "maps",
                        help="the maps folder")
    parser.add_argument("--saves", type=Path, default=Path.home() / ".cache" / "openh1-bench" / "saves" / "z",
                        help="a map cache whose files each run starts from (cloned)")
    parser.add_argument("--screens", default="", help="only the screens whose name has this in it (a regex)")
    parser.add_argument("--variants", default="all", help=f"some of {', '.join(VARIANTS)}, separated by commas")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--out", type=Path, help="the differences written as JSON here too")
    parser.add_argument("--baseline", action="store_true", help="the classic engine on both sides")
    parser.add_argument("--show", type=Path, help="a scratch folder each differing pair of frames is saved in, to "
                        "look at (the game's imagery: delete it after)")
    arguments = parser.parse_args(argv)
    if arguments.baseline:
        ENGINES["new"] = "classic"
    if not arguments.layout and not arguments.pixels and not arguments.behaviour:
        parser.error("say what to compare: --layout, --pixels, --behaviour")
    template = arguments.saves if arguments.saves.is_dir() else None
    failed = 0
    if arguments.behaviour:
        differences = behaviour(arguments, template)
        for key, lines in sorted(differences.items()):
            print(f"DIFFERS behaviour {key}")
            for line in lines:
                print(f"    {line}")
        print(f"menu_parity: behaviour: {sum(1 for script in SCRIPTS if re.search(arguments.scripts, script[0])) - len(differences)} "
              f"scripts the same, {len(differences)} differ")
        failed = 1 if differences else 0
        if not arguments.layout and not arguments.pixels:
            return failed
    if arguments.pixels:
        # (the frames as drawn, compared exactly: not scaled down as the gallery's are)
        menu_gallery.read_shot = raw_shot
    variant_names = list(VARIANTS) if arguments.variants == "all" else \
        [name.strip() for name in arguments.variants.split(",") if name.strip()]
    for name in variant_names:
        if name not in VARIANTS:
            parser.error(f"no variant {name} ({', '.join(VARIANTS)})")
    screens = [screen for screen in menu_gallery.enumerate_screens(ROOT / menu_gallery.MENUS)
               if re.search(arguments.screens, screen.name)]
    results = run_layouts(arguments.build, screens, variant_names, arguments.data, template, arguments.jobs,
                          arguments.timeout, arguments.pixels)
    differences = compare_results(results, screens, variant_names, arguments)
    if arguments.pixels and differences:
        # (a frame that differs is drawn again, one run at a time on each side: what the textures still
        # streaming in when many runs share the computer leaves out of a frame is not the engine's)
        again = {}
        for key in differences:
            name, screen_name = key.split(" ", 1)
            again.setdefault(name, []).append(next(screen for screen in screens if screen.name == screen_name))
        print(f"menu_parity: {len(differences)} differ; drawn again one run at a time", file=sys.stderr, flush=True)
        for name, part in again.items():
            redone = run_layouts(arguments.build, part, [name], arguments.data, template, 2, arguments.timeout, True)
            confirmed = compare_results(redone, part, [name], arguments)
            for screen in part:
                key = f"{name} {screen.name}"
                if key not in confirmed:
                    del differences[key]
                else:
                    differences[key] = confirmed[key]
    same = len(screens) * len(variant_names) - len(differences)
    for key, lines in sorted(differences.items()):
        print(f"DIFFERS {key}")
        for line in lines:
            print(f"    {line}")
    print(f"menu_parity: {' and '.join(kind for kind, on in (('layout', arguments.layout), ('pixels', arguments.pixels)) if on)}: "
          f"{same} the same, {len(differences)} differ "
          f"({len(screens)} screens x {len(variant_names)} variants)")
    if arguments.out:
        arguments.out.write_text(json.dumps(differences, indent=1))
    return 1 if differences or failed else 0


if __name__ == "__main__":
    sys.exit(main())
