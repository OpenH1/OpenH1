#!/usr/bin/env python3
"""The menu studio's notes (debug.menu_studio's F9: port/linux/game/menu_studio.c):
list them, mark them done, and print a brief to act on.

    python tools/menu_notes.py                  # the open notes, by screen
    python tools/menu_notes.py brief [ID...]    # what to change, where: a brief an agent can act on
    python tools/menu_notes.py done ID... [--commit SHA]   # addressed (by HEAD's commit unless given)
    python tools/menu_notes.py --all            # the done ones too

A note is a JSON line in the notes file (build/menu_notes.jsonl unless
--file): its id, status ("open" or "done", with the commit that addressed
it), the note, the screen and the widget (its tag's name, its path down the
screen, its words, its box in the menus' units), the input and the window's
shape it was seen in, and a PNG of the screen with the widget outlined
(build/menu_notes/). An ID may be given by its first characters. The notes
and their pictures stay in build/, out of git.

After a batch is fixed, `python tools/menu_gallery.py --compare <ref> --notes`
shows those screens alone, before | after, each with its notes.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import menu_gallery  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NOTES = ROOT / "build" / "menu_notes.jsonl"


def read_notes(path: Path) -> list:
    """the notes of a file, in its order (a line that is not JSON skipped)"""
    notes = []
    if not path.is_file():
        return notes
    for line in path.read_text(errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            notes.append(json.loads(line))
        except json.JSONDecodeError:
            print(f"menu_notes: a line of {path} is not JSON: {line[:80]}", file=sys.stderr)
    return notes


def write_notes(path: Path, notes: list):
    """the file written again whole (beside it, then renamed over it)"""
    part = path.with_suffix(path.suffix + ".part")
    part.write_text("".join(json.dumps(note, ensure_ascii=False) + "\n" for note in notes))
    part.replace(path)


def menu_name(tag: str) -> str:
    """a widget's tag name (pc\\main_menu\\x) as the menu files name it (main_menu/x)"""
    tag = tag or ""
    if tag.startswith("pc\\"):
        tag = tag[3:]
    return tag.replace("\\", "/")


def widget_files(menus: Path) -> dict:
    """each widget's file and the generator that writes it: {name: (file, generator)}"""
    found = {}
    for path in sorted(menus.glob("*.xml")):
        text = path.read_text(errors="replace")
        match = re.search(r"<!--[^>]*\((tools/[\w./-]+\.py)\)", text)
        generator = match.group(1) if match else ""
        for name in re.findall(r'<widget name="([^"]+)"', text):
            found.setdefault(name, (path.name, generator))
    return found


def generator_lines(generator: str, widget: str, limit: int = 3) -> list:
    """where a generator names a widget: its last part's lines (file:line), the first few"""
    if not generator or not widget:
        return []
    path = ROOT / generator
    if not path.is_file():
        return []
    leaf = widget.rsplit("/", 1)[-1]
    lines = []
    for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        if re.search(rf"\b{re.escape(leaf)}\b", line):
            lines.append(f"{generator}:{number}")
            if len(lines) == limit:
                break
    return lines


def pick(notes: list, ids: list) -> list:
    """the notes of the IDs given (each by its first characters; all the open ones for none)"""
    if not ids:
        return [note for note in notes if note.get("status") == "open"]
    chosen = []
    for prefix in ids:
        matches = [note for note in notes if str(note.get("id", "")).startswith(prefix)]
        if len(matches) != 1:
            raise SystemExit(f"menu_notes: {prefix!r} is {'no note' if not matches else 'more than one note'}")
        chosen.append(matches[0])
    return chosen


def by_screen(notes: list) -> dict:
    grouped = {}
    for note in notes:
        grouped.setdefault(note.get("screen", "?"), []).append(note)
    return grouped


def describe(note: dict) -> str:
    widget = menu_name(note.get("widget")) if note.get("widget") else "the screen as a whole"
    words = " ".join((note.get("widget_text") or "").split())
    return f"{widget}" + (f' ("{words[:60]}")' if words else "")


