"""The server browser and Multiplayer in Settings' look (tools/port_settings.py's _browser and _hub, the menus'
shared parts; port/linux/game/menu_functions.c): the browser's tabs (every game, OpenH1's, Legacy ones, co-op, the
favorites) filter its list, a list with no games says why (the local network's, what to do), a text too long for
its column is cut short with "...", and the footers' key prompts are buttons a key of the keyboard presses
(menu_keys.c checks each key); the menu files are as the generator writes them, the parts' pictures as
tools/ce_menus.py draws them, and Multiplayer keeps every place it opened."""

import json
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, ROOT, build, enum_with, function, mutated, read, run, structure  # noqa: E402

CASES = ["tabs", "empty", "ellipsis", "ping"]

NEGATIVE_CONTROLS = {
    "favorites-on-every-tab": (("\tcase _browser_tab_favorites:\n\t\treturn favorite;\n", ""), "tabs"),
    "legacy-as-openh1": (("\t\treturn !entry->openh1;", "\t\treturn entry->openh1;"), "tabs"),
    "offline-on-every-tab": (("\tif (!browser_tab_lists_offline())\n\t\tbrowser_offline_count = 0;\n", ""), "tabs"),
    "lan-says-nothing-to-do": (("L\"No games on your network yet.\\r\\nHost one from Create Game.\"",
                                "L\"No games.\""), "empty"),
    "never-cut": (("if (font == NONE || width <= 0 || settings_text_width(font, fitted) <= width)",
                   "if (TRUE)"), "ellipsis"),
    "ping-unknown-as-measuring": (("\telse if (ping_state == _browser_ping_measuring)",
                                   "\telse if (ping_state != _browser_ping_measured)"), "ping"),
    "ping-bars-one-off": (("ping < PING_GOOD ? 3", "ping <= PING_GOOD ? 3"), "ping"),
    "cut-one-too-many": (("\t\tif (settings_text_width(font, fitted) <= width)\n\t\t\tlow = middle;",
                          "\t\tif (settings_text_width(font, fitted) < width)\n\t\t\tlow = middle;"), "ellipsis"),
}

MT = "main_menu/multiplayer_type_select"
CE = ROOT / "port/assets/menus/ce"


def generated(fault=None):
    menus = read("port/linux/game/menu_functions.c")
    config = "\n\n".join([
        enum_with(menus, "_multiplayer_mode_server_browser"), enum_with(menus, "_browser_sort_players"),
        enum_with(menus, "_browser_tab_all"), enum_with(menus, "_browser_ping_measured"),
        enum_with(menus, "PING_GOOD = 80"),
        "\n".join(re.findall(r"^#define BROWSER_PING_\w+ L[^\n]+", menus, re.M)),
        "#define BROWSER_MODE_LENGTH %s" % re.search(r"#define BROWSER_MODE_LENGTH (\d+)", menus).group(1),
        "#define MENU_TEXT_ELLIPSIS_LENGTH %s" % re.search(r"#define MENU_TEXT_ELLIPSIS_LENGTH (\d+)",
                                                           menus).group(1),
        structure(menus, "browser_sort_entry")]) + "\n"
    code = "\n\n".join(function(menus, name) for name in (
        "settings_wide_copy", "browser_text_compare", "browser_wide_compare", "browser_sort_before",
        "browser_tab_shows", "browser_tab_lists_offline", "browser_favorites_order", "browser_empty_text",
        "menu_text_cut", "menu_text_fit", "browser_ping_cell", "browser_ping_bars"))
    if fault:
        code = mutated(code, *fault)
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("browser_menu", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("browser_menu", generated(fault)), case)
    assert status == CHECK_FAILED, f"the code with a fault ({control}) passed '{case}': the test cannot see it"


def _port_settings():
    sys.path.insert(0, str(ROOT / "tools"))
    import port_settings

    return port_settings


def test_files_are_the_generators():
    """The browser's file and Multiplayer's are tools/port_settings.py's, the browser's folder its own (the PC
    version's tags, which ce_menus.py needs, write none of it, nor its filters), and both are embedded."""
    port_settings = _port_settings()
    files = port_settings.multiplayer_files()
    listed = json.loads((ROOT / "port/assets/menus/menus.json").read_text())["files"]
    for name in (f"{MT}/join_game".replace("/", ".") + ".xml", f"{MT}/hub".replace("/", ".") + ".xml"):
        assert (CE / name).read_text() == "\n".join(files[name]), f"run python3 tools/port_settings.py ({name})"
        assert f"ce/{name}" in listed
    assert port_settings.replaced(f"{MT}/join_game") and port_settings.replaced(f"{MT}/join_game/filters")
    assert not (CE / "main_menu.multiplayer_type_select.join_game.filters.xml").exists()


