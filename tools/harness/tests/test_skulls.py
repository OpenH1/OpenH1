"""The campaign's skulls: which a game plays, and each hook in the game's systems, the game's own value with no skull
(port/linux/game/skulls.c, network_skulls.c; game_globals.c's game_difficulty_get_team_value; weapons.c's
weapon_halve_ammunition)."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure

SKULLS = 'port/linux/game/skulls.c'


def generated(fault=None):
    header = read('port/linux/game/skulls.h')
    game = read('source/game/game.h')
    scenario_types = re.search(r'(_scenario_type_solo = 0,.*?_scenario_type_main_menu,)',
                               read('source/scenario/scenario_definitions.h'), re.S).group(1)
    defines = '\n'.join(re.findall(r'^#define SKULLS_\w+ .*$', header, re.M))
    enums = '\n'.join([enum_with(header, '_skull_iron'), defines,
                       enum_with(game, '_game_connection_local = 0'),
                       enum_with(game, '_game_difficulty_level_easy = 0'),
                       enum_with(game, '_game_team_default'),
                       enum_with(read('source/game/game_globals.h'), '_game_difficulty_value_enemy_damage = 0'),
                       'enum { ' + scenario_types + ' };',
                       enum_with(read('port/linux/game/network_distributed.h'), '_distributed_message_skulls')]) + '\n'
    source = read(SKULLS)
    names = re.search(r'static char const \*const skull_names\[NUMBER_OF_SKULLS\] =.*?\};', source, re.S).group(0)
    globals_source = read('source/game/game_globals.c')
    friends = re.search(r'short const global_difficulty_friend_settings\[NUMBER_OF_GAME_DIFFICULTY_VALUES\] =.*?\};',
                        globals_source, re.S).group(0)
    network = read('port/linux/game/network_skulls.c')
    code = '\n'.join([names, 'static unsigned long skulls_of_game;', 'static void weapon_halve_ammunition(long);',
                      friends,
                      *(function(source, name) for name in (
                          'skull_name', 'skulls_parse', 'skulls_format', 'skulls_initialize_for_new_map',
                          'skulls_game', 'skulls_set_game', 'skull_active', 'skulls_enemy_difficulty_value',
                          'skulls_material_modifier', 'skulls_evasion_chance', 'skulls_force_major_upgrade',
                          'skulls_actor_dropped_weapon', 'skulls_shields_recharge',
                          'skulls_melee_recharges_shields', 'skulls_motion_sensor_hidden', 'skulls_hud_hidden',
                          'skulls_unit_killed')),
                      function(globals_source, 'game_difficulty_get_value_by_difficulty'),
                      function(globals_source, 'game_difficulty_get_team_value'),
                      function(read('source/items/weapons.c'), 'weapon_halve_ammunition'),
                      structure(network, 'skulls_message'),
                      function(network, 'network_skulls_handle_message')])
    if fault:
        code = mutated(code, *fault)
    return (('enums.inc', enums), ('under_test.inc', code))


CASES = ['names', 'local', 'host', 'client', 'client_untrusted', 'demo', 'off', 'mythic', 'catch', 'tilt',
         'tough_luck', 'thunderstorm', 'black_eye', 'fog_blind', 'famine', 'grunt_birthday_party']


@pytest.mark.parametrize('case', CASES)
def test_case(case):
    status, output = run(build('skulls', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('fault,case', [
    # the enemies' values not passed through the skulls
    (('return skulls_enemy_difficulty_value(value_type,\n\t\tgame_difficulty_get_value_by_difficulty(value_type, '
      'difficulty));', 'return game_difficulty_get_value_by_difficulty(value_type, difficulty);'), 'mythic'),
    # Mythic's factor applied to the shields alone
    (('\tcase _game_difficulty_value_enemy_vitality:\n', ''), 'mythic'),
    # skulls in multiplayer
    (('&& !game_engine_running();', ';'), 'local'),
    # a mixed game's host plays its skulls
    (('if (network_standard_hosting() && !network_standard_hosting_mixed())', 'if (network_standard_hosting())'),
     'host'),
    # a client takes its own setting
    (('\tdefault:\n\t\t/* (a client', '\tcase _game_connection_network_client:\n\t\tskulls_of_game = '
      'skulls_parse(config_string("game.skulls"));\n\t\tbreak;\n\tdefault:\n\t\t/* (a client'), 'client'),
    # the message's reserved word not checked
    ((' || message.reserved != 0)', ')'), 'client_untrusted'),
    # Tough Luck's every chance, the impossible too
    (('return chance > 0.0f && skull_active(_skull_tough_luck)', 'return skull_active(_skull_tough_luck)'),
     'tough_luck'),
    # Thunderstorm's allies too
    (('skull_active(_skull_thunderstorm) && game_team_is_enemy(_game_team_player, team_index)',
      'skull_active(_skull_thunderstorm)'), 'thunderstorm'),
    # Black Eye's AI shields held too
    (('return !skull_active(_skull_black_eye) || player_index_from_unit_index(unit_index) == NONE;',
      'return !skull_active(_skull_black_eye);'), 'black_eye'),
    # Black Eye's melee on an ally
    (('\t\tgame_team_is_enemy(dealer_team_index, target_team_index);', '\t\tTRUE;'), 'black_eye'),
    # Famine's battery left full
    (('weapon->weapon.age = 1.0f-(1.0f-weapon->weapon.age)*0.5f;', ';'), 'famine'),
    # Grunt Birthday Party's confetti for a body kill
    (('if (!head || melee ||', 'if (melee ||'), 'grunt_birthday_party'),
    # ... for an elite's headshot
    ((' || !grunt_birthday_party_grunt(unit_index))', ')'), 'grunt_birthday_party'),
    # ... with the skull off
    (('!skull_active(_skull_grunt_birthday_party) || ', ''), 'grunt_birthday_party'),
    # ... for a melee to the head
    (('if (!head || melee ||', 'if (!head ||'), 'grunt_birthday_party'),
    # a demo's skulls this computer's
    (('if (demo_playback_skulls(&recorded))', 'if (FALSE && demo_playback_skulls(&recorded))'), 'demo'),
])
def test_negative_control(fault, case):
    status, output = run(build('skulls', generated(fault)), case)
    assert status == CHECK_FAILED, f'the fault went unnoticed: {output}'
