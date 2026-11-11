"""The settings pages' rows fit above the footer on every platform
(tools/port_settings.py's PAGES, packed as menu_functions.c's
settings_rows_pack packs them)."""
import re
from pathlib import Path

import pytest

from tools import port_settings
from tools.port_settings import (PAGES, PAGE_TOP, SECTION, SETTINGS_NOT_IN_VR, SETTINGS_ROW_HEIGHT,
                                 SETTINGS_ROW_STEP, SETTINGS_ROWS_BOTTOM, SETTINGS_SAME_PLACE, SETTINGS_SECTION_GAP,
                                 SETTINGS_SECTION_STEP, SETTINGS_TOP)

# the platforms each build's menus are for (menu_files.c's for_this_platform),
# every one the build may have at once: the desktop's (the VR build's in the
# headset, on a renderer that draws HDR; and out of it), Android's, iOS's
# (desktop's menus but not "computer"'s, for a touchscreen)
BUILDS = {
    "desktop": {"desktop", "computer", "pointer", "hdr", "vr"},
    "desktop out of VR": {"desktop", "computer", "pointer", "hdr"},
    "android": {"android", "touch", "hdr"},
    "ios": {"desktop", "touch", "hdr"},
}


def _key(entry):
    label, setting, _choices, _help, _platform, *named = entry
    return named[0] if named else setting.split(".", 1)[1] if setting else None


def _profile_row(entry):
    """a row hidden with no profile (its condition in the model: profile)"""
    return (entry[1] or "").startswith("profile.") or _key(entry) in ("name", "color", "profiles")


def _rows_bottom(page, platforms, profile):
    """where the last row shown ends, the rows packed as settings_rows_pack
    packs them: a step apart, a section's title over its rows (none for a
    section with no row shown), a gap above a section after rows"""
    y = SETTINGS_TOP if "tab" in page else PAGE_TOP
    bottom, rows_above, section, previous_shown = y, False, False, False
    for entry in page["rows"]:
        if entry[0] == SECTION:
            section, previous_shown = True, False
            continue
        shown = entry[4] is None or bool(set(entry[4].split()) & platforms)
        if _key(entry) == "create_profile":
            shown = shown and not profile
        elif not profile and _profile_row(entry):
            shown = False
        if "vr" in platforms and _key(entry) in SETTINGS_NOT_IN_VR:
            shown = False
        # (one of the rows in the same place at most: Resolution's or Window
        # Size, the desktop's anti-aliasing or Android's)
        if shown and previous_shown and (_key(entry) in SETTINGS_SAME_PLACE or entry[1] == "display.window_size"):
            continue
        previous_shown = shown
        if not shown:
            continue
        if section:
            y += (SETTINGS_SECTION_GAP if rows_above else 0) + SETTINGS_SECTION_STEP
            section = False
        bottom = y + SETTINGS_ROW_HEIGHT
        y += SETTINGS_ROW_STEP
        rows_above = True
    return bottom


@pytest.mark.parametrize("folder", sorted(PAGES))
def test_settings_rows_fit(folder):
    """no page's rows reach the footer's band, which they would be drawn
    over (the list does not scroll), for any build, with a profile or
    without"""
    page = PAGES[folder]
    for build, platforms in BUILDS.items():
        for profile in (True, False):
            bottom = _rows_bottom(page, platforms, profile)
            assert bottom <= SETTINGS_ROWS_BOTTOM, \
                f"{folder}'s rows end at {bottom} on {build}{'' if profile else ' with no profile'}, past " \
                f"{SETTINGS_ROWS_BOTTOM}: move a row to a page with room"


def test_rows_bottom_counts_a_page_that_overflows():
    """the check sees an overflow: a tab of many rows"""
    page = {"tab": "TEST", "rows": [(SECTION, "A")] + [(f"R{n}", f"x.r{n}", port_settings.ON_OFF, "", None)
                                                        for n in range(15)]}
    assert _rows_bottom(page, BUILDS["desktop"], True) == SETTINGS_TOP + SETTINGS_SECTION_STEP + \
        14 * SETTINGS_ROW_STEP + SETTINGS_ROW_HEIGHT
    assert _rows_bottom(page, BUILDS["desktop"], True) > SETTINGS_ROWS_BOTTOM


def _main_menu_items():
    """the main menu's items (the headset's VR TUTORIAL left out), in their
    order, with their places"""
    menu = (Path(__file__).resolve().parent.parent / "port/assets/menus/ce/main_menu.xml").read_text()
    select = menu.split('<widget name="main_menu/main_menu_select_list"', 1)[1].split("</widget>", 1)[0]
    return [(name, int(x), int(y)) for name, x, y, platform in
            re.findall(r'<child widget="main_menu/main_menu_item_(\w+)" x="(\d+)" y="(\d+)"( platform="\w+")?/>',
                       select) if not platform]


