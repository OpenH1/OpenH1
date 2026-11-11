#!/usr/bin/env python3
"""The menu engine's parity with the game's widgets (docs/menu_engine.md), proved by machine: every generated
screen run on each engine (display.menu_engine "classic" and "new"), in each aspect ratio and with each input,
and what each draws compared.

    python tools/menu_parity.py --layout              # every screen's debug.menu_layout, both engines, diffed
    python tools/menu_parity.py --layout --screens settings --variants 16x9-keyboard
    python tools/menu_parity.py --behaviour --sweep   # the scripts of presses, and every screen's navigation

--layout runs tools/menu_gallery.py's tour (debug.menu_tour: each screen opened alone, hidden, muted, no renderer,
from the player profile the behaviour scripts start from, the game's functions that open it run first:
menu_gallery.TOUR_SETUP) once on each engine for each variant, and compares each screen's layout
(port/linux/game/menu_layout.c: every widget's box and clip, every string's text, box, clip and extent, every
button's glyph and every pointer target), item by item. A screen with no layout on either side (it closed at once,
or another was drawn) is not the same but NOT COMPARED, which fails it, but for the name screens the game shows
only under the virtual keyboard (menu_gallery.UNDER_KEYBOARD), whose frames --pixels compares. The variants are the
four aspect ratios (4:3, 16:10, 16:9, 21:9) with each input (a controller, the keyboard and mouse, touch).

--behaviour runs scripts of presses (debug.test_input "menu:ready ...", SCRIPTS) on each engine from the same
config.toml and player profile and compares where the focus was after each press, config.toml, the profile's files,
the settings written and the last screen's layout; --sweep adds every generated screen's navigation (SWEEP).

The new engine draws each widget of the generated screens from its look in the model (model/look:
tools/menu_look.py), never its widget definition; a widget it draws from its definition is logged ("menu engine:
look from the widget definition"), which fails it too. --look-control proves that this is noticed: a model with a
widget's look taken out must log it.

The runs reuse one map cache (a clone of --saves's, by default ~/.cache/openh1-bench/saves/z: APFS clones on a
Mac, so nothing is unpacked again), each with its own data and save folders, deleted afterwards. A difference
not listed in DELIBERATE fails it (exit status 1).
"""

import argparse
import concurrent.futures
import json
import os
import re
import shutil
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


# (the places an item has, which the browsers' tall layout moves: menu_layout.c's rectangles and a text's scale)
PLACE_KEYS = {"box", "clip", "extent", "whole", "wide", "scale"}


DELIBERATE = [("profile_settings", _shows_menu_engine)]

# the screens the new engine lays out tall (the server browser's and PRO 2V2's browser's: tools/port_settings.py's
# BROWSER_TALL), whose items may differ in their places, their titles words in place of pictures
TALL_SCREENS = re.compile(r"join_game/join_game_screen$|doubles/browser/doubles_browser_screen$")
# (their titles' widgets: tools/port_settings.py's BROWSER_TITLE_NAMES)
TALL_TITLES = re.compile(r"\\(header_lan|header_direct_link|header_server_browser|header_doubles_browser)$")


def tall_items(items: list) -> list:
    """a tall screen's items as the tall layout may not change them: without their places (PLACE_KEYS), their
    titles (a picture's widget on one side, words on the other) and a text's line breaks (a wider box wraps it
    elsewhere)"""
    kept = []
    for item in items:
        if TALL_TITLES.search(str(item.get("path", item.get("of", "")))):
            continue
        kept.append({key: " ".join(value.split()) if key == "text" and isinstance(value, str) else value
                     for key, value in item.items() if key not in PLACE_KEYS})
    return kept


def is_deliberate_frame(screen: str, classic: dict, new: dict) -> bool:
    """whether two frames' difference is a deliberate one: their layouts differ only so (the frames of a
    screen whose layouts are the same must be the same)"""
    if classic is None or new is None:
        return False
    a, b = layout_items(classic), layout_items(new)
    if TALL_SCREENS.search(screen):
        return a != b and tall_items(a) == tall_items(b)
    return len(a) == len(b) and a != b and all(
        left == right or any(re.search(pattern, screen) and meant(left, right) for pattern, meant in DELIBERATE)
        for left, right in zip(a, b))


