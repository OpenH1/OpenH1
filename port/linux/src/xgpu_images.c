/*
XGPU_IMAGES.C

The high-res replacements' pictures where the renderer's backend is on the
host (xgpu_images.h). The host reads an image's pixels a frame or two after
the game draws it, so a deleted image's pixels are freed only after the
frames in flight are done.
*/

#include "xgpu_images.h"

#include <stdlib.h>
#include <string.h>

#define IMAGE_MAXIMUM 4096
/* frames after its deletion that an image's pixels are kept: those the
host may still be drawing (xgpu_stream_encode.c's frames in flight, and the
frame being written) */
#define IMAGE_KEPT_FRAMES 4

static struct
{
	struct xgpu_image image;
	BOOL used;
	/* deleted: its pixels are freed at frame freed_at */
	BOOL deleted;
	unsigned long freed_at;
} images[IMAGE_MAXIMUM];
static unsigned long image_serial;
static unsigned long image_frame;

unsigned int xgpu_image_create(void *pixels, unsigned long width, unsigned long height, unsigned long levels,
	int format, BOOL owned)
{
	unsigned int index;

	for (index = 0; index < IMAGE_MAXIMUM; index++)
	{
		if (!images[index].used)
			break;
	}
	if (index == IMAGE_MAXIMUM)
	{
		platform_log("images: more than %d high-res pictures; one is not drawn", IMAGE_MAXIMUM);
		if (owned)
			free(pixels);
		return 0;
	}
	memset(&images[index], 0, sizeof(images[index]));
	images[index].used = TRUE;
	images[index].image.pixels = pixels;
	images[index].image.width = width;
	images[index].image.height = height;
	images[index].image.levels = levels ? levels : 1;
	images[index].image.format = format;
	images[index].image.serial = ++image_serial;
	images[index].image.owned = owned;
	return index + 1;
}

void xgpu_image_changed(unsigned int image)
{
	if (image && image <= IMAGE_MAXIMUM && images[image - 1].used && !images[image - 1].deleted)
		images[image - 1].image.serial = ++image_serial;
}

void xgpu_image_delete(unsigned int image)
{
	if (!image || image > IMAGE_MAXIMUM || !images[image - 1].used || images[image - 1].deleted)
		return;
	images[image - 1].deleted = TRUE;
	images[image - 1].freed_at = image_frame + IMAGE_KEPT_FRAMES;
}

const struct xgpu_image *xgpu_image_get(unsigned int image)
{
	if (!image || image > IMAGE_MAXIMUM || !images[image - 1].used || images[image - 1].deleted)
		return NULL;
	return &images[image - 1].image;
}

void xgpu_images_frame_end(void)
{
	unsigned int index;

	image_frame++;
	for (index = 0; index < IMAGE_MAXIMUM; index++)
	{
		if (images[index].used && images[index].deleted && image_frame >= images[index].freed_at)
		{
			if (images[index].image.owned)
				free(images[index].image.pixels);
			memset(&images[index], 0, sizeof(images[index]));
		}
	}
}