def test_main_menu_items():
    """the main menu is CAMPAIGN, MULTIPLAYER, 2V2, SETTINGS and QUIT (the
    profiles and the mods are Settings' PROFILES and MODS tabs'), a row apart
    with no gap"""
    items = _main_menu_items()
    assert [name for name, _, _ in items] == ["load_camp", "multiplayer", "doubles", "settings", "quit_game"]
    assert {after[2] - before[2] for before, after in zip(items, items[1:])} == {36}
    assert items[2][1:] == port_settings.DOUBLES_ITEM_PLACE
    assert items[3][1:] == port_settings.SETTINGS_ITEM_PLACE
    assert items[4][1:] == port_settings.QUIT_ITEM_PLACE


def test_profiles_tab_opens_profiles():
    """the PROFILES tab opens the profile manager (the main menu's Profile's
    was), beside its own rows"""
    page = PAGES["profile_settings"]
    assert page["tab"] == "PROFILES"
    links = {_key(entry): entry[2] for entry in page["rows"] if entry[0] != SECTION and not entry[1]}
    assert links["profiles"] == port_settings.PROFILE_MANAGER
    assert {"name", "color", "create_profile", "about"} <= set(links)
    assert "mods" not in links


def test_mods_tab():
    """MODS is a tab of its own (issue 598), the last, after PROFILES: it opens
    the Mods screen, and says where mods go, in a game too (MANAGE MODS is the
    main menu's)"""
    assert port_settings.SETTINGS_TAB_PAGES[-2:] == ["profile_settings", "mods_settings"]
    page = PAGES["mods_settings"]
    assert page["tab"] == "MODS"
    links = {_key(entry): entry[2] for entry in page["rows"] if entry[0] != SECTION and not entry[1]}
    assert links["mods"] == f"{port_settings.MODS}/mods_screen"
    assert "mods_folder" in links
    assert port_settings.SETTINGS_ROW_WHEN["mods"] == "!in_game" and "mods_folder" not in port_settings.SETTINGS_ROW_WHEN


def test_row_conditions():
    """every row's condition in the model (SETTINGS_ROW_WHEN, SETTINGS_ROW_ENABLED; a profile's setting's) names terms
    that menu_functions.c's settings_condition_term knows, and other rows' keys that are on its page with their
    values; each condition is written into the model's rows (beside their platforms)"""
    functions = (Path(__file__).resolve().parent.parent / "port/linux/game/menu_functions.c").read_text()
    terms = set(re.findall(r'!strcmp\(name, "(\w+)"\)', function_body(functions, "settings_condition_term")))
    platforms = set(port_settings.MENU_PLATFORMS)
    model = (Path(__file__).resolve().parent.parent / "port/assets/menus/model/settings.xml").read_text()
    for folder, page in PAGES.items():
        rows = {_key(entry) or re.sub(r"[^a-z0-9]+", "_", entry[0].lower()).strip("_"): entry
                for entry in page["rows"] if entry[0] != SECTION}
        for key, entry in rows.items():
            for condition in (port_settings.SETTINGS_ROW_WHEN.get(key), port_settings.SETTINGS_ROW_ENABLED.get(key)):
                for term in (condition or "").replace("+", " ").split():
                    term = term.lstrip("!")
                    if "=" in term:
                        other, value = term.split("=", 1)
                        assert other in rows, (folder, key, term)
                        assert value in [v for _, v in rows[other][2]], (folder, key, term)
                    else:
                        assert term in terms | platforms, (folder, key, term)
            when = port_settings._settings_row_when(key, entry[1], entry[4])
            if when:
                assert f'key="{key}"' in model and f'when="{when}"' in model, (folder, key, when)
    for key in port_settings.SETTINGS_ROW_WHEN:
        assert any(key in {_key(entry) for entry in page["rows"] if entry[0] != SECTION} for page in PAGES.values()), key


def function_body(source, name):
    """a C function's body, by its name"""
    start = source.index(f"static boolean {name}(")
    return source[start:source.index("\n}\n", start)]


def _engine_tabs():
    """the menus' tabs as menu_functions.c finds them (settings_tab_screen): the model's screens laid out as tabs,
    in order"""
    import xml.etree.ElementTree as ET
    model = ET.parse(Path(__file__).resolve().parent.parent / "port/assets/menus/model/settings.xml").getroot()
    return [screen.get("name") for screen in model.findall("screen") if screen.get("layout") == "tab"]


def test_engine_tabs_are_the_generators():
    """the tabs Q and E, LB and RB step through (and a click opens) are the
    generator's, in its order; the panel's words for each are its screen's"""
    assert _engine_tabs() == [f"{port_settings.PE}/{folder}/{PAGES[folder]['screen']}"
                              for folder in port_settings.SETTINGS_TAB_PAGES]
    functions = (Path(__file__).resolve().parent.parent / "port/linux/game/menu_functions.c").read_text()
    assert "settings_tabs[]" not in functions


