/*
REPORT.C

Save Report: a zip of what helps find a problem, saved in reports/ beside
maps/ (reports/<date>_<time>.zip) for the player to send to the developers.
Asked for by controls.report (F8, in a game too: xinput_sdl.c) or Settings >
Profiles > REPORT A PROBLEM (menu_functions.c), both report_request, and
saved at the next frame presented (d3d8_device.c: report_write), whose
picture is its screenshot: the moment the key was pressed. Then a new issue
on OpenH1's GitHub repository opens in the browser, its title and body
filled in with report.md, and (on a computer) the reports folder: the
player says what happened, drags the zip in and submits it. Nothing is
sent by the game itself. Its issue is public, so what could say who the
player is never goes in: settings.txt leaves the private settings out, and
every text in it is scrubbed (report_scrub: the home folder, links,
addresses, other players' chat lines). Asked for in a game (report_game_playing), the
issue waits until the player is back in the menus, so that nothing is
opened over the game. A second ask within REPORT_AGAIN_SECONDS of one is
let go (a key pressed twice opens one issue).

The zip holds:
  report.md      a GitHub issue's body, its first lines for the player to
                 fill in (report_issue_url): the build, the platform, the GPU and its driver (the
                 renderer's host's: xgpu_backend_device), the processor, the
                 display's settings, the map and the game, how long the game
                 has run, and the frame times of the last REPORT_SECONDS
                 (report_frame_summary: the frame rate, its 1% and 0.1% lows,
                 the longest frames and when)
  log.txt        this run's log (platform_log's lines), its last
                 REPORT_LOG_SIZE bytes, kept here: on Linux and Android the
                 log is not otherwise in a file
  halo.log       the host's log file, where it has one (Windows beside
                 halo.exe, the macOS app's in ~/Library/Logs/OpenH1), its end
  debug.txt      the game's own log, in the data folder
  settings.txt   the settings changed from their defaults (not config.toml:
                 never the passwords, this machine's addresses, keys)
  screenshot.png the frame shown when it was asked for

A frame's time costs a clock reading and two stores (report_frame_presented,
on the render thread, as report_write is); a log line a copy into the ring.
*/

#include "platform.h"
#include "port_config.h"
#include "report.h"
#include "xgpu_backend.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int mkdir(const char *path, unsigned int mode);
/* updater.c's: the build, in a few words */
void updater_build_text(char *text, size_t size);
/* p2p_discord.c's: the network game this machine is in, as the game told */
void p2p_discord_game(int *hosting, int *playing, char *description, int size, int *player_count,
	int *maximum_player_count);

/* the frames kept (a power of 2: at 240 frames a second, 4.5 minutes) */
#define REPORT_FRAMES 65536
/* the frames summed: the last 5 minutes */
#define REPORT_SECONDS 300
/* a frame this long (milliseconds) or longer is a hitch */
#define REPORT_HITCH_MS 50
/* the longest frames listed */
#define REPORT_LONGEST 8
#define REPORT_LOG_SIZE (512 * 1024)
/* the most of a log file's end taken */
#define REPORT_FILE_MOST (4 * 1024 * 1024)
#define REPORT_ZIP_FILES 8
#define REPORT_AGAIN_SECONDS 5
/* the most issues waiting for the game's end */
#define REPORT_PENDING 4

static struct
{
	uint32_t micros[REPORT_FRAMES];
	uint32_t at_ms[REPORT_FRAMES];
	unsigned long count;
	long long start_ns, last_ns;
} report_frames;

static struct
{
	pthread_mutex_t lock;
	char text[REPORT_LOG_SIZE];
	unsigned long long written;
} report_log = { PTHREAD_MUTEX_INITIALIZER, { 0 }, 0 };

static volatile int report_requested;
/* the issues of the reports asked for in a game, opened once it is left
(their addresses, malloc()'s) */
static struct
{
	char *urls[REPORT_PENDING];
	short count;
} report_pending;
/* (the last ask let through: another this soon after it is let go) */
static long long report_last_ns;

static void report_open_issue(const char *url);

/* the issues waiting opened: the game left, or quit (report_at_exit) */
static void report_open_pending(void)
{
	short index;

	for (index = 0; index < report_pending.count; index++)
	{
		report_open_issue(report_pending.urls[index]);
		free(report_pending.urls[index]);
	}
	report_pending.count = 0;
}

static long long report_now_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
}

/* ---------- what is kept */