def test_parts_pictures_are_drawn():
    """Each of the shared parts' pictures is in bitmaps.xml as ce_menus.py's Art.port_picture writes it (a frame
    each, its PNG embedded, a whole multiple of its size), its SVG the generator's, and ce_menus.py draws them."""
    port_settings = _port_settings()
    bitmaps = (CE / "bitmaps.xml").read_text()
    listed = json.loads((ROOT / "port/assets/menus/menus.json").read_text())["files"]
    for name, frames in port_settings.parts_pictures():
        lines = [f'\t<bitmap name="{name}">']
        for index, (svg, width, height) in enumerate(frames):
            png = f"ce/port/{name}__{index}.png"
            lines.append(f'\t\t<frame png="{png}" width="{width}" height="{height}"/>')
            assert png in listed, png
            assert (ROOT / "port/assets/menus/port_svg" / f"{name}__{index}.svg").read_text() == svg, name
        lines.append("\t</bitmap>")
        assert "\n".join(lines) in bitmaps, f"{name} is not in bitmaps.xml as ce_menus.py writes it"
    assert "port_settings.parts_pictures()" in read("tools/ce_menus.py")


def test_browser_layout():
    """The browser: its five tabs (LB and RB, Q and E: the screen's white and black), its columns' titles over the
    rows' columns (each title where its column is, Players' and Ping's at their right), the details panel beside the
    rows with JOIN GAME, the words for no games, and the footer's prompts, each a button its key presses."""
    port_settings = _port_settings()
    text = (CE / "main_menu.multiplayer_type_select.join_game.xml").read_text()
    screen = re.search(rf'<widget name="{MT}/join_game/join_game_screen".*?</widget>', text, re.S).group(0)
    for event, run_name in (("white", "port browser tab previous"), ("black", "port browser tab next"),
                            ("x", "port browser sort"), ("y", "port browser favorite"),
                            ("right_trigger", "port browser refresh"), ("b", "gamespy back handler")):
        assert f'<on event="{event}" run="{run_name}"/>' in screen, event
    tabs = re.findall(rf'<widget name="{MT}/join_game/(tab_\w+)"[^>]*text="([^"]*)"', text)
    assert tabs == port_settings.BROWSER_TABS == [("tab_all", "ALL"), ("tab_openh1", "OPENH1"),
                                                 ("tab_legacy", "LEGACY"), ("tab_coop", "CO-OP"),
                                                 ("tab_favorites", "FAVORITES")]
    # (the tabs in menu_functions.c's order)
    menus = read("port/linux/game/menu_functions.c")
    order = re.findall(r"_browser_tab_(\w+),", enum_with(menus, "_browser_tab_all"))
    assert order == [name[4:] for name, _ in tabs]
    # (each title over its column: at its left, Players' at its right with its numbers, Ping's at its bars;
    # the row at the list's left; no two titles in one place)
    places = []
    for header, words, column in port_settings.BROWSER_HEADERS:
        x, width = port_settings.BROWSER_COLUMNS[column]
        title_x = port_settings._header_x(column)
        title_width = port_settings.BROWSER_HEADER_WIDTHS.get(column, width)
        assert f'<child widget="{MT}/join_game/{header}" x="{port_settings.BROWSER_LIST_X + title_x}"/>' in text, header
        assert re.search(rf'<widget name="{MT}/join_game/{header}" type="text" width="{title_width}"', text), header
        if column == "players":
            assert title_x + title_width == x + width
        else:
            assert x - 8 <= title_x <= x
        places.append((title_x, title_x + title_width))
    assert all(places[index][1] <= places[index + 1][0] for index in range(len(places) - 1)), places
    assert [words for _, words, _ in port_settings.BROWSER_HEADERS] == ["SERVER", "MAP", "MODE", "PLAYERS", "PING"]
    # (the ping's column: its bars, then its number at its right, as wide as the column, its title at its right)
    x, width = port_settings.BROWSER_COLUMNS["ping"]
    assert f'<child widget="{MT}/join_game/server_item_ping_ms" x="{x}"/>' in text
    assert re.search(rf'<widget name="{MT}/join_game/server_item_ping_ms" type="text" width="{width}"[^>]*'
                     r'align="right"', text)
    assert width >= port_settings.BROWSER_PING_BARS + 24
    assert port_settings._header_x("ping") + width == x + width
    for column in ("server_name", "map", "type", "players", "ping"):
        x, _ = port_settings.BROWSER_COLUMNS[column]
        assert f'<child widget="{MT}/join_game/server_item_{column}" x="{x}"' in text, column
    # (the panel beside the rows, inside the screen, and the list's description)
    left, top, width, height = port_settings.BROWSER_PANEL
    assert port_settings.BROWSER_LIST_X + port_settings.BROWSER_LIST_WIDTH < left and left + width <= 640
    assert f'description="{MT}/join_game/browser_panel"' in text
    assert re.search(r'name="[^"]*/button_join"[^>]*text="%a-button JOIN GAME"', text)
    assert f'<widget name="{MT}/join_game/browser_empty"' in text
    # (the footer's prompts: each its button's picture, a key of the keyboard's: menu_keys.c)
    keys = function(read("source/interface/ui_widget.c"), "ui_widget_port_menu_key")
    for name, words, _, _, handler in port_settings.BROWSER_PROMPTS + port_settings.HUB_PROMPTS:
        icon = re.match(r"%([a-z-]+) ", words).group(1).replace("-", "_")
        assert f"case _icon_{icon}:" in keys, f"{name}'s {icon} has no key"
    assert [words.split(" ", 1)[1] for _, words, *_ in port_settings.BROWSER_PROMPTS] == \
        ["FAVORITE", "REFRESH", "PASTE LINK", "SORT", "BACK"]
    # (the sort is the column titles': each sorts by its column, X the next of them; the title sorted by
    # brighter, its arrow its way; no sort of its own by the tabs)
    sorts = re.findall(r'\{ "(op_sort_header_\w+)", L"(\w+)", (?:TRUE|FALSE) \}', menus)
    assert sorted(sorts) == sorted((header, words) for header, words, _ in port_settings.BROWSER_HEADERS)
    assert "browser_sort_header_current(widget)" in function(menus, "pc_menu_text_color")
    assert "browser_sort" not in text.replace("browser_sort_", "")


