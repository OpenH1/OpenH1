"""The menu engine's model of the widgets' look (tools/menu_look.py): written again from ce/'s widgets it is the
same, byte for byte, as the files in port/assets/menus/model/look, and the same each time it is written, so that
the generators may write it alone in place of the widgets' XML (docs/menu_engine.md, step 5)."""

import json
from pathlib import Path

import menu_look

MENUS = Path(__file__).resolve().parent.parent / "port" / "assets" / "menus"


def test_look_files_are_the_widgets():
    made = menu_look.look_files(MENUS / "ce")
    written = {f"{menu_look.LOOK}/{path.name}": path.read_text() for path in (MENUS / menu_look.LOOK).glob("*.xml")}
    assert sorted(made) == sorted(written)
    for name, lines in made.items():
        assert "\n".join(lines) == written[name], f"{name} is not what tools/menu_look.py writes: run it"


def test_look_files_are_the_same_each_time():
    assert menu_look.look_files(MENUS / "ce") == menu_look.look_files(MENUS / "ce")


def test_look_files_are_embedded():
    listed = set(json.loads((MENUS / "menus.json").read_text())["files"])
    assert {name for name in menu_look.look_files(MENUS / "ce")} <= listed
