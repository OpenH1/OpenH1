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
    if not arguments.layout and not arguments.pixels:
        parser.error("say what to compare: --layout, --pixels")
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
    template = arguments.saves if arguments.saves.is_dir() else None
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
    return 1 if differences else 0


if __name__ == "__main__":
    sys.exit(main())
