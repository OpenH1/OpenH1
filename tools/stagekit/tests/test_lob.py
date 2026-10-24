"""scenarios/lob.c: the player with a launcher of grenades that land and go off on a fuse (tools/scorecard's grenade
scenario measures them; here, that its own grenades going off near it leave it standing: they do no damage)."""

PLAY = dict(seconds=9, input="script:0.4 aim=0,-60; 0.5 fire")


def test_lives_through_its_grenades(play):
    session = play("lob")
    assert session.deaths() == 0
    assert all(abs(x) < 1e-3 and abs(y) < 1e-3 for x, y, _ in session.places()), session.places()
