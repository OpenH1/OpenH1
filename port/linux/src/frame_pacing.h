/*
FRAME_PACING.H

When the game may start its next frame, and how long its input takes to
reach the screen: one policy for every renderer, the command stream's host
(port/android/host/host_render_stream.c, the Metal backend first) and the
renderer inside the game (xgpu_gl.c through d3d8_device.c).

A frame's input is read when the game starts it: D3DDevice_Present pumps
the window's events after the frame before has gone to the renderer, and
the main loop's input_update reads them at once (source/main/main.c). So
the game should start a frame as late as it can and still have it on
screen at the next refresh: every frame queued before it is a refresh
more between the player's hand and the picture.

- The time from that read to the frame on screen (the display's own time
  for it) is kept for each frame: frame_pacing_latency_add, and its median
  and 95th percentile since the last look, frame_pacing_latency_take.
- The game starts a frame only while at most FRAME_PACING_QUEUED of its
  frames are finished and not yet taken by the display (the one on screen
  not counted): frame_pacing_may_start. With 1, the display has the next
  frame ready at each refresh while the game writes the one after.
- And not before frame_pacing_start_time: a display that says when it
  latches each refresh's frame (the last moment a frame can still be taken
  for it) and how far apart its refreshes are has each frame started its
  budget before the latch it is for, the one after those its frames
  already wait for. The budget is the most a frame took from its start to
  ready for the display (its renderer done) of the last FRAME_PACING_COSTS,
  and FRAME_PACING_MARGIN_NS more: frame_pacing_cost_add,
  frame_pacing_budget.
- Battery saver (display.battery_saver, battery_saver.c) shows each frame
  for a whole number of the display's refreshes, each on screen as long as
  the others, so that the game draws BATTERY_SAVER_MOST frames a second or
  fewer, but never fewer than BATTERY_SAVER_LEAST: frame_pacing_divisor.
  At least 30, so that every one of the game's 30 ticks a second is drawn;
  at most 40, half or less of the work at 60 Hz and more (30 at 60 or 90 Hz,
  40 at 80, 120 or 240 Hz, 36 at 144). A display no whole number brings
  between them keeps the least above 30 it can (42.5 at 85 Hz), and one
  below 60 Hz its own rate.

The times are nanoseconds of the game's monotonic clock (CLOCK_MONOTONIC
as the game reads it; a guest's is its host's). Header only, C89, with no
other header of the port's: the guests, the hosts and the native builds
include it as it is.
*/

#ifndef FRAME_PACING_H
#define FRAME_PACING_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* finished frames that may wait for the display while the game writes the
next (the one on screen not counted) */
#define FRAME_PACING_QUEUED 1
/* the longest the game waits for the display to take a frame: a display
that takes none (a window covered, minimized, between the desktop and
fullscreen) must not stop the game */
#define FRAME_PACING_WAIT_MAXIMUM_NS 100000000ull
/* the frames whose start to ready times the budget is the most of, and the
time added to it */
#define FRAME_PACING_COSTS 128
#define FRAME_PACING_MARGIN_NS 1500000ull
/* the input to screen times kept between looks */
#define FRAME_PACING_SAMPLES 1024
/* battery saver's frames a second, at least and at most (frame_pacing_divisor) */
#define BATTERY_SAVER_LEAST 30
#define BATTERY_SAVER_MOST 40

/* (the game's renderer front end, d3d8_device.c: not in the hosts) when the
game read the input of the frame it draws, as D3DDevice_Present stamped it */
uint64_t d3d_frame_input_time(void);

struct frame_pacing_latency
{
	/* (written by the display's thread, taken by the logging one) */
	uint32_t count;
	float milliseconds[FRAME_PACING_SAMPLES];
};

struct frame_pacing_costs
{
	uint64_t nanoseconds[FRAME_PACING_COSTS];
	uint32_t next;
};

#if defined(__GNUC__) || defined(__clang__)
#define FRAME_PACING_UNUSED __attribute__((unused))
#else
#define FRAME_PACING_UNUSED
#endif

/* whether the game may start a frame: submitted, the frames it has given
the renderer; taken, those the display has taken (shown, or dropped) */
static FRAME_PACING_UNUSED int frame_pacing_may_start(uint32_t submitted, uint32_t taken, uint32_t queued)
{
	return (int32_t)(submitted - taken) <= (int32_t)queued;
}

