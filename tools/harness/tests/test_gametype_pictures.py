"""The gametypes' pictures (menu_functions.c's gametype_picture_frame): each engine's frame in the PC version's
order, which the gametype lists' pictures (bitmaps/game_type_grafix) and Game Options' (rules_options) share; and
the gametype editor's panel (playlist_edit_description), which shows the gametype's own picture on every row, in
place of the PC version's pictures of its rows, which have no redraw."""
import re
import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, ROOT, build, enum_with, function, mutated, read, run  # noqa: E402

PLAYLIST_EDIT = "main_menu/settings_select/multiplayer_setup/playlist_edit"
RULES_PICTURES = f"{PLAYLIST_EDIT}/game_type_select/rules_options"


def generated(fault=None):
    source = read("port/linux/game/menu_functions.c")
    defines = "\n".join(re.findall(r"^#define PLAYLIST_EDIT_\w+ \d+$", source, re.M))
    code = "\n".join([enum_with(read("source/game/game_engine.h"), "game_engine_none = 0"),
                      enum_with(source, "_gametype_picture_ctf = 0"), function(source, "gametype_picture_frame"),
                      defines, function(source, "playlist_edit_description"), ""])
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", ["frames", "editor"])
def test_case(case):
    status, output = run(build("gametype_pictures", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("fault,case", [
    # (the engine's number as the frame, as the gametype lists had it: Capture the Flag's showed King's crown)
    (("\tswitch (engine)\n", "\treturn (short)(engine < 0 || engine > 5 ? 5 : engine);\n\tswitch (engine)\n"),
     "frames"),
    # (the row's picture left, the PC version's)
    (("\tpicture->animation.current_frame_index = frame == _gametype_picture_unknown ? 0 : frame;\n", ""), "editor"),
    # (an unknown engine's "?" shown)
    (("\tvisible_set(picture, frame != _gametype_picture_unknown);\n", ""), "editor"),
])
def test_negative_control(fault, case):
    status, output = run(build("gametype_pictures", generated(fault)), case)
    assert status == CHECK_FAILED, output


def enum_members(text, first):
    body = enum_with(text, first)
    return re.findall(r"^\s*(_\w+)", body[body.index("{"):], re.M)


def test_frames_are_the_pc_versions_order():
    """the PC version's own order of the pictures (ui_widget_game_data_input_functions.c), member for member"""
    ours = enum_members(read("port/linux/game/menu_functions.c"), "_gametype_picture_ctf = 0")
    pc = enum_members(read("source/interface/ui_widget_game_data_input_functions.c"), "_multiplayer_game_bitmap_ctf = 0")
    assert [name.replace("_gametype_picture_", "") for name in ours] == \
        [name.replace("_multiplayer_game_bitmap_", "") for name in pc]


def menus(relative):
    return ElementTree.fromstring((ROOT / relative).read_bytes())


def test_game_options_pictures_are_in_that_order():
    """Game Options' list (its rows' pictures are rules_options' frames) names the game types in that order"""
    strings = next(element for element in menus("port/assets/menus/ce/strings.xml").iter("strings")
                   if element.get("name") == f"{PLAYLIST_EDIT}/game_type_select/gametype_select_list_options")
    assert [string.get("text") for string in strings][:5] == \
        ["CAPTURE THE FLAG", "KING OF THE HILL", "SLAYER", "ODDBALL", "RACE"]


def test_editor_panel_shows_the_gametypes_picture():
    """the editor's panel's picture is Game Options' pictures, each frame a redraw (not drawn empty)"""
    screen = menus(f"port/assets/menus/ce/{PLAYLIST_EDIT.replace('/', '.')}.xml")
    picture = next(widget for widget in screen.iter("widget")
                   if widget.get("name") == f"{PLAYLIST_EDIT}/playlist_edit_ext_desc_pic")
    assert picture.get("bitmap") == RULES_PICTURES
    bitmap = next(element for element in menus("port/assets/menus/ce/bitmaps.xml").iter("bitmap")
                  if element.get("name") == RULES_PICTURES)
    frames = [frame.get("png") for frame in bitmap.iter("frame")][:5]
    empty = read("port/assets/menus/NON_HANDDRAWN.md")
    assert len(frames) == 5 and all(frame and f"`{frame}`" not in empty for frame in frames), frames
