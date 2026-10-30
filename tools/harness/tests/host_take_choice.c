/* What a host's tick that begins takes of a client's player's predictions
(distributed_host_take_choice): the latest, the one before it whose input
has come, none where the prediction of the input heard was lost, or none for
a tick where the input heard ran the tick before and the latest is ahead of
it (the phase in which the prediction comes before its tick's input). */
#include "harness.h"
enum { MAXIMUM_TRACKED_PLAYERS = 4, TICKS_PER_SECOND = 30 };
#include "config.inc"
static struct
{
	long last_run[MAXIMUM_TRACKED_PLAYERS];
	boolean held[MAXIMUM_TRACKED_PLAYERS];
} distributed_host_takes;
#include "under_test.inc"

static char const *names[NUMBER_OF_DISTRIBUTED_TAKES] = { "none", "latest", "late", "earlier", "lost", "hold" };

static void expect(char const *name, short take, short expected)
{
	CHECK(take == expected, "%s: took %s, not %s", name, take >= 0 && take < NUMBER_OF_DISTRIBUTED_TAKES ?
		names[take] : "?", names[expected]);
}

int main(int argc, char **argv) {
	char const *name = argc > 1 ? argv[1] : "";
	short tick;
	short take;

	/* (the input heard 30, its tick before ran 30; the latest heard 31) */
	distributed_host_takes.last_run[1] = 30;
	if (!strcmp(name, "caught-up"))
	{
		expect(name, distributed_host_take_choice(1, 30, 30, FALSE, NONE), _distributed_take_latest);
		expect(name, distributed_host_take_choice(1, 31, 30, FALSE, NONE), _distributed_take_latest);
	}
	else if (!strcmp(name, "no-input"))
		expect(name, distributed_host_take_choice(1, NONE, 31, FALSE, NONE), _distributed_take_latest);
	else if (!strcmp(name, "far"))
		expect(name, distributed_host_take_choice(1, 30, 30 + TICKS_PER_SECOND + 1, FALSE, NONE),
			_distributed_take_latest);
	else if (!strcmp(name, "earlier"))
		expect(name, distributed_host_take_choice(1, 30, 31, TRUE, 30), _distributed_take_earlier);
	else if (!strcmp(name, "lost"))
		expect(name, distributed_host_take_choice(1, 30, 31, TRUE, 29), _distributed_take_lost);
	else if (!strcmp(name, "earlier-ahead"))
	{
		/* (one before it that is ahead of the input heard too: not taken) */
		distributed_host_takes.last_run[1] = 29;
		expect(name, distributed_host_take_choice(1, 30, 32, TRUE, 31), _distributed_take_late);
	}
	else if (!strcmp(name, "hold"))
		expect(name, distributed_host_take_choice(1, 30, 31, FALSE, NONE), _distributed_take_hold);
	else if (!strcmp(name, "held-once"))
	{
		distributed_host_takes.held[1] = TRUE;
		expect(name, distributed_host_take_choice(1, 30, 31, FALSE, NONE), _distributed_take_late);
	}
	else if (!strcmp(name, "late-new-input"))
	{
		/* (the input heard not run yet: its tick runs, from the latest) */
		distributed_host_takes.last_run[1] = 29;
		expect(name, distributed_host_take_choice(1, 30, 31, FALSE, NONE), _distributed_take_late);
	}
	else if (!strcmp(name, "out-of-range"))
		expect(name, distributed_host_take_choice(MAXIMUM_TRACKED_PLAYERS, 30, 31, FALSE, NONE),
			_distributed_take_latest);
	else if (!strcmp(name, "steady-late"))
	{
		/* (each prediction comes before its tick's input, a tick's input
		and the next's prediction between two of the host's ticks. Held
		once; then each tick the one before it, of the input heard) */
		long input = 30, heard = 31;
		boolean earlier_valid = FALSE;
		long earlier = NONE;
		boolean heard_valid = TRUE;
		short held = 0;

		for (tick = 0; tick < 20; tick++)
		{
			take = distributed_host_take_choice(1, input, heard, earlier_valid, earlier);
			held += take == _distributed_take_hold;
			CHECK(take == _distributed_take_hold || take == _distributed_take_earlier ||
				(tick == 0 && take == _distributed_take_late), "steady-late: tick %d took %s", tick, names[take]);
			if (take == _distributed_take_late)
			{
				heard_valid = FALSE;
				distributed_host_takes.last_run[1] = heard;
			}
			else if (take == _distributed_take_earlier)
				distributed_host_takes.last_run[1] = input;
			distributed_host_takes.held[1] = take == _distributed_take_hold;
			earlier_valid = FALSE;
			/* (the input of the latest heard, then the next's prediction) */
			input = heard;
			if (heard_valid) { earlier_valid = TRUE; earlier = heard; }
			heard++;
			heard_valid = TRUE;
		}
		CHECK(held == 1, "steady-late: held %d ticks, not 1", held);
	}
	else { fprintf(stderr, "no case %s\n", name); return 2; }
	return 0;
}
