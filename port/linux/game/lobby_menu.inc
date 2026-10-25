/*
LOBBY_MENU.INC

The lobby on the computer (tools/port_settings.py's _lobby_board), the
network game's and a game of this machine alone's (PLAY WITH BOTS, split
screen), in Multiplayer's look: the menus' shared parts, included in
menu_functions.c after party_menu.inc, whose helpers it uses. The
touchscreen keeps the list of players and buttons it had (lobby_update).

It draws what this machine knows of the game, and sends nothing of its own
but what the Xbox's lobby sent (a player's team, the start): the game's
players (the host's record, which every machine has), each in its team's
column (one list, both columns', in a game without teams, and in Infection,
whose zombies are chosen when it starts), with a tag: HOST (the host's
machine: the host's own; for a player, the game's first machine, which the
host's own client is), BOT (bots.c's machines), PARTY (a member of this
machine's party, by the name the party knows it by), OPENCE or OPENH1 (the
host knows each machine's join token: network_standard.c; a player knows
only its own). This machine's players are framed. In each column the host
first, then the party's members together, then the others as they joined,
the bots last. The details panel: the next game (its gametype, map and
limits; OPENH1 n TICK or LEGACY, whether OpenCE's players may join, which
the host alone knows), a game of this machine alone's bots (Server Setup's
rows: bots.count, bots.skill, bots.teams, written as they change, which
the host's bots follow), how the game is, and the host's GAME SETTINGS
(Server Setup, in the lobby's place) and START GAME, or a player's WAITING
FOR THE HOST.

Left and right (the arrow keys, the D-pad) put the pressing controller's
player on red's or blue's team, a click on a column on its team; in a game
without teams they do nothing. A or Start on the players (Enter) starts the
game, the host's. The pure parts (lobby_board_tag, lobby_board_lay_out,
lobby_board_teams, lobby_board_controls_for) are
tools/harness/tests/test_lobby_board.py's.
*/

/* (each column's rows and the columns, LOBBY_BOARD_ROWS and
LOBBY_BOARD_COLUMNS, are menu_functions.c's, for the voice chat's icons) */
#define LOBBY_BOARD_ENTRIES HALO_PORT_MAXIMUM_NETWORK_PLAYERS
/* between the columns (tools/port_settings.py's LOBBY_COLUMN_GAP) */
#define LOBBY_BOARD_COLUMN_GAP 12
/* milliseconds the panel says the invite link was copied */
#define LOBBY_BOARD_COPIED_TIME 4000

/* a row's tag */
enum
{
	_lobby_tag_none,
	_lobby_tag_host,
	_lobby_tag_bot,
	_lobby_tag_party,
	_lobby_tag_openh1,
	_lobby_tag_opence,
	NUMBER_OF_LOBBY_TAGS
};

/* what this machine knows of a player's machine */
enum
{
	_lobby_kind_unknown,
	_lobby_kind_openh1,
	_lobby_kind_opence,
};

/* what the lobby knows of a player, for its row */
struct lobby_board_entry
{
	/* its team (0 red, 1 blue) */
	short team;
	/* this machine's; the host's machine's; a bot; in this machine's party */
	boolean own;
	boolean host;
	boolean bot;
	boolean party;
	/* _lobby_kind_* */
	short kind;
};

/* the rows: each column's entries (NONE: an empty row), how many players
each column has (a game without teams: all of them, column 0's), and how many
more than its rows show (the last row then says so: in a game without teams,
blue's column's) */
struct lobby_board_layout
{
	short rows[LOBBY_BOARD_COLUMNS][LOBBY_BOARD_ROWS];
	short counts[LOBBY_BOARD_COLUMNS];
	short more[LOBBY_BOARD_COLUMNS];
};

/* what the lobby offers this machine */
struct lobby_board_controls
{
	/* the host's START GAME and GAME SETTINGS; a player's WAITING FOR THE
	HOST */
	boolean start;
	boolean settings;
	boolean waiting;
	/* the bots' rows (a game of this machine alone's), locked (dimmed: a
	gametype the bots do not play) */
	boolean bots;
	boolean bots_locked;
	/* CHANGE TEAM, COPY INVITE, ADD PLAYER */
	boolean team_change;
	boolean invite;
	boolean add_player;
};

static wchar_t const *const lobby_tag_words[NUMBER_OF_LOBBY_TAGS] =
{
	L"", L"HOST", L"BOT", L"PARTY", L"OPENH1", L"OPENCE",
};

static struct
{
	/* the rows' players as last shown, by column and row (NULL: none): what
	a row's own action finds its player by (lobby_board_row_player) */
	struct network_player *players[LOBBY_BOARD_COLUMNS][LOBBY_BOARD_ROWS];
	struct lobby_board_entry entries[LOBBY_BOARD_COLUMNS][LOBBY_BOARD_ROWS];
	/* each row's player INVITE TO PARTY could ask (lobby_board_party_invitable) */
	boolean invitable[LOBBY_BOARD_COLUMNS][LOBBY_BOARD_ROWS];
	/* when COPY INVITE last copied it */
	unsigned long copied_time;
} lobby_board;

