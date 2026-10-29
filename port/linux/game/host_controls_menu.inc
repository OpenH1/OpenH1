/*
HOST_CONTROLS_MENU.INC

Host Controls (tools/port_settings.py's _host_controls, in the server
browser's look: the menus' shared parts), the host's of a network game: the
pause menu's HOST CONTROLS (menu_tags.c's pause_patch) and the lobby's (its
HOST CONTROLS: the players there, before the game starts), which the
scoreboard's "Host controls..." and the console's host_controls open too
(network_host_controls_open). Included in menu_functions.c, whose widgets'
helpers it uses. What it does is network_host_controls.c's and
network_host_game.c's, by the host's own ways: this file only shows it.

- PLAYERS: the game's players (their name, team, score, ping and tag:
  HOST, BOT, OPENH1, OPENCE; in a Legacy game the host cannot tell the
  last two apart, and shows neither), and for the player chosen the panel's
  KICK, BAN... (then how long: an hour, a day or for ever, and a REASON
  typed), MOVE TO RED or BLUE (in the lobby, every game; in a game, one whose
  machines all hear of it), MUTE... (then how long) or UNMUTE, those the
  player may have done to them (network_host_controls_player_actions).
- GAME: End game now and Restart round (each pressed twice), CHANGE MAP...
  (MAPS), Shuffle and Balance teams (pressed twice in a game, where the
  players moved die), Announce (typed: every machine's notice), Score limit
  and Time limit (left and right: the lobby's at once, else the next
  game's), Password (typed: a public internet game's, at once), Lock teams,
  Allow votekick, Listing (an internet game's), Max players (left and
  right: never fewer than are in it), Allow OpenCE players (an OpenH1
  game's), then the bots' rows (network_host_controls_bot_rows).
- MAPS: the gametype (left and right), then the multiplayer maps, the
  Xbox's and Custom Edition's; for the map chosen the panel's PLAY NOW
  (pressed twice in a game: it ends), AFTER THIS GAME and ADD TO PLAYLIST.
- PLAYLIST: a change asked for, then network.host_playlist's games, the next
  marked; for the one chosen PLAY NOW, PLAY NEXT, REMOVE and CLEAR PLAYLIST
  (pressed twice), or a change's CANCEL.
- BANS: bans.txt's lines (read again each second), and the panel's UNBAN.

A or a click on a player's, a map's, a playlist's or a ban's row chooses it
and puts the focus on the panel's buttons; on a game's row it changes the
row (left and right step it; a typed row's A begins and ends the typing,
B leaves it as it was). What was done is said in the panel for a few
seconds.
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
/* a ban's reason, typed */
#define HOST_CONTROLS_REASON_LENGTH 64
/* an announcement, typed (a notice's, network_distributed.c) */
#define HOST_CONTROLS_ANNOUNCE_LENGTH 100

enum
{
	_host_tab_players,
	_host_tab_game,
	_host_tab_maps,
	_host_tab_playlist,
	_host_tab_bans,
	NUMBER_OF_HOST_TABS
};

