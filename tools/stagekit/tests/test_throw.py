"""scenarios/throw.c: the player with four grenades and no weapon throws one a second in, aimed level along x; the
game logs no grenades, so this checks that the throw happens and the player plays on (the scorecard's
grenade_thrown measures each grenade, debug.trace's projectile lines)."""

PLAY = dict(seconds=4, input="script:0.5 aim=0,10; 1 throw")


def test_no_engine_complaint(play):
    session = play("throw")
    for complaint in ("animation", "grenade", "exception"):
        assert complaint not in session.debug.lower(), session.debug


def test_the_player_stays(play):
    """(a throw does not move it)"""
    places = play("throw").places()
    assert places and max(abs(x) + abs(y) for x, y, _ in places) < 0.1, places
