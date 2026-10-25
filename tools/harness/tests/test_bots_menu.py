"""The bots in the menus: Multiplayer's PLAY WITH BOTS (a game of this machine alone, whose lobby the bots join), and
Server Setup's bots rows (bots.count, bots.skill, bots.teams), which tools/port_settings.py writes and
port/linux/game/menu_functions.c's server_settings_bots shows in a game of this machine alone, writes as they change,
and locks (dimmed, passed over, a line saying why) for a gametype the bots do not play."""

import json
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, ROOT, build, function, mutated, read, run, structure  # noqa: E402

CASES = ["network", "local", "locked", "network-row", "network-row-locked"]

NEGATIVE_CONTROLS = {
    "network-rows-kept": (("visible_set(named(list, server_settings_network_rows[index], 0), !local);", ""), "local"),
    "rows-always-shown": (("visible_set(row, local);", "visible_set(row, TRUE);"), "network"),
    "never-written": (("\t\t\tsetting_changed_save(spinner, setting);\n", ""), "local"),
    "written-in-network-game": (("if (local && setting)", "if (setting)"), "network"),
    "never-locked": (("boolean locked = local && !playable;", "boolean locked = FALSE;"), "locked"),
    "not-dimmed": (("\t\trow->alpha_modifier = locked ? 0.4f : 1.0f;", "\t\trow->alpha_modifier = 1.0f;"), "locked"),
    "network-row-not-dimmed": (("\n\trow->alpha_modifier = locked ? 0.4f : 1.0f;", "\n\trow->alpha_modifier = 1.0f;"),
                               "network-row-locked"),
    "no-line": (('visible_set(named(screen_of(list), "bots_hint", 0), locked);', ""), "locked"),
    "focus-kept": (("\t\tif (locked && list->focused_child == row)", "\t\tif (0)"), "locked"),
    "help-of-another": (("(short)(BOTS_HELP + index)", "(short)BOTS_HELP"), "local"),
    "network-row-hidden": (("\tvisible_set(row, shown);", "\tvisible_set(row, FALSE);"), "network-row"),
    "network-row-everywhere": (("\tvisible_set(row, shown);", "\tvisible_set(row, TRUE);"), "network-row"),
    "fill-always-shown": (("else if (bots_make_room())", "else if (1)"), "network-row"),
    "network-row-never-locked": (("boolean locked = shown && !playable;", "boolean locked = FALSE;"),
                                 "network-row-locked"),
    "network-row-focus-kept": (("\tif (locked && list->focused_child == row)\n\t{", "\tif (0)\n\t{"),
                               "network-row-locked"),
}

MT = "main_menu/multiplayer_type_select"
CE = ROOT / "port/assets/menus/ce"


def generated(fault=None):
    menus = read("port/linux/game/menu_functions.c")
    config = "".join("#define %s %s\n" % (name, re.search(rf"#define {name} (\d+)", menus).group(1))
                     for name in ("BOTS_HELP", "BOT_OPTIONS_HELP", "BOT_OPTIONS_LOCKED_HELP"))
    config += structure(menus, "pc_menu_setting") + "\n"
    rows = menus[menus.index("static char const *const server_settings_network_rows"):]
    rows = rows[:rows.index("};", rows.index("server_settings_bots_rows")) + 2]
    code = "\n\n".join([rows] + [function(menus, name) for name in (
        "screen_of", "setting_changed_save", "server_settings_bots", "server_settings_network_bots")])
    if fault:
        code = mutated(code, *fault)
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("bots_menu", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("bots_menu", generated(fault)), case)
    assert status == CHECK_FAILED, f"the code with a fault ({control}) passed '{case}': the test cannot see it"


def _port_settings():
    sys.path.insert(0, str(ROOT / "tools"))
    import port_settings

    return port_settings