/* Game's rows (the bots' after them) */
enum
{
	_host_game_end,
	_host_game_restart,
	_host_game_change_map,
	_host_game_shuffle_teams,
	_host_game_balance_teams,
	_host_game_announce,
	_host_game_score_limit,
	_host_game_time_limit,
	_host_game_password,
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

/* the panel's buttons' actions: the player's (_host_controls_*), and the
others' */
enum
{
	_host_button_unban = NUMBER_OF_HOST_CONTROLS_ACTIONS,
	/* (a ban's and a mute's lengths, the ban's reason, and leaving them) */
	_host_button_ban_hour,
	_host_button_ban_day,
	_host_button_ban_forever,
	_host_button_ban_reason,
	_host_button_mute_short,
	_host_button_mute_hour,
	_host_button_mute_day,
	_host_button_unmute,
	_host_button_cancel,
	/* (a map's) */
	_host_button_map_now,
	_host_button_map_after,
	_host_button_map_add,
	/* (a playlist's entry's, and a change's) */
	_host_button_entry_now,
	_host_button_entry_next,
	_host_button_entry_remove,
	_host_button_playlist_clear,
	_host_button_change_cancel,
	/* (what a second press confirms: a game's row plus
	MAXIMUM_HOST_GAME_ROWS, a button plus HOST_CONFIRM_BUTTON) */
	_host_confirm_none = NONE,
	HOST_CONFIRM_BUTTON = 1000,
};

/* the playlist's tab's entry for a change asked for (before its games) */
#define HOST_PLAYLIST_CHANGE (-2)

/* the steps of the score to win and of the time limit (minutes; 0: none) */
static long const host_score_steps[] = { 1, 2, 3, 4, 5, 7, 10, 15, 20, 25, 30, 40, 50, 75, 100, 150, 200, 250, 500,
	999 };
static short const host_time_steps[] = { 0, 5, 10, 15, 20, 25, 30, 45, 60, 90, 120 };
/* a ban's lengths (seconds; 0: for ever) and a mute's (minutes), as the
buttons have them */
#define HOST_BAN_HOUR (60L * 60)
#define HOST_BAN_DAY (24L * 60 * 60)
#define HOST_MUTE_SHORT 10
#define HOST_MUTE_HOUR 60
#define HOST_MUTE_DAY (24 * 60)

static struct
{
	short tab;
	/* the first entry the rows show */
	short first;
	/* the entry chosen: a player (datum index, or a lobby's slot), a game's
	row, a map (its level), a playlist's entry (HOST_PLAYLIST_CHANGE: the
	change), a ban's line */
	long chosen_player;
	long chosen_slot;
	short chosen_game_row;
	char chosen_map[HOST_GAME_MAP_SIZE];
	short chosen_entry;
	char chosen_ban[HOST_CONTROLS_BAN_LINE_SIZE];
	/* the panel's buttons asking how long (a _host_controls_ban or _mute;
	NONE: not), and the ban's reason typed */
	short choosing;
	char reason[HOST_CONTROLS_REASON_LENGTH + 1];
	/* the gametype the maps play (network_host_game_gametypes' index) */
	short gametype;
	/* the press a second confirms, and when it was asked */
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
	struct host_game_map const *maps;
	short map_count;
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

/* what was done, said as it is (a reason why not, from the host's code) */
static void host_notice_text(char const *text)
{
	wchar_t wide[HOST_CONTROLS_TEXT_LENGTH];

	text_to_wide(text, wide, NUMBEROF(wide));
	if (wide[0] >= 'a' && wide[0] <= 'z')
		wide[0] = (wchar_t)(wide[0] - 'a' + 'A');
	host_notice(L"%s.", wide);
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

/* whether the entry is the player chosen (a game's by datum, a lobby's by
slot) */
static boolean host_player_chosen(struct host_controls_player const *player)
{
	return player->player_index != NONE ? player->player_index == host_menu.chosen_player :
		host_menu.chosen_player == NONE && player->slot == host_menu.chosen_slot;
}

/* (the chosen player's entry, else NULL) */
static struct host_controls_player *host_chosen_player(void)
{
	short index;

	for (index = 0; index < host_menu.player_count; index++)
	{
		if (host_player_chosen(&host_menu.players[index]))
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

/* whether the game (the lobby's, or the one played) has teams */
static boolean host_game_teams(void)
{
	struct network_game *game = host_game();

	if (network_host_controls_in_lobby())
		return game && game->variant.universal_variant.teams;
	return game_engine_running() && game_engine_has_teams();
}

/* the rows Game shows, in their order */
static void host_game_rows_read(void)
{
	boolean teams = host_game_teams();
	boolean playing = game_engine_running() && !network_host_controls_in_lobby();
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
			shown = playing && game_engine_can_score() && !network_host_controls_restarting();
			break;
		case _host_game_change_map:
			shown = network_host_game_hosting();
			break;
		case _host_game_shuffle_teams:
		case _host_game_balance_teams:
			shown = teams && network_host_game_teams_arrangeable();
			break;
		case _host_game_announce:
			shown = playing && game_engine_can_score();
			break;
		case _host_game_score_limit:
		case _host_game_time_limit:
			shown = network_host_game_hosting() && host_game() && host_game()->variant.game_engine_index;
			break;
		case _host_game_lock_teams:
			shown = teams;
			break;
		case _host_game_password:
		case _host_game_listing:
			shown = multiplayer.mode == _multiplayer_mode_host_internet && config_boolean("network.online") &&
				config_boolean("network.public_lobby") &&
				(index == _host_game_listing || !server_settings_private());
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

/* the score to win and the minutes of the lobby's game, or the ones that
wait for the next game's (waiting: whether they do) */
static void host_game_limits(long *score, short *minutes, boolean *waiting)
{
	struct network_game *game = host_game();
	long score_waiting;
	short minutes_waiting;

	*score = game ? game->variant.universal_variant.score_to_win : 0;
	*minutes = game ? game->variant_options.time_limit : 0;
	*waiting = network_host_game_limits_waiting(&score_waiting, &minutes_waiting);
	if (score_waiting != NONE)
		*score = score_waiting;
	if (minutes_waiting != NONE)
		*minutes = minutes_waiting;
}

/* a game's row's label, value and what it does */
static void host_game_row_text(short row, wchar_t *label, wchar_t *value, wchar_t *help, short size)
{
	struct network_game *game = host_game();
	boolean confirming = host_confirming((short)(row + MAXIMUM_HOST_GAME_ROWS));
	boolean lobby = network_host_controls_in_lobby();
	long score;
	short minutes;
	boolean waiting;

	label[0] = value[0] = help[0] = 0;
	host_game_limits(&score, &minutes, &waiting);
	switch (row)
	{
	case _host_game_end:
		usnprintf(label, size - 1, L"END GAME NOW");
		usnprintf(value, size - 1, confirming ? L"PRESS AGAIN" : L"");
		usnprintf(help, size - 1, L"Ends the game now, as its time limit does: the scores, then the lobby, "
			L"with the playlist's next game if there is one.");
		break;
	case _host_game_restart:
		usnprintf(label, size - 1, L"RESTART ROUND");
		usnprintf(value, size - 1, confirming ? L"PRESS AGAIN" : L"");
		usnprintf(help, size - 1, L"Ends the game and starts it again on this map and gametype, after a moment "
			L"in the lobby. Scores start over.");
		break;
	case _host_game_change_map:
	{
		struct server_entry next;
		boolean now;

		usnprintf(label, size - 1, L"CHANGE MAP...");
		if (network_host_game_next(&next, &now))
			network_host_game_entry_shown(&next, value, size);
		usnprintf(help, size - 1, lobby ? L"Pick the map and gametype this lobby plays, or build a playlist." :
			L"Pick the next map and gametype: now, or after this game. Or build a playlist.");
		break;
	}
	case _host_game_shuffle_teams:
	case _host_game_balance_teams:
		usnprintf(label, size - 1, row == _host_game_shuffle_teams ? L"SHUFFLE TEAMS" : L"BALANCE TEAMS");
		usnprintf(value, size - 1, confirming ? L"PRESS AGAIN" : L"");
		if (row == _host_game_shuffle_teams)
			usnprintf(help, size - 1, lobby ? L"Puts the players on teams at random, as many on each." :
				L"Puts the players on teams at random, as many on each. Those who move die and respawn.");
		else
			usnprintf(help, size - 1, lobby ? L"Evens the teams' numbers, moving as few players as it can." :
				L"Evens the teams' numbers, and their scores. Those who move die and respawn.");
		break;
	case _host_game_announce:
		usnprintf(label, size - 1, L"ANNOUNCE...");
		usnprintf(help, size - 1, L"Type a message every player sees, as a server notice. Enter sends it.");
		break;
	case _host_game_score_limit:
		usnprintf(label, size - 1, L"SCORE LIMIT");
		usnprintf(value, size - 1, waiting && !lobby ? L"%ld NEXT GAME" : L"%ld", score);
		usnprintf(help, size - 1, lobby ? L"The score to win. Changes the lobby's game now." :
			L"The score to win, from the next game: the game being played keeps its own.");
		break;
	case _host_game_time_limit:
		usnprintf(label, size - 1, L"TIME LIMIT");
		if (minutes > 0)
			usnprintf(value, size - 1, waiting && !lobby ? L"%d MIN NEXT GAME" : L"%d MIN", minutes);
		else
			usnprintf(value, size - 1, waiting && !lobby ? L"NONE NEXT GAME" : L"NONE");
		usnprintf(help, size - 1, lobby ? L"The game's length. Changes the lobby's game now." :
			L"The game's length, from the next game: the game being played keeps its own.");
		break;
	case _host_game_password:
	{
		short index;

		usnprintf(label, size - 1, L"PASSWORD");
		for (index = 0; multiplayer.game_password[index] && index < size - 1 && index < 16; index++)
			value[index] = L'*';
		value[index] = 0;
		if (!index)
			usnprintf(value, size - 1, L"NONE");
		usnprintf(help, size - 1, L"Players from the server browser need it to join. Changes at once; those "
			L"in the game stay.");
		break;
	}
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

/* the next value of a list past the current, either way (none: the
current) */
static long host_step_value(long const *values, short count, long current, short step)
{
	short index;

	if (step > 0)
	{
		for (index = 0; index < count; index++)
		{
			if (values[index] > current)
				return values[index];
		}
	}
	else
	{
		for (index = (short)(count - 1); index >= 0; index--)
		{
			if (values[index] < current)
				return values[index];
		}
	}
	return current;
}

/* the widget of a game's row shown on the list (NULL: none) */
static struct widget_instance *host_game_row_widget(struct widget_instance *list, short row)
{
	short index;

	for (index = 0; index < host_menu.game_row_count; index++)
	{
		if (host_menu.game_rows[index] == row && index >= host_menu.first &&
			index < host_menu.first + host_rows_of(list))
		{
			char name[24];

			snprintf(name, sizeof(name), "host_item_%d", index - host_menu.first + 1);
			return named(list, name, 0);
		}
	}
	return NULL;
}

/* (the typing's ends) the password, at once; an announcement, sent */
static void host_password_done(char const *text)
{
	snprintf(multiplayer.game_password, sizeof(multiplayer.game_password), "%s", text);
	p2p_set_hosting_password(multiplayer.mode == _multiplayer_mode_host_internet && !server_settings_private() &&
		multiplayer.game_password[0] ? multiplayer.game_password : NULL);
	host_notice(multiplayer.game_password[0] ? L"The password is changed." : L"The game has no password.", NULL);
}

static void host_announce_done(char const *text)
{
	if (!text[0])
		return;
	if (network_host_game_announce(text))
		host_notice(L"Everyone sees your message.", NULL);
	else
		host_notice(L"Nobody else sees a message now.", NULL);
}

static void host_reason_done(char const *text)
{
	snprintf(host_menu.reason, sizeof(host_menu.reason), "%s", text);
}

/* a game's row stepped (left -1, right 1) or chosen (A: 0) */
static void host_game_row_change(struct widget_instance *row_widget, short row, short step)
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
	case _host_game_change_map:
		if (step)
			return;
		host_menu.tab = _host_tab_maps;
		host_menu.first = 0;
		host_menu.confirm = _host_confirm_none;
		host_menu.focus_first = TRUE;
		break;
	case _host_game_shuffle_teams:
	case _host_game_balance_teams:
	{
		short moved;

		/* (in a game, those moved die: pressed twice) */
		if (step || (!network_host_controls_in_lobby() && !host_confirmed((short)(row + MAXIMUM_HOST_GAME_ROWS))))
			return;
		moved = network_host_game_teams_arrange(row == _host_game_shuffle_teams ? _host_game_shuffle :
			_host_game_balance);
		if (moved == NONE)
		{
			ui_play_audio_feedback_sound(SOUND_ERROR);
			return;
		}
		if (moved == 0)
			host_notice(L"The teams are even already.", NULL);
		else
		{
			wchar_t count[16];

			usnprintf(count, NUMBEROF(count) - 1, L"%d", moved);
			count[NUMBEROF(count) - 1] = 0;
			host_notice(moved == 1 ? L"%s player moved." : L"%s players moved.", count);
		}
		ui_play_audio_feedback_sound(SOUND_FORWARD);
		return;
	}
	case _host_game_announce:
	case _host_game_password:
		if (step || !row_widget)
			return;
		if (text_field_editing(row_widget))
		{
			text_field_end(TRUE);
			ui_play_audio_feedback_sound(SOUND_FORWARD);
			return;
		}
		if (row == _host_game_announce)
			text_field_begin(row_widget, "", HOST_CONTROLS_ANNOUNCE_LENGTH, host_announce_done);
		else
			text_field_begin_masked(row_widget, multiplayer.game_password, PASSWORD_LENGTH, host_password_done);
		break;
	case _host_game_score_limit:
	case _host_game_time_limit:
	{
		long score, chosen;
		short minutes;
		boolean waiting;

		host_game_limits(&score, &minutes, &waiting);
		if (row == _host_game_score_limit)
		{
			chosen = host_step_value(host_score_steps, NUMBEROF(host_score_steps), score, (short)(step ? step : 1));
			if (chosen == score || !network_host_game_set_limits(chosen, NONE))
			{
				ui_play_audio_feedback_sound(SOUND_ERROR);
				return;
			}
		}
		else
		{
			long values[NUMBEROF(host_time_steps)];
			short index;

			for (index = 0; index < NUMBEROF(host_time_steps); index++)
				values[index] = host_time_steps[index];
			chosen = host_step_value(values, NUMBEROF(values), minutes, (short)(step ? step : 1));
			if (chosen == minutes || !network_host_game_set_limits(NONE, (short)chosen))
			{
				ui_play_audio_feedback_sound(SOUND_ERROR);
				return;
			}
		}
		break;
	}
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

/* ---- the maps and the playlist */

/* the gametype the maps play (its rotation name) */
static struct host_game_gametype const *host_gametype(void)
{
	struct host_game_gametype const *gametypes;
	short count = network_host_game_gametypes(&gametypes);

	if (!count)
		return NULL;
	host_menu.gametype = (short)PIN(host_menu.gametype, 0, count - 1);
	return &gametypes[host_menu.gametype];
}

/* the map chosen with the gametype, as a playlist's entry's text */
static boolean host_map_entry_text(struct host_game_map const *map, char *text, short size)
{
	struct host_game_gametype const *gametype = host_gametype();

	if (!map || !gametype)
		return FALSE;
	snprintf(text, size, "%s%s:%s", map->custom ? "custom_maps\\" : "", map->name, gametype->name);
	return TRUE;
}

/* the map chosen (NULL: none) */
static struct host_game_map const *host_chosen_map(void)
{
	short index;

	for (index = 0; index < host_menu.map_count; index++)
	{
		if (!strcmp(host_menu.maps[index].level, host_menu.chosen_map))
			return &host_menu.maps[index];
	}
	return NULL;
}

/* whether the game (lobby's or played) is the map's */
static boolean host_map_current(struct host_game_map const *map)
{
	struct network_game *game = host_game();

	return game && !_stricmp(game->map.name, map->level);
}

/* (the playlist's tab's entries: a change asked for, then the playlist's)
the change asked for (TRUE), the one network_host_game_next gives when
it is no playlist's next */
static boolean host_change_asked(struct server_entry *entry)
{
	struct server_rotation const *playlist = network_host_game_playlist();
	long next = network_host_game_playlist_after(playlist, network_host_game_playlist_index());
	boolean now;

	if (!network_host_game_next(entry, &now))
		return FALSE;
	return next == NONE || strcmp(entry->text, playlist->entries[next].text) || now;
}

static short host_playlist_entries(void)
{
	struct server_entry change;

	return (short)(network_host_game_playlist()->count + (host_change_asked(&change) ? 1 : 0));
}

/* the playlist's entry of the tab's (NONE, or HOST_PLAYLIST_CHANGE) */
static short host_playlist_entry(short entry)
{
	struct server_entry change;
	boolean asked = host_change_asked(&change);

	if (asked && entry == 0)
		return HOST_PLAYLIST_CHANGE;
	return (short)(entry - (asked ? 1 : 0));
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
	case _host_tab_maps:
		return (short)(host_menu.map_count + 1);
	case _host_tab_playlist:
		return host_playlist_entries();
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
	if (text_field_editing(NULL))
		text_field_end(FALSE);
	host_menu.tab = (short)((tab + NUMBER_OF_HOST_TABS) % NUMBER_OF_HOST_TABS);
	host_menu.first = 0;
	host_menu.confirm = _host_confirm_none;
	host_menu.choosing = NONE;
	host_menu.bans_read = FALSE;
	host_menu.focus_first = TRUE;
}

/* "port host init" (the list made): Players, nothing chosen */
static boolean host_controls_initialize(void)
{
	host_menu.tab = _host_tab_players;
	host_menu.first = 0;
	host_menu.chosen_player = NONE;
	host_menu.chosen_slot = NONE;
	host_menu.chosen_game_row = NONE;
	host_menu.chosen_map[0] = 0;
	host_menu.chosen_entry = NONE;
	host_menu.chosen_ban[0] = 0;
	host_menu.choosing = NONE;
	host_menu.reason[0] = 0;
	host_menu.confirm = _host_confirm_none;
	host_menu.notice_time = 0;
	host_menu.bans_read = FALSE;
	host_menu.focus_chosen = FALSE;
	host_menu.focus_first = TRUE;
	host_menu.tab_shown = NONE;
	return TRUE;
}

/* "port host open" (the pause menu's and the lobby's HOST CONTROLS): the
host's alone */
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

/* "port host back" (B, Escape): a typing let go of, as it was, staying;
else the screen left */
static boolean host_controls_back(void)
{
	if (text_field_editing(NULL))
	{
		text_field_end(FALSE);
		return FALSE;
	}
	if (host_menu.choosing != NONE)
	{
		host_menu.choosing = NONE;
		ui_play_audio_feedback_sound(SOUND_CURSOR);
		return FALSE;
	}
	return TRUE;
}

/* the entries of the tab shown, read anew */
static void host_entries_read(void)
{
	unsigned long now = system_milliseconds();

	host_menu.player_count = network_host_controls_players(host_menu.players, NUMBEROF(host_menu.players));
	host_game_rows_read();
	if (host_menu.tab == _host_tab_maps)
		host_menu.map_count = network_host_game_maps(&host_menu.maps);
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
		if (!host_player_chosen(&host_menu.players[entry]))
		{
			host_menu.confirm = _host_confirm_none;
			host_menu.choosing = NONE;
			host_menu.reason[0] = 0;
		}
		host_menu.chosen_player = host_menu.players[entry].player_index;
		host_menu.chosen_slot = host_menu.players[entry].slot;
		break;
	case _host_tab_game:
		if (host_menu.game_rows[entry] != host_menu.chosen_game_row)
			host_menu.confirm = _host_confirm_none;
		host_menu.chosen_game_row = host_menu.game_rows[entry];
		break;
	case _host_tab_maps:
		/* (the gametype's row chooses no map) */
		if (entry == 0)
		{
			host_menu.chosen_map[0] = 0;
			break;
		}
		if (strcmp(host_menu.maps[entry - 1].level, host_menu.chosen_map))
			host_menu.confirm = _host_confirm_none;
		csstrncpy(host_menu.chosen_map, host_menu.maps[entry - 1].level, sizeof(host_menu.chosen_map) - 1);
		host_menu.chosen_map[sizeof(host_menu.chosen_map) - 1] = 0;
		break;
	case _host_tab_playlist:
		if (host_playlist_entry(entry) != host_menu.chosen_entry)
			host_menu.confirm = _host_confirm_none;
		host_menu.chosen_entry = host_playlist_entry(entry);
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
		if ((host_menu.tab == _host_tab_players && host_player_chosen(&host_menu.players[index])) ||
			(host_menu.tab == _host_tab_game && host_menu.game_rows[index] == host_menu.chosen_game_row) ||
			(host_menu.tab == _host_tab_maps && index > 0 &&
				!strcmp(host_menu.maps[index - 1].level, host_menu.chosen_map)) ||
			(host_menu.tab == _host_tab_playlist && host_playlist_entry(index) == host_menu.chosen_entry) ||
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
		{ L"MAP", L"", L"", L"", L"" },
		{ L"MAP", L"", L"", L"", L"GAMETYPE" },
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
	boolean typed = FALSE;

	label[0] = value[0] = 0;
	host_menu.row_teams[row_index] = NONE;
	switch (host_menu.tab)
	{
	case _host_tab_players:
	{
		struct host_controls_player const *player = &host_menu.players[entry];

		ustrncpy(label, player->name, NUMBEROF(label) - 1);
		label[NUMBEROF(label) - 1] = 0;
		if (host_game_teams() && player->team != NONE)
		{
			usnprintf(team, NUMBEROF(team) - 1, L"%s", host_team_name(player->team));
			host_menu.row_teams[row_index] = (short)PIN(player->team, 0, 1);
		}
		if (game_engine && player->player_index != NONE)
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
	{
		short game_row = host_menu.game_rows[entry];

		host_game_row_text(game_row, label, value, help, HOST_CONTROLS_TEXT_LENGTH);
		/* (a typed row's words as they are typed) */
		if ((game_row == _host_game_announce || game_row == _host_game_password) && text_field_editing(row))
		{
			typed = TRUE;
			text_field_show(named(row, "host_item_value", 0), "", TRUE);
		}
		break;
	}
	case _host_tab_maps:
		if (entry == 0)
		{
			struct host_game_gametype const *gametype = host_gametype();

			usnprintf(label, NUMBEROF(label) - 1, L"GAMETYPE");
			usnprintf(value, NUMBEROF(value) - 1, L"\x2039 %s \x203A", gametype ? gametype->shown : L"");
		}
		else
		{
			struct host_game_map const *map = &host_menu.maps[entry - 1];

			lobby_board_map_text(map->level, label, NUMBEROF(label));
			usnprintf(value, NUMBEROF(value) - 1, L"%s%s%s", host_map_current(map) ? L"PLAYING" : L"",
				host_map_current(map) && map->custom ? L" \x00B7 " : L"", map->custom ? L"CUSTOM EDITION" : L"");
		}
		break;
	case _host_tab_playlist:
	{
		struct server_rotation const *playlist = network_host_game_playlist();
		short index = host_playlist_entry(entry);
		struct server_entry change;
		struct server_entry const *shown;
		long next = network_host_game_playlist_after(playlist, network_host_game_playlist_index());
		wchar_t text[HOST_CONTROLS_TEXT_LENGTH];
		wchar_t *dot;

		if (index == HOST_PLAYLIST_CHANGE)
		{
			boolean now;

			network_host_game_next(&change, &now);
			shown = &change;
		}
		else
			shown = &playlist->entries[index];
		network_host_game_entry_shown(shown, text, NUMBEROF(text));
		/* (the map's name, then its gametype: "Blood Gulch \x00B7 CTF") */
		dot = text;
		while (*dot && *dot != 0x00B7)
			dot++;
		if (*dot)
		{
			ustrncpy(value, dot[1] ? dot + 2 : dot + 1, NUMBEROF(value) - 1);
			value[NUMBEROF(value) - 1] = 0;
			if (dot > text)
				dot[-1] = 0;
			else
				*dot = 0;
		}
		if (index == HOST_PLAYLIST_CHANGE)
			usnprintf(label, NUMBEROF(label) - 1, L"NEXT: %s", text);
		else
			usnprintf(label, NUMBEROF(label) - 1, L"%d. %s", index + 1, text);
		if (index != HOST_PLAYLIST_CHANGE && shown->rejected[0])
			usnprintf(value, NUMBEROF(value) - 1, L"CAN'T BE PLAYED");
		else if (index != HOST_PLAYLIST_CHANGE && index == next && !host_change_asked(&change))
		{
			size_t length = ustrlen(value);

			usnprintf(value + length, NUMBEROF(value) - 1 - length, L" \x00B7 NEXT");
		}
		break;
	}
	default:
	{
		struct host_controls_ban const *ban = &host_menu.bans[entry];

		text_to_wide(ban->players[0] ? ban->players : ban->address, label, HOST_CONTROLS_TEXT_LENGTH);
		text_to_wide(ban->when, value, HOST_CONTROLS_TEXT_LENGTH);
		break;
	}
	}
	label[NUMBEROF(label) - 1] = value[NUMBEROF(value) - 1] = 0;
	menu_text_box_fit(named(row, "host_item_label", 0), label, 0);
	text_set(named(row, "host_item_team", 0), team);
	menu_text_box_fit(named(row, "host_item_score", 0), score, 0);
	text_set(named(row, "host_item_ping", 0), ping);
	if (!typed)
		menu_text_box_fit(named(row, "host_item_value", 0), value, 0);
	visible_set(tag, tag_index != _host_controls_tag_none);
	if (tag)
	{
		text_set(tag, host_tag_name(tag_index));
		tag->animation.current_frame_index = tag_index == _host_controls_tag_openh1 ? 0 :
			tag_index == _host_controls_tag_host ? 2 : 1;
	}
}

/* the buttons' actions for the entry chosen: a player's actions (or how
long a ban or mute is), a map's, a playlist's entry's, a ban's UNBAN; their
count */
static short host_buttons_of(struct host_controls_player const *player, short chosen, short *buttons)
{
	static short const order[] = { _host_controls_kick, _host_controls_ban, _host_controls_move_team,
		_host_controls_mute };
	boolean lobby = network_host_controls_in_lobby();
	short shown = 0, index;

	switch (host_menu.tab)
	{
	case _host_tab_players:
		if (!player)
			break;
		if (host_menu.choosing == _host_controls_ban && TEST_FLAG(player->actions, _host_controls_ban))
		{
			buttons[shown++] = _host_button_ban_hour;
			buttons[shown++] = _host_button_ban_day;
			buttons[shown++] = _host_button_ban_forever;
			buttons[shown++] = _host_button_ban_reason;
			break;
		}
		if (host_menu.choosing == _host_controls_mute && TEST_FLAG(player->actions, _host_controls_mute))
		{
			buttons[shown++] = _host_button_mute_short;
			buttons[shown++] = _host_button_mute_hour;
			buttons[shown++] = _host_button_mute_day;
			buttons[shown++] = _host_button_cancel;
			break;
		}
		for (index = 0; index < NUMBEROF(order); index++)
		{
			if (TEST_FLAG(player->actions, order[index]))
				buttons[shown++] = order[index] == _host_controls_mute && network_host_game_player_mute_minutes(player) ?
					_host_button_unmute : order[index];
		}
		break;
	case _host_tab_maps:
		if (!host_chosen_map())
			break;
		buttons[shown++] = _host_button_map_now;
		if (!lobby)
			buttons[shown++] = _host_button_map_after;
		buttons[shown++] = _host_button_map_add;
		break;
	case _host_tab_playlist:
		if (chosen == NONE)
			break;
		if (host_menu.chosen_entry == HOST_PLAYLIST_CHANGE)
		{
			buttons[shown++] = _host_button_change_cancel;
			break;
		}
		buttons[shown++] = _host_button_entry_now;
		if (!lobby)
			buttons[shown++] = _host_button_entry_next;
		buttons[shown++] = _host_button_entry_remove;
		buttons[shown++] = _host_button_playlist_clear;
		break;
	case _host_tab_bans:
		if (chosen != NONE)
			buttons[shown++] = _host_button_unban;
		break;
	default:
		break;
	}
	return shown;
}

/* a button's words */
static void host_button_text(short action, struct host_controls_player const *player, wchar_t *text, short size)
{
	boolean lobby = network_host_controls_in_lobby();
	boolean confirming = host_confirming((short)(action + HOST_CONFIRM_BUTTON));

	text[0] = 0;
	switch (action)
	{
	case _host_controls_kick:
		usnprintf(text, size - 1, L"KICK");
		break;
	case _host_controls_ban:
		usnprintf(text, size - 1, L"BAN...");
		break;
	case _host_controls_move_team:
		usnprintf(text, size - 1, player && player->team == 1 ? L"MOVE TO RED" : L"MOVE TO BLUE");
		break;
	case _host_controls_mute:
		usnprintf(text, size - 1, L"MUTE...");
		break;
	case _host_button_unmute:
		usnprintf(text, size - 1, L"UNMUTE");
		break;
	case _host_button_unban:
		usnprintf(text, size - 1, L"UNBAN");
		break;
	case _host_button_ban_hour:
		usnprintf(text, size - 1, L"BAN FOR AN HOUR");
		break;
	case _host_button_ban_day:
		usnprintf(text, size - 1, L"BAN FOR A DAY");
		break;
	case _host_button_ban_forever:
		usnprintf(text, size - 1, L"BAN FOR GOOD");
		break;
	case _host_button_ban_reason:
	{
		wchar_t reason[HOST_CONTROLS_REASON_LENGTH + 1];

		text_to_wide(host_menu.reason, reason, NUMBEROF(reason));
		usnprintf(text, size - 1, reason[0] ? L"WHY: %s" : L"ADD A REASON", reason);
		break;
	}
	case _host_button_mute_short:
		usnprintf(text, size - 1, L"MUTE FOR 10 MINUTES");
		break;
	case _host_button_mute_hour:
		usnprintf(text, size - 1, L"MUTE FOR AN HOUR");
		break;
	case _host_button_mute_day:
		usnprintf(text, size - 1, L"MUTE FOR A DAY");
		break;
	case _host_button_cancel:
	case _host_button_change_cancel:
		usnprintf(text, size - 1, action == _host_button_cancel ? L"CANCEL" : L"CANCEL CHANGE");
		break;
	case _host_button_map_now:
	case _host_button_entry_now:
		usnprintf(text, size - 1, lobby ? L"PLAY IN THIS LOBBY" : confirming ? L"PRESS AGAIN" : L"PLAY NOW");
		break;
	case _host_button_map_after:
	case _host_button_entry_next:
		usnprintf(text, size - 1, L"AFTER THIS GAME");
		break;
	case _host_button_map_add:
		usnprintf(text, size - 1, L"ADD TO PLAYLIST");
		break;
	case _host_button_entry_remove:
		usnprintf(text, size - 1, L"REMOVE");
		break;
	case _host_button_playlist_clear:
		usnprintf(text, size - 1, confirming ? L"PRESS AGAIN" : L"CLEAR PLAYLIST");
		break;
	default:
		break;
	}
	text[size - 1] = 0;
}

/* the panel's buttons for the entry chosen */
static void host_buttons_show(struct widget_instance *list, struct host_controls_player const *player,
	short chosen)
{
	short shown, index;
	short buttons[HOST_CONTROLS_ACTIONS];

	for (index = 0; index < HOST_CONTROLS_ACTIONS; index++)
		buttons[index] = host_menu.buttons[index] = NONE;
	shown = host_buttons_of(player, chosen, buttons);
	for (index = 0; index < shown; index++)
		host_menu.buttons[index] = buttons[index];
	/* (the buttons from the panel's foot up: its last the lowest) */
	for (index = 0; index < HOST_CONTROLS_ACTIONS; index++)
	{
		char name[24];
		struct widget_instance *button;
		short action = index >= HOST_CONTROLS_ACTIONS - shown ?
			host_menu.buttons[index - (HOST_CONTROLS_ACTIONS - shown)] : NONE;
		wchar_t text[96];

		snprintf(name, sizeof(name), "host_action_%d", index);
		button = named(list, name, 0);
		visible_set(button, action != NONE);
		/* (a reason being typed: its words as typed) */
		if (button && action == _host_button_ban_reason && text_field_editing(button))
		{
			text_field_show(button, "", TRUE);
			continue;
		}
		host_button_text(action, player, text, NUMBEROF(text));
		/* (the confirm key's prompt on the button with the focus alone, as
		the device in use has it: ui_widget.c; a touchscreen's buttons are
		tapped, and name no key) */
		if (text[0] && button && button == list->focused_child && !ui_widget_port_touch_menus())
		{
			wchar_t words[96];

			ustrncpy(words, text, NUMBEROF(words) - 1);
			words[NUMBEROF(words) - 1] = 0;
			usnprintf(text, NUMBEROF(text) - 1, L"%%a-button %s", words);
		}
		text[NUMBEROF(text) - 1] = 0;
		menu_text_box_fit(button, text, 0);
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
	boolean lobby = network_host_controls_in_lobby();

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
		else if (player->tag == _host_controls_tag_opence || player->tag == _host_controls_tag_openh1)
			usnprintf(where, NUMBEROF(where) - 1, player->tag == _host_controls_tag_opence ? L"OpenCE" : L"OpenH1");
		if (host_game_teams() && player->team != NONE)
		{
			size_t length = ustrlen(where);

			usnprintf(where + length, NUMBEROF(where) - 1 - length, L"%s%s team", length ? L"\r\n" : L"",
				host_team_name(player->team));
		}
		if (host_menu.choosing == _host_controls_ban)
			usnprintf(status, NUMBEROF(status) - 1, L"For how long? They can't join your games until then, or "
				L"until you unban them.");
		else if (host_menu.choosing == _host_controls_mute)
			usnprintf(status, NUMBEROF(status) - 1, L"For how long? Nobody sees their chat or hears their voice.");
		else if (!lobby && game_engine_running() && game_engine_has_teams() &&
			!TEST_FLAG(player->actions, _host_controls_move_team))
		{
			usnprintf(status, NUMBEROF(status) - 1, network_standard_hosting() ?
				L"Players move teams in a game only when everyone is on OpenH1. In the lobby they always can." :
				L"Players can't move teams in a Legacy game. In the lobby they can.");
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
			usnprintf(status, NUMBEROF(status) - 1, lobby ? L"Change the next game before it starts." :
				L"Change the game while it plays. Choose a row to see what it does.");
		}
		break;
	case _host_tab_maps:
	{
		struct host_game_map const *map = host_chosen_map();
		struct host_game_gametype const *gametype = host_gametype();

		if (!map)
		{
			usnprintf(title, NUMBEROF(title) - 1, L"Maps");
			usnprintf(where, NUMBEROF(where) - 1, L"%s", gametype ? gametype->shown : L"");
			usnprintf(status, NUMBEROF(status) - 1, L"Choose the gametype (left and right), then a map: play it "
				L"now, after this game, or add it to the playlist.");
			break;
		}
		lobby_board_map_text(map->level, title, NUMBEROF(title));
		usnprintf(where, NUMBEROF(where) - 1, L"%s%s", gametype ? gametype->shown : L"",
			map->custom ? L"\r\nCustom Edition" : L"");
		if (host_confirming((short)(_host_button_map_now + HOST_CONFIRM_BUTTON)))
			usnprintf(status, NUMBEROF(status) - 1, L"Press PLAY NOW again: this game ends, with no scores, and "
				L"this one starts.");
		else
			usnprintf(status, NUMBEROF(status) - 1, lobby ? L"Play it in this lobby, or add it to the playlist." :
				L"Play it now (this game ends), after this game, or add it to the playlist.");
		break;
	}
	case _host_tab_playlist:
	{
		struct server_rotation const *playlist = network_host_game_playlist();

		if (chosen == NONE)
		{
			usnprintf(title, NUMBEROF(title) - 1, L"Playlist");
			usnprintf(status, NUMBEROF(status) - 1, playlist->count ?
				L"After each game the next one loads. Choose a game to play it, or take it out." :
				L"Add games from MAPS: after each game the next one loads.");
			break;
		}
		if (host_menu.chosen_entry == HOST_PLAYLIST_CHANGE)
		{
			struct server_entry change;
			boolean now;

			network_host_game_next(&change, &now);
			usnprintf(title, NUMBEROF(title) - 1, L"Next game");
			network_host_game_entry_shown(&change, where, NUMBEROF(where));
			usnprintf(status, NUMBEROF(status) - 1, now ? L"Starting now." :
				L"Plays after this game. The playlist goes on after it.");
			break;
		}
		if (host_menu.chosen_entry >= 0 && host_menu.chosen_entry < playlist->count)
		{
			struct server_entry const *entry = &playlist->entries[host_menu.chosen_entry];
			wchar_t reason[HOST_CONTROLS_TEXT_LENGTH];

			usnprintf(title, NUMBEROF(title) - 1, L"Game %d", host_menu.chosen_entry + 1);
			network_host_game_entry_shown(entry, where, NUMBEROF(where));
			text_to_wide(entry->rejected, reason, NUMBEROF(reason));
			if (host_confirming((short)(_host_button_playlist_clear + HOST_CONFIRM_BUTTON)))
				usnprintf(status, NUMBEROF(status) - 1, L"Press CLEAR PLAYLIST again to take every game out.");
			else if (reason[0])
				usnprintf(status, NUMBEROF(status) - 1, L"%s", reason);
			else
				usnprintf(status, NUMBEROF(status) - 1, lobby ? L"Play it in this lobby, or take it out." :
					L"Play it now (this game ends) or after this game, or take it out.");
		}
		break;
	}
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
	host_buttons_show(list, player, chosen);
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
		host_menu.chosen_slot = NONE;
		host_menu.focus_chosen = opening_player != NONE;
	}
	host_entries_read();
	count = host_entry_count();
	rows = (short)MIN(count, shown);
	/* (a list of more entries than rows: the focus kept off its ends while
	there are more past them, the list moving instead; not while a row's
	words are typed) */
	if (!text_field_editing(NULL))
	{
		if (focused_row == rows - 1 && host_menu.first + rows < count && list_scrolls_at_end())
			host_menu.first++;
		else if (focused_row == 0 && host_menu.first > 0 && list_scrolls_at_end())
			host_menu.first--;
	}
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
	/* (a typed row gone from the list: its typing let go of) */
	if (text_field_editing(NULL) && host_menu.tab == _host_tab_game &&
		!host_game_row_widget(list, _host_game_announce) && !host_game_row_widget(list, _host_game_password))
	{
		text_field_end(FALSE);
	}
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
	/* (the players, and the game's kind: its tick rate, or LEGACY; in the
	lobby, so said) */
	if (network_standard_hosting())
	{
		usnprintf(text, NUMBEROF(text) - 1, L"%d %s    %ld TICK%s", host_menu.player_count,
			host_menu.player_count == 1 ? L"PLAYER" : L"PLAYERS", network_standard_server_rate(FALSE),
			network_host_controls_in_lobby() ? L"    LOBBY" : L"");
	}
	else
	{
		usnprintf(text, NUMBEROF(text) - 1, L"%d %s    LEGACY%s", host_menu.player_count,
			host_menu.player_count == 1 ? L"PLAYER" : L"PLAYERS", network_host_controls_in_lobby() ? L"    LOBBY" : L"");
	}
	text[NUMBEROF(text) - 1] = 0;
	menu_text_box_fit(named(screen, "host_counts", 0), text, 0);
	host_headers_show(screen);
	visible_set(named(screen, "host_empty", 0), count == 0);
	if (!count)
	{
		menu_text_box_wrap(named(screen, "host_empty", 0), !network_host_controls_available() ?
			L"The game is over." : host_menu.tab == _host_tab_bans ? L"No one is banned." :
			host_menu.tab == _host_tab_playlist ? L"No games yet: add them from MAPS." : L"Nothing here.", 3);
	}
}

/* "port host tab", "port host tab next", "port host tab previous" */
static boolean host_controls_tab(struct widget_instance *widget, short step)
{
	/* (not while a row's words are typed: its keys are the typing's) */
	if (text_field_editing(NULL))
		return TRUE;
	host_tab_set(step ? (short)(host_menu.tab + step) : sibling_index(widget));
	ui_play_audio_feedback_sound(SOUND_CURSOR);
	return TRUE;
}

/* the panel's first button shown given the focus: TRUE if one was */
static boolean host_focus_buttons(struct widget_instance *list)
{
	short index;

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
			return TRUE;
		}
	}
	return FALSE;
}

/* "port host row" (A or a click on a row): a player's, a map's, a
playlist's or a ban's chosen, the focus to its buttons; a game's row
changed; the gametype's stepped */
static boolean host_controls_row(struct widget_instance *row)
{
	short entry = host_row_entry(host_row_index(row));
	struct widget_instance *list = row->parent;

	if (entry == NONE || !list)
		return FALSE;
	host_choose(entry);
	if (host_menu.tab == _host_tab_game)
	{
		host_game_row_change(row, host_menu.game_rows[entry], 0);
		return TRUE;
	}
	if (host_menu.tab == _host_tab_maps && entry == 0)
	{
		struct host_game_gametype const *gametypes;
		short gametype_count = network_host_game_gametypes(&gametypes);

		if (gametype_count)
			host_menu.gametype = (short)((host_menu.gametype + 1) % gametype_count);
		ui_play_audio_feedback_sound(SOUND_CURSOR);
		return TRUE;
	}
	if (host_focus_buttons(list))
		ui_play_audio_feedback_sound(SOUND_CURSOR);
	else
		ui_play_audio_feedback_sound(SOUND_ERROR);
	return TRUE;
}

/* "port host left", "port host right": a game's row's value stepped, the
maps' gametype */
static boolean host_controls_step(struct widget_instance *row, short step)
{
	short entry = host_row_entry(host_row_index(row));

	if (entry == NONE || text_field_editing(NULL))
		return FALSE;
	if (host_menu.tab == _host_tab_maps && entry == 0)
	{
		struct host_game_gametype const *gametypes;
		short count = network_host_game_gametypes(&gametypes);

		if (!count)
			return FALSE;
		host_menu.gametype = (short)((host_menu.gametype + step + count) % count);
		ui_play_audio_feedback_sound(SOUND_CURSOR);
		return TRUE;
	}
	if (host_menu.tab != _host_tab_game)
		return FALSE;
	host_choose(entry);
	switch (host_menu.game_rows[entry])
	{
	case _host_game_end:
	case _host_game_restart:
	case _host_game_change_map:
	case _host_game_shuffle_teams:
	case _host_game_balance_teams:
	case _host_game_announce:
	case _host_game_password:
		return FALSE;
	default:
		host_game_row_change(row, host_menu.game_rows[entry], step);
		return TRUE;
	}
}

/* a map's or a playlist's entry played (now or after this game) or added:
said in the panel */
static boolean host_play(char const *text, short when, long playlist_index)
{
	char reason[SERVER_REASON_SIZE];

	if (!network_host_game_change(text, when, playlist_index, reason, sizeof(reason)))
	{
		host_notice_text(reason);
		return FALSE;
	}
	if (network_host_controls_in_lobby())
		host_notice(L"The lobby plays it now.", NULL);
	else if (when == _host_game_now && !strncmp(reason, "now", 3))
	{
		/* (back to the game, which ends) */
		ui_widgets_close_all();
	}
	else
		host_notice(L"It plays after this game.", NULL);
	return TRUE;
}

/* "port host action" (a panel's button) */
static boolean host_controls_action(struct widget_instance *button)
{
	short index = host_action_index(button);
	short shown = 0, action;
	struct host_controls_player *player = host_chosen_player();
	struct host_game_map const *map = host_chosen_map();
	wchar_t name[16];
	char text[SERVER_ENTRY_TEXT_SIZE];
	char reason[SERVER_REASON_SIZE];

	if (index == NONE)
		return FALSE;
	/* (a reason typed: its button's A ends it) */
	if (text_field_editing(button))
	{
		text_field_end(TRUE);
		ui_play_audio_feedback_sound(SOUND_CURSOR);
		return TRUE;
	}
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
		if (!player || !network_host_game_kick_player(player, FALSE, 0, NULL))
			break;
		host_notice(L"%s was kicked.", name);
		ui_play_audio_feedback_sound(SOUND_FORWARD);
		return TRUE;
	case _host_controls_ban:
	case _host_controls_mute:
		if (!player)
			break;
		host_menu.choosing = action;
		host_menu.reason[0] = 0;
		host_focus_buttons(button->parent);
		ui_play_audio_feedback_sound(SOUND_CURSOR);
		return TRUE;
	case _host_button_ban_hour:
	case _host_button_ban_day:
	case _host_button_ban_forever:
		if (!player || !network_host_game_kick_player(player, TRUE, action == _host_button_ban_hour ? HOST_BAN_HOUR :
			action == _host_button_ban_day ? HOST_BAN_DAY : 0, host_menu.reason))
		{
			break;
		}
		host_menu.choosing = NONE;
		host_notice(action == _host_button_ban_hour ? L"%s is banned for an hour." :
			action == _host_button_ban_day ? L"%s is banned for a day." : L"%s is banned.", name);
		ui_play_audio_feedback_sound(SOUND_FORWARD);
		return TRUE;
	case _host_button_ban_reason:
		text_field_begin(button, host_menu.reason, HOST_CONTROLS_REASON_LENGTH, host_reason_done);
		ui_play_audio_feedback_sound(SOUND_CURSOR);
		return TRUE;
	case _host_button_mute_short:
	case _host_button_mute_hour:
	case _host_button_mute_day:
		if (!player || !network_host_game_mute_player(player, action == _host_button_mute_short ? HOST_MUTE_SHORT :
			action == _host_button_mute_hour ? HOST_MUTE_HOUR : HOST_MUTE_DAY))
		{
			break;
		}
		host_menu.choosing = NONE;
		host_notice(action == _host_button_mute_short ? L"%s is muted for 10 minutes." :
			action == _host_button_mute_hour ? L"%s is muted for an hour." : L"%s is muted for a day.", name);
		ui_play_audio_feedback_sound(SOUND_FORWARD);
		return TRUE;
	case _host_button_unmute:
		if (!player || !network_host_game_mute_player(player, 0))
			break;
		host_notice(L"%s can talk again.", name);
		ui_play_audio_feedback_sound(SOUND_FORWARD);
		return TRUE;
	case _host_button_cancel:
		host_menu.choosing = NONE;
		host_focus_buttons(button->parent);
		ui_play_audio_feedback_sound(SOUND_CURSOR);
		return TRUE;
	case _host_controls_move_team:
		if (!player || !network_host_game_move_player(player))
			break;
		host_notice(player->team == 1 ? L"%s moves to Red." : L"%s moves to Blue.", name);
		ui_play_audio_feedback_sound(SOUND_FORWARD);
		return TRUE;
	case _host_button_map_now:
	case _host_button_map_after:
		if (!host_map_entry_text(map, text, sizeof(text)))
			break;
		/* (now, in a game: pressed twice, as it ends) */
		if (action == _host_button_map_now && !network_host_controls_in_lobby() &&
			!host_confirmed((short)(action + HOST_CONFIRM_BUTTON)))
		{
			return TRUE;
		}
		if (!host_play(text, action == _host_button_map_now ? _host_game_now : _host_game_after, NONE))
			break;
		ui_play_audio_feedback_sound(SOUND_FORWARD);
		return TRUE;
	case _host_button_map_add:
		if (!host_map_entry_text(map, text, sizeof(text)))
			break;
		if (!network_host_game_playlist_add(text, reason, sizeof(reason)))
		{
			host_notice_text(reason);
			break;
		}
		{
			wchar_t count[16];

			usnprintf(count, NUMBEROF(count) - 1, L"%d", network_host_game_playlist()->count);
			count[NUMBEROF(count) - 1] = 0;
			host_notice(L"Added: the playlist has %s games.", count);
		}
		ui_play_audio_feedback_sound(SOUND_FORWARD);
		return TRUE;
	case _host_button_entry_now:
	case _host_button_entry_next:
	{
		struct server_rotation const *playlist = network_host_game_playlist();

		if (host_menu.chosen_entry < 0 || host_menu.chosen_entry >= playlist->count)
			break;
		if (action == _host_button_entry_now && !network_host_controls_in_lobby() &&
			!host_confirmed((short)(action + HOST_CONFIRM_BUTTON)))
		{
			return TRUE;
		}
		snprintf(text, sizeof(text), "%s", playlist->entries[host_menu.chosen_entry].text);
		if (!host_play(text, action == _host_button_entry_now ? _host_game_now : _host_game_after,
			host_menu.chosen_entry))
		{
			break;
		}
		ui_play_audio_feedback_sound(SOUND_FORWARD);
		return TRUE;
	}
	case _host_button_entry_remove:
		if (!network_host_game_playlist_remove(host_menu.chosen_entry))
			break;
		host_notice(L"Taken out of the playlist.", NULL);
		host_menu.chosen_entry = NONE;
		ui_play_audio_feedback_sound(SOUND_FORWARD);
		return TRUE;
	case _host_button_playlist_clear:
		if (!host_confirmed((short)(action + HOST_CONFIRM_BUTTON)))
			return TRUE;
		if (!network_host_game_playlist_set("", reason, sizeof(reason)))
			break;
		host_notice(L"The playlist is empty.", NULL);
		host_menu.chosen_entry = NONE;
		ui_play_audio_feedback_sound(SOUND_FORWARD);
		return TRUE;
	case _host_button_change_cancel:
		network_host_game_cancel();
		host_notice(L"The change is cancelled.", NULL);
		host_menu.chosen_entry = NONE;
		ui_play_audio_feedback_sound(SOUND_FORWARD);
		return TRUE;
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
