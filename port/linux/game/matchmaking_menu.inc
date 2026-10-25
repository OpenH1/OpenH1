/*
MATCHMAKING_MENU.INC

Matchmaking's menus (the search itself is matchmaking.c's): Multiplayer's
MATCHMAKING (its note and panel: matchmaking_hub_note,
matchmaking_hub_status) and the Matchmaking screen (tools/port_settings.py's
_matchmaking, in Multiplayer's look): a row for each playlist, the panel
beside them (the playlist chosen, the search's time and what it does), and
the footer's SEARCH, CANCEL and BACK. A row's A, or SEARCH on the row
chosen, searches its playlist for the whole party; CANCEL (X) gives the
search up. The search goes on whatever screen is shown: its game's lobby
takes this one's place as it is joined or hosted. Included in
menu_functions.c, whose widgets' helpers it uses. The search's words
(matchmaking_status_text) are tools/harness/tests/test_matchmaking.py's.
*/

#define MATCHMAKING_ROWS 5

static struct
{
	/* the row chosen */
	short chosen;
} matchmaking_menu;

/* milliseconds as minutes and seconds ("1:05") */
static void matchmaking_time_text(unsigned long milliseconds, wchar_t *text, short size)
{
	unsigned long seconds = milliseconds / 1000;

	usnprintf(text, size - 1, L"%lu:%02lu", seconds / 60, seconds % 60);
	text[size - 1] = 0;
}

/* the panel's headline and line for the search (status: matchmaking.c's;
the playlist's name): what it does, or why it ended, or could not begin */
static void matchmaking_status_text(struct matchmaking_status const *status, wchar_t const *playlist,
	wchar_t *headline, short headline_size, wchar_t *detail, short detail_size)
{
	wchar_t time[16], host[MATCHMAKING_HOST_NAME_SIZE];
	short index;

	matchmaking_time_text(status->elapsed, time, NUMBEROF(time));
	for (index = 0; index < MATCHMAKING_HOST_NAME_SIZE - 1 && status->host_name[index]; index++)
		host[index] = (wchar_t)(unsigned char)status->host_name[index];
	host[index] = 0;
	headline[0] = detail[0] = 0;
	switch (status->stage)
	{
	case _matchmaking_looking:
		usnprintf(headline, headline_size - 1, L"SEARCHING %s", time);
		if (status->games_with_room)
			usnprintf(detail, detail_size - 1, L"%d %s with room for you%s.", status->games_with_room,
				status->games_with_room == 1 ? L"game" : L"games", status->party_size > 1 ? L"r party" : L"");
		else if (status->games)
			usnprintf(detail, detail_size - 1, L"%d %s, none with room for you%s yet.", status->games,
				status->games == 1 ? L"game" : L"games", status->party_size > 1 ? L"r party" : L"");
		else
			usnprintf(detail, detail_size - 1, L"Looking for %s games. If none is found, you host one.", playlist);
		break;
	case _matchmaking_reaching:
	case _matchmaking_joining:
		usnprintf(headline, headline_size - 1, L"JOINING %s", time);
		usnprintf(detail, detail_size - 1, L"%s's game.", host);
		break;
	case _matchmaking_handing:
		usnprintf(headline, headline_size - 1, L"FINDING A HOST %s", time);
		usnprintf(detail, detail_size - 1, L"No game has room. A party member with a better connection hosts one.");
		break;
	case _matchmaking_following:
		usnprintf(headline, headline_size - 1, L"FOLLOWING %s", time);
		usnprintf(detail, detail_size - 1, L"Joining the game your party's new leader hosts.");
		break;
	case _matchmaking_hosting:
		usnprintf(headline, headline_size - 1, L"HOSTING %s", time);
		if (status->most_players > status->players)
		{
			usnprintf(detail, detail_size - 1, L"Looking for %d more. Others searching %s join it.",
				status->most_players - status->players, playlist);
		}
		else
			usnprintf(detail, detail_size - 1, L"Full: it starts now.");
		break;
	default:
		switch (status->problem)
		{
		case _matchmaking_problem_offline:
			usnprintf(headline, headline_size - 1, L"INTERNET PLAY IS OFF");
			usnprintf(detail, detail_size - 1, L"Turn it on in Settings, Network Setup.");
			break;
		case _matchmaking_problem_member:
			usnprintf(headline, headline_size - 1, L"YOUR LEADER SEARCHES");
			usnprintf(detail, detail_size - 1, L"Your party's leader picks the playlist; you come along.");
			break;
		case _matchmaking_problem_party_too_big:
			usnprintf(headline, headline_size - 1, L"PARTY TOO BIG");
			usnprintf(detail, detail_size - 1, L"%s takes smaller parties. Pick another playlist.", playlist);
			break;
		case _matchmaking_problem_no_profile:
			usnprintf(headline, headline_size - 1, L"NO PLAYER PROFILE");
			usnprintf(detail, detail_size - 1, L"Make a profile in Settings first.");
			break;
		case _matchmaking_problem_hosting:
			usnprintf(headline, headline_size - 1, L"COULD NOT HOST");
			usnprintf(detail, detail_size - 1, L"The game could not be hosted. Try again.");
			break;
		case _matchmaking_problem_cancelled:
			usnprintf(headline, headline_size - 1, L"SEARCH CANCELLED");
			break;
		default:
			usnprintf(headline, headline_size - 1, L"PICK A PLAYLIST");
			usnprintf(detail, detail_size - 1, status->party_size > 1 ?
				L"For your party of %d: a game with room for everyone." : L"A game with room for you, by ping.",
				status->party_size);
			break;
		}
		break;
	}
	headline[headline_size - 1] = detail[detail_size - 1] = 0;
}

