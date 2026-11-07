/*
REPORT_GAME.C

What a Save Report's report.md says of the game (port/linux/src/report.c):
the map loaded (its scenario's tag, as play_log.c names it) and, in a
multiplayer game, its gametype's name (its variant's, where it has one);
the mods played and the map's file, by their names; whether a game is
being played, not the menus (a report's issue waits until it is not); and
its note's line, the text chat's (network_chat.c).
*/

#include "cseries.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "main/console.h"
#include "cache/cache_files.h"
#include "custom_edition_cache.h"
#include "mods.h"
#include "network_chat.h"

#include <stdio.h>
#include <string.h>

/* the platform layer's (port_config.h) */
int config_boolean(const char *name);

/* how many mods are listed at most (the others counted) */
#define REPORT_GAME_MODS_LISTED 12

int report_game_playing(void)
{
	char const *map_name = global_scenario_index != NONE ? tag_get_name(global_scenario_index) : NULL;

	return map_name && map_name[0] && game_in_progress() && strcmp(map_name, "ui") && !strstr(map_name, "\\ui");
}

void report_game_describe(char *text, size_t size)
{
	char const *map_name = global_scenario_index != NONE ? tag_get_name(global_scenario_index) : NULL;
	struct game_variant *variant;
	char gametype[16];
	short index;

	if (!size)
		return;
	if (!map_name || !map_name[0] || !game_in_progress())
	{
		snprintf(text, size, "the menus");
		return;
	}
	if (!strcmp(map_name, "ui") || strstr(map_name, "\\ui"))
	{
		snprintf(text, size, "the main menu");
		return;
	}
	variant = game_engine_running() ? game_engine_get_variant() : NULL;
	gametype[0] = 0;
	for (index = 0; variant && index < NUMBEROF(gametype) - 1 && index < NUMBEROF(variant->human_readable_game_description) &&
		variant->human_readable_game_description[index]; index++)
	{
		wchar_t character = variant->human_readable_game_description[index];

		gametype[index] = (char)(character >= 0x20 && character < 0x7F ? character : '?');
		gametype[index + 1] = 0;
	}
	if (gametype[0])
		snprintf(text, size, "%s, %s", map_name, gametype);
	else
		snprintf(text, size, "%s (%s)", map_name, variant ? "multiplayer" : "campaign");
}

/* a name a mod.toml or a map's file gives, as one line's text */
static void report_game_name(char *name)
{
	for (; *name; name++)
	{
		if ((unsigned char)*name < 0x20 || *name == 0x7F)
			*name = ' ';
	}
}

/* the size of a file, by its Xbox path (0: none, or unreadable) */
static unsigned long report_game_file_size(char const *path)
{
	FILE *file = fopen(path, "rb");
	long size = 0;

	if (!file)
		return 0;
	if (!fseek(file, 0, SEEK_END))
		size = ftell(file);
	fclose(file);

	return size > 0 ? (unsigned long)size : 0;
}

void report_game_mods_and_maps(char *text, size_t size)
{
	char const *map_name = cache_file_loaded_map_name();
	char const *mod_map = mods_map_loaded() ? mods_map_name() : NULL;
	char mods[2048], file[256], path[1024], line[160];
	unsigned long file_size = 0;
	long counts[4];
	boolean in_game, played;
	int mod_count, listed = 0;
	size_t used;
	char *at;

	if (!size)
		return;
	text[0] = 0;
	/* (a map played, not the menus') */
	in_game = map_name && map_name[0] && strcmp(map_name, "ui") && report_game_playing();
	if (!in_game)
		mod_map = NULL;
	/* (the mods: those on, in the load order, each by its mod.toml's name
	and version) */
	mod_count = port_mods_report(mods, sizeof(mods), mod_map, REPORT_GAME_MODS_LISTED);
	for (at = mods; *at; at++)
	{
		if (*at == '\n')
			listed++;
	}
	played = mods_played_on_map(counts) && in_game;
	if (!mod_count)
		snprintf(text, size, "- Mods: none%s\n", loose_sounds_enabled() ? " (loose sound files on: audio.loose_sounds)" : "");
	else
	{
		line[0] = 0;
		if (played && !(counts[0] | counts[1] | counts[2] | counts[3]))
			snprintf(line, sizeof(line), ", none of the map's tags theirs");
		else if (played)
			snprintf(line, sizeof(line), ", playing %ld bitmaps, %ld sounds, %ld models, %ld string lists of the map",
				counts[1], counts[2], counts[3], counts[0]);
		else if (in_game)
			snprintf(line, sizeof(line), ", not played on this map");
		snprintf(text, size, "- Mods, in load order: %d on%s%s\n", mod_count, line,
			loose_sounds_enabled() ? " (and loose sound files: audio.loose_sounds)" : "");
		used = strlen(text);
		snprintf(text + used, size - used, "%s", mods);
		used = strlen(text);
		if (listed < mod_count)
			snprintf(text + used, size - used, "  - and %d more\n", mod_count - listed);
	}
	used = strlen(text);

	/* (the map: by its file's name and size, and the CRC its cache header
	gives, the loader's, the file not read again) */
	if (!in_game)
		snprintf(text + used, size - used, "- Map: none (the menus)\n");
	else if (custom_edition_cache_tags_loaded())
	{
		char const *loaded = custom_edition_cache_loaded_path();
		char const *slash = loaded ? strrchr(loaded, '\\') : NULL;

		if (loaded && strrchr(loaded, '/') > slash)
			slash = strrchr(loaded, '/');
		snprintf(file, sizeof(file), "%s", slash ? slash + 1 : loaded ? loaded : "unknown");
		report_game_name(file);
		file_size = loaded ? report_game_file_size(loaded) : 0;
		if (file_size)
			snprintf(line, sizeof(line), "%.1f MB, ", file_size / 1048576.0);
		else
			line[0] = 0;
		snprintf(text + used, size - used, "- Map: %s%s, Custom Edition (%s, %sheader CRC %08lx)\n",
			CUSTOM_EDITION_LEVEL_NAME_PREFIX, map_name, file, line, cache_files_get_checksum());
	}
	else
	{
		snprintf(file, sizeof(file), "%s.map", map_name);
		report_game_name(file);
		snprintf(path, sizeof(path), "%s%s", cache_files_map_directory(), file);
		file_size = report_game_file_size(path);
		if (file_size)
			snprintf(line, sizeof(line), "%.1f MB, ", file_size / 1048576.0);
		else
			line[0] = 0;
		snprintf(text + used, size - used, "- Map: %s, stock (%s, %sheader CRC %08lx)\n", map_name, file, line,
			cache_files_get_checksum());
	}
	used = strlen(text);
	snprintf(text + used, size - used, "- Custom Edition maps (game.custom_edition): %s\n",
		config_boolean("game.custom_edition") ? "on" : "off");
}

/* the line for the note: the text chat's (network_chat.c) */
int report_game_note_begin(void)
{
	if (!network_chat_report_note_begin())
		return FALSE;
	console_printf(FALSE, "Report captured: type what happened, then Enter (Esc skips)");
	return TRUE;
}

void report_game_note_cancel(void)
{
	network_chat_report_note_cancel();
}

void report_game_say(const char *text)
{
	if (text && text[0])
		console_printf(FALSE, "%s", text);
}
