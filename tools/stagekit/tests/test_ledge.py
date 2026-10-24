"""scenarios/ledge.c: the player on a block 2 high, a unit from its edge (x 1), walking toward it from half a
second on: it walks off, falls 2 and walks on along the floor."""

PLAY = dict(seconds=4, input="script:0.5-4 move=0,1")
EDGE = 1.0
HEIGHT = 2.0


def test_starts_on_the_ledge(play):
    assert abs(play("ledge").places()[0][2] - HEIGHT) < 0.05


def test_walks_off_and_lands_on_the_floor(play):
    """past the edge, on the floor by the end, never below it"""
    places = play("ledge").places()
    assert places[-1][0] > EDGE + 1.0 and abs(places[-1][2]) < 0.01, places
    assert min(z for _, _, z in places) > -0.01, places