def test_server_setup_has_the_rows():
    """Server Setup's three bots rows: spinners of bots.count (0 to 15), bots.skill and bots.teams with the values
    port_config.c takes, in the places of GAME, INVITE LINK and LISTING (which a game of this machine alone hides),
    the line below them, and the file in port/assets/menus as tools/port_settings.py writes it."""
    port_settings = _port_settings()
    name = f"{MT}/server_settings".replace("/", ".") + ".xml"
    lines = port_settings.multiplayer_files()[name]
    assert (CE / name).read_text() == "\n".join(lines), "run python3 tools/port_settings.py"
    text = "\n".join(lines)
    config = read("port/linux/src/port_config.c")
    expected = {
        "bots.count": [str(count) for count in range(16)],
        "bots.skill": ["easy", "normal", "heroic", "legendary"],
        "bots.teams": ["even", "against"],
    }
    for setting, values in expected.items():
        key = setting.replace(".", "_")
        spinner = re.search(rf'<widget name="{MT}/server_settings/{key}_spinner"[^>]*>', text).group(0)
        assert f'setting="{setting}"' in spinner and f'values="{"|".join(values)}"' in spinner, spinner
        assert f'{{ "{setting}", _config_' in config
        child = re.search(rf'<child widget="{MT}/server_settings/op_{key}" x="54" y="(\d+)"/>', text)
        assert child, f"op_{key} is not in Server Setup's list"
    places = [int(re.search(rf'<child widget="{MT}/server_settings/op_{row}" x="54" y="(\d+)"/>', text).group(1))
              for row in ("game_mode", "invite", "listing", "bots_count", "bots_skill", "bots_teams")]
    assert places[3:] == places[:3], places
    assert f'<child widget="{MT}/server_settings/bots_hint"/>' in text
    assert port_settings.BOTS_HINT in text
    # (each row's help, where menu_functions.c's BOTS_HELP says)
    helps = re.search(rf'<strings name="{MT}/server_settings/help_strings">(.*?)</strings>', text, re.S).group(1)
    helps = re.findall(r'<string text="([^"]*)"/>', helps)
    first = int(re.search(r"#define BOTS_HELP (\d+)", read("port/linux/game/menu_functions.c")).group(1))
    assert helps[first:first + 3] == [help_text for _, _, _, help_text in port_settings.BOTS_ROWS]
    labels = re.search(rf'<strings name="{MT}/server_settings/labels">(.*?)</strings>', text, re.S).group(1)
    labels = re.findall(r'<string text="([^"]*)"/>', labels)
    for key, label, _, _ in port_settings.BOTS_ROWS:
        index = int(re.search(rf'<widget name="{MT}/server_settings/{key}_label"[^>]*string_index="(\d+)"', text)
                    .group(1))
        assert labels[index] == label, (key, labels[index])


def test_server_setup_has_the_network_bots_row():
    """a network game's BOTS row, after the gametype's rows (its own place), opening the bots' screen
    (tools/port_settings.py's BOT_SETUP), whose spinners are gametype_options' settings of bots.count, bots.skill,
    bots.teams, bots.make_room and bots.fill_to with the values port_config.c takes, FILL TO last; and the row's
    helps where menu_functions.c says"""
    port_settings = _port_settings()
    name = f"{MT}/server_settings".replace("/", ".") + ".xml"
    text = "\n".join(port_settings.multiplayer_files()[name])
    menus = read("port/linux/game/menu_functions.c")
    config = read("port/linux/src/port_config.c")
    places = {row: int(re.search(rf'<child widget="{MT}/server_settings/op_{row}" x="54" y="(\d+)"/>', text).group(1))
              for row in ("team_options", "bot_options")}
    assert places["bot_options"] == places["team_options"] + port_settings.SERVER_SETTINGS_SPACING, places
    help_top = int(re.search(rf'<widget name="{MT}/server_settings/help"[^>]*top="(\d+)"', text).group(1))
    assert places["bot_options"] + 24 <= help_top, "the BOTS row runs into the help"
    key, _, _, rows = port_settings.BOT_SETUP
    row = re.search(rf'<widget name="{MT}/server_settings/op_{key}"[^>]*>(.*?)</widget>', text, re.S).group(1)
    assert f'open="{MT}/server_settings/{key}/{key}_screen"' in row
    expected = {"bot_count": ("bots.count", [str(count) for count in range(16)]),
                "bot_skill": ("bots.skill", ["easy", "normal", "heroic", "legendary"]),
                "bot_teams": ("bots.teams", ["even", "against"]),
                "bot_make_room": ("bots.make_room", ["true", "false"]),
                "bot_fill_to": ("bots.fill_to", [str(count) for count in port_settings.BOT_FILLS])}
    assert [row_key for row_key, *_ in rows] == list(expected) and rows[-1][0] == "bot_fill_to"
    for row_key, label, choices, helps in rows:
        setting, values = expected[row_key]
        entry = re.search(rf'\{{ "{row_key}_spinner", _option_setting, 0, 0, (\d+), \{{ 0 \}}, "{re.escape(setting)}",'
                          r'\s*\{([^}]*)\}', menus)
        assert entry, f"no gametype_options entry for {row_key}"
        assert int(entry.group(1)) == len(values) == len(choices) == len(helps), row_key
        assert re.findall(r'"([^"]*)"', entry.group(2)) == values, row_key
        assert f'{{ "{setting}", _config_' in config, setting
    helps = re.search(rf'<strings name="{MT}/server_settings/help_strings">(.*?)</strings>', text, re.S).group(1)
    helps = re.findall(r'<string text="([^"]*)"/>', helps)
    first = int(re.search(r"#define BOT_OPTIONS_HELP (\d+)", menus).group(1))
    locked = int(re.search(r"#define BOT_OPTIONS_LOCKED_HELP (\d+)", menus).group(1))
    assert helps[first] == port_settings.BOT_SETUP[2] and helps[locked] == port_settings.BOTS_HINT
    labels = re.search(rf'<strings name="{MT}/server_settings/labels">(.*?)</strings>', text, re.S).group(1)
    labels = re.findall(r'<string text="([^"]*)"/>', labels)
    index = int(re.search(rf'<widget name="{MT}/server_settings/{key}_label"[^>]*string_index="(\d+)"', text).group(1))
    assert labels[index] == "BOTS:"


