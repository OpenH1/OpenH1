/* Diagnostic GPU readbacks, never enabled during presentation timing runs. */
#ifndef HOST_LOADING_CAPTURE_H
#define HOST_LOADING_CAPTURE_H

#include "loading_presentation.h"
#include "host_load_trace.h"
#include <stdio.h>
#include <stdlib.h>

static const char *host_loading_capture_directory;
static uint64_t host_loading_capture_epoch;
static unsigned host_loading_capture_count;
/* HALO_LOADING_CAPTURE_EVERY=<frames>: every that many frames of a load,
not only its first and one half a second in */
static unsigned host_loading_capture_every;

static int host_loading_capture_due(const struct loading_presentation_frame *frame)
{
	static int checked;

	if (!checked)
	{
		const char *every = getenv("HALO_LOADING_CAPTURE_EVERY");

		host_loading_capture_directory = getenv("HALO_LOADING_CAPTURE_DIR");
		host_loading_capture_every = every ? (unsigned)strtoul(every, NULL, 10) : 0;
		checked = 1;
	}
	if (!host_loading_capture_directory || !*host_loading_capture_directory)
		return 0;
	if (host_loading_capture_epoch != frame->epoch)
	{
		host_loading_capture_epoch = frame->epoch;
		host_loading_capture_count = 0;
	}
	if (host_loading_capture_every)
		return frame->serial % host_loading_capture_every == 0;
	return host_loading_capture_count == 0 ? frame->serial >= 2 :
		host_loading_capture_count == 1 && frame->elapsed_ns >= 500000000;
}

static void host_loading_capture_write(const struct loading_presentation_frame *frame,
	const unsigned char *pixels, uint32_t width, uint32_t height, int bottom_up, int bgra)
{
	char path[1024];
	FILE *file;
	uint32_t x, y;
	unsigned char *row;

	host_loading_capture_count++;
	if (snprintf(path, sizeof(path), "%s/loading-%llu-%llu.ppm", host_loading_capture_directory,
		(unsigned long long)frame->epoch, (unsigned long long)frame->serial) >= (int)sizeof(path))
		return;
	row = malloc((size_t)width * 3);
	if (!row)
		return;
	file = fopen(path, "wb");
	if (file)
	{
		fprintf(file, "P6\n%u %u\n255\n", width, height);
		for (y = 0; y < height; y++)
		{
			const unsigned char *source = pixels + (size_t)(bottom_up ? height - 1 - y : y) * width * 4;

			for (x = 0; x < width; x++)
			{
				row[x * 3] = source[x * 4 + (bgra ? 2 : 0)];
				row[x * 3 + 1] = source[x * 4 + 1];
				row[x * 3 + 2] = source[x * 4 + (bgra ? 0 : 2)];
			}
			fwrite(row, 3, width, file);
		}
		fclose(file);
		host_load_trace("loading.capture", "epoch=%llu serial=%llu path=%s",
			(unsigned long long)frame->epoch, (unsigned long long)frame->serial, path);
	}
	free(row);
}

#endif
