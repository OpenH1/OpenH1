"""scenarios/shoot.c: the player with a rifle among walls 2, 10 and 40 away, shooting at each (tools/scorecard's
first_shot and impact scenarios measure the shots; here, that the player stays put while it shoots)."""

PLAY = dict(seconds=5, input="script:0.5 fire; 1 aim=90,0; 1.5 fire; 2 aim=180,0; 2.5 fire")


def test_stands_still_and_shoots(play):
    session = play("shoot")
    assert all(abs(x) < 1e-3 and abs(y) < 1e-3 for x, y, _ in session.places()), session.places()
    assert session.deaths() == 0
