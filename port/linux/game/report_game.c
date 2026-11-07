/*
REPORT_GAME.C

What a Save Report's report.md says of the game (port/linux/src/report.c):
the map loaded (its scenario's tag, as play_log.c names it) and, in a
multiplayer game, its gametype's name (its variant's, where it has one);
whether a game is being played, not the menus (a report's issue waits
until it is not); and its note's line, the text chat's (network_chat.c).
*/

#include "cseries.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "main/console.h"
#include "network_chat.h"

#include <stdio.h>
#include <string.h>

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
