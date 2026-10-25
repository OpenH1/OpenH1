/*
HOST_SCREENSHOT.C

The screenshot key's files (controls.screenshot): the frame the guest asks
for (XGPU_STREAM_SCREENSHOT, which host_render_stream.c reads from the
backend, tone mapped) written as a PNG with SDL into the folder the guest
names (its data folder's screenshots), named for the local date and time,
YYYY-MM-DD_HH.MM.SS.png; another in the same second YYYY-MM-DD_HH.MM.SS_2.png,
_3 and on, so that none is written over.
*/

#include "host_render_stream.h"

#include <SDL3/SDL.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* (pictures in one second, at most) */
#define SCREENSHOT_COLLISIONS 1000

void host_screenshot_name(char *name, uint32_t name_size, int year, int month, int day, int hour, int minute,
	int second, unsigned collision)
{
	if (collision)
		snprintf(name, name_size, "%04d-%02d-%02d_%02d.%02d.%02d_%u.png", year, month, day, hour, minute, second,
			collision + 1);
	else
		snprintf(name, name_size, "%04d-%02d-%02d_%02d.%02d.%02d.png", year, month, day, hour, minute, second);
}

/* (a failure's reason in name, for the guest's message) */
static int failed(char *name, uint32_t name_size, const char *reason)
{
	snprintf(name, name_size, "%s", reason);
	return 0;
}

int host_screenshot_write(unsigned char *bgra, uint32_t width, uint32_t height, const char *directory, char *name,
	uint32_t name_size)
{
	SDL_Time now;
	SDL_DateTime date;
	SDL_Surface *surface;
	size_t path_size, pixel;
	unsigned collision;
	char *path;
	int written;

	if (!name_size)
		return 0;
	if (!width || !height || width > INT_MAX / 4 || height > INT_MAX / width)
		return failed(name, name_size, "no picture");
	if (!SDL_CreateDirectory(directory))
		return failed(name, name_size, "the screenshots folder cannot be made");
	if (!SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &date, true))
		return failed(name, name_size, "no clock");
	path_size = strlen(directory) + 1 + name_size;
	path = SDL_malloc(path_size);
	if (!path)
		return failed(name, name_size, "out of memory");
	for (collision = 0; collision < SCREENSHOT_COLLISIONS; collision++)
	{
		host_screenshot_name(name, name_size, date.year, date.month, date.day, date.hour, date.minute, date.second,
			collision);
		snprintf(path, path_size, "%s/%s", directory, name);
		if (!SDL_GetPathInfo(path, NULL))
			break;
	}
	if (collision == SCREENSHOT_COLLISIONS)
	{
		SDL_free(path);
		return failed(name, name_size, "too many in one second");
	}
	/* (the frame's alpha is the game's scratch, not transparency) */
	for (pixel = 0; pixel < (size_t)width * height; pixel++)
		bgra[pixel * 4 + 3] = 0xff;
	surface = SDL_CreateSurfaceFrom((int)width, (int)height, SDL_PIXELFORMAT_BGRA32, bgra, (int)(width * 4));
	written = surface && SDL_SavePNG(surface, path);
	SDL_DestroySurface(surface);
	SDL_free(path);
	if (!written)
		return failed(name, name_size, "the file cannot be written");
	return 1;
}
