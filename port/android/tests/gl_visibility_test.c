/*
GL_VISIBILITY_TEST.C

The OpenGL ES backend's visibility tests counted by atomic counters
(xgpu_gl.c, XGPU_SAMPLE_COUNTERS), built as the Linux arm64 host builds it
(HALO_GLES, XGPU_GL_HOST) over a fake OpenGL ES whose GPU runs behind the
CPU, as a driver's that queues frames does: draws and copies wait in a
queue and run when a frame's fence is waited for or a buffer is read.
What the specification leaves undefined, it makes garbage: a buffer's
contents from glBufferData without data, and what a queued command reads
of a range the CPU wrote through an unsynchronized mapping while it was
queued (OpenGL ES 3.1, MapBufferRange). Each frame makes tests of known
counts, and every count the backend answers must be one of a test of its
slot (never one with the top bit set: the game halts on that,
rasterizer_xbox_widgets.c).

Built and run by tools/test_gl_visibility.py.
*/

#include "xgpu_gl.c"

#include <stdio.h>
#include <stdlib.h>

#define GARBAGE 0xa5a5a5a5u
#define FAKE_BUFFERS 16
#define QUEUE_SIZE 65536

static struct
{
	GLuint *data;
	GLsizeiptr size;
} buffers[FAKE_BUFFERS];
static GLuint next_buffer = 1;
static GLuint bound[4]; /* atomic counter, copy read, copy write, other */

enum { DRAW, COPY, FENCE };

/* the GPU's queue: a draw adds to a counter, a copy copies words */
static struct command
{
	int kind;
	GLuint source, target, count;
	GLsizeiptr source_offset, target_offset, size;
	int garbage;
} queue[QUEUE_SIZE];
static unsigned long queue_first, queue_last;

static int binding(GLenum target)
{
	switch (target)
	{
	case GL_ATOMIC_COUNTER_BUFFER: return 0;
	case GL_COPY_READ_BUFFER: return 1;
	case GL_COPY_WRITE_BUFFER: return 2;
	default: return 3;
	}
}

static void gpu_run(unsigned long until)
{
	while (queue_first < until)
	{
		struct command *command = &queue[queue_first++ % QUEUE_SIZE];
		GLsizeiptr word;

		if (command->kind == DRAW)
			buffers[command->target].data[command->target_offset / 4] += command->count;
		else if (command->kind == COPY)
		{
			for (word = 0; word < command->size / 4; word++)
				buffers[command->target].data[command->target_offset / 4 + word] = command->garbage ? GARBAGE :
					buffers[command->source].data[command->source_offset / 4 + word];
		}
	}
}

static void fake_glGenBuffers(GLsizei count, GLuint *names)
{
	GLsizei index;

	for (index = 0; index < count; index++)
		names[index] = next_buffer++;
}

static void fake_glBindBuffer(GLenum target, GLuint buffer)
{
	bound[binding(target)] = buffer;
}

static void fake_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
	GLuint buffer = bound[binding(target)];
	GLsizeiptr word;

	(void)usage;
	free(buffers[buffer].data);
	buffers[buffer].data = malloc((size_t)size);
	buffers[buffer].size = size;
	for (word = 0; word < size / 4; word++)
		buffers[buffer].data[word] = data ? ((const GLuint *)data)[word] : GARBAGE;
}

static void fake_glCopyBufferSubData(GLenum read_target, GLenum write_target, GLintptr read_offset,
	GLintptr write_offset, GLsizeiptr size)
{
	struct command *command = &queue[queue_last++ % QUEUE_SIZE];

	command->kind = COPY;
	command->source = bound[binding(read_target)];
	command->target = bound[binding(write_target)];
	command->source_offset = read_offset;
	command->target_offset = write_offset;
	command->size = size;
	command->garbage = 0;
}

static void fake_glMemoryBarrier(GLbitfield barriers)
{
	(void)barriers;
}

static void fake_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
	GLuint buffer = bound[binding(target)];

	/* (ordered with the queue: it runs first) */
	gpu_run(queue_last);
	memcpy((char *)buffers[buffer].data + offset, data, (size_t)size);
}