def list_notes(notes: list, everything: bool) -> str:
    shown = notes if everything else [note for note in notes if note.get("status") == "open"]
    if not shown:
        return "No open notes." if not everything else "No notes."
    lines = []
    for screen, group in by_screen(shown).items():
        lines.append(f"{screen}")
        for note in group:
            status = "" if note.get("status") == "open" else f" [done {str(note.get('commit', ''))[:9]}]"
            lines.append(f"  {note.get('id')}{status}  {describe(note)}: {note.get('note')}")
    return "\n".join(lines)


def brief(notes: list) -> str:
    """what an agent needs for each note: the screen and its generator, the widget and where the generator
    names it, the note, the input and shape, the picture; and the rules"""
    files = widget_files(ROOT / menu_gallery.MENUS)
    screens = {screen.name: screen for screen in menu_gallery.enumerate_screens(ROOT / menu_gallery.MENUS, True)}
    out = [f"{len(notes)} menu note{'s' if len(notes) != 1 else ''} to address. The menu files in "
           "port/assets/menus/ce are generated: change the generator named, run it (tools/port_settings.py "
           "runs alone; tools/ce_menus.py needs the PC tags, port/assets/menus/README.md), check `git status` "
           "shows only the screens meant to change, then `python tools/menu_gallery.py --check`. When done: "
           "`python tools/menu_notes.py done <id> --commit <sha>`, and review with "
           "`python tools/menu_gallery.py --compare <ref> --notes`.", ""]
    for number, note in enumerate(notes, 1):
        screen = note.get("screen", "?")
        widget = menu_name(note.get("widget")) if note.get("widget") else ""
        file, generator = files.get(widget or screen, ("", ""))
        if not file and screen in screens:
            file, generator = screens[screen].file, screens[screen].generator
        out.append(f"{number}. [{note.get('id')}] {note.get('note')}")
        out.append(f"   screen: {screen} ({file or '?'}, {generator or 'no generator named'})")
        if widget:
            out.append(f"   widget: {widget}")
            path = " > ".join(menu_name(part).rsplit("/", 1)[-1] for part in note.get("widget_path", []))
            if path:
                out.append(f"   its path: {path}")
            words = " ".join((note.get("widget_text") or "").split())
            if words:
                out.append(f'   its words: "{words}"')
            if note.get("box"):
                out.append(f"   its box (menu units, 640x480 centred): {note.get('box')}")
            where = generator_lines(generator, widget)
            if where:
                out.append(f"   the generator names it at: {', '.join(where)}")
        else:
            out.append("   on the screen as a whole")
        out.append(f"   seen with: {note.get('input', '?')} input, {note.get('shape', '?')} window")
        if note.get("picture"):
            out.append(f"   picture: {note.get('picture')}")
        out.append("")
    return "\n".join(out).rstrip() + "\n"


def mark_done(path: Path, notes: list, chosen: list, commit: str) -> int:
    ids = {note.get("id") for note in chosen}
    for note in notes:
        if note.get("id") in ids:
            note["status"] = "done"
            note["commit"] = commit
    write_notes(path, notes)
    return len(ids)


def note_screens(path: Path) -> list:
    """the screens notes are on (menu_gallery.py --notes's), in the file's order"""
    seen = []
    for note in read_notes(path):
        if note.get("screen") and note["screen"] not in seen:
            seen.append(note["screen"])
    return seen


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="The menu studio's notes.", allow_abbrev=False)
    parser.add_argument("action", nargs="?", default="list", choices=("list", "brief", "done"))
    parser.add_argument("ids", nargs="*", help="notes' IDs (their first characters)")
    parser.add_argument("--file", type=Path, default=NOTES, help="the notes file (build/menu_notes.jsonl)")
    parser.add_argument("--all", action="store_true", help="list the done notes too")
    parser.add_argument("--commit", help="done: the commit that addressed them (HEAD's)")
    arguments = parser.parse_args(argv)
    notes = read_notes(arguments.file)
    if arguments.action == "list":
        print(list_notes(notes, arguments.all))
        return 0
    if arguments.action == "brief":
        chosen = pick(notes, arguments.ids)
        print(brief(chosen) if chosen else "No open notes.")
        return 0
    if not arguments.ids:
        parser.error("done needs the notes' IDs")
    commit = arguments.commit or subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short=12", "HEAD"],
                                                capture_output=True, text=True).stdout.strip()
    count = mark_done(arguments.file, notes, pick(notes, arguments.ids), commit)
    print(f"menu_notes: {count} note{'s' if count != 1 else ''} done ({commit})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