/* ---- the pure parts */

/* whether the game's players choose a team in the lobby: a team game's, but
Infection's (its zombies are chosen as it starts) */
static boolean lobby_board_teams(boolean teams, short mode)
{
	return teams && mode != _game_mode_infection;
}

/* a row's tag: what is most worth knowing of its player */
static short lobby_board_tag(struct lobby_board_entry const *entry)
{
	if (entry->bot)
		return _lobby_tag_bot;
	if (entry->host)
		return _lobby_tag_host;
	if (entry->kind == _lobby_kind_opence)
		return _lobby_tag_opence;
	if (entry->party)
		return _lobby_tag_party;
	if (entry->kind == _lobby_kind_openh1)
		return _lobby_tag_openh1;
	return _lobby_tag_none;
}

/* a player's place in its column: the host first, the party's members
together, the others, the bots last */
static short lobby_board_rank(struct lobby_board_entry const *entry)
{
	if (entry->host && !entry->bot)
		return 0;
	if (entry->party && !entry->bot)
		return 1;
	return entry->bot ? 3 : 2;
}

/* the entries (in the game's order) into the columns' rows: a team game's
by team, else one list down red's column and on down blue's; a column
with more than its rows shows one fewer, and how many more in its last */
static void lobby_board_lay_out(struct lobby_board_entry const *entries, short count, boolean teams,
	struct lobby_board_layout *layout)
{
	short order[LOBBY_BOARD_ENTRIES];
	short column, row, index, rank, shown, total = 0;

	for (column = 0; column < LOBBY_BOARD_COLUMNS; column++)
	{
		for (row = 0; row < LOBBY_BOARD_ROWS; row++)
			layout->rows[column][row] = NONE;
		layout->counts[column] = 0;
		layout->more[column] = 0;
	}
	count = (short)MIN(MAX(count, 0), LOBBY_BOARD_ENTRIES);
	/* (by rank, each rank in the game's order) */
	for (rank = 0; rank < 4; rank++)
	{
		for (index = 0; index < count; index++)
		{
			if (lobby_board_rank(&entries[index]) == rank)
				order[total++] = index;
		}
	}
	if (!teams)
	{
		shown = (short)(total <= LOBBY_BOARD_COLUMNS * LOBBY_BOARD_ROWS ? total :
			LOBBY_BOARD_COLUMNS * LOBBY_BOARD_ROWS - 1);
		for (index = 0; index < shown; index++)
			layout->rows[index / LOBBY_BOARD_ROWS][index % LOBBY_BOARD_ROWS] = order[index];
		layout->counts[0] = total;
		layout->more[LOBBY_BOARD_COLUMNS - 1] = (short)(total - shown);
		return;
	}
	for (column = 0; column < LOBBY_BOARD_COLUMNS; column++)
	{
		short in_column = 0;

		for (index = 0; index < total; index++)
		{
			/* (a team past blue's, which no game has, in red's) */
			short team = entries[order[index]].team == 1 ? 1 : 0;

			if (team != column)
				continue;
			if (in_column < LOBBY_BOARD_ROWS)
				layout->rows[column][in_column] = order[index];
			in_column++;
		}
		layout->counts[column] = in_column;
		if (in_column > LOBBY_BOARD_ROWS)
		{
			layout->rows[column][LOBBY_BOARD_ROWS - 1] = NONE;
			layout->more[column] = (short)(in_column - LOBBY_BOARD_ROWS + 1);
		}
	}
}

/* what the lobby offers: the host's start and settings (a player waits for
the host), a game of this machine alone's bots (locked for a gametype they
do not play), a team change while this machine has a player in a game whose
players choose a team, the invite link while there is one, ADD PLAYER while
this machine has room for another */
static void lobby_board_controls_for(boolean host, boolean local, boolean bots_play, boolean teams,
	boolean own_player, boolean invite, boolean room, struct lobby_board_controls *controls)
{
	controls->start = host;
	controls->settings = host;
	controls->waiting = !host;
	controls->bots = host && local;
	controls->bots_locked = controls->bots && !bots_play;
	controls->team_change = teams && own_player;
	controls->invite = invite;
	controls->add_player = room;
}

/* ---- what this machine knows */

/* a player's name as a party knows it (party_name_tell: its characters
past ASCII as "?") */
static boolean lobby_board_party_name(wchar_t const *name, short length, char const *member)
{
	short index;

	for (index = 0; index < length && name[index]; index++)
	{
		wchar_t character = name[index];

		if (member[index] != (char)(character >= 0x20 && character < 0x7F ? character : '?'))
			return FALSE;
	}
	return index > 0 && member[index] == 0;
}

/* whether a player is a member of this machine's party (one of more than
this machine) */
static boolean lobby_board_in_party(struct p2p_party_status const *status, struct network_player const *player)
{
	short index;

	if (status->state != _p2p_party_member || status->count < 2)
		return FALSE;
	for (index = 0; index < status->count; index++)
	{
		if (lobby_board_party_name(player->name, NUMBEROF(player->name), status->members[index].name))
			return TRUE;
	}
	return FALSE;
}

