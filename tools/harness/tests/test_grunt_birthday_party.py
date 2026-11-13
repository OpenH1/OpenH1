"""The Grunt Birthday Party skull's confetti: whether a map's tags get it, which units are grunts, the particle's size
from its bitmap's sprites (port/linux/game/grunt_birthday_party.c). When it bursts is test_skulls.py's."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run

SOURCE = 'port/linux/game/grunt_birthday_party.c'


def generated(fault=None):
    source = read(SOURCE)
    scenario_types = re.search(r'(_scenario_type_solo = 0,.*?_scenario_type_main_menu,)',
                               read('source/scenario/scenario_definitions.h'), re.S).group(1)
    enums = '\n'.join([enum_with(read('port/linux/game/skulls.h'), '_skull_iron'),
                       enum_with(read('source/game/game.h'), '_game_connection_local = 0'),
                       enum_with(read('source/ai/actor_types.h'), '_actor_elite = 0'),
                       'enum { ' + scenario_types + ' };',
                       enum_with(source, 'GROUP_BITMAP')]) + '\n'
    state = re.search(r'^static struct\n\{\n\t/\* the map\'s table.*?^\} grunt_birthday_party = .*?;$', source,
                      re.S | re.M).group(0)
    code = '\n'.join([state, *(function(source, name) for name in (
        'grunt_birthday_party_may_play', 'grunt_units_find', 'grunt_birthday_party_grunt',
        'confetti_inverse_sprite_size'))])
    if fault:
        code = mutated(code, *fault)
    return (('enums.inc', enums), ('under_test.inc', code))


@pytest.mark.parametrize('case', ['may_play', 'grunts', 'sprite_size'])
def test_case(case):
    status, output = run(build('grunt_birthday_party', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('fault,case', [
    # a client makes no confetti until its host's word, which comes after its map has loaded
    (('\tif (game_connection() == _game_connection_network_client)\n\t\treturn TRUE;\n', ''), 'may_play'),
    # a demo plays this machine's choice, not its recording's
    (('\tif (demo_playback_skulls(&recorded))\n\t\treturn TEST_FLAG(recorded, _skull_grunt_birthday_party);\n', ''),
     'may_play'),
    # a multiplayer map's tags get it too
    ((' || scenario->type != _scenario_type_solo', ''), 'may_play'),
    # every actor's unit a grunt
    (('\t\t\tactor_definition_get(variant->actor_reference.index)->type != _actor_grunt ||\n', ''), 'grunts'),
    # the sprites' widths alone
    (('(sprite->bounds.y1 - sprite->bounds.y0) * data->height',
      '(sprite->bounds.x1 - sprite->bounds.x0) * data->height'), 'sprite_size'),
])
def test_negative_control(fault, case):
    status, output = run(build('grunt_birthday_party', generated(fault)), case)
    assert status == CHECK_FAILED, f'the fault went unnoticed: {output}'