# the engine each side runs (--baseline: the classic on both, to see what differs between two runs of one)
ENGINES = {}

# what the new engine logs for a widget it drew from its widget definition, not its look in the model
# (port/linux/game/menu_tags.c's look_missing)
LOOK_FROM_DEFINITION = "menu engine: look from the widget definition"


def looks_from_definitions(logs: list) -> list:
    """the widgets the new engine drew from their widget definitions, by the runs' logs"""
    return sorted({line.split(LOOK_FROM_DEFINITION + ": ", 1)[1].strip() for log in logs for line in log.splitlines()
                   if LOOK_FROM_DEFINITION in line})


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


def compare_layouts(screen: str, classic: dict, new: dict, opened: bool = False) -> list:
    """the differences of two layouts of a screen, as lines (none: the same). No layout on either side is not the
    same but a screen not compared, a failure too, but for a screen the game shows only under the keyboard
    (menu_gallery.UNDER_KEYBOARD: its frames are compared); opened: the layouts must be the screen's own, not
    another's it left for"""
    if classic is None or new is None:
        if classic is None and new is None:
            return [] if menu_gallery.UNDER_KEYBOARD.search(screen) else \
                ["NOT COMPARED: no layout on either engine (the screen closed, or was never drawn)"]
        return [f"layout written by {'classic' if classic is not None else 'new'} only"]
    lines = []
    if opened:
        for engine, layout in (("classic", classic), ("new", new)):
            drawn = str(layout.get("screen_name", "")).replace("\\", "/")
            if not drawn.endswith(screen):
                lines.append(f"NOT COMPARED: {engine} drew {drawn or 'no screen'}, not {screen}")
        if lines:
            return lines
    for key in ("screen_name", "screen", "prompts", "touch_menus"):
        if classic.get(key) != new.get(key):
            lines.append(f"{key}: {classic.get(key)!r} | {new.get(key)!r}")
    a, b = layout_items(classic), layout_items(new)
    if TALL_SCREENS.search(screen):
        a, b = tall_items(a), tall_items(b)
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
    """the differences of two frames of a screen, as lines (none: the same pixels; no frame on either side: not
    compared, which fails)"""
    if classic is None or new is None:
        return ["NOT COMPARED: no frame from either engine"] if classic is None and new is None else \
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
                timeout: float, pictures: bool = False, logs: dict = None, profile: Path = None) -> dict:
    """{(variant, engine): {screen: (picture, layout)}}, each variant's screens split over parts run at once
    (pictures: each screen's frame too, read from the run's folder into memory, which is then deleted; logs:
    {engine: [each run's log]} filled in; profile: the player profiles each run starts with, MAKE_PROFILE's)"""
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
                                      False, timeout + len(part), template,
                                      logs.setdefault(engine, []) if logs is not None else None, profile)
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
# (the setup screens': the gametype editor's and Server Setup's)
MS = "main_menu/settings_select/multiplayer_setup"
MT = "main_menu/multiplayer_type_select"
# (the 4:3 window's fractions of the buttons of a Settings tab or page: the menus' 640 x 480 fill it)
OK_CLICK, CANCEL_CLICK, DEFAULTS_CLICK = "click:0.659:0.889", "click:0.859:0.889", "click:0.138:0.889"
# each script: its name, the screen it starts on (debug.menu_open) and its presses (debug.test_input
# "menu:ready ...", one a second of the frames' clock from when the screen is up: port/linux/src/xinput_sdl.c,
# port/linux/game/menu_tour.c)
SCRIPTS = [
    ("spinners", f"{PE}/audio_settings/audio_settings_screen",
     ["down", "right", "down", "right", "down", "right", "down", "down", "down", "right", "right", "left",
      OK_CLICK]),
    ("toggle-and-cancel", f"{PE}/audio_settings/audio_settings_screen",
     ["down", "down", "right", "down", "down", "right", "b"]),
    ("slider-keys-and-typed", f"{PE}/video_settings/video_settings_screen",
     ["down", "down", "down", "right", "right", "left", "down", "up", "a", "type:118", "enter",
      OK_CLICK]),
    ("sub-page-cancel", f"{PE}/video_settings/video_settings_screen",
     ["down", "down", "down", "down", "a", "wait", "down", "right", "down", "right", "b", "wait", OK_CLICK]),
    ("sub-page-keep", f"{PE}/video_settings/video_settings_screen",
     ["down", "down", "down", "down", "a", "wait", "down", "right", "right", OK_CLICK, "wait", "down", "right",
      OK_CLICK]),
    ("tabs", f"{PE}/audio_settings/audio_settings_screen",
     ["rb", "wait", "down", "right", "rb", "wait", "rb", "wait", "down", "right", "lb", "wait", "lb", "wait",
      "down", "right", OK_CLICK]),
    ("defaults", f"{PE}/audio_settings/audio_settings_screen",
     ["down", "right", DEFAULTS_CLICK, "wait", OK_CLICK]),
    # (a binding taken from the scripted mouse's events, which the binding's capture reads: its middle button,
    # the wheel turned)
    ("key-binding", f"{PE}/controls_setup/controls_settings_screen",
     ["down", "down", "a", "wait", "mouse:2", "wait", "down", "right", "a", "wait", "wheel:1", "wait", "up", "up",
      OK_CLICK]),
    ("mouse-and-cancel-button", f"{PE}/mouse_settings/mouse_settings_screen",
     ["down", "right", "right", "down", "right", "a", "type:3", "enter", CANCEL_CLICK]),
    ("profile-tab", f"{PE}/profile_settings/profile_settings_screen",
     ["down", "right", "right", "down", "down", "right", "up", OK_CLICK]),
    # (the 4:3 window's fractions of FIELD OF VIEW's track, MOVIES' spinner and the AUDIO tab)
    ("clicks-and-wheel", f"{PE}/video_settings/video_settings_screen",
     ["click:0.406:0.410", "click:0.53:0.498", "down", "down", "down", "wheel:1", "wheel:-3", "click:0.230:0.181",
      "wait", "down", "right", OK_CLICK]),
    # (sliders without stops, Defaults over a page's sliders, and stops that write a word of the setting's:
    # VEHICLE FIELD OF VIEW's DEFAULT and MATCH)
    ("slider-plain", f"{PE}/mouse_settings/mouse_settings_screen",
     ["right", "right", "left", "down", "up", "right", OK_CLICK]),
    ("slider-defaults", f"{PE}/video_settings/video_settings_screen",
     ["down", "down", "down", "right", "right", DEFAULTS_CLICK, "wait", "down", "down", "down", "left", OK_CLICK]),
    ("slider-words", f"{PE}/video_settings/video_settings_screen",
     ["down", "down", "down", "down", "a", "wait", "right", "right", "left", OK_CLICK, "wait", OK_CLICK]),
    # (rows shown by the model's conditions on other rows' values: WINDOW's Resolution, Window Size and Refresh Rate,
    # VSYNC's MAX FPS, HIGH-RES HUD's BUTTON STYLE)
    ("conditions-window", f"{PE}/video_settings/video_settings_screen",
     ["right", "down", "up", "right", "down", "down", "up", "up", "right", "down", "up", "left", OK_CLICK]),
    ("conditions-vsync", f"{PE}/video_settings/frame_rate/frame_rate_screen",
     ["right", "down", "down", "down", "down", "up", "up", "up", "up", "left", "down", "down", "down", "right",
      OK_CLICK]),
    ("conditions-hud", f"{PE}/interface_settings/interface_settings_screen",
     ["right", "down", "down", "up", "up", "left", "down", "right", OK_CLICK]),
    # (the setup screens' rows shown by their nodes' conditions in the model, on other rows' values: Item Options'
    # LOADOUT's weapons, OpenCE Options' MODE's rows, co-op's extras' amounts)
    ("setup-loadout", f"{MS}/item_options_edit/item_options_screen",
     ["down", "down", "down", "down", "right", "down", "down", "up", "up", "left", "down", "up", "right", "down"]),
    ("setup-opence", f"{MS}/opence_options_edit/opence_screen",
     ["right", "right", "down", "down", "down", "up", "up", "up", "right", "down", "down", "down", "up", "left"]),
    ("setup-coop", f"{MT}/server_settings/coop_options/coop_options_screen",
     ["down", "right", "down", "down", "down", "up", "up", "right", "down", "down", "up", "up", "up", "right",
      "down", "down", "right", "down"]),
    # (the host's own settings on a setup screen, their setting and values its nodes' in the model: Server Setup's
    # VOICE AND VOTING, changed and kept by its OK)
    ("setup-voice", f"{MT}/server_settings/voice_options/voice_options_screen",
     ["right", "down", "right", "down", "down", "right", "down", "down", "down", "down", "a"]),
    ("held-repeat", f"{PE}/video_settings/video_settings_screen",
     ["down", "down", "down", "hold:right", "hold:right", "wait", "hold:left", "wait", "hold:down", "hold:down",
      "hold:down", "wait", "hold:up", "wait", OK_CLICK]),
    ("controller", f"{PE}/audio_settings/audio_settings_screen",
     ["down", "right", "down", "down", "right", "rb", "wait", "down", "right", "lb", "wait", "up", "up", "right",
      "a"], "4x3-controller"),
    ("touch", f"{PE}/video_settings/video_settings_screen",
     ["click:0.406:0.410", "click:0.53:0.498", "down", "right", "click:0.230:0.181", "wait", OK_CLICK],
     "4x3-touch"),
]
# the script that makes the player profile every other run starts from (its text field typed into): run on the
# classic engine first, its saves kept and copied into each run, and itself compared like the others
MAKE_PROFILE = ("profile-create", f"{PE}/profile_settings/profile_settings_screen",
                ["a", "wait", "type:Tester", "enter", "wait", "wait"])
