"""scenarios/shield.c: the player's biped with a body and a shield of 75 each, recharging in 2 seconds. A second in,
the console drops the shield to nothing (unit_set_current_vitality), and at 4 seconds to half; the game logs no
shield, so these check only that the console finds what it sets and the player lives on (the scorecard's
shield_recharge measures the shield, debug.trace's player lines)."""

CUES = [(1, "unit_set_current_vitality (unit (list_get (players) 0)) 75 0"),
        (4, "unit_set_current_vitality (unit (list_get (players) 0)) 75 37.5")]
PLAY = dict(seconds=6, input="look:1", cues=CUES)


def test_the_console_sets_the_vitality(play):
    session = play("shield")
    for complaint in ("not a valid", "no such", "unknown", "expected"):
        assert complaint not in session.debug.lower(), session.debug


def test_the_player_lives(play):
    assert play("shield").deaths() == 0
