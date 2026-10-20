"""scenarios/lift.c: an AI's biped on the elevator's platform (0.25 high), made an actor half a second in; a second
in, the platform rises 5 units in 2 seconds, while the rider walks 3 units across it, and at 5 seconds comes down
while it walks back. The scorecard's elevator_ai_rider scenarios measure the rider's path at every rate (debug.trace's
AI lines), updated before the lift or after it, riding its floor or carried by it."""

CUES = [(0.5, "ai_attach_free rider rider"), (1, "device_set_position lift 1"),
        (1.2, "ai_command_list_by_unit rider across"), (5, "device_set_position lift 0"),
        (5.2, "ai_command_list_by_unit rider back")]
PLAY = dict(seconds=10, input="look:1", cues=CUES)


def test_the_console_finds_the_rider_and_its_lists(play):
    session = play("lift")
    for complaint in ("not a valid", "cannot execute command list", "no such", "unknown"):
        assert complaint not in session.debug.lower(), session.debug


def test_the_player_stays_on_the_floor(play):
    """(beside the lift, never on it)"""
    assert all(abs(z) < 0.05 for _, _, z in play("lift").places())