void report_frame_presented(void)
{
	long long now = report_now_ns();

	/* (the issues of reports asked for in a game, once it is left) */
	if (report_pending.count && !report_game_playing())
		report_open_pending();

	if (report_frames.last_ns)
	{
		unsigned long index = report_frames.count & (REPORT_FRAMES - 1);
		long long micros = (now - report_frames.last_ns) / 1000;

		report_frames.micros[index] = (uint32_t)(micros > 0xFFFFFFFFLL ? 0xFFFFFFFFLL : micros);
		report_frames.at_ms[index] = (uint32_t)((now - report_frames.start_ns) / 1000000);
		report_frames.count++;
	}
	else
		report_frames.start_ns = now;
	report_frames.last_ns = now;
}

void report_log_line(const char *line)
{
	size_t length = strlen(line), index;

	pthread_mutex_lock(&report_log.lock);
	for (index = 0; index <= length; index++)
	{
		report_log.text[report_log.written % REPORT_LOG_SIZE] = index < length ? line[index] : '\n';
		report_log.written++;
	}
	pthread_mutex_unlock(&report_log.lock);
}

void report_request(void)
{
	long long now = report_now_ns();

	if (report_last_ns && now - report_last_ns < REPORT_AGAIN_SECONDS * 1000000000LL)
		return;
	report_last_ns = now;
	report_requested = 1;
}

int report_take_request(void)
{
	int requested = report_requested;

	report_requested = 0;
	return requested;
}

/* ---------- the frame times */

static int report_compare_micros(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return x < y ? -1 : x > y;
}

/* the frame rate of the slowest share (per mille) of the frames sorted: their
mean time's */
static double report_low_fps(const uint32_t *sorted, unsigned long count, unsigned long per_mille)
{
	unsigned long slowest = count * per_mille / 1000, index;
	double total = 0;

	if (slowest < 1)
		slowest = 1;
	for (index = count - slowest; index < count; index++)
		total += sorted[index];
	return total > 0 ? 1000000.0 * (double)slowest / total : 0;
}

void report_frame_summary(const uint32_t *micros, const uint32_t *at_ms, unsigned long count, uint32_t now_ms,
	char *text, size_t size)
{
	uint32_t *sorted;
	unsigned long index, hitches = 0, shown;
	double total = 0;
	size_t used = 0;
	unsigned long longest[REPORT_LONGEST];

	if (!size)
		return;
	text[0] = 0;
	if (!count)
	{
		snprintf(text, size, "No frames yet.\n");
		return;
	}
	sorted = malloc(count * sizeof(*sorted));
	if (!sorted)
	{
		snprintf(text, size, "No memory to sum the frames.\n");
		return;
	}
	for (index = 0; index < count; index++)
	{
		sorted[index] = micros[index];
		total += micros[index];
		if (micros[index] >= REPORT_HITCH_MS * 1000)
			hitches++;
	}
	qsort(sorted, count, sizeof(*sorted), report_compare_micros);
#define REPORT_PRINT(...) \
	(used += (size_t)snprintf(text + (used < size ? used : size - 1), used < size ? size - used : 1, __VA_ARGS__))
	REPORT_PRINT("Frames: %lu in the last %.1f seconds\n", count, total / 1000000.0);
	REPORT_PRINT("Average: %.1f fps (%.2f ms a frame)\n", total > 0 ? 1000000.0 * (double)count / total : 0,
		total / 1000.0 / (double)count);
	REPORT_PRINT("Median frame: %.2f ms\n", sorted[count / 2] / 1000.0);
	REPORT_PRINT("1%% low: %.1f fps\n", report_low_fps(sorted, count, 10));
	REPORT_PRINT("0.1%% low: %.1f fps\n", report_low_fps(sorted, count, 1));
	REPORT_PRINT("Frames of %d ms or more: %lu\n", REPORT_HITCH_MS, hitches);
	/* (the longest, the latest first among equals: those of twice the
	median's time or more, a stall) */
	for (shown = 0; shown < REPORT_LONGEST && shown < count; shown++)
	{
		unsigned long best = count;

		for (index = count; index-- > 0;)
		{
			unsigned long taken;

			for (taken = 0; taken < shown && longest[taken] != index; taken++)
				;
			if (taken < shown)
				continue;
			if (best == count || micros[index] > micros[best])
				best = index;
		}
		if (micros[best] < 2 * (uint64_t)sorted[count / 2])
			break;
		longest[shown] = best;
	}
	REPORT_PRINT(shown ? "Longest frames:\n" : "Longest frames: none twice the median or longer\n");
	for (index = 0; index < shown; index++)
	{
		unsigned long frame = longest[index];

		REPORT_PRINT("  %.1f ms, %.1f s ago\n", micros[frame] / 1000.0,
			(now_ms >= at_ms[frame] ? now_ms - at_ms[frame] : 0) / 1000.0);
	}
#undef REPORT_PRINT
	free(sorted);
}

