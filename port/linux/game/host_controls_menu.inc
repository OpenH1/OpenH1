/*
HOST_CONTROLS_MENU.INC

Host Controls (tools/port_settings.py's _host_controls, in the server
browser's look: the menus' shared parts), the pause menu's HOST CONTROLS of
the host of a network game (menu_tags.c's pause_patch), which the
scoreboard's "Host controls..." and the console's host_controls open too
(network_host_controls_open). Included in menu_functions.c, whose widgets'
helpers it uses. What it does is network_host_controls.c's, by the host's
own ways: this file only shows it.

- PLAYERS: the game's players (their name, team, score, ping and tag:
  HOST, BOT, OPENH1, OPENCE; in a Legacy game the host cannot tell the
  last two apart, and shows neither), and for the player chosen the panel's
  KICK, BAN (pressed twice), MOVE TO RED or BLUE (a team game whose machines
  all hear of it), MUTE FOR EVERYONE, those the player may have done to them
  (network_host_controls_player_actions).
- GAME: End game now and Restart round (each pressed twice), Lock teams,
  Allow votekick, Listing (an internet game's), Max players (left and
  right: never fewer than are in it), Allow OpenCE players (an OpenH1
  game's), then the bots' rows (network_host_controls_bot_rows: BOTS, BOT
  SKILL, BOT TEAMS, MAKE ROOM FOR PLAYERS and FILL TO, in a gametype the
  bots play).
- BANS: bans.txt's lines (read again each second), and the panel's UNBAN.

A or a click on a player's or a ban's row chooses it and puts the focus on
the panel's buttons; on a game's row it changes the row (left and right
step it). What was done is said in the panel for a few seconds.
*/

/* the rows and the panel's buttons (port_settings.py's HOST_ROWS,
HOST_ACTIONS) */
#define HOST_CONTROLS_ROWS 14
#define HOST_CONTROLS_ACTIONS 4
/* milliseconds a second press confirms in, a notice shows, the bans are
read again after */
#define HOST_CONTROLS_CONFIRM_TIME 5000
#define HOST_CONTROLS_NOTICE_TIME 5000
#define HOST_CONTROLS_BANS_INTERVAL 1000
#define HOST_CONTROLS_TEXT_LENGTH 160
/* the team column's colours (red, blue) */
#define HOST_CONTROLS_RED 0xE86A5AFFUL
#define HOST_CONTROLS_BLUE 0x6FB4FFFFUL

enum
{
	_host_tab_players,
	_host_tab_game,
	_host_tab_bans,
	NUMBER_OF_HOST_TABS
};

/* Game's rows (the bots' after them) */
enum
{
	_host_game_end,
	_host_game_restart,
	_host_game_lock_teams,
	_host_game_votekick,
	_host_game_listing,
	_host_game_maximum_players,
	_host_game_opence,
	NUMBER_OF_HOST_GAME_ROWS,
	/* (the bots' rows: this and their row) */
	_host_game_bot_first = NUMBER_OF_HOST_GAME_ROWS,
	MAXIMUM_HOST_GAME_ROWS = NUMBER_OF_HOST_GAME_ROWS + 16,
};

/* the panel's buttons' actions: the player's (_host_controls_*), and a ban's */
enum
{
	_host_button_unban = NUMBER_OF_HOST_CONTROLS_ACTIONS,
	/* (what a second press confirms: Ban, or a game's row) */
	_host_confirm_none = NONE,
};