# (scripts that write nothing: the presses are taken back, or only move the focus)
# (and the setup screens', whose settings' writes the log does not name: config.toml is compared)
WRITE_NOTHING = ("toggle-and-cancel", "mouse-and-cancel-button", "sub-page-cancel", "setup-loadout", "setup-opence",
                 "setup-coop", "setup-voice")
# the navigation sweep: every generated screen opened, the focus moved about it with no press that leaves it
SWEEP = ["down", "down", "down", "down", "up", "up", "right", "down", "left", "hold:down", "hold:down", "up",
         "hold:up", "hold:up", "down", "right", "right", "left"]
BEHAVIOUR_VARIANT = "4x3-keyboard"


# (screens the sweep leaves out: a name's, which debug.menu_open does not bring up alone, its run never reaching
# the menus on either engine; profile-create types into a name's field)
SWEEP_SKIP = re.compile(r"/name_edit/")


def sweep_scripts(vr: bool = False) -> list:
    """the navigation sweep's scripts, one a screen (VR's too with vr)"""
    return [(f"sweep:{screen.name}", menu_gallery.tour_line(screen.name), SWEEP)
            for screen in menu_gallery.enumerate_screens(ROOT / menu_gallery.MENUS, vr)
            if not SWEEP_SKIP.search(screen.name)]


