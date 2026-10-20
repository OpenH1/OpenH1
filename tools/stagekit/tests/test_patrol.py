"""scenarios/patrol.c: an AI's biped beside the player; half a second in the console makes an actor of it, then has
it walk its command lists one after another (3 units along x, 2 at 25 degrees, 2 along x). The scorecard's
ai_patrol scenario measures its path at every rate (debug.trace's AI lines)."""

CUES = [(0.5, "ai_attach_free walker walker"), (1, "ai_command_list_by_unit walker leg1"),
        (4, "ai_command_list_by_unit walker leg2"), (7, "ai_command_list_by_unit walker leg3")]
PLAY = dict(seconds=8, input="look:1", cues=CUES)


def test_the_console_finds_the_actor_and_its_list(play):
    """the actor made and its list run: nothing the console could not find or do"""
    session = play("patrol")
    for complaint in ("not a valid", "cannot execute command list", "no such", "unknown"):
        assert complaint not in session.debug.lower(), session.debug