/* the game's invite link (the host's own, or a player's of the game it
joined), 0 if none: a LAN game, a game of this machine alone */
static int lobby_board_invite(char *link, int size)
{
	void *client = global_network_game_client_get();
	struct transport_address address;

	if (global_network_game_server_get())
		return network_game_is_splitscreen_local() ? 0 : p2p_invite_link(link, size);
	if (!client)
		return 0;
	csmemset(&address, 0, sizeof(address));
	network_game_client_get_remote_server_address(client, &address);
	return address.address.long_words[0] &&
		p2p_game_invite(party_network_order(address.address.long_words[0]), link, size);
}

/* whether a name (at most length characters) is another */
static boolean lobby_board_names_equal(wchar_t const *name, short length, wchar_t const *other)
{
	short index;

	for (index = 0; index < length && name[index]; index++)
	{
		if (name[index] != other[index])
			return FALSE;
	}
	return other[index] == 0;
}

/* a network game's name as the lobby says it: the host's own, or, while it
is the name the game was given (none; a name the server took from the map's
random players' names, ui\random_player_names, as its machine's nickname; or
the host's player's own, Server Setup's first), "<the host's player>'s game" */
static void lobby_board_game_name(struct network_game const *game, struct lobby_board_entry const *entries,
	struct network_player *const *players, short count, wchar_t *name, short size)
{
	long names = tag_loaded('ustr', "ui\\random_player_names");
	struct string_list *list = names != NONE ? unicode_string_list_definition_get(names) : NULL;
	struct network_player const *host = NULL;
	wchar_t host_name[NUMBEROF(host->name) + 1];
	boolean given = !game->name[0];
	short index;

	for (index = 0; index < count && !host; index++)
	{
		if (entries[index].host && !entries[index].bot)
			host = players[index];
	}
	for (index = 0; list && !given && index < list->strings.count; index++)
	{
		wchar_t const *random_name = unicode_string_list_get_string(names, index);

		given = random_name && lobby_board_names_equal(game->name, NUMBEROF(game->name), random_name);
	}
	if (host)
	{
		ustrncpy(host_name, host->name, NUMBEROF(host->name));
		host_name[NUMBEROF(host->name)] = 0;
		given |= lobby_board_names_equal(game->name, NUMBEROF(game->name), host_name);
	}
	if (host && host_name[0] && given)
		usnprintf(name, size - 1, L"%s's game", host_name);
	else
	{
		ustrncpy(name, game->name, MIN(size, (short)NUMBEROF(game->name) + 1) - 1);
		name[MIN(size, (short)NUMBEROF(game->name) + 1) - 1] = 0;
	}
	name[size - 1] = 0;
}

/* the game's map's name: a multiplayer map's (the Xbox's, a Custom Edition
map's), a co-op game's level's, else its file's */
static void lobby_board_map_text(char const *map_name, wchar_t *text, short size)
{
	char const *const *names;
	short count = xbox_multiplayer_map_count(ui_widget_port_multiplayer_maps(&names, NULL)), map = NONE, index;
	short level = campaign_level_of(map_name);
	char const *file = map_name;
	wchar_t name[ROW_TEXT_LENGTH];

	for (index = 0; index < count; index++)
	{
		if (!_stricmp(names[index], map_name))
			map = index;
	}
	if (custom_edition_level_name(map_name) && custom_edition_maps_display_index(map_name) != NONE)
		map = custom_edition_maps_display_index(map_name);
	name[0] = 0;
	if (level != NONE)
		map_level_text(level, name);
	else if (map != NONE)
		string_get("pc\\main_menu\\mp_map_list", map, name);
	if (!name[0])
	{
		for (index = 0; map_name[index]; index++)
		{
			if (map_name[index] == '\\')
				file = map_name + index + 1;
		}
		for (index = 0; index < ROW_TEXT_LENGTH - 1 && file[index]; index++)
			name[index] = (wchar_t)(unsigned char)file[index];
		name[index] = 0;
	}
	ustrncpy(text, name, size - 1);
	text[size - 1] = 0;
}

/* the next game's limits: what wins (by its game engine's score), its time */
static void lobby_board_limits_text(struct network_game const *game, wchar_t *text, short size)
{
	struct game_variant const *variant = &game->variant;
	long score = variant->universal_variant.score_to_win;
	short minutes = game->variant_options.time_limit;
	size_t length;

	switch (game_modes_variant_mode(variant, &game->variant_options))
	{
	case _game_mode_infection:
		usnprintf(text, size - 1, L"Zombies chosen at the start");
		break;
	case _game_mode_gun_game:
		usnprintf(text, size - 1, L"A kill with each weapon");
		break;
	default:
		switch (variant->game_engine_index)
		{
		case 1:
			usnprintf(text, size - 1, L"%ld captures", score);
			break;
		case 2:
			usnprintf(text, size - 1, L"%ld kills", score);
			break;
		case 5:
			usnprintf(text, size - 1, L"%ld laps", score);
			break;
		default:
			usnprintf(text, size - 1, variant->game_engine_index ? L"Score to win %ld" : L"", score);
			break;
		}
		break;
	}
	text[size - 1] = 0;
	length = ustrlen(text);
	if (minutes > 0)
		usnprintf(text + length, size - 1 - length, L"%s%d min", length ? L" \x00B7 " : L"", minutes);
	text[size - 1] = 0;
}