/* the row (0 up) of a widget of the screen's list, NONE if it is none */
static short matchmaking_row_of(struct widget_instance const *widget)
{
	return widget && !strncmp(widget->name, "mm_row_", 7) ? (short)atoi(widget->name + 7) : NONE;
}

/* "port matchmaking update" (the list's): the rows (each playlist's name,
and what its search does at its end), the panel, and the footer's SEARCH
or CANCEL (not inlined in the menus' function that runs it, as
party_screen_update is not) */
static __declspec(noinline) void matchmaking_screen_update(struct widget_instance *list)
{
	struct widget_instance *screen = screen_of(list);
	struct widget_instance *panel = named(screen, "matchmaking_panel", 0);
	struct matchmaking_status status;
	wchar_t name[MATCHMAKING_PLAYLIST_NAME_SIZE * 2], about[MATCHMAKING_ABOUT_SIZE * 2];
	wchar_t headline[64], detail[160];
	short focused = matchmaking_row_of(list->focused_child);
	short row;
	boolean searching;

	/* (the Party screen may open over it: a member following its leader) */
	party_screen_noted(list);
	matchmaking_status(&status);
	searching = status.stage != _matchmaking_idle;
	if (focused != NONE && !ui_widget_port_hover_focused())
		matchmaking_menu.chosen = focused;
	if (searching && status.playlist != NONE)
		matchmaking_menu.chosen = status.playlist;
	for (row = 0; row < MATCHMAKING_ROWS; row++)
	{
		char widget_name[16];
		struct widget_instance *item;

		snprintf(widget_name, sizeof(widget_name), "mm_row_%d", row);
		item = named(list, widget_name, 0);
		visible_set(item, row < matchmaking_playlist_count());
		if (!item || row >= matchmaking_playlist_count())
			continue;
		matchmaking_playlist_text(row, name, NUMBEROF(name), NULL, 0);
		text_set(item, name);
		{
			char note_name[16];

			snprintf(note_name, sizeof(note_name), "mm_note_%d", row);
			text_set(named(item, note_name, 0), searching && status.playlist == row ?
				(status.stage == _matchmaking_hosting ? L"HOSTING" : L"SEARCHING") : L"");
		}
	}
	matchmaking_playlist_text(matchmaking_menu.chosen, name, NUMBEROF(name), about, NUMBEROF(about));
	if (panel)
	{
		wchar_t playlist[MATCHMAKING_PLAYLIST_NAME_SIZE * 2];

		matchmaking_playlist_text(searching || status.problem ? status.playlist : matchmaking_menu.chosen, playlist,
			NUMBEROF(playlist), NULL, 0);
		/* (a problem of another row's search is said of the row chosen too) */
		if (!searching && status.problem == _matchmaking_problem_party_too_big)
			status.problem = matchmaking_can_search(matchmaking_menu.chosen);
		matchmaking_status_text(&status, playlist, headline, NUMBEROF(headline), detail, NUMBEROF(detail));
		menu_text_box_wrap(named(panel, "panel_title", 0), name, 1);
		menu_text_box_wrap(named(panel, "panel_about", 0), about, 2);
		menu_text_box_wrap(named(panel, "panel_status", 0), headline, 1);
		menu_text_box_wrap(named(panel, "panel_detail", 0), detail, 5);
	}
	{
		struct widget_instance *bar = named(list, "matchmaking_prompts", 0);

		menu_prompts_show(bar, "prompt_search", !searching);
		menu_prompts_show(bar, "prompt_cancel", searching);
	}
}