/* ---------- the zip (stored, not compressed: the PNG is already) */

uint32_t report_crc32(uint32_t crc, const unsigned char *data, size_t size)
{
	static uint32_t table[256];
	size_t index;

	if (!table[1])
	{
		uint32_t value, bit, entry;

		for (entry = 0; entry < 256; entry++)
		{
			value = entry;
			for (bit = 0; bit < 8; bit++)
				value = value & 1 ? 0xEDB88320u ^ (value >> 1) : value >> 1;
			table[entry] = value;
		}
	}
	crc = ~crc;
	for (index = 0; index < size; index++)
		crc = table[(crc ^ data[index]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

struct report_zip
{
	FILE *file;
	unsigned short time, date;
	int count;
	struct
	{
		char name[32];
		uint32_t crc, size, offset;
	} entries[REPORT_ZIP_FILES];
	int failed;
};

static void report_put16(FILE *file, unsigned int value)
{
	fputc((int)(value & 0xFF), file);
	fputc((int)((value >> 8) & 0xFF), file);
}

static void report_put32(FILE *file, uint32_t value)
{
	report_put16(file, value & 0xFFFF);
	report_put16(file, value >> 16);
}

static void report_zip_add(struct report_zip *zip, const char *name, const void *data, size_t size)
{
	size_t length = strlen(name);
	long offset = ftell(zip->file);

	if (zip->count >= REPORT_ZIP_FILES || length >= sizeof(zip->entries[0].name) || offset < 0 ||
		size > 0x7FFFFFFF)
	{
		zip->failed = 1;
		return;
	}
	snprintf(zip->entries[zip->count].name, sizeof(zip->entries[0].name), "%s", name);
	zip->entries[zip->count].crc = report_crc32(0, data, size);
	zip->entries[zip->count].size = (uint32_t)size;
	zip->entries[zip->count].offset = (uint32_t)offset;
	report_put32(zip->file, 0x04034B50u);
	report_put16(zip->file, 10);
	report_put16(zip->file, 0);
	report_put16(zip->file, 0);
	report_put16(zip->file, zip->time);
	report_put16(zip->file, zip->date);
	report_put32(zip->file, zip->entries[zip->count].crc);
	report_put32(zip->file, (uint32_t)size);
	report_put32(zip->file, (uint32_t)size);
	report_put16(zip->file, (unsigned int)length);
	report_put16(zip->file, 0);
	fwrite(name, 1, length, zip->file);
	if (size && fwrite(data, 1, size, zip->file) != size)
		zip->failed = 1;
	zip->count++;
}

static int report_zip_end(struct report_zip *zip)
{
	long start = ftell(zip->file), end;
	int index;

	for (index = 0; index < zip->count; index++)
	{
		size_t length = strlen(zip->entries[index].name);

		report_put32(zip->file, 0x02014B50u);
		report_put16(zip->file, 20);
		report_put16(zip->file, 10);
		report_put16(zip->file, 0);
		report_put16(zip->file, 0);
		report_put16(zip->file, zip->time);
		report_put16(zip->file, zip->date);
		report_put32(zip->file, zip->entries[index].crc);
		report_put32(zip->file, zip->entries[index].size);
		report_put32(zip->file, zip->entries[index].size);
		report_put16(zip->file, (unsigned int)length);
		report_put16(zip->file, 0);
		report_put16(zip->file, 0);
		report_put16(zip->file, 0);
		report_put16(zip->file, 0);
		report_put32(zip->file, 0);
		report_put32(zip->file, zip->entries[index].offset);
		fwrite(zip->entries[index].name, 1, length, zip->file);
	}
	end = ftell(zip->file);
	report_put32(zip->file, 0x06054B50u);
	report_put16(zip->file, 0);
	report_put16(zip->file, 0);
	report_put16(zip->file, (unsigned int)zip->count);
	report_put16(zip->file, (unsigned int)zip->count);
	report_put32(zip->file, (uint32_t)(end - start));
	report_put32(zip->file, (uint32_t)start);
	report_put16(zip->file, 0);
	return !zip->failed && start >= 0 && !ferror(zip->file);
}

/* ---------- what is kept out of a report (its issue is public) */

/* whether a setting ("server.password") is one a report leaves out: a
password, an address of this machine's, a key */
static int report_setting_private(const char *name)
{
	static const char *const named[] = {
		"network.public_address", "server.master_publish_address", "debug.update_key",
	};
	size_t index;

	if (strstr(name, "password"))
		return 1;
	for (index = 0; index < sizeof(named) / sizeof(named[0]); index++)
	{
		if (!strcmp(name, named[index]))
			return 1;
	}
	return 0;
}

/* an IPv4 address at text (none of 127.0.0.0/8's, nor 0.0.0.0): its length,
0 if it is not one */
static size_t report_address_length(const char *text, size_t left)
{
	size_t at = 0;
	int part, value, digits, loopback = 0, zero = 1;

	for (part = 0; part < 4; part++)
	{
		if (part)
		{
			if (at >= left || text[at] != '.')
				return 0;
			at++;
		}
		for (value = 0, digits = 0; at < left && text[at] >= '0' && text[at] <= '9' && digits < 4; at++, digits++)
			value = value * 10 + (text[at] - '0');
		if (!digits || digits > 3 || value > 255)
			return 0;
		if (!part)
			loopback = value == 127;
		zero = zero && !value;
	}
	if (at < left && ((text[at] >= '0' && text[at] <= '9') || (text[at] == '.' && at + 1 < left &&
		text[at + 1] >= '0' && text[at + 1] <= '9')))
		return 0;
	return loopback || zero ? 0 : at;
}

char *report_scrub(const char *text, size_t size, const char *home, size_t *scrubbed_size)
{
	size_t room = size * 2 + 4096, used = 0, at = 0, home_length = home ? strlen(home) : 0;
	char *out = malloc(room);

	*scrubbed_size = 0;
	if (!out)
		return NULL;
#define REPORT_PUT(data, length) 	do { if (used + (length) < room) { memcpy(out + used, (data), (length)); used += (length); } } while (0)
	while (at < size)
	{
		size_t end = at, index;
		const char *line = text + at, *chat;

		while (end < size && text[end] != '\n')
			end++;
		/* (another player's chat line, as debug.txt logs it) */
		chat = NULL;
		for (index = at; index + 5 <= end && !chat; index++)
		{
			if (!memcmp(text + index, "chat", 4) && (index == at || text[index - 1] == ' ') &&
				(!memcmp(text + index + 4, ": ", end - index - 4 >= 2 ? 2 : 0) ||
				!memcmp(text + index + 4, " (", end - index - 4 >= 2 ? 2 : 0)) &&
				memcmp(text + index + 4, " notice", end - index - 4 >= 7 ? 7 : 0))
			{
				chat = text + index;
			}
		}
		if (chat && end - at >= 6)
		{
			REPORT_PUT(line, (size_t)(chat - line));
			REPORT_PUT("chat: (a chat line, removed from the report)", 44);
			if (end < size)
				REPORT_PUT("\n", 1);
			at = end + 1;
			continue;
		}
		for (index = at; index < end;)
		{
			size_t length;

			/* (the home folder: the account's name) */
			if (home_length > 1 && end - index >= home_length && !memcmp(text + index, home, home_length))
			{
				REPORT_PUT("~", 1);
				index += home_length;
			}
			/* (an invite's or a party's link) */
			else if (end - index >= 9 && !memcmp(text + index, "openh1://", 9))
			{
				REPORT_PUT("openh1://(removed)", 18);
				index += 9;
				while (index < end && text[index] != ' ' && text[index] != '\t' && text[index] != '"' &&
					text[index] != ')')
					index++;
			}
			else if ((index == at || !((text[index - 1] >= '0' && text[index - 1] <= '9') || text[index - 1] == '.')) &&
				(length = report_address_length(text + index, end - index)) != 0)
			{
				REPORT_PUT("x.x.x.x", 7);
				index += length;
			}
			else
			{
				REPORT_PUT(text + index, 1);
				index++;
			}
		}
		if (end < size)
			REPORT_PUT("\n", 1);
		at = end + 1;
	}
#undef REPORT_PUT
	*scrubbed_size = used;
	return out;
}

/* a text file's part of the zip, scrubbed (report_scrub) */
static void report_zip_add_text(struct report_zip *zip, const char *name, const char *data, size_t size)
{
	const char *home = getenv("HOME");
	size_t scrubbed_size;
	char *scrubbed;

#ifdef HALO_WINDOWS
	if (getenv("USERPROFILE"))
		home = getenv("USERPROFILE");
#endif
	scrubbed = report_scrub(data, size, home, &scrubbed_size);
	if (scrubbed)
	{
		report_zip_add(zip, name, scrubbed, scrubbed_size);
		free(scrubbed);
	}
}

/* settings.txt: each setting changed from its default, as config.toml has
it, but those report_setting_private names (never written); free() it */
static char *report_settings_text(size_t *size)
{
	struct config_setting_info info;
	size_t room = 64 * 1024, used = 0;
	char *text = malloc(room), value[300];
	int index;

	*size = 0;
	if (!text)
		return NULL;
	used = (size_t)snprintf(text, room, "# The settings changed from their defaults (config.toml), but the passwords, "
		"this machine's addresses and keys, which a report leaves out\n");
	for (index = 0; config_setting_info(index, &info) && used < room; index++)
	{
		if (!info.name || report_setting_private(info.name) || !config_text(info.name, value, sizeof(value)) ||
			!strcmp(value, info.default_text))
			continue;
		/* (a number as its default, written another way: "0" for "0.0") */
		if (info.type && (!strcmp(info.type, "real") || !strcmp(info.type, "integer")) &&
			strtod(value, NULL) == strtod(info.default_text, NULL))
			continue;
		used += (size_t)snprintf(text + used, room - used, "%s = %s\n", info.name, value);
	}
	*size = used < room ? used : room - 1;
	return text;
}

/* ---------- the report's parts */

/* a file's end (its last most bytes), or NULL; free() it */
static char *report_file_end(const char *path, size_t most, size_t *size)
{
	FILE *file = fopen(path, "rb");
	long length;
	char *data;

	*size = 0;
	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0)
	{
		fclose(file);
		return NULL;
	}
	if ((size_t)length > most)
		fseek(file, length - (long)most, SEEK_SET);
	else
		fseek(file, 0, SEEK_SET);
	data = malloc((size_t)length > most ? most : (size_t)length + 1);
	if (data)
		*size = fread(data, 1, (size_t)length > most ? most : (size_t)length, file);
	fclose(file);
	return data;
}

/* the log kept, oldest first; free() it */
static char *report_log_text(size_t *size)
{
	char *text;
	size_t kept, index;

	pthread_mutex_lock(&report_log.lock);
	kept = report_log.written < REPORT_LOG_SIZE ? (size_t)report_log.written : REPORT_LOG_SIZE;
	text = malloc(kept + 1);
	if (text)
	{
		for (index = 0; index < kept; index++)
			text[index] = report_log.text[(report_log.written - kept + index) % REPORT_LOG_SIZE];
	}
	pthread_mutex_unlock(&report_log.lock);
	*size = text ? kept : 0;
	return text;
}

static const char *report_platform(void)
{
#if defined(HALO_IOS)
	return "iOS";
#elif defined(HALO_MACOS)
	return "macOS";
#elif defined(HALO_ANDROID)
	return "Android";
#elif defined(HALO_WINDOWS)
	return "Windows";
#elif defined(HALO_VR) && defined(__aarch64__)
	return "Linux arm64, VR";
#elif defined(HALO_VR)
	return "Linux x86-64, VR";
#elif defined(__aarch64__)
	return "Linux arm64";
#else
	return "Linux x86-64";
#endif
}

/* the processor's name (Linux's and Android's /proc/cpuinfo) */
static void report_processor(char *text, size_t size)
{
	FILE *file = fopen("/proc/cpuinfo", "r");
	char line[256];
	long cores = sysconf(_SC_NPROCESSORS_ONLN);

	text[0] = 0;
	while (file && fgets(line, sizeof(line), file))
	{
		char *colon = strchr(line, ':');

		if (colon && (!strncmp(line, "model name", 10) || !strncmp(line, "Hardware", 8)))
		{
			colon++;
			while (*colon == ' ' || *colon == '\t')
				colon++;
			colon[strcspn(colon, "\r\n")] = 0;
			snprintf(text, size, "%s", colon);
			break;
		}
	}
	if (file)
		fclose(file);
	if (cores > 0)
	{
		size_t used = strlen(text);

		snprintf(text + used, size - used, "%s%ld logical cores", used ? ", " : "", cores);
	}
	if (!text[0])
		snprintf(text, size, "unknown");
}

/* report.md: a GitHub issue's body (report_send pastes it) */
static char *report_text(const char *name)
{
	static const char *const settings[] = {
		"display.graphics", "display.mode", "display.resolution", "display.window_size", "display.resolution_scaling",
		"display.vsync", "display.max_fps", "display.low_latency", "display.anti_aliasing", "display.hdr",
		"display.refresh_rate", "game.sim_rate", "network.online",
	};
	size_t size = 64 * 1024, used = 0, index;
	char *text = malloc(size), line[512], build[160];
	uint32_t *micros, *at_ms;
	unsigned long count, first;
	int hosting = 0, playing = 0, players = 0, most = 0;
	long long now = report_now_ns();
	uint32_t now_ms = report_frames.start_ns ? (uint32_t)((now - report_frames.start_ns) / 1000000) : 0;

	if (!text)
		return NULL;
#define REPORT_PRINT(...) \
	(used += (size_t)snprintf(text + (used < size ? used : size - 1), used < size ? size - used : 1, __VA_ARGS__))
	REPORT_PRINT("## System\n\n");
	updater_build_text(build, sizeof(build));
	REPORT_PRINT("- Build: %s\n", build);
	REPORT_PRINT("- Platform: %s\n", report_platform());
	REPORT_PRINT("- GPU: %s\n", xgpu_backend_device()[0] ? xgpu_backend_device() : "unknown (no renderer)");
	report_processor(line, sizeof(line));
	REPORT_PRINT("- Processor: %s\n", line);
	REPORT_PRINT("- Running for: %.0f seconds\n", report_frames.start_ns ? (now - report_frames.start_ns) / 1e9 : 0.0);
	report_game_describe(line, sizeof(line));
	REPORT_PRINT("- Where: %s\n", line);
	p2p_discord_game(&hosting, &playing, line, (int)sizeof(line), &players, &most);
	if (line[0])
		REPORT_PRINT("- Network game: %s, %s, %d of %d players\n", line, hosting ? "hosting" : "joined", players, most);
	REPORT_PRINT("- Report: %s\n", name);
	REPORT_PRINT("\n## Settings\n\nThe display's (the others changed: settings.txt in the zip)\n\n```\n");
	for (index = 0; index < sizeof(settings) / sizeof(settings[0]); index++)
	{
		if (config_text(settings[index], line, sizeof(line)))
			REPORT_PRINT("%s = %s\n", settings[index], line);
	}
	REPORT_PRINT("```\n");
	REPORT_PRINT("\n## Frame times\n\nThe last %d seconds, loading screens included\n\n```\n", REPORT_SECONDS);
	count = report_frames.count < REPORT_FRAMES ? report_frames.count : REPORT_FRAMES;
	first = report_frames.count - count;
	while (count && report_frames.at_ms[first & (REPORT_FRAMES - 1)] + REPORT_SECONDS * 1000u < now_ms)
	{
		first++;
		count--;
	}
	micros = malloc((count ? count : 1) * sizeof(*micros));
	at_ms = malloc((count ? count : 1) * sizeof(*at_ms));
	if (micros && at_ms)
	{
		char *summary = malloc(4096);

		for (index = 0; index < count; index++)
		{
			micros[index] = report_frames.micros[(first + index) & (REPORT_FRAMES - 1)];
			at_ms[index] = report_frames.at_ms[(first + index) & (REPORT_FRAMES - 1)];
		}
		if (summary)
		{
			report_frame_summary(micros, at_ms, count, now_ms, summary, 4096);
			REPORT_PRINT("%s", summary);
			free(summary);
		}
	}
	free(micros);
	free(at_ms);
	REPORT_PRINT("```\n");
#undef REPORT_PRINT
	return text;
}

/* ---------- the issue */

#define REPORT_ISSUES "https://github.com/OpenH1/OpenH1/issues/new"
/* GitHub's addresses: some 8 KB at most */
#define REPORT_URL_MOST 7800

static size_t report_percent_encode(const char *text, size_t length, char *out, size_t room)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t used = 0, index;

	for (index = 0; index < length; index++)
	{
		unsigned char character = (unsigned char)text[index];
		int plain = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
			(character >= '0' && character <= '9') || character == '-' || character == '_' || character == '.' ||
			character == '~';

		if (used + (plain ? 1 : 3) >= room)
			return (size_t)-1;
		if (plain)
			out[used++] = (char)character;
		else
		{
			out[used++] = '%';
			out[used++] = hex[character >> 4];
			out[used++] = hex[character & 15];
		}
	}
	out[used] = 0;
	return used;
}

/* a folder's file: address (its path's separators kept, the rest encoded;
a Windows drive's path after a third slash) */
static void report_file_url(const char *folder, char *url, size_t size)
{
	size_t used = (size_t)snprintf(url, size, folder[0] == '/' ? "file://" : "file:///"), index;

	for (index = 0; folder[index] && used + 4 < size; index++)
	{
		char character = folder[index] == '\\' ? '/' : folder[index];

		if (character == '/' || character == ':')
			url[used++] = character;
		else
		{
			size_t written = report_percent_encode(&character, 1, url + used, size - used);

			if (written == (size_t)-1)
				break;
			used += written;
		}
	}
	url[used < size ? used : size - 1] = 0;
}

int report_issue_url(const char *title, const char *body, char *url, size_t most)
{
	size_t used, length = strlen(body), written;

	used = (size_t)snprintf(url, most, "%s?title=", REPORT_ISSUES);
	if (used >= most)
		return 0;
	written = report_percent_encode(title, strlen(title), url + used, most - used);
	if (written == (size_t)-1)
		return 0;
	used += written;
	if (used + 6 >= most)
		return 0;
	memcpy(url + used, "&body=", 7);
	used += 6;
	/* (the body cut after a line's end until it fits) */
	while ((written = report_percent_encode(body, length, url + used, most - used)) == (size_t)-1)
	{
		if (!length)
			return 0;
		length--;
		while (length && body[length - 1] != '\n')
			length--;
		if (!length)
			return 0;
	}
	return 1;
}

/* a run nobody watches (as the clipboard is left alone in it:
sdl_platform.c's platform_clipboard_left_alone): no browser opened */
static int report_unattended(void)
{
	return config_boolean("debug.no_clipboard") || config_boolean("debug.null_renderer") ||
		config_string("debug.test_input")[0] || config_boolean("debug.fixed_step") ||
		config_string("debug.network_test")[0] || config_real("debug.exit_after") > 0.0;
}

/* an issue opened in the browser, and the reports folder beside it (a run
nobody watches leaves the desktop alone: the address logged) */
static void report_open_issue(const char *url)
{
	char folder[1024], folder_url[1400];

	if (report_unattended())
	{
		platform_log("report: the issue's address: %s", url);
		return;
	}
	platform_open_url(url);
#if !defined(HALO_ANDROID) && !defined(HALO_IOS)
	/* (the folder, for the zip to be dragged into the issue) */
	snprintf(folder, sizeof(folder), "%s/reports", platform_data_root());
	report_file_url(folder, folder_url, sizeof(folder_url));
	platform_open_url(folder_url);
#else
	(void)folder;
	(void)folder_url;
#endif
	platform_log("report: opened a new issue for it");
}

/* ---------- the report */

/* the report being finished: its name, its summary (report_text's, made as
it was asked for), its screenshot's file in reports/ (or none), whether its
issue is opened; between the key and the note typed in a game */
static struct
{
	int waiting;
	char name[64];
	char *text;
	char shot[96];
	int send;
} report_capture;

/* the zip written, and its issue opened or held for the menus: the note
typed, NULL for none; what the player is told into message */
static int report_finish(const char *note, char *message, size_t message_size)
{
	char folder[1024], path[1200], file_path[1200];
	const char *name = report_capture.name;
	struct report_zip zip;
	struct tm local;
	time_t now = time(NULL);
	size_t size, issue_size;
	char *data, *issue;
	int saved;

	snprintf(folder, sizeof(folder), "%s/reports", platform_data_root());
	snprintf(path, sizeof(path), "%s/%s.zip", folder, name);
	localtime_r(&now, &local);
	memset(&zip, 0, sizeof(zip));
	zip.time = (unsigned short)(local.tm_hour << 11 | local.tm_min << 5 | local.tm_sec / 2);
	zip.date = (unsigned short)((local.tm_year - 80) << 9 | (local.tm_mon + 1) << 5 | local.tm_mday);
	/* (report.md: what happened, the player's note or where it goes, over
	the summary) */
	issue_size = (report_capture.text ? strlen(report_capture.text) : 0) + (note ? strlen(note) : 0) + 512;
	issue = malloc(issue_size);
	if (issue)
	{
		if (note && note[0])
			snprintf(issue, issue_size, "**What happened:** %s\n\n(Drag %s.zip, from the reports folder that "
				"opened, into this box.)\n\n%s", note, name, report_capture.text ? report_capture.text : "");
		else
			snprintf(issue, issue_size, "**What happened:** \n\n(Say what you saw and what you expected. Then drag "
				"%s.zip, from the reports folder that opened, into this box.)\n\n%s", name,
				report_capture.text ? report_capture.text : "");
	}
	free(report_capture.text);
	report_capture.text = NULL;
	report_capture.waiting = 0;
	zip.file = fopen(path, "wb");
	if (!zip.file)
	{
		free(issue);
		snprintf(message, message_size, "Report failed: can't write in %s", folder);
		platform_log("report: could not write %s", path);
		return 0;
	}
	if (issue)
		report_zip_add_text(&zip, "report.md", issue, strlen(issue));
	data = report_log_text(&size);
	if (data)
	{
		report_zip_add_text(&zip, "log.txt", data, size);
		free(data);
	}
	/* (the host's log file, where it has one) */
	config_folder(file_path, sizeof(file_path));
	snprintf(file_path + strlen(file_path), sizeof(file_path) - strlen(file_path), "halo.log");
	data = report_file_end(file_path, REPORT_FILE_MOST, &size);
#ifdef HALO_MACOS
	if (!data && getenv("HOME"))
	{
		snprintf(file_path, sizeof(file_path), "%s/Library/Logs/OpenH1/halo.log", getenv("HOME"));
		data = report_file_end(file_path, REPORT_FILE_MOST, &size);
	}
#endif
	if (data)
	{
		report_zip_add_text(&zip, "halo.log", data, size);
		free(data);
	}
	snprintf(file_path, sizeof(file_path), "%s/debug.txt", platform_data_root());
	data = report_file_end(file_path, REPORT_FILE_MOST, &size);
	if (data)
	{
		report_zip_add_text(&zip, "debug.txt", data, size);
		free(data);
	}
	/* (not config.toml: the settings changed, the private ones never) */
	data = report_settings_text(&size);
	if (data)
	{
		report_zip_add_text(&zip, "settings.txt", data, size);
		free(data);
	}
	/* (the frame, written by the host beside the zip as it was asked for,
	then taken in) */
	if (report_capture.shot[0])
	{
		snprintf(file_path, sizeof(file_path), "%s/%s", folder, report_capture.shot);
		data = report_file_end(file_path, 64 * 1024 * 1024, &size);
		if (data)
		{
			report_zip_add(&zip, "screenshot.png", data, size);
			free(data);
		}
		remove(file_path);
	}
	saved = report_zip_end(&zip);
	saved = fclose(zip.file) == 0 && saved;
	if (!saved)
	{
		free(issue);
		remove(path);
		snprintf(message, message_size, "Report failed: couldn't write %s.zip", name);
		platform_log("report: failed to write %s", path);
		return 0;
	}
	snprintf(message, message_size, "Report saved: reports/%s.zip", name);
	platform_log("report: saved %s (%d files)%s", path, zip.count, note && note[0] ? ", with a note" : "");
	if (report_capture.send && issue)
	{
		char *url = malloc(REPORT_URL_MOST + 1), title[160], build[120];
		int made;

		/* (a release's number, "build-1739", or a local build's) */
		updater_build_text(build, sizeof(build));
		if (!strncmp(build, "build-", 6))
			build[strcspn(build, " ")] = 0;
		else
			snprintf(build, sizeof(build), "local build");
		snprintf(title, sizeof(title), "Problem report (%s, %s)", report_platform(), build);
		made = url && report_issue_url(title, issue, url, REPORT_URL_MOST + 1);
		if (made && report_game_playing() && report_pending.count < REPORT_PENDING)
		{
			/* (in a game, nothing opened over it: the issue waits for the
			menus, report_frame_presented, or the game's end: report_at_exit) */
			report_pending.urls[report_pending.count++] = url;
			url = NULL;
			snprintf(message, message_size, "Report saved: its GitHub issue opens when you're back in the menus");
			platform_log("report: its issue opens once the game is left");
		}
		else if (made)
		{
			report_open_issue(url);
#if !defined(HALO_ANDROID) && !defined(HALO_IOS)
			snprintf(message, message_size, "Report saved: drag reports/%s.zip into the issue", name);
#else
			snprintf(message, message_size, "Report saved: attach reports/%s.zip to the issue", name);
#endif
		}
		free(url);
	}
	free(issue);
	return 1;
}

/* the game quit: a report waiting for its note finished without one, and
the issues held for the menus opened */
static void report_at_exit(void)
{
	char message[160];

	if (report_capture.waiting)
		report_finish(NULL, message, sizeof(message));
	report_open_pending();
}

int report_write(unsigned int target, int send, char *message, size_t message_size)
{
	static int at_exit;
	char folder[1024];
	time_t now = time(NULL);
	struct tm local;

	message[0] = 0;
	if (!at_exit)
		at_exit = atexit(report_at_exit) == 0;
	/* (a report still waiting for its note is finished first, without one) */
	if (report_capture.waiting)
	{
		report_game_note_cancel();
		report_finish(NULL, message, message_size);
	}
	localtime_r(&now, &local);
	strftime(report_capture.name, sizeof(report_capture.name), "%Y-%m-%d_%H.%M.%S", &local);
	snprintf(folder, sizeof(folder), "%s/reports", platform_data_root());
	mkdir(folder, 0755);
	/* (what is the moment's, now: the summary and the frame) */
	report_capture.text = report_text(report_capture.name);
	report_capture.shot[0] = 0;
	if (target && !xgpu_backend_screenshot(target, folder, report_capture.shot, sizeof(report_capture.shot)))
		report_capture.shot[0] = 0;
	report_capture.send = send;
	/* (in a game, a line for the player's note first: report_note_done
	finishes it) */
	if (report_game_playing() && report_game_note_begin())
	{
		report_capture.waiting = 1;
		platform_log("report: %s asked for, waiting for its note", report_capture.name);
		return 1;
	}
	return report_finish(NULL, message, message_size);
}

void report_note_done(const char *note)
{
	char message[160];

	if (!report_capture.waiting)
		return;
	report_finish(note, message, sizeof(message));
	report_game_say(message);
}
