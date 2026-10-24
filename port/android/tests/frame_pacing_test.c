/*
FRAME_PACING_TEST.C

Tests of battery saver's frames a second (port/linux/src/frame_pacing.h,
frame_pacing_divisor): the refreshes each frame is shown for at a display's
rate. Built and run by tools/test_host_units.py; exits nonzero on a failure.
*/

#include "frame_pacing.h"

#include <stdio.h>

static int failures;

static void check(float refresh_rate, uint32_t expected)
{
	uint32_t divisor = frame_pacing_divisor(refresh_rate, BATTERY_SAVER_LEAST, BATTERY_SAVER_MOST);

	if (divisor != expected)
	{
		printf("FAIL %g Hz: a frame every %u refreshes, not %u\n", (double)refresh_rate, divisor, expected);
		failures++;
	}
}

int main(void)
{
	float rate;

	/* (the common displays: between 30 and 60 frames a second) */
	check(60.0f, 1);
	check(59.94f, 1);
	check(90.0f, 2);
	check(120.0f, 2);
	check(119.88f, 2);
	check(144.0f, 3);
	check(165.0f, 3);
	check(240.0f, 4);
	check(80.0f, 2);
	check(75.0f, 2);
	check(100.0f, 2);
	check(360.0f, 6);
	/* (a display at or below 60 Hz keeps its rate) */
	check(40.0f, 1);
	check(30.0f, 1);
	check(24.0f, 1);
	check(48.0f, 1);
	check(50.0f, 1);
	/* (unknown: taken as 60 Hz) */
	check(0.0f, 1);
	check(-1.0f, 1);
	/* (no most: every refresh) */
	if (frame_pacing_divisor(144.0f, BATTERY_SAVER_LEAST, 0) != 1)
	{
		printf("FAIL no most\n");
		failures++;
	}
	check(85.0f, 2);
	/* (never below the least, and above the most only where one refresh
	more would be below the least, or within frame_pacing_divisor's
	allowance of 5% over it: 62 Hz is a 60 Hz display's rate, not one to
	halve to 31) */
	for (rate = 41.0f; rate <= 360.0f; rate += 0.5f)
	{
		uint32_t divisor = frame_pacing_divisor(rate, BATTERY_SAVER_LEAST, BATTERY_SAVER_MOST);
		float shown = rate / (float)divisor;

		if (shown > (float)BATTERY_SAVER_MOST * 1.06f && rate / (float)(divisor + 1) >= (float)BATTERY_SAVER_LEAST)
		{
			printf("FAIL %g Hz: %g frames a second\n", (double)rate, (double)shown);
			failures++;
		}
		if (rate >= 60.0f && shown < (float)BATTERY_SAVER_LEAST - 1.0f)
		{
			printf("FAIL %g Hz: %g frames a second\n", (double)rate, (double)shown);
			failures++;
		}
	}
	if (failures)
		return 1;
	printf("PASS\n");
	return 0;
}
