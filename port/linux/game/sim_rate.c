/*
SIM_RATE.C

The simulation's two clocks (sim_rate.h): game.sim_rate, the rate a tick
takes, and the fast phase's sub-steps inside each tick (game.c's
game_tick_substep, after the objects' update). The clock (game_time.c)
runs each sub-step when its time has come, so a tick's sub-steps are
spread over the frames that pass meanwhile; here is which is next, and
whether the frame's input was read since the last.

The rate is taken at each tick's start and holds for the tick. It is 30,
whatever game.sim_rate says, in a game that must stay as it was:

- A network game, hosting or joining: the netcode is OpenCE's (NETCODE.md),
  whose machines each simulate the players from the same input, so all of
  them must step alike, and OpenCE steps at 30. A protocol of OpenH1's own
  could agree on a rate with every machine before the game starts; it would
  say so in sim_rate_network (below), the one place that decides it. (A
  split screen game, and so the bots' games, is a network game of one
  machine, its own server's: 30 too, for now.)
- A saved film's playback (game_connection's film playback), which replays
  a recording's input tick by tick.

A game saved is the game state at a tick's end, which nothing of a sub-step
outlives (sim_rate.h), so it is the same at every rate and loads at any.
The determinism check (debug.determinism_log) writes the rate it ran at,
and tools/determinism_compare.py compares two runs only at one rate unless
asked to compare across rates: a run to compare against is made at 30.

120 rather than 128: it is four times 30, so a tick is a whole number of
sub-steps, a sub-step's share of a tick (1/4) and its square are powers of
two (the linear and quadratic conversions scale exactly), a duration in
ticks is a whole number of sub-steps, and a display at 120 Hz has one
sub-step a frame.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "game/game.h"
#include "sim_rate.h"

/* port/linux/src/port_config.c */
long config_integer(char const *name);
unsigned long config_changes(void);

/* ---------- globals */

static struct
{
	/* game.sim_rate as read (30, 60 or 120) */
	unsigned long read_at;
	long setting;
	boolean read;
	/* this tick's */
	long rate;
	short substep_count;
	short substep_index;
	/* the open tick's next sub-step (0: none open) */
	short substep_next;
	/* no sub-step has run since the frame's input was read, and the running
	one is the first since */
	boolean frame_fresh;
	boolean first_of_frame;
	sim_substep_procedure procedures[NUMBER_OF_SIM_SUBSTEP_SLOTS];
} sim_rate_globals = { 0, SIM_RATE_CLASSIC, FALSE, SIM_RATE_CLASSIC, 1, NONE, 0, TRUE, FALSE };

/* ---------- private code */

/* game.sim_rate: 30, 60 or 120, anything else 30 */
static long sim_rate_setting(
	void)
{
	if (!sim_rate_globals.read || sim_rate_globals.read_at != config_changes())
	{
		long setting = config_integer("game.sim_rate");

		sim_rate_globals.read = TRUE;
		sim_rate_globals.read_at = config_changes();
		sim_rate_globals.setting = (setting == 60 || setting == 120) ? setting : SIM_RATE_CLASSIC;
	}
	return sim_rate_globals.setting;
}

/* the rate of a network game: the one every machine of it agreed to. The
classic netcode (OpenCE's, which this one plays with) agrees on none, so
30. A protocol of OpenH1's own would bring the rate the host chose and every
machine took (and only one every machine can run) here, set before the
game's first tick and the same for its whole length. */
static long sim_rate_network(
	void)
{
	return SIM_RATE_CLASSIC;
}

/* the rate a tick begun now takes */
static long sim_rate_for_connection(
	void)
{
	switch (game_connection())
	{
	case _game_connection_local:
		return sim_rate_setting();
	case _game_connection_network_client:
	case _game_connection_network_server:
		return sim_rate_network();
	default:
		return SIM_RATE_CLASSIC;
	}
}

/* a root of a factor, its count-th: exactly the factor at 30, and with
correctly rounded square roots, the same on every machine (no pow) */
static real sim_root(
	real factor)
{
	short count = sim_rate_globals.substep_count;

	if (count >= 2)
		factor = square_root(factor);
	if (count >= 4)
		factor = square_root(factor);
	return factor;
}

/* ---------- public code */

long sim_rate_get(
	void)
{
	return sim_rate_globals.rate;
}

short sim_substep_count(
	void)
{
	return sim_rate_globals.substep_count;
}