/* this machine's player of the controller, NULL if none */
static struct network_player *lobby_board_own_player(short controller)
{
	return lobby_local_player(controller >= 0 && controller < MAXIMUM_LOCAL_PLAYERS ? controller : 0);
}

/* the row's player as last shown (column, row), NULL if none: the hook for
a row's own action. A player another machine has, not a bot and not in this
machine's party, is one INVITE TO PARTY could ask (lobby_board_party_invitable;
the scoreboard's invites, network_party.c's network_party_player_actions,
take a player of the game being played, which the lobby has not yet: the
row's network player's machine is what they would ask) */
static struct network_player *lobby_board_row_player(short column, short row)
{
	if (column < 0 || column >= LOBBY_BOARD_COLUMNS || row < 0 || row >= LOBBY_BOARD_ROWS)
		return NULL;
	return lobby_board.players[column][row];
}

static boolean lobby_board_party_invitable(short column, short row)
{
	struct lobby_board_entry const *entry;

	if (!lobby_board_row_player(column, row))
		return FALSE;
	entry = &lobby_board.entries[column][row];
	return !entry->own && !entry->bot && !entry->party;
}

/* ---- the screen */

/* a row's parts by their names (board_<part>_<column>_<row>) */
static struct widget_instance *lobby_board_part(struct widget_instance *screen, char const *part, short column,
	short row)
{
	char name[32];

	snprintf(name, sizeof(name), "board_%s_%d_%d", part, column, row);
	return named(screen, name, 0);
}

