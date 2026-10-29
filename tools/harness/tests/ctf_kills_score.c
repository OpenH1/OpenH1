/*
CTF_KILLS_SCORE.C (test)

Capture the Flag's Kills Score (source/game/game_engine_ctf.c, OpenCE's
#467): with the variant's kills_score on, a kill scores a point for the
killer's team and a betrayal takes one away, a capture is worth the
variant's points_per_capture (25 where it keeps none), and a player's score
is their kills less their betrayals and their captures' points; off, only
captures score, one each, as January's. A client of the distributed
netcode, a player who quit and a game that no longer scores add nothing.
test_ctf_kills_score.py takes the constants (ctf_types.inc) and the code
(under_test.inc).
*/

#include "harness.h"

#define VALID_INDEX(index, count) ((index) >= 0 && (index) < (count))

#include "ctf_types.inc"

enum get_score_type { _get_score_individual = 0, _get_score_team };

/* what the code reads of a player and the variant */
struct player_datum
{
	long team_index;
	boolean quit_out_of_game;
	struct
	{
		short kills[4];
		short friendly_fire_kills;
		struct { struct { short flag_scores; } ctf_statistics; } multiplayer_statistics;
	} statistics;
};
static struct
{
	struct { struct { long points_per_capture; long kills_score; } ctf; } game_engine_variant;
} variant;
static struct { long scores[NUMBER_OF_CTF_TEAMS]; } ctf_globals;
static struct player_datum players[4];
static boolean distributed_client, can_score = TRUE;

#define game_engine_get_variant() (&variant)
#define player_get(index) (&players[index])
#define network_game_distributed_client() (distributed_client)
#define game_engine_can_score() (can_score)

#include "under_test.inc"

enum { RED_PLAYER, RED_TEAMMATE, BLUE_PLAYER, QUITTER };

static void new_game(long kills_score, long points_per_capture)
{
	memset(players, 0, sizeof(players));
	memset(&ctf_globals, 0, sizeof(ctf_globals));
	players[RED_PLAYER].team_index = 0;
	players[RED_TEAMMATE].team_index = 0;
	players[BLUE_PLAYER].team_index = 1;
	players[QUITTER].team_index = 1;
	players[QUITTER].quit_out_of_game = TRUE;
	variant.game_engine_variant.ctf.kills_score = kills_score;
	variant.game_engine_variant.ctf.points_per_capture = points_per_capture;
	distributed_client = FALSE;
	can_score = TRUE;
}

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";

	CASE("off")
	{
		/* January's Capture the Flag: kills score nothing, a capture one */
		new_game(FALSE, 25);
		ctf_engine_player_killed_player(RED_PLAYER, 0, BLUE_PLAYER, FALSE);
		CHECK(ctf_globals.scores[0] == 0 && ctf_globals.scores[1] == 0, "a kill scored %ld and %ld",
			ctf_globals.scores[0], ctf_globals.scores[1]);
		CHECK(ctf_capture_points() == 1, "a capture is worth %ld", ctf_capture_points());
		players[RED_PLAYER].statistics.kills[0] = 6;
		players[RED_PLAYER].statistics.multiplayer_statistics.ctf_statistics.flag_scores = 2;
		CHECK(ctf_get_score(RED_PLAYER, _get_score_individual) == 2, "the player's score is %ld, not their 2 captures",
			ctf_get_score(RED_PLAYER, _get_score_individual));
		return 0;
	}
	CASE("kills-score")
	{
		new_game(TRUE, 25);
		ctf_engine_player_killed_player(RED_PLAYER, 0, BLUE_PLAYER, FALSE);
		ctf_engine_player_killed_player(RED_TEAMMATE, 0, BLUE_PLAYER, FALSE);
		ctf_engine_player_killed_player(BLUE_PLAYER, 0, RED_PLAYER, FALSE);
		CHECK(ctf_globals.scores[0] == 2 * CTF_KILL_POINTS && ctf_globals.scores[1] == CTF_KILL_POINTS,
			"three kills scored %ld and %ld", ctf_globals.scores[0], ctf_globals.scores[1]);
		/* a betrayal takes a point away */
		ctf_engine_player_killed_player(RED_PLAYER, 0, RED_TEAMMATE, TRUE);
		CHECK(ctf_globals.scores[0] == CTF_KILL_POINTS, "after a betrayal red has %ld", ctf_globals.scores[0]);
		/* a quitter's death, a death of no player's, a client's copy, a game over: nothing */
		ctf_engine_player_killed_player(RED_PLAYER, 0, QUITTER, FALSE);
		ctf_engine_player_killed_player(NONE, 0, BLUE_PLAYER, FALSE);
		distributed_client = TRUE;
		ctf_engine_player_killed_player(RED_PLAYER, 0, BLUE_PLAYER, FALSE);
		distributed_client = FALSE;
		CHECK(!distributed_client, "still a client");
		can_score = FALSE;
		ctf_engine_player_killed_player(RED_PLAYER, 0, BLUE_PLAYER, FALSE);
		CHECK(ctf_globals.scores[0] == CTF_KILL_POINTS && ctf_globals.scores[1] == CTF_KILL_POINTS,
			"kills that score nothing scored: %ld and %ld", ctf_globals.scores[0], ctf_globals.scores[1]);
		return 0;
	}
	CASE("capture-points")
	{
		new_game(TRUE, 10);
		CHECK(ctf_capture_points() == 10, "a capture is worth %ld, not 10", ctf_capture_points());
		new_game(TRUE, 100);
		CHECK(ctf_capture_points() == 100, "a capture is worth %ld, not 100", ctf_capture_points());
		/* a gametype that keeps none (one of a build without Kills Score) */
		new_game(TRUE, 0);
		CHECK(ctf_capture_points() == CTF_DEFAULT_CAPTURE_POINTS && CTF_DEFAULT_CAPTURE_POINTS == 25,
			"a capture of no points kept is worth %ld", ctf_capture_points());
		new_game(FALSE, 10);
		CHECK(ctf_capture_points() == 1, "with Kills Score off a capture is worth %ld", ctf_capture_points());
		return 0;
	}
	CASE("player-points")
	{
		/* 7 kills, 2 betrayals, 3 captures at 10: 35 */
		new_game(TRUE, 10);
		players[RED_PLAYER].statistics.kills[0] = 7;
		players[RED_PLAYER].statistics.friendly_fire_kills = 2;
		players[RED_PLAYER].statistics.multiplayer_statistics.ctf_statistics.flag_scores = 3;
		CHECK(ctf_get_score(RED_PLAYER, _get_score_individual) == 35, "the player's score is %ld, not 35",
			ctf_get_score(RED_PLAYER, _get_score_individual));
		ctf_globals.scores[0] = 41;
		CHECK(ctf_get_score(RED_PLAYER, _get_score_team) == 41, "the team's score is %ld",
			ctf_get_score(RED_PLAYER, _get_score_team));
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