__typeof__(halo_glGenBuffers) halo_glGenBuffers = fake_glGenBuffers;
__typeof__(halo_glBindBuffer) halo_glBindBuffer = fake_glBindBuffer;
__typeof__(halo_glBufferData) halo_glBufferData = fake_glBufferData;
__typeof__(halo_glBufferSubData) halo_glBufferSubData = fake_glBufferSubData;
__typeof__(halo_glCopyBufferSubData) halo_glCopyBufferSubData = fake_glCopyBufferSubData;
__typeof__(halo_glMemoryBarrier) halo_glMemoryBarrier = fake_glMemoryBarrier;

/* ---------- the host's (port/android/host/host_gl.c) */

static unsigned long fences[STREAM_BUFFER_RING];

void host_gl_fence_frame(unsigned int slot)
{
	fences[slot] = queue_last;
}

void host_gl_wait_frame(unsigned int slot)
{
	gpu_run(fences[slot]);
}

void host_gl_read_buffer(unsigned int buffer, unsigned int offset, unsigned int size, void *data)
{
	/* (a mapping to read waits for the commands that write the buffer) */
	gpu_run(queue_last);
	memcpy(data, (char *)buffers[buffer].data + offset, size);
}

/* an unsynchronized write: now, ahead of the queue, and what a queued
command reads of the range is undefined */
void host_gl_buffer_write(unsigned int target, unsigned int offset, unsigned int size, const void *data)
{
	GLuint buffer = bound[binding(target)];
	unsigned long index;

	for (index = queue_first; index < queue_last; index++)
	{
		struct command *command = &queue[index % QUEUE_SIZE];

		if (command->kind == COPY && command->source == buffer &&
			command->source_offset < (GLsizeiptr)(offset + size) &&
			(GLsizeiptr)offset < command->source_offset + command->size)
			command->garbage = 1;
	}
	memcpy((char *)buffers[buffer].data + offset, data, size);
}

void xgpu_texture_cache_begin_frame(void)
{
}

void platform_log(const char *format, ...)
{
	(void)format;
}

/* ---------- the test */

/* a draw of the current test that passes count samples (as the draw's
counter binding, xgpu_backend_draw_state, makes it) */
static void draw(GLuint count)
{
	struct command *command = &queue[queue_last++ % QUEUE_SIZE];

	command->kind = DRAW;
	command->target = device.visibility_counters;
	command->target_offset = (GLsizeiptr)(device.counter_active * sizeof(GLuint));
	command->count = count;
}

#define FRAMES 40
#define SLOTS 64

int main(void)
{
	/* each slot's count in each frame */
	static GLuint counts[FRAMES][SLOTS];
	int frame, slot, failures = 0;

	xgpu_capabilities.atomic_counters = TRUE;
	sample_counters_create();
	for (frame = 0; frame < FRAMES; frame++)
	{
		/* (more tests a frame than counters, every few frames: a slot is
		tested several times) */
		int tests = frame % 4 == 3 ? VISIBILITY_TEST_SLOTS + 100 : 1500;
		int test;

		for (test = 0; test < tests; test++)
		{
			slot = test % SLOTS;
			counts[frame][slot] = (GLuint)(frame * 100 + slot);
			xgpu_backend_visibility_begin();
			draw(counts[frame][slot] / 2);
			draw(counts[frame][slot] - counts[frame][slot] / 2);
			xgpu_backend_visibility_end((unsigned long)slot, 1.0f, 1, 1);
		}
		for (slot = 0; slot < SLOTS; slot++)
		{
			UINT result = xgpu_backend_visibility_result((unsigned long)slot);
			int earlier;
			BOOL known = result == 0;

			for (earlier = 0; earlier < frame && !known; earlier++)
				known = result == counts[earlier][slot];
			if (!known || (result & 0x80000000u))
			{
				if (failures++ < 10)
					fprintf(stderr, "frame %d, slot %d: count %#x is no test's\n", frame, slot, result);
			}
		}
		xgpu_backend_frame_end();
	}
	/* (and the counts came through: a frame's are known two frames on) */
	for (slot = 0; slot < SLOTS; slot++)
	{
		UINT result = xgpu_backend_visibility_result((unsigned long)slot);

		if (result != counts[FRAMES - 3][slot] && result != counts[FRAMES - 2][slot])
		{
			if (failures++ < 20)
				fprintf(stderr, "slot %d: count %u, not a recent frame's\n", slot, result);
		}
	}
	if (failures)
	{
		fprintf(stderr, "%d wrong counts\n", failures);
		return 1;
	}
	printf("gl visibility test passed\n");
	return 0;
}
