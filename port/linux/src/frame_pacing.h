/*
FRAME_PACING.H

How long the game's input takes to reach the screen: the same measure for
every renderer, the command stream's host (port/android/host/
host_render_stream.c, the Metal backend first) and the renderer inside the
game (xgpu_gl.c through d3d8_device.c).

A frame's input is read when the game starts it: D3DDevice_Present pumps
the window's events after the frame before has gone to the renderer, and
the main loop's input_update reads them at once (source/main/main.c). The
time from that read to the frame on screen (the display's own time for it)
is kept for each frame: frame_pacing_latency_add, and its median and 95th
percentile since the last look, frame_pacing_latency_take.

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

/* the input to screen times kept between looks */
#define FRAME_PACING_SAMPLES 1024

/* (the game's renderer front end, d3d8_device.c: not in the hosts) when the
game read the input of the frame it draws, as D3DDevice_Present stamped it */
uint64_t d3d_frame_input_time(void);

struct frame_pacing_latency
{
	/* (written by the display's thread, taken by the logging one) */
	uint32_t count;
	float milliseconds[FRAME_PACING_SAMPLES];
};

#if defined(__GNUC__) || defined(__clang__)
#define FRAME_PACING_UNUSED __attribute__((unused))
#else
#define FRAME_PACING_UNUSED
#endif

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
