"""scenarios/crouch.c: the player crouches from half a second to a second and a half, then stands again; the game logs
no crouch, so this checks that the crouch seats it without a complaint and leaves it where it stood (the scorecard's
crouch_* measure the crouch and the eye, debug.trace's player lines)."""

PLAY = dict(seconds=3, input="script:0.5-1.5 crouch")


def test_no_engine_complaint(play):
    session = play("crouch")
    for complaint in ("animation", "seat", "exception"):
        assert complaint not in session.debug.lower(), session.debug


def test_the_player_stays(play):
    """(on the floor, where it started: a crouch on the ground moves it nowhere)"""
    places = play("crouch").places()
    assert places and max(abs(x) + abs(y) + abs(z) for x, y, z in places) < 0.01, places
