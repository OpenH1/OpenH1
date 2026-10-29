/* The client's tick the host's tick runs, for a player whose input and
prediction it takes as the tick begins (distributed_host_run_time): the input
heard, or the tick after the prediction taken where that is later. */
#include "harness.h"
enum { MAXIMUM_TRACKED_PLAYERS = 4, TICKS_PER_SECOND = 30 };
static long input_key;
static boolean at_tick_start = TRUE;
static long update_queues_distributed_input_key(long player_index) { (void)player_index; return input_key; }
static boolean distributed_prediction_at_tick_start(short player_index) { (void)player_index; return at_tick_start; }
static long distributed_host_run_times[MAXIMUM_TRACKED_PLAYERS];
#include "under_test.inc"
int main(int argc, char **argv) {
    char const *name = argc > 1 ? argv[1] : "";
    long expected;
    /* (the prediction of the end of the client's tick 29, heard before the
    input of tick 30, which follows it by a sub-step: the tick runs 30) */
    input_key = 29; distributed_host_run_times[1] = 30; expected = 30;
    if (!strcmp(name, "input-ahead")) { input_key = 31; expected = 31; }
    else if (!strcmp(name, "same")) { input_key = 30; expected = 30; }
    else if (!strcmp(name, "no-input")) { input_key = NONE; expected = 30; }
    else if (!strcmp(name, "no-prediction")) { distributed_host_run_times[1] = NONE; expected = 29; }
    /* (a prediction's time far past the input: the client's game begun again) */
    else if (!strcmp(name, "far")) { distributed_host_run_times[1] = 29 + TICKS_PER_SECOND + 1; expected = 29; }
    /* (a player whose prediction the host takes as its tick ends: the input) */
    else if (!strcmp(name, "at-tick-end")) { at_tick_start = FALSE; expected = 29; }
    else if (strcmp(name, "prediction-ahead")) { fprintf(stderr, "no case %s\n", name); return 2; }
    CHECK(distributed_host_run_time(1) == expected, "%s: ran %ld, not %ld", name, distributed_host_run_time(1),
        expected);
    CHECK(distributed_host_run_time(MAXIMUM_TRACKED_PLAYERS) == NONE, "a player out of range ran a tick");
    return 0;
}
