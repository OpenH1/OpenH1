"""scenarios/drive.c: cars to drive (tools/scorecard's vehicle scenarios measure them; here, that the player sits in
one and drives it: along x, faster than a biped walks, alive)."""

CUES = [(0.1, 'unit_enter_vehicle (unit (list_get (players) 0)) car "stand"')]
PLAY = dict(seconds=6, input="script:0.5 aim=0,0; 1-6 move=0,1", cues=CUES)


def test_drives_the_car(play):
    session = play("drive")
    places = session.places()
    assert session.deaths() == 0
    assert len(places) == len(session.seconds), places
    # (a second after the throttle, a little way; by the fifth, more than 20 units along x, as a biped never goes)
    assert places[2][0] > 1.0, places
    assert places[5][0] > 20.0, places
    assert all(abs(y) < 0.5 for _, y, _ in places), places
