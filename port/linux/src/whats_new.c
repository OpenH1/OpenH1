/*
WHATS_NEW.C

What's new (whats_new.h, docs/menu_screens.md): a small card over the new
menu engine's main menu, the first time it is shown after the game was
updated to a new build: "WHAT'S NEW IN BUILD 2290", that build's highlights,
READ ALL (its release's page, in the browser) and OK. Once dismissed it is
not shown again until the build changes.

The notes are the build's own: a release's build embeds them with the
menus' files as whats_new.txt (tools/release_notes.py writes it in CI, from
the pull requests merged since the last release), so nothing is fetched. A
build without them (a local one, a branch's) never shows the card.

The build seen last is kept in whats_new_seen.txt beside config.toml. With
none there, a fresh install (config.toml written with the defaults as this
run started) notes its build without the card; an older install (one from
before the card) shows it. A run nobody watches (report.c's: a test's)
neither shows it nor notes the build, unless debug.whats_new is "show".
*/

#include "whats_new.h"
#include "port_config.h"
#include "report.h"
#include "halo_menus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HALO_GUEST
#include <SDL3/SDL.h>
#endif

void platform_log(const char *format, ...);
int platform_open_url(const char *url);
/* (menu_functions.c's: the new engine's main menu drawn since the last
asked, and the card opened over it) */
int pc_menu_whats_new_ready(void);
int pc_menu_whats_new_open(void);

#define WHATS_NEW_NOTES "whats_new.txt"
#define WHATS_NEW_SEEN "whats_new_seen.txt"
/* (the notes' file read no further: it is a few lines) */
#define WHATS_NEW_NOTES_MAXIMUM 8192

static struct
{
	/* decided (the card opened, or not due) */
	int done;
	int notes_read;
	struct whats_new_notes notes;
} whats_new;

/* ---------- the notes */

int whats_new_parse(char const *text, unsigned long size, struct whats_new_notes *notes)
{
	unsigned long at = 0;

	memset(notes, 0, sizeof(*notes));
	if (!text)
		return 0;
	if (size > WHATS_NEW_NOTES_MAXIMUM)
		size = WHATS_NEW_NOTES_MAXIMUM;
	while (at < size)
	{
		char line[512];
		unsigned long end = at, length;

		while (end < size && text[end] != '\n')
			end++;
		length = end - at;
		if (length && text[at + length - 1] == '\r')
			length--;
		if (length < sizeof(line))
		{
			memcpy(line, text + at, length);
			line[length] = 0;
			if (!strncmp(line, "build ", 6))
			{
				char *number_end = NULL;
				long build = strtol(line + 6, &number_end, 10);

				if (number_end && !*number_end && build > 0)
					notes->build = build;
			}
			else if (!strncmp(line, "page https://", 13) && !strpbrk(line + 5, " \t\"'<>"))
			{
				snprintf(notes->page, sizeof(notes->page), "%s", line + 5);
			}
			else if (!strncmp(line, "- ", 2) && line[2] && notes->line_count < WHATS_NEW_MAXIMUM_LINES)
			{
				unsigned long index;
				int printable = 1;

				/* (no control characters: a line of words) */
				for (index = 2; line[index]; index++)
				{
					if ((unsigned char)line[index] < 0x20 || line[index] == 0x7f)
						printable = 0;
				}
				if (printable && strlen(line + 2) < WHATS_NEW_LINE_LENGTH)
					snprintf(notes->lines[notes->line_count++], WHATS_NEW_LINE_LENGTH, "%s", line + 2);
			}
		}
		at = end + 1;
	}
	if (!notes->build || !notes->line_count)
	{
		memset(notes, 0, sizeof(*notes));
		return 0;
	}
	return 1;
}

int whats_new_decide(long build, long seen, int fresh)
{
	if (build <= 0 || seen == build)
		return WHATS_NEW_NOTHING;
	if (seen <= 0 && fresh)
		return WHATS_NEW_NOTE;
	return WHATS_NEW_SHOW;
}

/* the notes embedded with the menus' files (read once the menus are) */
static struct whats_new_notes const *whats_new_notes(void)
{
	char const *path;
	unsigned char const *data;
	unsigned long size;
	long index;