# the host's warning of a stalled thread (port/android/host/host_stall.c), which a busy computer's runs print, written
# into the middle of the game's line of the focus: not the game's
HOST_STALL = re.compile(r"halo-host: warning: stall: .*$")


def focus_path(text: str) -> list:
    """where the focus was after each second of a run's script (menu_tour.c's "menus: focus" lines)"""
    return [HOST_STALL.sub("", line.split("menus: focus ", 1)[1]) for line in text.splitlines()
            if "menus: focus " in line]


def profile_digests(saves: Path) -> dict:
    """the player profiles' files' digests, by their place (the profile's folder, named at random as it is
    made, and its friend key, made at random, left out)"""
    import hashlib
    found = {}
    for path in sorted((saves / "u").rglob("*")) if (saves / "u").is_dir() else []:
        relative = path.relative_to(saves).as_posix()
        if path.is_file() and path.name != "friends.txt":
            found[re.sub(r"^u/UDATA/[^/]+/", "u/UDATA/*/", relative)] = hashlib.sha256(path.read_bytes()).hexdigest()
    return found


def behaviour_run(build: Path, script: tuple, engine: str, maps: Path, template: Path, timeout: float,
                  profile: Path = None, keep: Path = None) -> dict:
    """one script on one engine: {"config": config.toml's lines, "profile": the saved profiles' files'
    digests, "writes": the settings the log says were written, "focus": where the focus was after each press,
    "layout": the last screen's layout, "done": whether the script ran to its end, "log"} (profile: a save
    folder's player profiles the run starts with; keep: its own copied there afterwards)"""
    import shutil
    import signal
    import subprocess
    import tempfile
    name, screen, presses = script[:3]
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
        variant = engine_variant(VARIANTS[script[3] if len(script) > 3 and script[3] else BEHAVIOUR_VARIANT],
                                 ENGINES.get(engine, engine))
        environment = menu_gallery.run_environment(variant, folder, timeout, True)
        environment.pop("HALO_MENU_TOUR", None)
        presses = list(presses) + ["wait"] * 2
        environment.update({
            "HALO_MENU_OPEN": screen, "HALO_TEST_INPUT": "menu:ready " + " ".join(presses),
            "HALO_EXIT_AFTER": str(int(timeout)), "HALO_MENU_LAYOUT": "layout.json",
        })
        if template:
            menu_gallery.map_cache_clone(template, folder / "saves" / "z")
        if profile:
            shutil.copytree(profile, folder / "saves" / "u", dirs_exist_ok=True)
        (folder / "data" / "maps").symlink_to(maps.resolve())
        if sys.platform != "darwin":
            environment["SDL_VIDEO_DRIVER"] = "offscreen"
        with open(folder / "halo.log", "w") as log:
            process = subprocess.Popen([str(game / "halo")], cwd=game, env=environment, stdin=subprocess.DEVNULL,
                                       stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        text = (folder / "halo.log").read_text(errors="replace")
        # (the run's config.toml, beside its executable: never the computer's own)
        config = (game / "config.toml").read_text(errors="replace").splitlines() \
            if (game / "config.toml").is_file() else []
        profiles = profile_digests(folder / "saves")
        if keep and (folder / "saves" / "u").is_dir():
            shutil.copytree(folder / "saves" / "u", keep, dirs_exist_ok=True)
        layout = None
        if (folder / "data" / "layout.json").is_file():
            try:
                layout = json.loads((folder / "data" / "layout.json").read_text(errors="replace"))
            except json.JSONDecodeError:
                layout = None
        writes = [line.split("menus: ", 1)[1] for line in text.splitlines()
                  if "menus: " in line and " = " in line and "could not" not in line and "menus: focus " not in line]
        return {"config": config, "profile": profiles, "writes": writes, "focus": focus_path(text), "layout": layout,
                "done": "debug.test_input: done" in text, "log": text[-3000:]}
    finally:
        if process and process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
        shutil.rmtree(folder, ignore_errors=True)


def make_profile(arguments, template: Path) -> tuple:
    """(a folder of the player profile the runs start from, made by a script on the classic engine: the same for
    both engines; the script's result), the folder deleted by the caller"""
    import shutil
    import tempfile
    profile = Path(tempfile.mkdtemp(prefix="menu-parity-profile-"))
    made = behaviour_run(arguments.build, MAKE_PROFILE, "classic", arguments.data, template, arguments.timeout,
                         keep=profile)
    if not made["profile"]:
        shutil.rmtree(profile, ignore_errors=True)
        raise SystemExit("menu_parity: no player profile made for the runs:\n" + made["log"][-800:])
    return profile, made


def behaviour(arguments, template: Path) -> dict:
    """{script: lines} of the scripts whose results differ between the engines"""
    scripts = [script for script in [MAKE_PROFILE] + SCRIPTS + (sweep_scripts(arguments.vr) if arguments.sweep else [])
               if re.search(arguments.scripts, script[0])]
    import shutil
    tasks = [(script, engine) for script in scripts for engine in ("classic", "new")]
    results = {}
    profile, made = make_profile(arguments, template)

    def one(task):
        script, engine = task
        # (--against: the classic side another build's, the one before a change both engines run)
        build = arguments.against if arguments.against and engine == "classic" else arguments.build
        result = behaviour_run(build, script, engine, arguments.data, template, arguments.timeout,
                               None if script is MAKE_PROFILE else profile)
        if not result["done"]:
            # (once more: a run that did not start, or was starved by the others, is not the engine's)
            result = behaviour_run(build, script, engine, arguments.data, template, arguments.timeout,
                                   None if script is MAKE_PROFILE else profile)
        return task, result

    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(arguments.jobs, len(tasks)))) as pool:
            for (script, engine), result in pool.map(one, tasks):
                results[(script[0], engine)] = result
    finally:
        shutil.rmtree(profile, ignore_errors=True)
    differences = {}
    for script in scripts:
        a, b = results[(script[0], "classic")], results[(script[0], "new")]
        lines = []
        for engine, result in (("classic", a), ("new", b)):
            if not result["done"]:
                lines.append(f"{engine}: the script did not finish\n" + result["log"][-800:])
        if not a["writes"] and a["profile"] == (made["profile"] if script is not MAKE_PROFILE else {}) and not script[0].startswith("sweep:") and \
                script[0] not in WRITE_NOTHING:
            lines.append("nothing written by the classic engine: the script did not reach its rows\n" +
                         "\n".join(a["focus"])[-1500:])
        for key in ("focus", "config", "profile", "writes"):
            if a[key] != b[key]:
                lines.append(f"{key}: {json.dumps(a[key])[:1500]}\n      | {json.dumps(b[key])[:1500]}")
        lines += compare_layouts(script[1].split("\t")[0], a["layout"], b["layout"])
        if lines:
            differences[script[0]] = lines
        print(f"menu_parity: behaviour {script[0]}: {'differs' if lines else 'the same'} "
              f"({len(a['focus'])} steps, {len(a['writes'])} settings written: "
              f"{', '.join(a['writes'])[:300]})", file=sys.stderr, flush=True)
    return differences, len(scripts)


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
                lines = compare_layouts(screen.name, a[1], b[1], opened=True) if arguments.layout else []
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


