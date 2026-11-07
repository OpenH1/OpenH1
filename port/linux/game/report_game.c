/*
REPORT_GAME.C

What a Save Report's report.md says of the game (port/linux/src/report.c):
the map loaded (its scenario's tag, as play_log.c names it) and, in a
multiplayer game, its gametype's name (its variant's, where it has one);
whether a game is being played (the note's line closes when it is not), or
the main menu is up (a report's issue waits for it); and its note's line,
the text chat's (network_chat.c).
*/

#include "cseries.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "main/console.h"
#include "interface/ui_widget.h"
#include "network_chat.h"

#include <stdio.h>
#include <string.h>

/* the map loaded and running: 1 for a game's, 2 for the ui map's (the
menus), 0 for none (a loading screen, the time between levels) */
static int report_game_map(void)
{
	char const *map_name = global_scenario_index != NONE ? tag_get_name(global_scenario_index) : NULL;

	if (!map_name || !map_name[0] || !game_in_progress())
		return 0;
	return strcmp(map_name, "ui") && !strstr(map_name, "\\ui") ? 1 : 2;
}

int report_game_playing(void)
{
	return report_game_map() == 1;
}

/* (the ui map, its menus up: not its loading, nor the movie before them) */
int report_game_menus(void)
{
	return report_game_map() == 2 && ui_widgets_active();
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
