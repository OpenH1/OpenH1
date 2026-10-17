/*
WEB_MAIN.C

The web build's entry point, and the WebAssembly memory's layout.

main runs on a thread of its own (Emscripten's PROXY_TO_PTHREAD), where it
may block as the game does. It mounts the site's Origin Private File System,
where the page put the game's maps folder (port/web/site/xiso-worker.js) and
where the saved games and config.toml are kept, at /data; then it starts the
game (shell_xbox.c's main, renamed halo_game_main by tools/web_build.py).
*/

#include <emscripten.h>
#include <emscripten/wasmfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "web_shared.h"
#include "loading_art.h"

int halo_game_main(void);
/* web_net.c: the arguments, for posix_command_line_argument */
void web_net_arguments(int count, char **values);
/* web_library.js: a line for the page (kind 0 a status, 2 an invite link, 3 a
fatal error) */
void web_js_post(int kind, const char *text);
void web_js_loading(unsigned int state, unsigned int frames);

/* ---------- memory

The Xbox window (port/linux/src/platform.h) is the top 128 MB of the
WebAssembly memory, 0x80000000 to 0x88000000 (tools/web_build.py makes the
memory that size). sbrk grows the C heap up to the heap size this reports,
so reporting the window's start keeps the heap below it. */

#define WEB_HEAP_LIMIT 0x80000000UL

size_t emscripten_get_heap_size(void)
{
	size_t size = (size_t)__builtin_wasm_memory_size(0) << 16;

	return size > WEB_HEAP_LIMIT ? WEB_HEAP_LIMIT : size;
}

/* ---------- what the page reads and writes (web_shared.h) */

volatile int32_t web_shared[WEB_SHARED_WORDS];

EMSCRIPTEN_KEEPALIVE volatile int32_t *web_state(void)
{
	return web_shared;
}

void web_frame_shown(void)
{
	__atomic_add_fetch(&web_shared[_web_frames_shown], 1, __ATOMIC_RELAXED);
}

/* Explicit lifecycle state stays visible to the page while this worker is
blocked in map I/O or WebGL compilation. No simulation state crosses here. */
void web_loading_set(int active)
{
	unsigned int state = (unsigned int)__atomic_load_n(&web_shared[_web_loading_state], __ATOMIC_RELAXED);
	unsigned int frames;

	if ((state & 1u) == (unsigned int)(active != 0))
		return;
	state = active ? ((state + 2u) | 1u) : (state & ~1u);
	__atomic_store_n(&web_shared[_web_loading_state], (int32_t)state, __ATOMIC_RELEASE);
	frames = (unsigned int)__atomic_load_n(&web_shared[_web_frames_shown], __ATOMIC_RELAXED);
	web_js_loading(state, frames);
}

/* The worker publishes an owned picture. Atomic bytes avoid a C data race
when the page samples an update; the generation rejects an incomplete copy. */
static uint8_t loading_pixels[LOADING_ART_BYTES];
static unsigned int loading_generation, loading_valid;

void web_loading_art(const uint8_t *rgba)
{
	unsigned int index;

	__atomic_add_fetch(&loading_generation, 1, __ATOMIC_SEQ_CST);
	if (rgba)
	{
		for (index = 0; index < LOADING_ART_BYTES; index++)
			__atomic_store_n(&loading_pixels[index], rgba[index], __ATOMIC_RELAXED);
	}
	__atomic_store_n(&loading_valid, rgba != NULL, __ATOMIC_RELAXED);
	__atomic_add_fetch(&loading_generation, 1, __ATOMIC_RELEASE);
}

/* Called only by the page's main-thread WASM instance. It never enters the
game worker or shares the compositor's history with it. */
EMSCRIPTEN_KEEPALIVE const uint8_t *web_loading_picture(unsigned int epoch, double elapsed_ms)
{
	static struct loading_art art;
	static uint8_t rgba[LOADING_ART_BYTES];
	static unsigned int installed_generation;
	struct loading_presentation_frame frame;
	unsigned int generation, valid, index;

	generation = __atomic_load_n(&loading_generation, __ATOMIC_ACQUIRE);
	if (!(generation & 1u) && generation != installed_generation)
	{
		valid = __atomic_load_n(&loading_valid, __ATOMIC_RELAXED);
		if (valid)
		{
			for (index = 0; index < LOADING_ART_BYTES; index++)
				rgba[index] = __atomic_load_n(&loading_pixels[index], __ATOMIC_RELAXED);
		}
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (generation == __atomic_load_n(&loading_generation, __ATOMIC_ACQUIRE))
		{
			loading_art_set(&art, valid ? rgba : NULL);
			installed_generation = generation;
		}
	}
	memset(&frame, 0, sizeof(frame));
	frame.epoch = epoch;
	frame.active = 1;
	/* RAF supplies finite nonnegative milliseconds; reject invalid values
before converting to the compositor's unsigned nanoseconds. */
	if (!(elapsed_ms >= 0.0) || elapsed_ms >= 18446744073709.0)
		return NULL;
	frame.elapsed_ns = (uint64_t)(elapsed_ms * 1000000.0);
	return loading_art_render(&art, &frame);
}

/* ---------- start */

/* a setting from the browser, unless the page's arguments gave it */
static void set_default(const char *name, const char *value)
{
	setenv(name, value, 0);
}

int main(int argc, char **argv)
{
	backend_t opfs;
	int index;

	/* --NAME=value arguments from the page (port/web/site/app.js) become
	environment variables, which come before config.toml
	(port/linux/src/port_config.c); the others are the command line's */
	for (index = 1; index < argc; index++)
	{
		const char *equals = strchr(argv[index], '=');

		if (!strncmp(argv[index], "--", 2) && equals && equals - argv[index] - 2 < 128)
		{
			char name[128];
			size_t length = (size_t)(equals - argv[index] - 2);

			memcpy(name, argv[index] + 2, length);
			name[length] = '\0';
			setenv(name, equals + 1, 1);
		}
	}

	opfs = wasmfs_create_opfs_backend();
	if (!opfs || wasmfs_create_directory("/data", 0777, opfs) != 0)
	{
		web_js_post(3, "The browser's private storage (OPFS) cannot be opened.");
		return EXIT_FAILURE;
	}
	mkdir("/data/save", 0777);

	set_default("HALO_DATA_ROOT", "/data");
	set_default("HALO_SAVE_ROOT", "/data/save");
	/* the canvas fills the page, which the page's own button takes
	fullscreen (a page may only do so from a click) */
	set_default("HALO_DISPLAY_MODE", "windowed");
	/* (Custom Edition maps' window would be the C heap's here:
	port/linux/src/xbox_memory.c) */
	set_default("HALO_CUSTOM_EDITION", "false");
	/* (an invite link among them is joined: port/linux/src/p2p.c) */
	web_net_arguments(argc, argv);
	web_js_post(0, "starting");
	return halo_game_main();
}