def look_control(build: Path, screens: list, maps: Path, template: Path, timeout: float) -> int:
    """the new engine run on the first screen with its own look taken out of the model (a menus folder's file
    in place of its look's): 0 if the run logs that it drew it from its definition"""
    import shutil
    import tempfile
    screen = screens[0]
    folder = Path(tempfile.mkdtemp(prefix="menu-parity-look-"))
    try:
        found = None
        for path in sorted((ROOT / menu_gallery.MENUS.parent / "model" / "look").glob("*.xml")):
            text = path.read_text()
            start = text.find(f'\t<look name="{screen.name}"')
            if start >= 0:
                end = text.find("\n\t<look ", start + 1)
                (folder / "model" / "look").mkdir(parents=True)
                (folder / "model" / "look" / path.name).write_text(text[:start] + text[end + 1:])
                found = path.name
                break
        if not found:
            print(f"menu_parity: the model has no look of {screen.name}")
            return 1
        logs = []
        menu_gallery.tour(build, [screen], engine_variant(VARIANTS["16x9-keyboard"], "new"), maps, folder, False,
                          False, timeout, template, logs)
        noticed = looks_from_definitions(logs)
        print(f"menu_parity: --look-control: {screen.name}'s look taken out of {found}; "
              f"{'noticed: ' + ', '.join(noticed) if noticed else 'NOT noticed'}")
        return 0 if any(item.startswith(screen.name + " ") for item in noticed) else 1
    finally:
        shutil.rmtree(folder, ignore_errors=True)