def test_hub_keeps_every_place():
    """Multiplayer's list, on the computer: PLAY ONLINE's items, HOST's (CREATE GAME's and CO-OP CAMPAIGN's pages
    and PLAY WITH BOTS) and EDIT GAMETYPES, each opening what the PC version's list did (the same functions and
    screens: the touchscreen keeps its own list), its panel with ONLINE NOW and BEST FOR YOU, the panel's
    descriptions one for each of menu_functions.c's items, and REFRESH and BACK in its footer."""
    port_settings = _port_settings()
    hub = (CE / "main_menu.multiplayer_type_select.hub.xml").read_text()
    pc = (CE / "main_menu.multiplayer_type_select.xml").read_text()
    port = "\n".join((CE / name).read_text() for name in ("main_menu.multiplayer_type_select.bots.xml",
                                                          "main_menu.multiplayer_type_select.coop.xml"))

    def handlers(text, name):
        widget = re.search(rf'<widget name="{name}"[^>]*>(.*?)</widget>', text, re.S).group(1)
        return [line.strip() for line in widget.splitlines() if line.strip().startswith('<on event="a"')]

    pairs = {
        "hub_join_internet": "multiplayer_type_join_internet_item",
        "hub_join_lan_item": "multiplayer_type_join_lan_item",
        "hub_join_direct": "multiplayer_type_join_direct_item",
        "hub_create_inter": "multiplayer_type_create_internet_item",
        "hub_create_lan_item": "multiplayer_type_create_lan_item",
        "hub_create_bots": "multiplayer_type_create_bots_item",
        "hub_online_coop": "multiplayer_type_online_coop_item",
        "hub_split_coop": "multiplayer_type_coop_item",
        "hub_gametypes": "multiplayer_type_gametypes_item",
    }
    for ours, theirs in pairs.items():
        assert handlers(hub, f"{MT}/hub/{ours}") == handlers(pc + port, f"{MT}/{theirs}"), ours
    # (the screen shows the hub's list and line on the computer, the touchscreen's list as before)
    screen = re.search(rf'<widget name="{MT}/multiplayer_type_select_screen".*?</widget>', pc, re.S).group(0)
    assert f'<child widget="{MT}/hub/hub_list" platform="pointer"/>' in screen
    x, y = port_settings.MULTIPLAYER_LINE_PLACE
    assert f'<child widget="{MT}/hub/hub_divider" x="{x}" y="{y}" platform="pointer"/>' in screen
    assert f'<child widget="{MT}/touch/multiplayer_touch_list" platform="touch"/>' in screen
    assert '<on event="right_trigger" run="port multiplayer refresh"/>' in screen
    # (the items' names menu_functions.c knows, and its descriptions)
    menus = read("port/linux/game/menu_functions.c")
    known = re.findall(r'\{ "(hub_\w+)", _multiplayer_item_\w+ \}', menus)
    assert sorted(known) == sorted(set(re.findall(rf'<widget name="{MT}/hub/(hub_(?!list|divider|panel|prompts)\w+)"',
                                                  hub)) - {"hub_prompts"})
    items = re.findall(r"_multiplayer_item_(\w+),", enum_with(menus, "_multiplayer_item_server_browser"))
    assert len(items) == len(port_settings.HUB_DESCRIPTIONS)
    assert "Press %a-button" in port_settings.HUB_DESCRIPTIONS[0]
    for name in ("online_now_label", "online_games_0", "best_label", "best_0_name", "best_0_badge", "best_0_about",
                 "multiplayer_options_txt"):
        assert f'<child widget="{MT}/hub/{name}"/>' in hub, name