def test_multiplayer_has_play_with_bots():
    """Multiplayer's PLAY WITH BOTS: its item (a file of its own the game embeds), in the list under Create Game
    above EDIT GAMETYPES' line, its name and description where the list's others have theirs, and the mode it
    chooses: a game of this machine alone (menu_functions.c's multiplayer_mode_set, multiplayer_host)."""
    port_settings = _port_settings()
    name = f"{MT}/bots".replace("/", ".") + ".xml"
    assert (CE / name).read_text() == "\n".join(port_settings.multiplayer_files()[name])
    assert f"ce/{name}" in json.loads((ROOT / "port/assets/menus/menus.json").read_text())["files"]
    item = (CE / name).read_text()
    assert 'name="main_menu/multiplayer_type_select/multiplayer_type_create_bots_item"' in item
    assert 'run="join controller to mp game"' in item and 'run="mp type set mode"' in item
    menu = (CE / "main_menu.multiplayer_type_select.xml").read_text()
    for row, (x, y) in port_settings.MULTIPLAYER_ROWS.items():
        place = (f' x="{x}"' if x else "") + f' y="{y}"'
        assert f'<child widget="{MT}/{row}"{place}/>' in menu, row
    # (the computer's list, Multiplayer's own: PLAY WITH BOTS under HOST, above EDIT GAMETYPES' line,
    # test_browser_menu.py's test_hub_keeps_every_place)
    hub = (CE / "main_menu.multiplayer_type_select.hub.xml").read_text()
    bots_y = int(re.search(rf'<child widget="{MT}/hub/hub_create_bots" x="\d+" y="(\d+)"/>', hub).group(1))
    assert bots_y + port_settings.HUB_ITEM[1] <= port_settings.MULTIPLAYER_LINE_PLACE[1]
    assert hub.index(f"{MT}/hub/section_host") < hub.index(f"{MT}/hub/hub_create_bots") < \
        hub.index(f'{MT}/hub/hub_gametypes" x=')
    strings = (CE / "strings.xml").read_text()
    options = re.search(rf'<strings name="{MT}/multiplayer_options"[^>]*>(.*?)</strings>', strings, re.S).group(1)
    assert re.findall(r'text="([^"]*)"', options) == port_settings.STRING_OVERRIDES[f"{MT}/multiplayer_options"]
    assert re.findall(r'text="([^"]*)"', options)[9] == "PLAY WITH BOTS"
    descriptions = re.search(rf'<strings name="{MT}/multiplayer_option_descriptions"[^>]*>(.*?)</strings>', strings,
                             re.S).group(1)
    assert re.findall(r'text="([^"]*)"', descriptions) == \
        port_settings.STRING_OVERRIDES[f"{MT}/multiplayer_option_descriptions"]
    menus = read("port/linux/game/menu_functions.c")
    mode_set = function(menus, "multiplayer_mode_set")
    assert 'strstr(name, "create_bots") ? _multiplayer_mode_host_local' in mode_set
    host = function(menus, "multiplayer_host")
    assert "ui_widget_port_host_local(widget, event, widget_deleted)" in host
    local = function(read("source/interface/ui_widget_event_handler_functions.c"), "ui_widget_port_host_local")
    assert "network_game_accept_remote_connections(FALSE);" in local