static struct
{
	short tab;
	/* the first entry the rows show */
	short first;
	/* the entry chosen: a player (datum index), a game's row, a ban's line */
	long chosen_player;
	short chosen_game_row;
	char chosen_ban[HOST_CONTROLS_BAN_LINE_SIZE];
	/* the press a second confirms (a _host_controls_ban, a game's row plus
	MAXIMUM_HOST_GAME_ROWS), and when it was asked */
	short confirm;
	unsigned long confirm_time;
	/* what was done, and when */
	wchar_t notice[HOST_CONTROLS_TEXT_LENGTH];
	unsigned long notice_time;
	/* this frame's entries */
	struct host_controls_player players[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	short player_count;
	short game_rows[MAXIMUM_HOST_GAME_ROWS];
	short game_row_count;
	struct host_controls_ban bans[MAXIMUM_HOST_CONTROLS_BANS];
	char ban_lines[MAXIMUM_HOST_CONTROLS_BANS][HOST_CONTROLS_BAN_LINE_SIZE];
	short ban_count;
	unsigned long bans_time;
	boolean bans_read;
	/* the buttons' actions shown (NONE: hidden) */
	short buttons[HOST_CONTROLS_ACTIONS];
	/* each row's team (NONE: none), for its colour */
	short row_teams[HOST_CONTROLS_ROWS];
	/* the opening's player, to give the focus to once its row is shown;
	the first row, once shown (the screen opened, a tab chosen) */
	boolean focus_chosen;
	boolean focus_first;
	/* the tab the strip last showed */
	short tab_shown;
} host_menu;

/* a Host Controls row's index (0 up), else NONE */
static short host_row_index(struct widget_instance const *row)
{
	return row && !strncmp(row->name, "host_item_", 10) && row->name[10] >= '1' && row->name[10] <= '9' ?
		(short)(atoi(row->name + 10) - 1) : NONE;
}

static short host_action_index(struct widget_instance const *button)
{
	return button && !strncmp(button->name, "host_action_", 12) ? (short)atoi(button->name + 12) : NONE;
}

/* the rows the list has (tools/port_settings.py's HOST_ROWS on the
computer, its touchscreen's fewer and taller: TOUCH_HOST_ROWS), at most
HOST_CONTROLS_ROWS */
static short host_rows_of(struct widget_instance *list)
{
	struct widget_instance *child;
	short rows = 0;

	for (child = list->child; child; child = child->next)
	{
		if (host_row_index(child) != NONE && rows < HOST_CONTROLS_ROWS)
			rows++;
	}
	return rows ? rows : HOST_CONTROLS_ROWS;
}

static void host_focus(struct widget_instance *list, struct widget_instance *child)
{
	struct widget_instance *each;
	short index = 0;

	for (each = list->child; each; each = each->next, index++)
	{
		if (each == child)
		{
			list->focused_child = child;
			list->parameters.list.selected_index = index;
			return;
		}
	}
}

/* ui_widget.c's colours (pc_menu_text_color): a player's team in its own */
static boolean host_controls_text_color(struct widget_instance *widget, real_argb_color *color)
{
	short row = widget->parent ? host_row_index(widget->parent) : NONE;
	unsigned long rgba;

	if (row < 0 || row >= HOST_CONTROLS_ROWS || strcmp(widget->name, "host_item_team") ||
		host_menu.row_teams[row] == NONE)
	{
		return FALSE;
	}
	rgba = host_menu.row_teams[row] ? HOST_CONTROLS_BLUE : HOST_CONTROLS_RED;
	color->red = (real)((rgba >> 24) & 0xFF) / 255.0f;
	color->green = (real)((rgba >> 16) & 0xFF) / 255.0f;
	color->blue = (real)((rgba >> 8) & 0xFF) / 255.0f;
	return TRUE;
}

static void host_notice(wchar_t const *format, wchar_t const *name)
{
	usnprintf(host_menu.notice, NUMBEROF(host_menu.notice) - 1, format, name ? name : L"");
	host_menu.notice[NUMBEROF(host_menu.notice) - 1] = 0;
	host_menu.notice_time = system_milliseconds();
}

static boolean host_confirming(short confirm)
{
	return host_menu.confirm == confirm && confirm != _host_confirm_none &&
		system_milliseconds() - host_menu.confirm_time < HOST_CONTROLS_CONFIRM_TIME;
}

/* a press that is done on the second: TRUE once confirmed */
static boolean host_confirmed(short confirm)
{
	if (host_confirming(confirm))
	{
		host_menu.confirm = _host_confirm_none;
		return TRUE;
	}
	host_menu.confirm = confirm;
	host_menu.confirm_time = system_milliseconds();
	ui_play_audio_feedback_sound(SOUND_CURSOR);
	return FALSE;
}

static wchar_t const *host_team_name(short team)
{
	return team == 0 ? L"Red" : team == 1 ? L"Blue" : L"";
}

static wchar_t const *host_tag_name(short tag)
{
	switch (tag)
	{
	case _host_controls_tag_host:
		return L"HOST";
	case _host_controls_tag_bot:
		return L"BOT";
	case _host_controls_tag_openh1:
		return L"OPENH1";
	case _host_controls_tag_opence:
		return L"OPENCE";
	default:
		return L"";
	}
}

/* (the chosen player's entry, else NULL) */
static struct host_controls_player *host_chosen_player(void)
{
	short index;

	for (index = 0; index < host_menu.player_count; index++)
	{
		if (host_menu.players[index].player_index == host_menu.chosen_player)
			return &host_menu.players[index];
	}
	return NULL;
}

/* ---- the game's rows */

/* the game this machine hosts (its record) */
static struct network_game *host_game(void)
{
	void *server = global_network_game_server_get();