/* "port matchmaking button" (a row's A or click, and the prompts'): a
playlist searched, the search given up, or back */
static boolean matchmaking_screen_button(struct widget_instance *widget, short controller)
{
	short row = matchmaking_row_of(widget);
	struct matchmaking_status status;

	matchmaking_status(&status);
	if (!strcmp(widget->name, "prompt_back"))
	{
		event_manager_post_button(controller, BUTTON_B);
		return TRUE;
	}
	if (!strcmp(widget->name, "prompt_cancel"))
	{
		matchmaking_cancel();
		ui_play_audio_feedback_sound(SOUND_BACK);
		return TRUE;
	}
	if (status.stage != _matchmaking_idle)
		return TRUE;
	if (row == NONE)
		row = matchmaking_menu.chosen;
	matchmaking_menu.chosen = row;
	if (!matchmaking_search(row, controller >= 0 && controller < 4 ? controller : 0))
		return campaign_fail();
	ui_play_audio_feedback_sound(SOUND_FORWARD);
	return TRUE;
}

/* "port matchmaking x" (the screen's X): CANCEL, while it is shown */
static boolean matchmaking_screen_key(struct widget_instance *widget, short controller)
{
	struct widget_instance *cancel = named(screen_of(widget), "prompt_cancel", 0);

	(void)controller;
	if (cancel && cancel->visible)
	{
		matchmaking_cancel();
		ui_play_audio_feedback_sound(SOUND_BACK);
	}
	return TRUE;
}

/* Multiplayer's MATCHMAKING: the note at its end (a search's), and the
panel's headline and line */
static void matchmaking_hub_note(wchar_t *text, short size)
{
	struct matchmaking_status status;

	matchmaking_status(&status);
	text[0] = 0;
	if (status.stage == _matchmaking_hosting)
		usnprintf(text, size - 1, L"HOSTING");
	else if (status.stage != _matchmaking_idle)
		usnprintf(text, size - 1, L"SEARCHING");
	text[size - 1] = 0;
}

static void matchmaking_hub_status(wchar_t *headline, short headline_size, wchar_t *detail, short detail_size)
{
	struct matchmaking_status status;
	wchar_t playlist[MATCHMAKING_PLAYLIST_NAME_SIZE * 2];

	matchmaking_status(&status);
	if (status.stage == _matchmaking_idle)
	{
		usnprintf(headline, headline_size - 1, L"FIND A GAME");
		usnprintf(detail, detail_size - 1, L"Team Slayer, Slayer, CTF,\r\nSWAT and Big Team.");
		headline[headline_size - 1] = detail[detail_size - 1] = 0;
		return;
	}
	matchmaking_playlist_text(status.playlist, playlist, NUMBEROF(playlist), NULL, 0);
	matchmaking_status_text(&status, playlist, headline, headline_size, detail, detail_size);
}