/* "port lobby board update" (the list's) */
static void lobby_board_update(struct widget_instance *list)
{
	static struct lobby_board_entry entries[LOBBY_BOARD_ENTRIES];
	static struct network_player *players[LOBBY_BOARD_ENTRIES];
	struct widget_instance *screen = screen_of(list);
	struct widget_instance *panel = named(screen, "board_panel", 0);
	struct widget_instance *bar = named(list, "board_prompts", 0);
	void *client = global_network_game_client_get();
	struct network_game *game = client ? network_game_client_get_game(client) : NULL;
	short state_data, state = client ? network_game_client_get_state(client, &state_data) : NONE;
	boolean host = global_network_game_server_get() != NULL;
	boolean local = host && network_game_is_splitscreen_local();
	short machine = network_game_client_get_local_machine_index();
	boolean players_focused = list->focused_child && !strcmp(list->focused_child->name, "board_players");
	struct p2p_party_status party;
	struct lobby_board_layout layout;
	struct lobby_board_controls controls;
	wchar_t text[SETTINGS_PANEL_TEXT_LENGTH];
	char link[TEXT_FIELD_LENGTH];
	short count = 0, column, row, index, mode = _game_mode_none, seconds = NONE;
	boolean teams = FALSE, own_player = FALSE;

	if (!game || state < _client_state_pregame)
		game = NULL;
	p2p_party_status(&party);
	for (index = 0; game && index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
	{
		struct network_player *player = &game->players[index];
		struct lobby_board_entry *entry = &entries[count];
		long player_machine = player->machine_index;

		if (!network_player_is_valid(player))
			continue;
		players[count++] = player;
		entry->team = player->team_index;
		entry->bot = bots_machine_is_bot(player_machine);
		entry->own = !entry->bot && player_machine == machine;
		/* (a player knows the host's machine as the game's first: the
		host's own client joins it first) */
		entry->host = !entry->bot && player_machine == (host ? machine : 0);
		entry->party = !entry->bot && lobby_board_in_party(&party, player);
		entry->kind = _lobby_kind_unknown;
		if (host && network_standard_machine_is_opence(player_machine))
			entry->kind = _lobby_kind_opence;
		else if ((host && network_standard_machine_is_openh1(player_machine)) || entry->own)
			entry->kind = _lobby_kind_openh1;
		own_player |= entry->own;
	}
	lobby_player_count = count;
	if (game)
	{
		mode = game_modes_variant_mode(&game->variant, &game->variant_options);
		teams = lobby_board_teams(game->variant.universal_variant.teams, mode);
		seconds = network_game_client_get_seconds_to_game_start(client);
	}
	lobby_board_lay_out(entries, count, teams, &layout);
	lobby_board_controls_for(host, local, game && game->variant.game_engine_index == game_engine_slayer, teams,
		own_player, lobby_board_invite(link, sizeof(link)) > 0, lobby_local_player_count() < MAXIMUM_LOCAL_PLAYERS,
		&controls);

	/* (the game's name, where it is, its players: a game of this machine
	alone's none, as no one else sees it) */
	text[0] = 0;
	if (game && local)
	{
		boolean bots = FALSE;

		for (index = 0; index < count; index++)
			bots |= entries[index].bot;
		usnprintf(text, NUMBEROF(text) - 1, count == 1 ? L"%s \x00B7 1 player" : L"%s \x00B7 %d players",
			bots ? L"Play with Bots" : L"This computer", count);
	}
	else if (game)
	{
		wchar_t name[ROW_TEXT_LENGTH];
		short bots = 0;

		/* (and the host's bots among them, bots.c's) */
		for (index = 0; index < count; index++)
			bots += entries[index].bot ? 1 : 0;
		lobby_board_game_name(game, entries, players, count, name, NUMBEROF(name));
		usnprintf(text, NUMBEROF(text) - 1, L"%s \x00B7 %s \x00B7 %d of %d players", name,
			controls.invite ? L"Internet" : L"Local network", count, game->maximum_players);
		if (bots)
		{
			size_t length = ustrlen(text);

			usnprintf(text + length, NUMBEROF(text) - 1 - length, bots == 1 ? L" \x00B7 1 bot" : L" \x00B7 %d bots",
				bots);
		}
	}
	text[NUMBEROF(text) - 1] = 0;
	menu_text_box_fit(named(screen, "board_subtitle", 0), text, 0);

	/* (the teams' columns, or one list) */
	for (column = 0; column < LOBBY_BOARD_COLUMNS; column++)
	{
		char name[24];
		struct widget_instance *count_box;

		snprintf(name, sizeof(name), "board_team_%d", column);
		visible_set(named(screen, name, 0), teams);
		/* (its edge its team's: part_team_panel's frames) */
		if (named(screen, name, 0))
			named(screen, name, 0)->animation.current_frame_index = column;
		snprintf(name, sizeof(name), "board_title_%d", column);
		visible_set(named(screen, name, 0), teams);
		snprintf(name, sizeof(name), "board_count_%d", column);
		count_box = named(screen, name, 0);
		if (teams)
			usnprintf(text, NUMBEROF(text) - 1, layout.counts[column] == 1 ? L"1 PLAYER" : L"%d PLAYERS",
				layout.counts[column]);
		else
			usnprintf(text, NUMBEROF(text) - 1, game ? L"%d OF %d" : L"", count, game ? game->maximum_players : 0);
		text[NUMBEROF(text) - 1] = 0;
		/* (a game without teams': at its panel's right, blue's place) */
		visible_set(count_box, teams || column == LOBBY_BOARD_COLUMNS - 1);
		text_set(count_box, text);
	}
	visible_set(named(screen, "board_all", 0), !teams);
	visible_set(named(screen, "board_title_all", 0), !teams);

	/* (each row: its player's dot, name and tag, this machine's framed,
	brighter while the players have the focus; or how many more) */
	lobby_icons.count = 0;
	for (column = 0; column < LOBBY_BOARD_COLUMNS; column++)
	{
		for (row = 0; row < LOBBY_BOARD_ROWS; row++)
		{
			short shown = layout.rows[column][row];
			boolean more = shown == NONE && row == LOBBY_BOARD_ROWS - 1 && layout.more[column] > 0;
			struct widget_instance *row_box = lobby_board_part(screen, "row", column, row);
			struct widget_instance *name_box = lobby_board_part(screen, "name", column, row);
			struct widget_instance *dot = lobby_board_part(screen, "dot", column, row);
			struct widget_instance *tag = lobby_board_part(screen, "tag", column, row);
			struct lobby_board_entry const *entry = shown != NONE ? &entries[shown] : NULL;

			lobby_board.players[column][row] = entry ? players[shown] : NULL;
			if (entry)
				lobby_board.entries[column][row] = *entry;
			lobby_board.invitable[column][row] = lobby_board_party_invitable(column, row);
			visible_set(row_box, entry || more);
			visible_set(dot, entry != NULL);
			visible_set(tag, entry != NULL);
			if (row_box)
				row_box->animation.current_frame_index = (short)(entry && entry->own ? (players_focused ? 2 : 1) : 0);
			if (more)
			{
				usnprintf(text, NUMBEROF(text) - 1, L"+%d more", layout.more[column]);
				text[NUMBEROF(text) - 1] = 0;
				menu_text_box_fit(name_box, text, 0);
			}
			if (!entry)
				continue;
			if (dot)
				dot->animation.current_frame_index = (short)(teams ? (entry->team == 1 ? 1 : 0) : 2);
			{
				struct network_player const *player = players[shown];
				wchar_t name[NUMBEROF(player->name) + 1];

				ustrncpy(name, player->name, NUMBEROF(player->name));
				name[NUMBEROF(player->name)] = 0;
				/* (this machine's players, when it has more than one: their
				controllers) */
				if (entry->own && lobby_local_player_count() >= 2)
					usnprintf(text, NUMBEROF(text) - 1, L"%s (P%d)", name, player->controller_index + 1);
				else
					usnprintf(text, NUMBEROF(text) - 1, L"%s", name);
				text[NUMBEROF(text) - 1] = 0;
				menu_text_box_fit(name_box, text, 0);
			}
			text_set(tag, lobby_tag_words[lobby_board_tag(entry)]);
			/* (its speaker, in voice chat: menu_functions_text_box_drawn) */
			if (lobby_icons.count < LOBBY_ICON_ROWS)
			{
				lobby_icons.texts[lobby_icons.count] = name_box;
				lobby_icons.machines[lobby_icons.count] = players[shown]->machine_index;
				lobby_icons.count++;
			}
		}
	}
	lobby_icons.updated_at = system_milliseconds();

	/* (the details panel: the next game) */
	visible_set(panel, game != NULL);
	if (game)
	{
		struct widget_instance *pill;
		wchar_t const *kind = game_modes_variant_kind_name(&game->variant, &game->variant_options);
		wchar_t gametype[NUMBEROF(game->variant.human_readable_game_description) + 1];
		boolean standard = host ? network_standard_hosting() : network_standard_agreed_rate() > 0;
		long rate = network_standard_agreed_rate();

		ustrncpy(gametype, game->variant.human_readable_game_description, NUMBEROF(gametype) - 1);
		gametype[NUMBEROF(gametype) - 1] = 0;
		if (!gametype[0])
			ustrncpy(gametype, kind ? kind : game->variant.game_engine_index ?
				engine_names[PIN(game->variant.game_engine_index, 0, 5)] : L"CO-OP", NUMBEROF(gametype) - 1);
		menu_text_box_fit(named(panel, "board_gametype", 0), gametype, 0);
		lobby_board_map_text(game->map.name, text, NUMBEROF(text));
		/* (a co-op game's difficulty after its level) */
		if (!game->variant.game_engine_index && campaign_level_of(game->map.name) != NONE)
		{
			wchar_t difficulty[ROW_TEXT_LENGTH];
			size_t length = ustrlen(text);

			map_difficulty_text(game->difficulty, difficulty);
			usnprintf(text + length, NUMBEROF(text) - 1 - length, L" \x00B7 %s", difficulty);
			text[NUMBEROF(text) - 1] = 0;
		}
		menu_text_box_fit(named(panel, "board_map", 0), text, 0);
		lobby_board_limits_text(game, text, NUMBEROF(text));
		menu_text_box_fit(named(panel, "board_limits", 0), text, 0);
		if ((pill = named(panel, "board_kind", 0)) != NULL)
		{
			pill->animation.current_frame_index = standard ? 0 : 1;
			text_set(pill, standard ? L"OPENH1" : L"LEGACY");
		}
		if ((pill = named(panel, "board_tick", 0)) != NULL)
		{
			pill->animation.current_frame_index = 2;
			usnprintf(text, NUMBEROF(text) - 1, L"%ld TICK", standard && rate > 0 ? rate : 30L);
			text[NUMBEROF(text) - 1] = 0;
			text_set(pill, text);
		}
		if ((pill = named(panel, "board_opence", 0)) != NULL)
		{
			pill->animation.current_frame_index = 2;
			text_set(pill, L"OPENCE CAN JOIN");
		}
		visible_set(named(panel, "board_opence", 0), host && !local && network_standard_hosting_mixed());

		/* (how the game is: the countdown, the link just copied, how
		another player of this machine joins, Infection's teams, waiting
		for other machines) */
		text[0] = 0;
		if (seconds > 0)
			usnprintf(text, NUMBEROF(text) - 1, L"The game starts in %d.", seconds);
		else if (seconds == 0)
			usnprintf(text, NUMBEROF(text) - 1, L"The game is starting.");
		else if (lobby_board.copied_time && system_milliseconds() - lobby_board.copied_time < LOBBY_BOARD_COPIED_TIME)
			usnprintf(text, NUMBEROF(text) - 1, L"Invite copied: paste it in a chat to your friends.");
		else if (controls.bots_locked)
			usnprintf(text, NUMBEROF(text) - 1, L"Bots play Slayer, SWAT, Infection and Gun Game only.");
		else if (lobby_join.adding)
			usnprintf(text, NUMBEROF(text) - 1, L"New player: press START.");
		else if (mode == _game_mode_infection)
			usnprintf(text, NUMBEROF(text) - 1, L"Infection: the zombies are chosen when the game starts.");
		else if (host && !local && game->machine_count < 2)
			usnprintf(text, NUMBEROF(text) - 1, L"Waiting for players to join.");
		else if (lobby_local_player_count() < MAXIMUM_LOCAL_PLAYERS)
		{
			short controller;

			for (controller = 0; controller < MAXIMUM_LOCAL_PLAYERS; controller++)
			{
				if (input_has_gamepad(controller) && !lobby_controller_playing(controller))
					usnprintf(text, NUMBEROF(text) - 1, L"Another controller: START joins.");
			}
		}
		text[NUMBEROF(text) - 1] = 0;
		menu_text_box_wrap(named(panel, "board_status", 0), text, 2);
		if (seconds > 0)
			usnprintf(text, NUMBEROF(text) - 1, controls.start ? L"STARTING IN %d" : L"Starting in %d", seconds);
		else if (seconds == 0)
			usnprintf(text, NUMBEROF(text) - 1, controls.start ? L"STARTING..." : L"Starting...");
		else
			usnprintf(text, NUMBEROF(text) - 1, controls.start ? L"%%a-button START GAME" : L"Waiting for the host");
		text[NUMBEROF(text) - 1] = 0;
		text_set(named(list, "board_start", 0), text);
		text_set(named(panel, "board_waiting", 0), text);
	}

	/* (the host's controls, a game of this machine alone's bots, the
	prompts that apply) */
	visible_set(named(list, "board_start", 0), game && controls.start);
	visible_set(named(list, "board_settings", 0), game && controls.settings);
	visible_set(named(panel, "board_waiting", 0), controls.waiting);
	visible_set(named(panel, "board_section_bots", 0), controls.bots);
	for (index = 0; index < NUMBEROF(server_settings_bots_rows); index++)
	{
		char name[32];
		struct widget_instance *bots_row, *spinner;
		struct pc_menu_setting *setting;

		snprintf(name, sizeof(name), "board_%s", server_settings_bots_rows[index][0] + 3);
		bots_row = named(list, name, 0);
		snprintf(name, sizeof(name), "board_%s", server_settings_bots_rows[index][1]);
		spinner = named(list, name, 0);
		setting = spinner ? pc_menu_setting_get(spinner->definition_tag_index) : NULL;
		if (!bots_row || !spinner)
			continue;
		visible_set(bots_row, controls.bots);
		bots_row->disabled = controls.bots_locked;
		spinner->disabled = controls.bots_locked;
		bots_row->alpha_modifier = controls.bots_locked ? 0.4f : 1.0f;
		/* (the focus on a row locked goes to the players) */
		if (controls.bots_locked && list->focused_child == bots_row)
		{
			list->focused_child = list->child;
			list->parameters.list.selected_index = 0;
		}
		/* (written to config.toml as it changes: the host's bots follow it;
		setting_changed_save passes over a locked spinner) */
		if (controls.bots && setting)
			setting_changed_save(spinner, setting);
	}
	menu_prompts_show(bar, "prompt_team", controls.team_change);
	menu_prompts_show(bar, "prompt_invite", controls.invite);
	menu_prompts_show(bar, "prompt_add", controls.add_player);
	/* (the focus off what is hidden: on the players) */
	focus_off_hidden(list);
}

/* "port lobby team red", "blue" (left and right on the players), "toggle"
(CHANGE TEAM's prompt) and "click" (a click on a column; A or Enter
without one starts the game, the host's): the controller's player on that
team, in a game whose players choose one */
static boolean lobby_board_team(struct widget_instance *widget, short controller, short team)
{
	void *client = global_network_game_client_get();
	struct network_game *game = client ? network_game_client_get_game(client) : NULL;
	struct network_player *own = lobby_board_own_player(controller);
	struct network_player changed;

	if (!game || !own || !lobby_board_teams(game->variant.universal_variant.teams,
		game_modes_variant_mode(&game->variant, &game->variant_options)))
	{
		return FALSE;
	}
	if (team == NONE)
		team = own->team_index == 1 ? 0 : 1;
	if (own->team_index == team)
		return TRUE;
	changed = *own;
	changed.team_index = (char)team;
	if (!network_game_client_update_local_player_data(client, &changed))
		return campaign_fail();
	ui_play_audio_feedback_sound(SOUND_FORWARD);
	return TRUE;
}

/* "port lobby start" (START GAME, A or Start on the players, Start on the
panel's controls): the host's game starts, at once (the host's own faster:
network_game_server_game_start_request_event) once every machine has the
map */
static boolean lobby_board_start(void)
{
	void *client = global_network_game_client_get();

	if (!global_network_game_server_get() || !client || !lobby_local_player_count())
		return FALSE;
	if (!network_game_client_request_start_time_change(client, TRUE))
		return campaign_fail();
	ui_play_audio_feedback_sound(SOUND_FORWARD);
	return TRUE;
}

static boolean lobby_board_click(struct widget_instance *widget, short controller)
{
	struct widget_instance *blue = named(screen_of(widget), "board_team_1", 0);
	short x, y;

	if (!ui_widget_port_take_click(&x, &y))
		return lobby_board_start();
	if (!blue || !blue->visible)
		return TRUE;
	return lobby_board_team(widget, controller,
		(short)(x >= settings_slider_track_x(blue) - LOBBY_BOARD_COLUMN_GAP / 2 ? 1 : 0));
}

/* "port lobby invite" (Y, COPY INVITE's prompt): a message with the
party's link (in a party: a friend joins it, then follows its leader into
the game), else the game's, copied (a line with the join page's address of
it, which chat apps make clickable, and one with the link:
p2p_invite_message), said in the panel */
static boolean lobby_board_invite_copy(void)
{
	char link[TEXT_FIELD_LENGTH];
	char message[P2P_INVITE_MESSAGE_SIZE];
	struct p2p_party_status party;

	p2p_party_status(&party);
	if (party.state != _p2p_party_none)
		snprintf(link, sizeof(link), "%s", party.link);
	else if (lobby_board_invite(link, sizeof(link)) <= 0)
		return FALSE;
	if (!p2p_invite_message(link, message, sizeof(message)))
		return FALSE;
	platform_clipboard_set(message);
	lobby_board.copied_time = system_milliseconds() | 1;
	ui_play_audio_feedback_sound(SOUND_FORWARD);
	return TRUE;
}

/* "port lobby settings" (GAME SETTINGS): the host's Server Setup, in the
lobby's place, out of the history (its START GAME the lobby again, whose B
leaves the game as before) */
static boolean lobby_board_settings(struct widget_instance *widget, boolean *widget_deleted)
{
	if (!global_network_game_server_get())
		return campaign_fail();
	ui_play_audio_feedback_sound(SOUND_FORWARD);
	return ui_widget_port_switch(widget, SERVER_SETUP_NAME, widget_deleted);
}

/* ---- what Discord is told (p2p_discord_set_game, from party_update) */

/* scenario.c's: the map played */
extern long global_scenario_index;

/* text as UTF-8 (Discord's), at most size bytes with its end */
static void lobby_utf8(wchar_t const *wide, char *text, short size)
{
	short length = 0;

	for (; *wide && length < size - 1; wide++)
	{
		unsigned long code = (unsigned short)*wide;

		if (code < 0x80)
			text[length++] = (char)code;
		else if (code < 0x800 && length < size - 2)
		{
			text[length++] = (char)(0xC0 | code >> 6);
			text[length++] = (char)(0x80 | (code & 0x3F));
		}
		else if (code >= 0x800 && (code < 0xD800 || code > 0xDFFF) && length < size - 3)
		{
			text[length++] = (char)(0xE0 | code >> 12);
			text[length++] = (char)(0x80 | ((code >> 6) & 0x3F));
			text[length++] = (char)(0x80 | (code & 0x3F));
		}
		else
			break;
	}
	text[length] = 0;
}

/* the game this machine is in, as Discord's activity says it: "OpenH1 120
Tick · Slayer on Blood Gulch" (Legacy for a game of OpenCE's netcode; a
game of this machine alone's without either), "Campaign on Halo"; whether
this machine hosts it for the network, and whether it is under way, its
invite link and players */
static void lobby_presence_tell(void)
{
	void *client = global_network_game_client_get();
	struct network_game *game = client ? network_game_client_get_game(client) : NULL;
	short state = party_client_state();
	wchar_t text[ROW_TEXT_LENGTH * 3], map[ROW_TEXT_LENGTH];
	char description[ROW_TEXT_LENGTH * 3];
	char link[TEXT_FIELD_LENGTH];
	boolean host = global_network_game_server_get() != NULL;
	boolean local = host && network_game_is_splitscreen_local();

	if (game && state >= _client_state_pregame)
	{
		wchar_t const *kind = game_modes_variant_kind_name(&game->variant, &game->variant_options);
		wchar_t gametype[NUMBEROF(game->variant.human_readable_game_description) + 1];
		boolean standard = host ? network_standard_hosting() : network_standard_agreed_rate() > 0;
		long rate = network_standard_agreed_rate();
		size_t length = 0;

		ustrncpy(gametype, game->variant.human_readable_game_description, NUMBEROF(gametype) - 1);
		gametype[NUMBEROF(gametype) - 1] = 0;
		if (!gametype[0])
			ustrncpy(gametype, kind ? kind : game->variant.game_engine_index ?
				engine_names[PIN(game->variant.game_engine_index, 0, 5)] : L"Co-op", NUMBEROF(gametype) - 1);
		lobby_board_map_text(game->map.name, map, NUMBEROF(map));
		text[0] = 0;
		if (!local)
		{
			if (standard)
				usnprintf(text, NUMBEROF(text) - 1, L"OpenH1 %ld Tick \x00B7 ", rate > 0 ? rate : 30L);
			else
				usnprintf(text, NUMBEROF(text) - 1, L"Legacy \x00B7 ");
			text[NUMBEROF(text) - 1] = 0;
			length = ustrlen(text);
		}
		usnprintf(text + length, NUMBEROF(text) - 1 - length, L"%s on %s", gametype, map);
		text[NUMBEROF(text) - 1] = 0;
		lobby_utf8(text, description, sizeof(description));
		p2p_discord_set_game(host && !local, state >= _client_state_ingame, description,
			lobby_board_invite(link, sizeof(link)) > 0 ? link : NULL, game->player_count, game->maximum_players);
		return;
	}
	/* (the campaign, or a game of its own without the network) */
	if (!client && pc_menu_game_playing() && global_scenario_index != NONE)
	{
		lobby_board_map_text(tag_get_name(global_scenario_index), map, NUMBEROF(map));
		usnprintf(text, NUMBEROF(text) - 1, L"Campaign on %s", map);
		text[NUMBEROF(text) - 1] = 0;
		lobby_utf8(text, description, sizeof(description));
		p2p_discord_set_game(FALSE, TRUE, description, NULL, 0, 0);
		return;
	}
	p2p_discord_set_game(FALSE, FALSE, NULL, NULL, 0, 0);
}