	return server ? network_game_server_get_game(server) : NULL;
}

/* the rows Game shows, in their order */
static void host_game_rows_read(void)
{
	boolean teams = game_engine_running() && game_engine_has_teams();
	short bots = network_host_controls_bot_rows();
	short index;

	host_menu.game_row_count = 0;
	for (index = 0; index < NUMBER_OF_HOST_GAME_ROWS; index++)
	{
		boolean shown = TRUE;

		switch (index)
		{
		case _host_game_end:
		case _host_game_restart:
			shown = game_engine_running() && game_engine_can_score() && !network_host_controls_restarting();
			break;
		case _host_game_lock_teams:
			shown = teams;
			break;
		case _host_game_listing:
			shown = multiplayer.mode == _multiplayer_mode_host_internet && config_boolean("network.online") &&
				config_boolean("network.public_lobby");
			break;
		case _host_game_opence:
			shown = network_standard_hosting();
			break;
		default:
			break;
		}
		if (shown)
			host_menu.game_rows[host_menu.game_row_count++] = index;
	}
	for (index = 0; index < bots && host_menu.game_row_count < MAXIMUM_HOST_GAME_ROWS; index++)
		host_menu.game_rows[host_menu.game_row_count++] = (short)(_host_game_bot_first + index);
}

/* a game's row's label, value and what it does */
static void host_game_row_text(short row, wchar_t *label, wchar_t *value, wchar_t *help, short size)
{
	struct network_game *game = host_game();
	boolean confirming = host_confirming((short)(row + MAXIMUM_HOST_GAME_ROWS));

	label[0] = value[0] = help[0] = 0;
	switch (row)
	{
	case _host_game_end:
		usnprintf(label, size - 1, L"END GAME NOW");
		usnprintf(value, size - 1, confirming ? L"PRESS AGAIN" : L"");
		usnprintf(help, size - 1, L"Ends the game now, as its time limit does: the scores, then the lobby, "
			L"where you pick the next game.");
		break;
	case _host_game_restart:
		usnprintf(label, size - 1, L"RESTART ROUND");
		usnprintf(value, size - 1, confirming ? L"PRESS AGAIN" : L"");
		usnprintf(help, size - 1, L"Ends the game and starts it again on this map and gametype, after a moment "
			L"in the lobby. Scores start over.");
		break;
	case _host_game_lock_teams:
		usnprintf(label, size - 1, L"LOCK TEAMS");
		usnprintf(value, size - 1, network_host_controls_teams_locked() ? L"ON" : L"OFF");
		usnprintf(help, size - 1, L"Players can't switch teams in the lobby. You can still move anyone, "
			L"and yourself.");
		break;
	case _host_game_votekick:
		usnprintf(label, size - 1, L"ALLOW VOTEKICK");
		usnprintf(value, size - 1, config_boolean("network.votekick") ? L"ON" : L"OFF");
		usnprintf(help, size - 1, L"Players may vote to kick a player, from the scoreboard. You can always "
			L"kick.");
		break;
	case _host_game_listing:
		usnprintf(label, size - 1, L"LISTING");
		usnprintf(value, size - 1, server_settings_private() ? L"PRIVATE" : L"PUBLIC");
		usnprintf(help, size - 1, server_settings_private() ? L"Only players with your invite link can join." :
			L"Listed in everyone's server browser.");
		break;
	case _host_game_maximum_players:
		usnprintf(label, size - 1, L"MAX PLAYERS");
		usnprintf(value, size - 1, L"%d", game ? (int)game->maximum_players : 0);
		usnprintf(help, size - 1, L"The most players who can be in the game (%d are in it now).",
			game ? (int)game->player_count : 0);
		break;
	case _host_game_opence:
		usnprintf(label, size - 1, L"ALLOW OPENCE PLAYERS");
		usnprintf(value, size - 1, network_host_controls_opence_joins() ? L"ON" : L"OFF");
		usnprintf(help, size - 1, L"Players of OpenCE may join from now on, playing at 30 ticks. Those in "
			L"the game stay.");
		break;
	default:
		network_host_controls_bot_row((short)(row - _host_game_bot_first), label, value, help, size);
		break;
	}
	label[size - 1] = value[size - 1] = help[size - 1] = 0;
}

/* a game's row stepped (left -1, right 1) or chosen (A: 0) */
static void host_game_row_change(short row, short step)
{
	struct network_game *game = host_game();

	switch (row)
	{
	case _host_game_end:
	case _host_game_restart:
		if (step || !host_confirmed((short)(row + MAXIMUM_HOST_GAME_ROWS)))
			return;
		if (row == _host_game_end ? network_host_controls_end_game() : network_host_controls_restart())
		{
			ui_play_audio_feedback_sound(SOUND_FORWARD);
			/* (back to the game, which shows its scores) */
			ui_widgets_close_all();
		}
		else
			ui_play_audio_feedback_sound(SOUND_ERROR);
		return;
	case _host_game_lock_teams:
		network_host_controls_lock_teams(!network_host_controls_teams_locked());
		break;
	case _host_game_votekick:
		if (!config_write("network.votekick", config_boolean("network.votekick") ? "false" : "true"))
			platform_log("menus: could not write network.votekick to config.toml");
		break;
	case _host_game_listing:
		server_settings_private_set(!server_settings_private());
		p2p_set_hosting_public(!server_settings_private());
		break;
	case _host_game_maximum_players:
	{
		short values[NUMBEROF(maximum_players)];
		short index, chosen;

		if (!game)
			return;
		for (index = 0; index < NUMBEROF(maximum_players); index++)
			values[index] = maximum_players[index];
		chosen = network_host_controls_maximum_step(values, NUMBEROF(values), game->maximum_players,
			game->player_count, (short)(step ? step : 1));
		if (chosen == game->maximum_players)
		{
			ui_play_audio_feedback_sound(SOUND_ERROR);
			return;
		}
		network_game_server_port_set_settings(game->name, chosen);
		/* (and Server Setup's, for the next game hosted) */
		for (index = 0; index < NUMBEROF(maximum_players); index++)
		{
			if (maximum_players[index] == chosen)
				*server_settings_maximum_players_index() = index;
		}
		break;
	}
	case _host_game_opence:
		network_host_controls_set_opence_joins(!network_host_controls_opence_joins());
		break;
	default:
		network_host_controls_bot_row_step((short)(row - _host_game_bot_first), (short)(step ? step : 1));
		break;
	}
	ui_play_audio_feedback_sound(SOUND_CURSOR);
}

/* ---- the screen */

static short host_entry_count(void)
{
	switch (host_menu.tab)
	{
	case _host_tab_players:
		return host_menu.player_count;
	case _host_tab_game:
		return host_menu.game_row_count;
	default:
		return host_menu.ban_count;
	}
}

/* the entry of the row (NONE: none) */
static short host_row_entry(short row)
{
	short entry = (short)(host_menu.first + row);

	return row != NONE && entry < host_entry_count() ? entry : NONE;
}

static void host_tab_set(short tab)
{
	host_menu.tab = (short)((tab + NUMBER_OF_HOST_TABS) % NUMBER_OF_HOST_TABS);
	host_menu.first = 0;
	host_menu.confirm = _host_confirm_none;
	host_menu.bans_read = FALSE;
	host_menu.focus_first = TRUE;
}

/* "port host init" (the list made): Players, nothing chosen */
static boolean host_controls_initialize(void)
{
	host_menu.tab = _host_tab_players;
	host_menu.first = 0;
	host_menu.chosen_player = NONE;
	host_menu.chosen_game_row = NONE;
	host_menu.chosen_ban[0] = 0;
	host_menu.confirm = _host_confirm_none;
	host_menu.notice_time = 0;
	host_menu.bans_read = FALSE;
	host_menu.focus_chosen = FALSE;
	host_menu.focus_first = TRUE;
	host_menu.tab_shown = NONE;
	return TRUE;
}

/* "port host open" (the pause menu's HOST CONTROLS): the host's alone */
static boolean host_controls_open(void)
{
	if (!network_host_controls_available())
	{
		ui_play_audio_feedback_sound(SOUND_ERROR);
		return FALSE;
	}
	ui_play_audio_feedback_sound(SOUND_FORWARD);
	return TRUE;
}

/* the entries of the tab shown, read anew */
static void host_entries_read(void)
{
	unsigned long now = system_milliseconds();

	host_menu.player_count = network_host_controls_players(host_menu.players, NUMBEROF(host_menu.players));
	host_game_rows_read();
	if (host_menu.tab == _host_tab_bans && (!host_menu.bans_read || now - host_menu.bans_time >= HOST_CONTROLS_BANS_INTERVAL))
	{
		host_menu.ban_count = network_host_controls_bans(host_menu.bans, host_menu.ban_lines,
			MAXIMUM_HOST_CONTROLS_BANS);
		host_menu.bans_time = now;
		host_menu.bans_read = TRUE;
	}
}

/* an entry chosen (the row's: its focus, a click, A) */
static void host_choose(short entry)
{
	if (entry == NONE)
		return;
	switch (host_menu.tab)
	{
	case _host_tab_players:
		if (host_menu.players[entry].player_index != host_menu.chosen_player)
			host_menu.confirm = _host_confirm_none;
		host_menu.chosen_player = host_menu.players[entry].player_index;
		break;
	case _host_tab_game:
		if (host_menu.game_rows[entry] != host_menu.chosen_game_row)
			host_menu.confirm = _host_confirm_none;
		host_menu.chosen_game_row = host_menu.game_rows[entry];
		break;
	default:
		if (strcmp(host_menu.ban_lines[entry], host_menu.chosen_ban))
			host_menu.confirm = _host_confirm_none;
		csstrncpy(host_menu.chosen_ban, host_menu.ban_lines[entry], sizeof(host_menu.chosen_ban) - 1);
		host_menu.chosen_ban[sizeof(host_menu.chosen_ban) - 1] = 0;
		break;
	}
}

/* the entry chosen's place among the tab's (NONE: none) */
static short host_chosen_entry(void)
{
	short index;

	for (index = 0; index < host_entry_count(); index++)
	{
		if ((host_menu.tab == _host_tab_players && host_menu.players[index].player_index == host_menu.chosen_player) ||
			(host_menu.tab == _host_tab_game && host_menu.game_rows[index] == host_menu.chosen_game_row) ||
			(host_menu.tab == _host_tab_bans && !strcmp(host_menu.ban_lines[index], host_menu.chosen_ban)))
		{
			return index;
		}
	}
	return NONE;
}

/* the column titles of the tab */
static void host_headers_show(struct widget_instance *screen)
{
	static wchar_t const *const headers[NUMBER_OF_HOST_TABS][5] =
	{
		{ L"PLAYER", L"TEAM", L"SCORE", L"PING", L"" },
		{ L"SETTING", L"", L"", L"", L"" },
		{ L"BANNED", L"", L"", L"", L"WHEN" },
	};
	static char const *const names[5] = { "header_label", "header_team", "header_score", "header_ping",
		"header_value" };
	short index;

	for (index = 0; index < 5; index++)
		text_set(named(screen, names[index], 0), headers[host_menu.tab][index]);
}

/* a row's texts (an entry of the tab), its team's colour, its tag's pill */
static void host_row_show(struct widget_instance *row, short row_index, short entry)
{
	wchar_t label[HOST_CONTROLS_TEXT_LENGTH], value[HOST_CONTROLS_TEXT_LENGTH], help[HOST_CONTROLS_TEXT_LENGTH];
	wchar_t team[16] = L"", score[64] = L"", ping[16] = L"";
	struct widget_instance *tag = named(row, "host_item_tag", 0);
	short tag_index = _host_controls_tag_none;

	label[0] = value[0] = 0;
	host_menu.row_teams[row_index] = NONE;
	switch (host_menu.tab)
	{
	case _host_tab_players:
	{
		struct host_controls_player const *player = &host_menu.players[entry];

		ustrncpy(label, player->name, NUMBEROF(label) - 1);
		label[NUMBEROF(label) - 1] = 0;
		if (game_engine_running() && game_engine_has_teams())
		{
			usnprintf(team, NUMBEROF(team) - 1, L"%s", host_team_name(player->team));
			host_menu.row_teams[row_index] = (short)PIN(player->team, 0, 1);
		}
		if (game_engine)
		{
			wchar_t formatted[256];

			formatted[0] = 0;
			game_engine->format_player_score(player->player_index, formatted);
			ustrncpy(score, formatted, NUMBEROF(score) - 1);
			score[NUMBEROF(score) - 1] = 0;
		}
		if (player->tag == _host_controls_tag_host || player->tag == _host_controls_tag_bot)
			usnprintf(ping, NUMBEROF(ping) - 1, L"");
		else if (player->ping == NONE)
			usnprintf(ping, NUMBEROF(ping) - 1, L"-");
		else
			usnprintf(ping, NUMBEROF(ping) - 1, L"%ld", MIN(player->ping, 999));
		tag_index = player->tag;
		break;
	}
	case _host_tab_game:
		host_game_row_text(host_menu.game_rows[entry], label, value, help, HOST_CONTROLS_TEXT_LENGTH);
		break;
	default:
	{
		struct host_controls_ban const *ban = &host_menu.bans[entry];

		text_to_wide(ban->players[0] ? ban->players : ban->address, label, HOST_CONTROLS_TEXT_LENGTH);
		text_to_wide(ban->when, value, HOST_CONTROLS_TEXT_LENGTH);
		break;
	}
	}
	menu_text_box_fit(named(row, "host_item_label", 0), label, 0);
	text_set(named(row, "host_item_team", 0), team);
	menu_text_box_fit(named(row, "host_item_score", 0), score, 0);
	text_set(named(row, "host_item_ping", 0), ping);
	menu_text_box_fit(named(row, "host_item_value", 0), value, 0);
	visible_set(tag, tag_index != _host_controls_tag_none);
	if (tag)
	{
		text_set(tag, host_tag_name(tag_index));
		tag->animation.current_frame_index = tag_index == _host_controls_tag_openh1 ? 0 :
			tag_index == _host_controls_tag_host ? 2 : 1;
	}
}

/* the panel's buttons for the entry chosen: a player's actions, a ban's
UNBAN */
static void host_buttons_show(struct widget_instance *list, struct host_controls_player const *player,
	boolean ban)
{
	static short const order[] = { _host_controls_kick, _host_controls_ban, _host_controls_move_team,
		_host_controls_mute };
	short shown = 0, index;

	for (index = 0; index < HOST_CONTROLS_ACTIONS; index++)
		host_menu.buttons[index] = NONE;
	if (player)
	{
		for (index = 0; index < NUMBEROF(order); index++)
		{
			if (TEST_FLAG(player->actions, order[index]))
				host_menu.buttons[shown++] = order[index];
		}
	}
	else if (ban)
		host_menu.buttons[shown++] = _host_button_unban;
	/* (the buttons from the panel's foot up: its last the lowest) */
	for (index = 0; index < HOST_CONTROLS_ACTIONS; index++)
	{
		char name[24];
		struct widget_instance *button;
		short action = index >= HOST_CONTROLS_ACTIONS - shown ?
			host_menu.buttons[index - (HOST_CONTROLS_ACTIONS - shown)] : NONE;
		wchar_t text[64];

		snprintf(name, sizeof(name), "host_action_%d", index);
		button = named(list, name, 0);
		visible_set(button, action != NONE);
		text[0] = 0;
		switch (action)
		{
		case _host_controls_kick:
			usnprintf(text, NUMBEROF(text) - 1, L"KICK");
			break;
		case _host_controls_ban:
			usnprintf(text, NUMBEROF(text) - 1, host_confirming(_host_controls_ban) ? L"CONFIRM BAN" : L"BAN");
			break;
		case _host_controls_move_team:
			usnprintf(text, NUMBEROF(text) - 1, player && player->team == 1 ? L"MOVE TO RED" : L"MOVE TO BLUE");
			break;
		case _host_controls_mute:
			usnprintf(text, NUMBEROF(text) - 1, player && network_chat_host_mute_minutes(
				(short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player->player_index)) ? L"UNMUTE FOR ALL" : L"MUTE FOR ALL");
			break;
		case _host_button_unban:
			usnprintf(text, NUMBEROF(text) - 1, L"UNBAN");
			break;
		default:
			break;
		}
		/* (the confirm key's prompt on the button with the focus alone, as
		the device in use has it: ui_widget.c; a touchscreen's buttons are
		tapped, and name no key) */
		if (text[0] && button && button == list->focused_child && !ui_widget_port_touch_menus())
		{
			wchar_t words[64];

			ustrncpy(words, text, NUMBEROF(words) - 1);
			words[NUMBEROF(words) - 1] = 0;
			usnprintf(text, NUMBEROF(text) - 1, L"%%a-button %s", words);
		}
		text[NUMBEROF(text) - 1] = 0;
		text_set(button, text);
	}
}

/* the panel: the entry chosen's name, what it is, and what was done or what
its row does */
static void host_panel_show(struct widget_instance *list, short chosen)
{
	struct widget_instance *panel = list->parameters.list.extended_description;
	wchar_t title[HOST_CONTROLS_TEXT_LENGTH], where[HOST_CONTROLS_TEXT_LENGTH], status[HOST_CONTROLS_TEXT_LENGTH];
	struct host_controls_player const *player = NULL;
	boolean notice = host_menu.notice_time && system_milliseconds() - host_menu.notice_time < HOST_CONTROLS_NOTICE_TIME;

	title[0] = where[0] = status[0] = 0;
	switch (host_menu.tab)
	{
	case _host_tab_players:
		player = chosen != NONE ? &host_menu.players[chosen] : NULL;
		if (!player)
		{
			usnprintf(title, NUMBEROF(title) - 1, L"Players");
			usnprintf(status, NUMBEROF(status) - 1, L"Choose a player to kick, ban, move or mute them.");
			break;
		}
		ustrncpy(title, player->name, NUMBEROF(title) - 1);
		title[NUMBEROF(title) - 1] = 0;
		if (player->tag == _host_controls_tag_host)
			usnprintf(where, NUMBEROF(where) - 1, L"Your player");
		else if (player->tag == _host_controls_tag_bot)
			usnprintf(where, NUMBEROF(where) - 1, L"A bot");
		else if (player->ping != NONE)
		{
			usnprintf(where, NUMBEROF(where) - 1, L"%s%s%ld ms", player->tag == _host_controls_tag_opence ?
				L"OpenCE \x00B7 " : player->tag == _host_controls_tag_openh1 ? L"OpenH1 \x00B7 " : L"",
				L"", player->ping);
		}
		if (game_engine_running() && game_engine_has_teams())
		{
			size_t length = ustrlen(where);

			usnprintf(where + length, NUMBEROF(where) - 1 - length, L"%s%s team", length ? L"\r\n" : L"",
				host_team_name(player->team));
		}
		if (host_confirming(_host_controls_ban))
			usnprintf(status, NUMBEROF(status) - 1, L"Press BAN again: they can't join your games until "
				L"you unban them.");
		else if (game_engine_running() && game_engine_has_teams() &&
			!TEST_FLAG(player->actions, _host_controls_move_team))
		{
			usnprintf(status, NUMBEROF(status) - 1, network_standard_hosting() ?
				L"Players move teams only when everyone is on OpenH1." :
				L"Players can't move teams in a Legacy game.");
		}
		break;
	case _host_tab_game:
		if (chosen != NONE)
		{
			wchar_t label[HOST_CONTROLS_TEXT_LENGTH];

			host_game_row_text(host_menu.game_rows[chosen], label, where, status, HOST_CONTROLS_TEXT_LENGTH);
			ustrncpy(title, label, NUMBEROF(title) - 1);
			title[NUMBEROF(title) - 1] = 0;
		}
		else
		{
			usnprintf(title, NUMBEROF(title) - 1, L"Game");
			usnprintf(status, NUMBEROF(status) - 1, L"Change the game while it plays. Choose a row to see "
				L"what it does.");
		}
		break;
	default:
		if (chosen != NONE)
		{
			struct host_controls_ban const *ban = &host_menu.bans[chosen];
			wchar_t reason[HOST_CONTROLS_TEXT_LENGTH];

			text_to_wide(ban->players[0] ? ban->players : ban->address, title, HOST_CONTROLS_TEXT_LENGTH);
			text_to_wide(ban->reason, reason, HOST_CONTROLS_TEXT_LENGTH);
			if (ban->when[0])
			{
				wchar_t when[HOST_CONTROLS_BAN_WHEN_SIZE];

				text_to_wide(ban->when, when, HOST_CONTROLS_BAN_WHEN_SIZE);
				usnprintf(where, NUMBEROF(where) - 1, L"Banned %s", when);
			}
			usnprintf(status, NUMBEROF(status) - 1, L"%s", reason[0] ? reason : L"Banned by hand in bans.txt");
		}
		else
		{
			usnprintf(title, NUMBEROF(title) - 1, L"Bans");
			usnprintf(status, NUMBEROF(status) - 1, L"Players you ban are listed here. They can't join your "
				L"games until you unban them.");
		}
		break;
	}
	if (notice)
		ustrncpy(status, host_menu.notice, NUMBEROF(status) - 1);
	title[NUMBEROF(title) - 1] = where[NUMBEROF(where) - 1] = status[NUMBEROF(status) - 1] = 0;
	host_buttons_show(list, player, host_menu.tab == _host_tab_bans && chosen != NONE);
	if (!panel)
		return;
	menu_text_box_wrap(named(panel, "panel_title", 0), title, 2);
	menu_text_box_wrap(named(panel, "panel_where", 0), where, 3);
	menu_text_box_wrap(named(panel, "panel_status", 0), status, 5);
}

/* "port host update" (the list's, each frame) */
static void host_controls_update(struct widget_instance *list)
{
	struct widget_instance *screen = screen_of(list);
	struct widget_instance *child;
	struct widget_instance *focused = list->focused_child;
	short focused_row = host_row_index(focused);
	short count, rows, chosen, index, shown = host_rows_of(list);
	long opening_player;
	short opening_tab;
	wchar_t text[HOST_CONTROLS_TEXT_LENGTH];

	/* (an opening's tab and player: the scoreboard's, the console's) */
	if (network_host_controls_take_opening(&opening_player, &opening_tab))
	{
		host_tab_set(opening_tab);
		host_menu.chosen_player = opening_player;
		host_menu.focus_chosen = opening_player != NONE;
	}
	host_entries_read();
	count = host_entry_count();
	rows = (short)MIN(count, shown);
	/* (a list of more entries than rows: the focus kept off its ends while
	there are more past them, the list moving instead) */
	if (focused_row == rows - 1 && host_menu.first + rows < count && list_scrolls_at_end())
		host_menu.first++;
	else if (focused_row == 0 && host_menu.first > 0 && list_scrolls_at_end())
		host_menu.first--;
	host_menu.first = (short)PIN(host_menu.first, 0, MAX(count - shown, 0));
	/* (the opening's player's row given the focus, once shown) */
	if (host_menu.focus_chosen)
	{
		chosen = host_chosen_entry();
		if (chosen != NONE)
		{
			char name[24];

			if (chosen < host_menu.first || chosen >= host_menu.first + shown)
				host_menu.first = (short)PIN(chosen - shown / 2, 0, MAX(count - shown, 0));
			snprintf(name, sizeof(name), "host_item_%d", chosen - host_menu.first + 1);
			host_focus(list, named(list, name, 0));
			focused = list->focused_child;
			focused_row = host_row_index(focused);
		}
		host_menu.focus_chosen = FALSE;
		host_menu.focus_first = FALSE;
	}
	/* (the first row given the focus, once shown: opened, a tab chosen) */
	if (host_menu.focus_first && !host_menu.focus_chosen)
	{
		struct widget_instance *first = named(list, "host_item_1", 0);

		if (count > 0 && first)
		{
			first->visible = TRUE;
			host_focus(list, first);
			focused = first;
			focused_row = 0;
		}
		host_menu.focus_first = FALSE;
	}
	/* (the row with the focus chooses its entry) */
	if (focused_row != NONE)
		host_choose(host_row_entry(focused_row));
	chosen = host_chosen_entry();
	for (child = list->child; child; child = child->next)
	{
		short row_index = host_row_index(child);
		short entry = host_row_entry(row_index);

		if (row_index == NONE)
			continue;
		child->visible = entry != NONE;
		if (entry != NONE)
			host_row_show(child, row_index, entry);
		/* (the rows' backgrounds, port_settings.py's PARTS row: the chosen
		entry's brighter, the focused one's outlined) */
		child->animation.current_frame_index = entry != NONE && entry == chosen ? 2 : child == focused ? 1 : 0;
	}
	host_panel_show(list, chosen);
	/* (the focus off what is hidden: the first row, else the tabs) */
	if (!focused || !focused->visible)
	{
		struct widget_instance *first = named(list, "host_item_1", 0);

		host_focus(list, first && first->visible ? first : named(list, "host_tabs", 0));
	}
	/* (the tabs, the counts, the titles, the words for no rows) */
	{
		struct widget_instance *strip = named(list, "host_tabs", 0);

		/* (a tab chosen by LB or RB, or opened on, takes the strip's focus) */
		if (strip && host_menu.tab_shown != host_menu.tab)
		{
			struct widget_instance *tab = strip->child;

			for (index = 0; tab && index < host_menu.tab; index++)
				tab = tab->next;
			if (tab)
			{
				strip->focused_child = tab;
				strip->parameters.list.selected_index = host_menu.tab;
			}
		}
		host_menu.tab_shown = host_menu.tab;
		menu_tabs_show(strip, host_menu.tab);
	}
	/* (the players, and the game's kind: its tick rate, or LEGACY) */
	if (network_standard_hosting())
	{
		usnprintf(text, NUMBEROF(text) - 1, L"%d %s    %ld TICK", host_menu.player_count,
			host_menu.player_count == 1 ? L"PLAYER" : L"PLAYERS", network_standard_server_rate(FALSE));
	}
	else
	{
		usnprintf(text, NUMBEROF(text) - 1, L"%d %s    LEGACY", host_menu.player_count,
			host_menu.player_count == 1 ? L"PLAYER" : L"PLAYERS");
	}
	text[NUMBEROF(text) - 1] = 0;
	menu_text_box_fit(named(screen, "host_counts", 0), text, 0);
	host_headers_show(screen);
	visible_set(named(screen, "host_empty", 0), count == 0);
	if (!count)
	{
		menu_text_box_wrap(named(screen, "host_empty", 0), !network_host_controls_available() ?
			L"The game is over." : host_menu.tab == _host_tab_bans ? L"No one is banned." : L"Nothing here.", 3);
	}
}

/* "port host tab", "port host tab next", "port host tab previous" */
static boolean host_controls_tab(struct widget_instance *widget, short step)
{
	host_tab_set(step ? (short)(host_menu.tab + step) : sibling_index(widget));
	ui_play_audio_feedback_sound(SOUND_CURSOR);
	return TRUE;
}

/* "port host row" (A or a click on a row): a player's or a ban's chosen,
the focus to its buttons; a game's row changed */
static boolean host_controls_row(struct widget_instance *row)
{
	short entry = host_row_entry(host_row_index(row));
	struct widget_instance *list = row->parent;
	short index;

	if (entry == NONE || !list)
		return FALSE;
	host_choose(entry);
	if (host_menu.tab == _host_tab_game)
	{
		host_game_row_change(host_menu.game_rows[entry], 0);
		return TRUE;
	}
	/* (the panel's buttons: its first shown) */
	host_panel_show(list, host_chosen_entry());
	for (index = 0; index < HOST_CONTROLS_ACTIONS; index++)
	{
		char name[24];
		struct widget_instance *button;

		snprintf(name, sizeof(name), "host_action_%d", index);
		button = named(list, name, 0);
		if (button && button->visible)
		{
			host_focus(list, button);
			ui_play_audio_feedback_sound(SOUND_CURSOR);
			return TRUE;
		}
	}
	ui_play_audio_feedback_sound(SOUND_ERROR);
	return TRUE;
}

/* "port host left", "port host right": a game's row's value stepped */
static boolean host_controls_step(struct widget_instance *row, short step)
{
	short entry = host_row_entry(host_row_index(row));

	if (host_menu.tab != _host_tab_game || entry == NONE)
		return FALSE;
	host_choose(entry);
	switch (host_menu.game_rows[entry])
	{
	case _host_game_end:
	case _host_game_restart:
		return FALSE;
	case _host_game_maximum_players:
		host_game_row_change(host_menu.game_rows[entry], step);
		return TRUE;
	default:
		host_game_row_change(host_menu.game_rows[entry], step);
		return TRUE;
	}
}

/* "port host action" (a panel's button): the player chosen kicked, banned
(pressed twice), moved or muted; the ban chosen taken out */
static boolean host_controls_action(struct widget_instance *button)
{
	short index = host_action_index(button);
	short shown = 0, action;
	struct host_controls_player *player = host_chosen_player();
	wchar_t name[16];

	if (index == NONE)
		return FALSE;
	/* (the buttons from the panel's foot up: host_buttons_show) */
	while (shown < HOST_CONTROLS_ACTIONS && host_menu.buttons[shown] != NONE)
		shown++;
	action = index >= HOST_CONTROLS_ACTIONS - shown ? host_menu.buttons[index - (HOST_CONTROLS_ACTIONS - shown)] : NONE;
	name[0] = 0;
	if (player)
	{
		ustrncpy(name, player->name, NUMBEROF(name) - 1);
		name[NUMBEROF(name) - 1] = 0;
	}
	switch (action)
	{
	case _host_controls_kick:
		if (!player || !network_host_controls_kick(player->player_index, FALSE))
			break;
		host_notice(L"%s was kicked.", name);
		ui_play_audio_feedback_sound(SOUND_FORWARD);
		return TRUE;
	case _host_controls_ban:
		if (!player)
			break;
		if (!host_confirmed(_host_controls_ban))
			return TRUE;
		if (!network_host_controls_kick(player->player_index, TRUE))
			break;
		host_notice(L"%s was banned.", name);
		ui_play_audio_feedback_sound(SOUND_FORWARD);
		return TRUE;
	case _host_controls_move_team:
		if (!player || !network_host_controls_move_team(player->player_index))
			break;
		host_notice(player->team == 1 ? L"%s moves to Red." : L"%s moves to Blue.", name);
		ui_play_audio_feedback_sound(SOUND_FORWARD);
		return TRUE;
	case _host_controls_mute:
	{
		short absolute_index;
		boolean muted;

		if (!player)
			break;
		absolute_index = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player->player_index);
		muted = network_chat_host_mute_minutes(absolute_index) != 0;
		network_chat_host_mute(absolute_index, !muted);
		host_notice(muted ? L"%s can talk again." : L"%s is muted for everyone.", name);
		ui_play_audio_feedback_sound(SOUND_FORWARD);
		return TRUE;
	}
	case _host_button_unban:
		if (!host_menu.chosen_ban[0] || !network_host_controls_unban(host_menu.chosen_ban))
			break;
		{
			struct host_controls_ban ban;
			wchar_t names[HOST_CONTROLS_TEXT_LENGTH];

			network_host_controls_ban_parse(host_menu.chosen_ban, &ban);
			text_to_wide(ban.players[0] ? ban.players : ban.address, names, HOST_CONTROLS_TEXT_LENGTH);
			host_notice(L"%s can join again.", names);
		}
		host_menu.chosen_ban[0] = 0;
		host_menu.bans_read = FALSE;
		ui_play_audio_feedback_sound(SOUND_FORWARD);
		return TRUE;
	default:
		break;
	}
	ui_play_audio_feedback_sound(SOUND_ERROR);
	return TRUE;
}