short sim_substep_index(
	void)
{
	return sim_rate_globals.substep_index;
}

boolean sim_substep_is_last(
	void)
{
	return sim_rate_globals.substep_index == NONE ||
		sim_rate_globals.substep_index == sim_rate_globals.substep_count - 1;
}

real sim_substep_seconds(
	void)
{
	return 1.0f / (real)sim_rate_globals.rate;
}

real sim_substep_tick_fraction(
	void)
{
	return 1.0f / (real)sim_rate_globals.substep_count;
}

real sim_substep_end_fraction(
	void)
{
	if (sim_rate_globals.substep_index == NONE)
		return 1.0f;
	return (real)(sim_rate_globals.substep_index + 1) / (real)sim_rate_globals.substep_count;
}

real sim_per_substep(
	real per_tick)
{
	return per_tick / (real)sim_rate_globals.substep_count;
}

real sim_per_substep_squared(
	real per_tick_squared)
{
	short count = sim_rate_globals.substep_count;

	return per_tick_squared / (real)(count * count);
}

real sim_decay_per_substep(
	real factor_per_tick)
{
	/* (not a number, or outside 0 to 1, as it is) */
	if (!(factor_per_tick >= 0.0f && factor_per_tick <= 1.0f))
		return factor_per_tick;
	return sim_root(factor_per_tick);
}

real sim_chance_per_substep(
	real chance_per_tick)
{
	if (sim_rate_globals.substep_count == 1 || !(chance_per_tick >= 0.0f && chance_per_tick <= 1.0f))
		return chance_per_tick;
	return 1.0f - sim_root(1.0f - chance_per_tick);
}

long sim_ticks_to_substeps(
	long ticks)
{
	return ticks * sim_rate_globals.substep_count;
}

void sim_substep_register(
	short slot,
	sim_substep_procedure procedure)
{
	if (slot >= 0 && slot < NUMBER_OF_SIM_SUBSTEP_SLOTS)
		sim_rate_globals.procedures[slot] = procedure;
}

void sim_substep_blend(
	real tick_fraction,
	short *substep_index,
	real *substep_fraction)
{
	short count = sim_rate_globals.substep_count;
	real scaled;
	short index;

	if (!(tick_fraction >= 0.0f))
		tick_fraction = 0.0f;
	if (tick_fraction > 1.0f)
		tick_fraction = 1.0f;
	scaled = tick_fraction * (real)count;
	index = (short)scaled;
	if (index > count - 1)
		index = count - 1;
	*substep_index = index;
	*substep_fraction = scaled - (real)index;
}

short sim_substep_next(
	void)
{
	return sim_rate_globals.substep_next;
}

boolean sim_substep_first_of_frame(
	void)
{
	return sim_rate_globals.first_of_frame;
}

long sim_rate_next(
	void)
{
	return sim_rate_globals.substep_next ? sim_rate_globals.rate : sim_rate_for_connection();
}

void sim_rate_frame_begin(
	void)
{
	sim_rate_globals.frame_fresh = TRUE;
}

void sim_rate_tick_drop(
	void)
{
	sim_rate_globals.substep_index = NONE;
	sim_rate_globals.substep_next = 0;
}

void sim_rate_tick_begin(
	void)
{
	long rate = sim_rate_for_connection();

	sim_rate_globals.rate = rate;
	sim_rate_globals.substep_count = (short)(rate / SIM_RATE_CLASSIC);
	sim_rate_globals.substep_index = NONE;
	sim_rate_globals.substep_next = 0;
}

void sim_substep_run(
	short substep_index)
{
	real seconds = sim_substep_seconds();
	short count = sim_rate_globals.substep_count;
	short slot;

	sim_rate_globals.first_of_frame = sim_rate_globals.frame_fresh;
	sim_rate_globals.frame_fresh = FALSE;
	sim_rate_globals.substep_index = substep_index;
	for (slot = 0; slot < NUMBER_OF_SIM_SUBSTEP_SLOTS; slot++)
	{
		if (sim_rate_globals.procedures[slot])
			sim_rate_globals.procedures[slot](substep_index, count, seconds);
	}
	sim_rate_globals.substep_index = NONE;
	sim_rate_globals.first_of_frame = FALSE;
	/* (the tick open until its last sub-step has run) */
	sim_rate_globals.substep_next = substep_index + 1 < count ? substep_index + 1 : 0;
}