	if (whats_new.notes_read)
		return whats_new.notes.build ? &whats_new.notes : NULL;
	whats_new.notes_read = 1;
	for (index = 0; halo_menus_file_at(index, &path, &data, &size); index++)
	{
		if (strcmp(path, WHATS_NEW_NOTES))
			continue;
		if (!whats_new_parse((char const *)data, size, &whats_new.notes))
			platform_log("what's new: %s has no build and highlights; no card", WHATS_NEW_NOTES);
		break;
	}
	return whats_new.notes.build ? &whats_new.notes : NULL;
}

/* ---------- the build seen */

static void whats_new_seen_path(char *path, size_t size)
{
	config_folder(path, size);
	snprintf(path + strlen(path), size - strlen(path), "%s", WHATS_NEW_SEEN);
}

static long whats_new_seen(void)
{
	char path[1100];
	size_t size = 0;
	char *text;
	long seen = 0;

	whats_new_seen_path(path, sizeof(path));
	text = config_file_read(path, &size);
	if (!text)
		return 0;
	seen = strtol(text, NULL, 10);
	free(text);
	return seen > 0 ? seen : 0;
}

static void whats_new_note(long build)
{
	char path[1100], temporary[1104], text[32];
	int written;
#ifdef HALO_GUEST
	FILE *file;
#endif

	whats_new_seen_path(path, sizeof(path));
	snprintf(temporary, sizeof(temporary), "%s.new", path);
	snprintf(text, sizeof(text), "%ld\n", build);
#ifdef HALO_GUEST
	file = fopen(temporary, "wb");
	written = file && fwrite(text, 1, strlen(text), file) == strlen(text);
	if (file)
		written = fclose(file) == 0 && written;
	if (written)
		written = rename(temporary, path) == 0;
	if (!written)
		remove(temporary);
#else
	written = SDL_SaveFile(temporary, text, strlen(text));
	if (written)
		written = SDL_RenamePath(temporary, path);
	if (!written)
		SDL_RemovePath(temporary);
#endif
	if (written)
		platform_log("what's new: build %ld noted as seen", build);
	else
		platform_log("what's new: cannot write %s", path);
}

/* ---------- the game's */

void whats_new_poll(void)
{
	struct whats_new_notes const *notes;
	long seen;
	int decision;

	if (whats_new.done || !pc_menu_whats_new_ready())
		return;
	notes = whats_new_notes();
	if (!notes)
	{
		whats_new.done = 1;
		return;
	}
	if (strcmp(config_string("debug.whats_new"), "show") && report_run_unattended())
	{
		whats_new.done = 1;
		platform_log("what's new: not shown in a run nobody watches (build %ld)", notes->build);
		return;
	}
	seen = whats_new_seen();
	decision = whats_new_decide(notes->build, seen, config_written_fresh());
	if (decision == WHATS_NEW_NOTE)
	{
		whats_new.done = 1;
		platform_log("what's new: a fresh install of build %ld; no card", notes->build);
		whats_new_note(notes->build);
		return;
	}
	if (decision != WHATS_NEW_SHOW)
	{
		whats_new.done = 1;
		return;
	}
	if (!pc_menu_whats_new_open())
		return;
	whats_new.done = 1;
	platform_log("what's new: showing build %ld's card (build %ld seen last)", notes->build, seen);
}

long whats_new_build(void)
{
	struct whats_new_notes const *notes = whats_new_notes();

	return notes ? notes->build : 0;
}

int whats_new_line(int index, char *text, unsigned long size)
{
	struct whats_new_notes const *notes = whats_new_notes();

	if (!notes || index < 0 || index >= notes->line_count)
		return 0;
	snprintf(text, size, "%s", notes->lines[index]);
	return 1;
}

void whats_new_dismiss(int read_all)
{
	struct whats_new_notes const *notes = whats_new_notes();

	if (!notes)
		return;
	if (read_all && notes->page[0])
	{
		if (report_run_unattended())
			platform_log("what's new: the notes' page: %s", notes->page);
		else if (!platform_open_url(notes->page))
			platform_log("what's new: %s could not be opened", notes->page);
	}
	whats_new_note(notes->build);
}
