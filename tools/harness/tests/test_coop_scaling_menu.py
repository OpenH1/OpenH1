"""All 27 enemy/pickup/vehicle modes, row positions and help indices: menu_functions.c's gametype_option_help, with
the rows' conditions of the menus' model (model/nodes.xml) for CO-OP OPTIONS."""
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, ROOT, build, enum_with, function, mutated, read, run


COOP_SCREEN = "main_menu/multiplayer_type_select/server_settings/coop_options/coop_options_screen"


def conditions(drop=None):
    """the co-op screen's rows' conditions in the menus' model (model/nodes.xml, tools/port_settings.py's
    SETUP_NODE_WHEN) as C: menu_functions.c's setup_nodes_conditions for that screen, its "<key>=<n>" terms its
    spinners' (drop: a term taken out, for a negative control)"""
    root = ET.parse(ROOT / "port/assets/menus/model/nodes.xml").getroot()
    screen = next(screen for screen in root.findall("screen") if screen.get("name") == COOP_SCREEN)
    lines = ["static void setup_nodes_conditions(struct widget_instance *list)", "{",
             '\tif (strcmp(list->name, "coop_options_menu"))', "\t\treturn;"]
    for node in screen.findall("node"):
        when = node.get("when")
        if not when:
            continue
        alternatives = []
        for alternative in when.split():
            terms = []
            for term in alternative.split("+"):
                if term == drop:
                    continue
                key, value = term.split("=")
                terms.append(f'(named(list, "{key}_spinner", 0) && '
                             f'named(list, "{key}_spinner", 0)->parameters.list.selected_index == {int(value)})')
            if terms:
                alternatives.append(" && ".join(terms))
        leaf = node.get("name").rsplit("/", 1)[1]
        lines.append(f'\tvisible_set(named(list, "{leaf}", 0), {" || ".join(alternatives) or "FALSE"});')
    return "\n".join(lines + ["}", ""])


def generated(fault=None, drop=None):
    source = read('port/linux/game/menu_functions.c')
    # (OPENCE OPTIONS' rows name the modes: game_engine.h)
    code = enum_with(read('source/game/game_engine.h'), '_game_mode_none = 0') + '\n' + conditions(drop) + '\n' + \
        function(source, 'gametype_option_help')
    if fault:
        code = mutated(code, *fault)
    return (('under_test.inc', code),)


@pytest.mark.parametrize('case', ['layout', 'help', 'unrelated'])
def test_case(case):
    status, output = run(build('coop_scaling_menu', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('fault,case', [
    (('row->vertical_offset = y;', 'row->vertical_offset = y; row->child->vertical_offset = y;'), 'layout'),
    (('if (row->visible && !strncmp(row->name, "op_coop_", 8))', 'if (!strncmp(row->name, "op_coop_", 8))'), 'layout'),
    (('index += spinner->parameters.list.number_of_items;',
      'if (row->visible) index += spinner->parameters.list.number_of_items;'), 'help'),
    (('\tsetup_nodes_conditions(list);\n', ''), 'layout'),
])
def test_negative_control(fault, case):
    status, output = run(build('coop_scaling_menu', generated(fault)), case)
    assert status == CHECK_FAILED, output


@pytest.mark.parametrize('drop', ['coop_extra_pickups=0', 'coop_extra_vehicles=1'])
def test_negative_control_model(drop):
    """the model's conditions with a spinner's term taken out: the rows' visibility is wrong"""
    status, output = run(build('coop_scaling_menu', generated(drop=drop)), 'layout')
    assert status == CHECK_FAILED, output
