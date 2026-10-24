/* Upload the shared loading picture in the context that owns presentation.
   No game texture, shader or framebuffer is borrowed. */
#ifndef LOADING_GL_H
#define LOADING_GL_H

#include "gl.h"
#include "loading_art.h"

struct loading_gl
{
	GLuint texture, framebuffer;
};

static inline void loading_gl_dispose(struct loading_gl *image)
{
	if (image->framebuffer)
		glDeleteFramebuffers(1, &image->framebuffer);
	if (image->texture)
		glDeleteTextures(1, &image->texture);
	memset(image, 0, sizeof(*image));
}

static inline int loading_gl_draw(struct loading_gl *image, const unsigned char *pixels, int width, int height)
{
	GLint texture, read_framebuffer, unpack_buffer, alignment, row_length, skip_rows, skip_pixels;
	int fit_width = width, fit_height = width * LOADING_ART_FRAME_HEIGHT / LOADING_ART_FRAME_WIDTH;
	int x, y, valid;

	if (!pixels || width <= 0 || height <= 0)
		return 0;
	if (fit_height > height)
	{
		fit_height = height;
		fit_width = height * LOADING_ART_FRAME_WIDTH / LOADING_ART_FRAME_HEIGHT;
	}
	x = (width - fit_width) / 2;
	y = (height - fit_height) / 2;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
	glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
	glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
	glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
	glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	if (!image->texture)
	{
		glGenTextures(1, &image->texture);
		glBindTexture(GL_TEXTURE_2D, image->texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, LOADING_ART_FRAME_WIDTH, LOADING_ART_FRAME_HEIGHT, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glGenFramebuffers(1, &image->framebuffer);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, image->framebuffer);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image->texture, 0);
	}
	glBindTexture(GL_TEXTURE_2D, image->texture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, LOADING_ART_FRAME_WIDTH, LOADING_ART_FRAME_HEIGHT,
		GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, image->framebuffer);
	valid = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	if (valid)
	{
		glClearColor(0.f, 0.f, 0.f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT);
		/* First the picture scaled to cover the whole screen, its top and
		bottom cut: a screen wider than 4:3 has the picture's own space at
		its sides, not black bars; the letterboxed picture over it keeps its
		text and sweep at their 4:3 size. */
		if (fit_width < width)
		{
			int rows = LOADING_ART_FRAME_WIDTH * height / width, top;

			if (rows < 1)
				rows = 1;
			top = (LOADING_ART_FRAME_HEIGHT - rows) / 2;
			glBlitFramebuffer(0, top + rows, LOADING_ART_FRAME_WIDTH, top, 0, 0, width, height,
				GL_COLOR_BUFFER_BIT, GL_LINEAR);
		}
		/* The shared pixels start with the top row; framebuffer Y starts below. */
		glBlitFramebuffer(0, LOADING_ART_FRAME_HEIGHT, LOADING_ART_FRAME_WIDTH, 0,
			x, y, x + fit_width, y + fit_height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_framebuffer);
	glBindTexture(GL_TEXTURE_2D, (GLuint)texture);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpack_buffer);
	glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels);
	return valid;
}

#endif