def main(argv=None) -> int:
    import signal
    # (stopped, it leaves as a raised exit, so that each run's folder, its own clone of the map cache, is deleted as
    # its run ends: a signal's default left them behind, gigabytes each)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--layout", action="store_true", help="compare the screens' layouts (debug.menu_layout)")
    parser.add_argument("--pixels", action="store_true", help="compare the screens' frames, pixel for pixel")
    parser.add_argument("--behaviour", action="store_true",
                        help="run the scripts of presses on each engine and compare what they leave")
    parser.add_argument("--scripts", default="", help="only the scripts whose name has this in it (a regex)")
    parser.add_argument("--sweep", action="store_true",
                        help="with --behaviour, the navigation sweep of every generated screen too")
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
    parser.add_argument("--against", type=Path,
                        help="with --behaviour, the classic side run from this build (a copy of the one before a "
                        "change that both engines run, as the model's sliders and conditions are): before | after")
    parser.add_argument("--show", type=Path, help="a scratch folder each differing pair of frames is saved in, to "
                        "look at (the game's imagery: delete it after)")
    parser.add_argument("--vr", action="store_true", help="VR's settings screens too (drawn flat, as the gallery's)")
    parser.add_argument("--look-control", action="store_true", help="prove that a widget drawn from its definition is "
                        "noticed: the new engine run with one widget's look taken out of the model")
    arguments = parser.parse_args(argv)
    if arguments.look_control:
        arguments.layout = True
    if arguments.baseline:
        ENGINES["new"] = "classic"
    if not arguments.layout and not arguments.pixels and not arguments.behaviour:
        parser.error("say what to compare: --layout, --pixels, --behaviour")
    template = arguments.saves if arguments.saves.is_dir() else None
    failed = 0
    if arguments.behaviour:
        differences, count = behaviour(arguments, template)
        for key, lines in sorted(differences.items()):
            print(f"DIFFERS behaviour {key}")
            for line in lines:
                print(f"    {line}")
        print(f"menu_parity: behaviour: {count - len(differences)} scripts the same, {len(differences)} differ")
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
    screens = [screen for screen in menu_gallery.enumerate_screens(ROOT / menu_gallery.MENUS, arguments.vr)
               if re.search(arguments.screens, screen.name)]
    if arguments.look_control:
        return look_control(arguments.build, screens, arguments.data, template, arguments.timeout)
    logs = {}
    # (the player profile each run starts with, as the behaviour scripts do: the screens that close at once with no
    # profile being edited, the profile's hub among them, drawn)
    profile, _ = make_profile(arguments, template)
    try:
        results = run_layouts(arguments.build, screens, variant_names, arguments.data, template, arguments.jobs,
                              arguments.timeout, arguments.pixels, logs, profile)
        differences = compare_results(results, screens, variant_names, arguments)
        again_one_at_a_time(arguments, differences, screens, template, profile)
    finally:
        shutil.rmtree(profile, ignore_errors=True)
    not_compared = sum(1 for lines in differences.values() if any(line.startswith("NOT COMPARED") for line in lines))
    under_keyboard = [screen.name for screen in screens if menu_gallery.UNDER_KEYBOARD.search(screen.name)]
    same = len(screens) * len(variant_names) - len(differences)
    for key, lines in sorted(differences.items()):
        print(f"DIFFERS {key}")
        for line in lines:
            print(f"    {line}")
    print(f"menu_parity: {' and '.join(kind for kind, on in (('layout', arguments.layout), ('pixels', arguments.pixels)) if on)}: "
          f"{same} the same, {len(differences) - not_compared} differ, {not_compared} not compared "
          f"({len(screens)} screens x {len(variant_names)} variants)")
    if under_keyboard:
        print(f"menu_parity: {len(under_keyboard)} screens shown only under the keyboard, which writes no layout"
              f"{': their frames compared' if arguments.pixels else ' (--pixels compares their frames)'}: "
              f"{', '.join(under_keyboard)}")
    from_definitions = [] if arguments.baseline else looks_from_definitions(logs.get("new", []))
    for widget in from_definitions:
        print(f"LOOK FROM ITS DEFINITION {widget}")
    print(f"menu_parity: the new engine drew {len(from_definitions)} widgets from their definitions, not the model")
    if arguments.out:
        arguments.out.write_text(json.dumps(differences, indent=1))
    return 1 if differences or failed or from_definitions else 0


def again_one_at_a_time(arguments, differences: dict, screens: list, template: Path, profile: Path):
    """the frames that differ drawn again, one run at a time on each side, and differences that do not recur
    dropped (what the textures still streaming in when many runs share the computer leaves out of a frame is not
    the engine's); the screens not compared are not drawn again"""
    if arguments.pixels and differences:
        again = {}
        for key, lines in differences.items():
            if any(line.startswith("NOT COMPARED") for line in lines):
                continue
            name, screen_name = key.split(" ", 1)
            again.setdefault(name, []).append(next(screen for screen in screens if screen.name == screen_name))
        print(f"menu_parity: {len(differences)} differ; drawn again one run at a time", file=sys.stderr, flush=True)
        for name, part in again.items():
            redone = run_layouts(arguments.build, part, [name], arguments.data, template, 2, arguments.timeout, True,
                                 None, profile)
            confirmed = compare_results(redone, part, [name], arguments)
            for screen in part:
                key = f"{name} {screen.name}"
                if key not in confirmed:
                    del differences[key]
                else:
                    differences[key] = confirmed[key]


if __name__ == "__main__":
    sys.exit(main())