/* a frame ready for the display: the time from its start (its input read)
to then */
static FRAME_PACING_UNUSED void frame_pacing_cost_add(struct frame_pacing_costs *costs, uint64_t start, uint64_t ready)
{
	if (!start || ready <= start || ready - start > 1000000000ull)
		return;
	costs->nanoseconds[costs->next++ % FRAME_PACING_COSTS] = ready - start;
}

/* the time to give a frame: the most of the last FRAME_PACING_COSTS, and
the margin; but not more than a refresh and a half (period): a frame that
took longer missed its refresh whenever it started, and an earlier start
for every frame after it would only add to their latency */
static FRAME_PACING_UNUSED uint64_t frame_pacing_budget(const struct frame_pacing_costs *costs, uint64_t period)
{
	uint64_t most = 0;
	uint32_t index;

	for (index = 0; index < FRAME_PACING_COSTS; index++)
	{
		if (costs->nanoseconds[index] > most && (!period || costs->nanoseconds[index] <= period * 3 / 2))
			most = costs->nanoseconds[index];
	}
	return most + FRAME_PACING_MARGIN_NS;
}

/* when the game should start its next frame: latch, the display's last
latch (passed); period, the time between refreshes; pending, the game's
frames not yet taken, which the latches after it take first (0 latch: the
display gives none, at once) */
static FRAME_PACING_UNUSED uint64_t frame_pacing_start_time(uint64_t latch, uint64_t period, uint32_t pending,
	uint64_t budget)
{
	uint64_t target;

	if (!latch || !period)
		return 0;
	target = latch + (uint64_t)(pending + 1) * period;
	return target > budget ? target - budget : 0;
}

/* the refreshes each frame is shown for, at a display's refresh rate (frames
a second; 0 or less: unknown, taken as 60): the fewest that bring the frames
a second to most or below, but not so many that they fall below least; 1
where the display's own rate is no more than most. (A rate a little off a
whole multiple, 59.94 Hz, counts as the multiple.) */
static FRAME_PACING_UNUSED uint32_t frame_pacing_divisor(float refresh_rate, uint32_t least, uint32_t most)
{
	float faster, slower;
	uint32_t divisor, longest;

	if (refresh_rate <= 0.0f)
		refresh_rate = 60.0f;
	if (!most || refresh_rate <= (float)most + 0.5f)
		return 1;
	/* (the fewest refreshes that bring it to most or below, and the most
	that keep it at least or above) */
	faster = refresh_rate / (float)most - 0.05f;
	divisor = (uint32_t)faster;
	if ((float)divisor < faster)
		divisor++;
	slower = least ? refresh_rate / (float)least + 0.05f : (float)divisor;
	longest = (uint32_t)slower;
	if (divisor > longest)
		divisor = longest;
	return divisor ? divisor : 1;
}

/* a frame on screen: the time its input was read and the time it was shown
(0 either: unknown, not counted) */
static FRAME_PACING_UNUSED void frame_pacing_latency_add(struct frame_pacing_latency *latency, uint64_t input,
	uint64_t shown)
{
	uint32_t sample;

	if (!input || !shown || shown <= input || shown - input > 1000000000ull)
		return;
	sample = __atomic_fetch_add(&latency->count, 1, __ATOMIC_RELAXED);
	if (sample < FRAME_PACING_SAMPLES)
		latency->milliseconds[sample] = (float)((double)(shown - input) / 1e6);
}

static FRAME_PACING_UNUSED int frame_pacing_float_compare(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;

	return x < y ? -1 : x > y;
}

/* the times added since the last look: their median and 95th percentile in
milliseconds; returns how many (0: none, and the values 0) */
static FRAME_PACING_UNUSED uint32_t frame_pacing_latency_take(struct frame_pacing_latency *latency, float *median,
	float *high)
{
	uint32_t count = __atomic_exchange_n(&latency->count, 0, __ATOMIC_RELAXED);
	float sorted[FRAME_PACING_SAMPLES];

	*median = *high = 0.0f;
	if (count > FRAME_PACING_SAMPLES)
		count = FRAME_PACING_SAMPLES;
	if (!count)
		return 0;
	memcpy(sorted, latency->milliseconds, sizeof(float) * count);
	qsort(sorted, count, sizeof(float), frame_pacing_float_compare);
	*median = sorted[count / 2];
	*high = sorted[count * 95 / 100];
	return count;
}

#endif