def _tab_problems(places, names):
    """the tabs that are not in the row (from the first's place to RB's
    prompt), not apart, or too narrow for their names and room either side"""
    problems = []
    for index, ((x, width), name) in enumerate(zip(places, names)):
        if width < port_settings.tab_name_width(name) + 2 * 6:
            problems.append(f"{name}: {width} wide, its name {port_settings.tab_name_width(name):.1f}")
        if index and x < places[index - 1][0] + places[index - 1][1] + port_settings.SETTINGS_TAB_GAP:
            problems.append(f"{name} at {x} reaches the tab before it")
    if places[0][0] < port_settings.SETTINGS_TABS_X or sum(places[-1]) > port_settings.SETTINGS_TABS_RIGHT:
        problems.append(f"the tabs {places[0][0]} to {sum(places[-1])} are out of their row")
    return problems


def test_settings_tabs_fit_their_names():
    """at 4:3, where the row is narrowest, every tab holds its name with room
    either side, apart from the next, between LB's and RB's prompts"""
    names = [PAGES[folder]["tab"] for folder in port_settings.SETTINGS_TAB_PAGES]
    assert _tab_problems(port_settings.settings_tab_places(), names) == []


def test_tab_check_sees_a_tab_too_narrow():
    """the check sees seven tabs of the old six's width, and tabs laid over each other"""
    names = [PAGES[folder]["tab"] for folder in port_settings.SETTINGS_TAB_PAGES]
    step = (port_settings.SETTINGS_TABS_RIGHT - port_settings.SETTINGS_TABS_X) // len(names)
    even = [(port_settings.SETTINGS_TABS_X + index * step, step - 2) for index in range(len(names))]
    assert any(name in problem for problem in _tab_problems(even, names) for name in ("INTERFACE", "CONTROLS"))
    places = port_settings.settings_tab_places()
    assert _tab_problems([places[0], (places[0][0] + 10, places[1][1])] + places[2:], names)


def test_map_storage_row():
    """MAP STORAGE (issue 603) sets game.map_cache_megabytes: Instant Load
    Times its default (0, every stock map kept), Disk Space Saver the Xbox's
    730 MB"""
    rows = [entry for page in PAGES.values() for entry in page["rows"]
            if entry[0] != SECTION and entry[1] == "game.map_cache_megabytes"]
    assert [row[0] for row in rows] == ["MAP STORAGE"]
    assert rows[0][2] == [("INSTANT LOADING", "0"), ("DISK SPACE SAVER", "730")]
    config = (Path(__file__).resolve().parent.parent / "port/linux/src/port_config.c").read_text()
    assert re.search(r'\{ "game\.map_cache_megabytes", _config_integer, "0",', config)


def test_settings_spinners_are_model_rows():
    """every spinner of a setting in the menus (its setting= and values=) is a choice row of its screen's in the
    model, of the same setting and values, in the order of the screen's widgets: menu_functions.c's settings_each
    and settings_setting_of take Settings' spinners' settings from the model's rows (but the lobby's, which the model
    has no screen of)"""
    import xml.etree.ElementTree as ET
    menus = Path(__file__).resolve().parent.parent / "port/assets/menus"
    widgets = {}
    for path in sorted((menus / "ce").glob("*.xml")):
        for widget in ET.parse(path).getroot().iter("widget"):
            widgets.setdefault(widget.get("name"), []).append(widget)
    model = ET.parse(menus / "model/settings.xml").getroot()
    rows = {}
    for screen in model.findall("screen"):
        folder = screen.get("name").rsplit("/", 1)[0]
        order = []

        def walk(name, seen):
            if name in seen:
                return
            seen.add(name)
            for widget in widgets.get(name, [])[:1]:
                if widget.get("setting"):
                    order.append(name)
                for child in widget.findall("child"):
                    walk(child.get("widget"), seen)

        walk(screen.get("name"), set())
        choices = [row for row in screen.findall("row") if row.get("kind") == "choice" and row.get("setting")]
        for row in choices:
            rows[f"{folder}/{row.get('key')}_spinner"] = row
        named = [f"{folder}/{row.get('key')}_spinner" for row in choices]
        assert [name for name in named if name in order] == order, screen.get("name")
    for name, variants in widgets.items():
        for widget in variants:
            if widget.get("setting") is None or "/lobby/" in name:
                continue
            row = rows.get(name)
            assert row is not None, name
            assert row.get("setting") == widget.get("setting"), name
            assert [choice.get("value") for choice in row.findall("choice")] == widget.get("values").split("|"), name
