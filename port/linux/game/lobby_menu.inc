/*
LOBBY_MENU.INC

The lobby on the computer (tools/port_settings.py's _lobby_board), the
network game's and a game of this machine alone's (Create Game's OFFLINE,
split screen), in Multiplayer's look: the menus' shared parts, included in
menu_functions.c after party_menu.inc, whose helpers it uses. The
touchscreen's is the same by the same names, its panel's controls and the
footer's buttons a finger's size (tools/port_settings.py's
_touch_lobby_board).

It draws what this machine knows of the game, and sends nothing of its own
but what the Xbox's lobby sent (a player's team, the start): the game's
players (the host's record, which every machine has), each in its team's
column (one list, both columns', in a game without teams, and in Infection,
whose zombies are chosen when it starts), with a tag: HOST (the host's
machine: the host's own; for a player, the game's first machine, which the
host's own client is), BOT and the bots' skill (bots.c's machines), PARTY
(a member of this machine's party, by the name the party knows it by), OPENCE or OPENH1 (the
host knows each machine's join token: network_standard.c; a player knows
only its own). This machine's players are framed. In each column the host
first, then the party's members together, then the others as they joined,
the bots last. The details panel: the next game (its gametype, map and
limits; OPENH1 n TICK or LEGACY, whether OpenCE's players may join, which
the host alone knows), THEN (a network game's host: the game after it),
the host's bots in any game but co-op (BOTS: NONE, ADD n or FILL TO n, the
choices Host Controls' BOTS steps, network_host_controls.c; SKILL and
TEAMS, bots.skill and bots.teams: each written as it changes, which the
host's bots follow, dimmed for a gametype the bots do not play, and BOTS
for a matchmaking game's, its playlist's), how the game is, and the host's
GAME SETTINGS (Server Setup, in the lobby's place), HOST CONTROLS and START
GAME, or a player's WAITING FOR THE HOST.

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
/* a row's tag's width a name leaves room for (tools/port_settings.py's
LOBBY_TAG_WIDTH: a bot's tag with its skill is wider, its name then cut
to the rest, else the tag BOT alone) and the space between them */
#define LOBBY_BOARD_TAG_WIDTH 58
#define LOBBY_BOARD_TAG_GAP 6

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
	short rows[LOBBY_BOARD_COLUMNS][LOBBY_LAYOUT_ROWS];
	short counts[LOBBY_BOARD_COLUMNS];
	short more[LOBBY_BOARD_COLUMNS];
	/* (each column's rows laid out) */
	short row_count;
};

/* what the lobby offers this machine */
struct lobby_board_controls
{
	/* the host's START GAME and GAME SETTINGS; a player's WAITING FOR THE
	HOST */
	boolean start;
	boolean settings;
	boolean waiting;
	/* the bots' rows (the host's, but in co-op), locked (dimmed: a gametype
	the bots do not play) */
	boolean bots;
	boolean bots_locked;
	/* CHANGE TEAM, COPY INVITE, ADD PLAYER */
	boolean team_change;
	boolean invite;
	boolean add_player;
};

static wchar_t const *const lobby_tag_words[NUMBER_OF_LOBBY_TAGS] =
{
	L"", L"HOST", L"BOT", L"PARTY", L"GOAT H1", L"OPENCE",
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
	/* the BOTS choice its spinner last showed ("port lobby bots load"): one
	the spinner leaves is the host's, written */
	short bots_shown;
} lobby_board;

/* the bots' rows (tools/port_settings.py's BOTS_ROWS), each a row and its
spinner: BOTS (the choices: lobby_board_bots_choice), SKILL and TEAMS
(bots.skill's and bots.teams' spinners) */
static char const *const lobby_board_bots_rows[] = { "board_bots_count", "board_bots_skill", "board_bots_teams" };

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

/* the entries (in the game's order) into the columns' rows (rows of them
each, at most LOBBY_LAYOUT_ROWS: the generated screen's LOBBY_BOARD_ROWS,
the hand-written one's as many as its lanes have): a team game's by team,
else one list down red's column and on down blue's; a column with more
than its rows shows one fewer, and how many more in its last */
static void lobby_board_lay_out_rows(struct lobby_board_entry const *entries, short count, boolean teams, short rows,
	struct lobby_board_layout *layout)
{
	short order[LOBBY_BOARD_ENTRIES];
	short column, row, index, rank, shown, total = 0;

	rows = (short)MIN(MAX(rows, 1), LOBBY_LAYOUT_ROWS);
	layout->row_count = rows;
	for (column = 0; column < LOBBY_BOARD_COLUMNS; column++)
	{
		for (row = 0; row < LOBBY_LAYOUT_ROWS; row++)
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
		shown = (short)(total <= LOBBY_BOARD_COLUMNS * rows ? total : LOBBY_BOARD_COLUMNS * rows - 1);
		for (index = 0; index < shown; index++)
			layout->rows[index / rows][index % rows] = order[index];
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
			if (in_column < rows)
				layout->rows[column][in_column] = order[index];
			in_column++;
		}
		layout->counts[column] = in_column;
		if (in_column > rows)
		{
			layout->rows[column][rows - 1] = NONE;
			layout->more[column] = (short)(in_column - rows + 1);
		}
	}
}

/* ... in the generated screen's rows */
static void lobby_board_lay_out(struct lobby_board_entry const *entries, short count, boolean teams,
	struct lobby_board_layout *layout)
{
	lobby_board_lay_out_rows(entries, count, teams, LOBBY_BOARD_ROWS, layout);
}

/* what the lobby offers: the host's start and settings (a player waits for
the host), the host's bots in any game but co-op (locked for a gametype
they do not play), a team change while this machine has a player in a game whose
players choose a team, the invite link while there is one, ADD PLAYER while
this machine has room for another */
static void lobby_board_controls_for(boolean host, boolean cooperative, boolean bots_play, boolean teams,
	boolean own_player, boolean invite, boolean room, struct lobby_board_controls *controls)
{
	controls->start = host;
	controls->settings = host;
	controls->waiting = !host;
	controls->bots = host && !cooperative;
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

/* a bot's row's tag: BOT and the bots' skill, if its name still fits beside
it, else BOT alone (the name box fitted to what is left) */
static void lobby_board_bot_tag(struct widget_instance *tag, struct widget_instance *name_box)
{
	static wchar_t const *const skills[] = { L"EASY", L"NORMAL", L"HEROIC", L"LEGENDARY" };
	wchar_t text[ROW_TEXT_LENGTH], name[ROW_TEXT_LENGTH];
	short name_width, tag_width, room;
	long font;

	if (!tag || !name_box || !name_box->parameters.text_box.text)
		return;
	ustrncpy(name, name_box->parameters.text_box.text, NUMBEROF(name) - 1);
	name[NUMBEROF(name) - 1] = 0;
	font = ui_widget_port_text_box_font(name_box, &name_width);
	usnprintf(text, NUMBEROF(text) - 1, L"BOT \x00B7 %s", skills[PIN(bots_skill(), 0, (short)NUMBEROF(skills) - 1)]);
	text[NUMBEROF(text) - 1] = 0;
	tag_width = menu_text_box_fit(tag, text, 0);
	room = (short)(name_width + LOBBY_BOARD_TAG_WIDTH - tag_width - LOBBY_BOARD_TAG_GAP);
	if (font == NONE || !tag_width || settings_text_width(font, name) > room)
		text_set(tag, lobby_tag_words[_lobby_tag_bot]);
}

/* BOTS' spinner (its strings the choices, network_host_controls.c's, in
order): the choice the settings make shown, or one the host stepped to
written (as Host Controls' BOTS writes it), unless locked */
static void lobby_board_bots_choice(struct widget_instance *spinner, boolean locked)
{
	short chosen = network_host_controls_bots_chosen();
	short shown = spinner->parameters.list.selected_index;
	long number;
	short kind;

	if (!locked && lobby_board.bots_shown != NONE && shown != lobby_board.bots_shown)
	{
		kind = network_host_controls_bot_choice(shown, &number);
		if (network_host_controls_choose_bots(kind, number))
		{
			lobby_board.bots_shown = shown;
			return;
		}
	}
	spinner->parameters.list.selected_index = chosen;
	lobby_board.bots_shown = chosen;
}

/* "port lobby bots load" (BOTS' spinner made): the choice the settings make */
static boolean lobby_board_bots_load(struct widget_instance *spinner)
{
	/* (one of the spinner's items, each made from the spinner's own
	definition and so given its created event too: ui_widget.c's list
	items, before the spinner's own) */
	if (spinner->parameters.list.number_of_items == 0)
		return TRUE;
	if (spinner->parameters.list.number_of_items != network_host_controls_bot_choice_count())
		platform_log("menus: the lobby's BOTS has %d choices, not %d", (int)spinner->parameters.list.number_of_items,
			(int)network_host_controls_bot_choice_count());
	lobby_board.bots_shown = NONE;
	lobby_board_bots_choice(spinner, TRUE);
	return TRUE;
}

/* what the lobby shows, worked out from what this machine knows: the
generated screen's widgets (lobby_board_update) and the hand-written
screen's table and bindings (screens/lobby.xml: pc_menu_lobby_source,
pc_menu_lobby_value) are its two views */
struct lobby_board_model
{
	void *client;
	struct network_game *game;
	short state;
	boolean host, local, dedicated, teams, own_player;
	short machine, count, mode, seconds;
	struct lobby_board_entry entries[LOBBY_BOARD_ENTRIES];
	struct network_player *players[LOBBY_BOARD_ENTRIES];
	struct lobby_board_layout layout;
	struct lobby_board_controls controls;
	/* (the next game's: an OpenH1 game's or Legacy's, its rate) */
	boolean standard;
	long rate;
	/* (the host's: whether OpenCE's players may join; THEN's game after this
	one, and HOST CONTROLS, the host's or a dedicated server's admin's) */
	boolean opence, then, host_controls;
	wchar_t subtitle[SETTINGS_PANEL_TEXT_LENGTH];
	wchar_t gametype[ROW_TEXT_LENGTH];
	wchar_t map[SETTINGS_PANEL_TEXT_LENGTH];
	wchar_t limits[SETTINGS_PANEL_TEXT_LENGTH];
	wchar_t after[SETTINGS_PANEL_TEXT_LENGTH];
	wchar_t status[SETTINGS_PANEL_TEXT_LENGTH];
	/* (START GAME's words, or the countdown; a player's waiting's) */
	wchar_t start[ROW_TEXT_LENGTH];
	wchar_t waiting[ROW_TEXT_LENGTH];
};

/* the model made now, its columns of rows rows (lobby_board_lay_out_rows);
and what the lobby does each frame its screen is shown, whichever draws it:
a dedicated server's map vote's screen (map_vote_lobby_check), the players
counted (lobby_player_count) */
static void lobby_board_model_make(struct lobby_board_model *model, short rows)
{
	void *client = global_network_game_client_get();
	struct network_game *game = client ? network_game_client_get_game(client) : NULL;
	short state_data, state = client ? network_game_client_get_state(client, &state_data) : NONE;
	boolean host = global_network_game_server_get() != NULL;
	struct p2p_party_status party;
	char link[TEXT_FIELD_LENGTH];
	short count = 0, index;
	wchar_t *text;

	model->client = client;
	model->state = state;
	model->host = host;
	model->local = host && network_game_is_splitscreen_local();
	/* (a dedicated server's game: no host's player, its own rules start it) */
	model->dedicated = !host && lobby_joined_dedicated;
	model->machine = network_game_client_get_local_machine_index();
	model->mode = _game_mode_none;
	model->seconds = NONE;
	model->teams = FALSE;
	model->own_player = FALSE;
	if (!game || state < _client_state_pregame)
		game = NULL;
	model->game = game;
	/* (a dedicated server's map vote begun: its screen, map_vote_lobby_check) */
	if (game)
		map_vote_lobby_check();
	p2p_party_status(&party);
	for (index = 0; game && index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
	{
		struct network_player *player = &game->players[index];
		struct lobby_board_entry *entry = &model->entries[count];
		long player_machine = player->machine_index;

		if (!network_player_is_valid(player))
			continue;
		model->players[count++] = player;
		entry->team = player->team_index;
		entry->bot = bots_machine_is_bot(player_machine);
		entry->own = !entry->bot && player_machine == model->machine;
		/* (a player knows the host's machine as the game's first: the
		host's own client joins it first) */
		entry->host = !entry->bot && player_machine == (host ? model->machine : 0);
		entry->party = !entry->bot && lobby_board_in_party(&party, player);
		entry->kind = _lobby_kind_unknown;
		if (host && network_standard_machine_is_opence(player_machine))
			entry->kind = _lobby_kind_opence;
		else if ((host && network_standard_machine_is_openh1(player_machine)) || entry->own)
			entry->kind = _lobby_kind_openh1;
		model->own_player |= entry->own;
	}
	model->count = count;
	lobby_player_count = count;
	if (game)
	{
		model->mode = game_modes_variant_mode(&game->variant, &game->variant_options);
		model->teams = lobby_board_teams(game->variant.universal_variant.teams, model->mode);
		model->seconds = network_game_client_get_seconds_to_game_start(client);
	}
	lobby_board_lay_out_rows(model->entries, count, model->teams, rows, &model->layout);
	lobby_board_controls_for(host, game && !game->variant.game_engine_index,
		game && bots_play_game_engine(game->variant.game_engine_index), model->teams,
		model->own_player, lobby_board_invite(link, sizeof(link)) > 0, lobby_local_player_count() < MAXIMUM_LOCAL_PLAYERS,
		&model->controls);

	/* (the game's name, where it is, its players: a game of this machine
	alone's none, as no one else sees it) */
	text = model->subtitle;
	text[0] = 0;
	if (game && model->local)
	{
		boolean bots = FALSE;

		for (index = 0; index < count; index++)
			bots |= model->entries[index].bot;
		usnprintf(text, SETTINGS_PANEL_TEXT_LENGTH - 1, count == 1 ? L"%s \x00B7 1 player" : L"%s \x00B7 %d players",
			bots ? L"Offline with bots" : L"Offline", count);
	}
	else if (game)
	{
		wchar_t name[ROW_TEXT_LENGTH];
		short bots = 0;

		/* (and the host's bots among them, bots.c's) */
		for (index = 0; index < count; index++)
			bots += model->entries[index].bot ? 1 : 0;
		lobby_board_game_name(game, model->entries, model->players, count, name, NUMBEROF(name));
		usnprintf(text, SETTINGS_PANEL_TEXT_LENGTH - 1, L"%s \x00B7 %s \x00B7 %d of %d players", name,
			model->dedicated ? L"Dedicated server" : model->controls.invite ? L"Internet" : L"Local network", count,
			game->maximum_players);
		if (bots)
		{
			size_t length = ustrlen(text);

			usnprintf(text + length, SETTINGS_PANEL_TEXT_LENGTH - 1 - length, bots == 1 ? L" \x00B7 1 bot" :
				L" \x00B7 %d bots", bots);
		}
	}
	text[SETTINGS_PANEL_TEXT_LENGTH - 1] = 0;

	/* (the next game) */
	model->gametype[0] = model->map[0] = model->limits[0] = model->status[0] = model->start[0] = 0;
	model->waiting[0] = model->after[0] = 0;
	model->standard = FALSE;
	model->rate = 0;
	model->opence = FALSE;
	if (game)
	{
		wchar_t const *kind = game_modes_variant_kind_name(&game->variant, &game->variant_options);

		model->standard = host ? network_standard_hosting() : network_standard_agreed_rate() > 0;
		model->rate = network_standard_agreed_rate();
		ustrncpy(model->gametype, game->variant.human_readable_game_description,
			MIN(NUMBEROF(model->gametype), NUMBEROF(game->variant.human_readable_game_description) + 1) - 1);
		model->gametype[MIN(NUMBEROF(model->gametype), NUMBEROF(game->variant.human_readable_game_description) + 1) - 1] =
			0;
		if (!model->gametype[0])
			ustrncpy(model->gametype, kind ? kind : game->variant.game_engine_index ?
				engine_names[PIN(game->variant.game_engine_index, 0, 5)] : L"CO-OP", NUMBEROF(model->gametype) - 1);
		model->gametype[NUMBEROF(model->gametype) - 1] = 0;
		lobby_board_map_text(game->map.name, model->map, NUMBEROF(model->map));
		/* (a co-op game's difficulty after its level) */
		if (!game->variant.game_engine_index && campaign_level_of(game->map.name) != NONE)
		{
			wchar_t difficulty[ROW_TEXT_LENGTH];
			size_t length = ustrlen(model->map);

			map_difficulty_text(game->difficulty, difficulty);
			usnprintf(model->map + length, NUMBEROF(model->map) - 1 - length, L" \x00B7 %s", difficulty);
			model->map[NUMBEROF(model->map) - 1] = 0;
		}
		lobby_board_limits_text(game, model->limits, NUMBEROF(model->limits));
		model->opence = host && !model->local && network_standard_hosting_mixed();

		/* (how the game is: the countdown, the link just copied, how
		another player of this machine joins, Infection's teams, waiting
		for other machines) */
		text = model->status;
		if (model->seconds > 0)
			usnprintf(text, SETTINGS_PANEL_TEXT_LENGTH - 1, L"The game starts in %d.", model->seconds);
		else if (model->seconds == 0)
			usnprintf(text, SETTINGS_PANEL_TEXT_LENGTH - 1, L"The game is starting.");
		else if (lobby_board.copied_time && system_milliseconds() - lobby_board.copied_time < LOBBY_BOARD_COPIED_TIME)
			usnprintf(text, SETTINGS_PANEL_TEXT_LENGTH - 1, L"Invite copied: paste it in a chat to your friends.");
		else if (model->controls.bots_locked)
			usnprintf(text, SETTINGS_PANEL_TEXT_LENGTH - 1, L"Bots play every gametype but Race.");
		else if (lobby_join.adding)
			usnprintf(text, SETTINGS_PANEL_TEXT_LENGTH - 1, L"New player: press START.");
		else if (model->mode == _game_mode_infection)
			usnprintf(text, SETTINGS_PANEL_TEXT_LENGTH - 1, L"Infection: the zombies are chosen when the game starts.");
		else if (host && !model->local && game->machine_count < 2)
			usnprintf(text, SETTINGS_PANEL_TEXT_LENGTH - 1, L"Waiting for players to join.");
		else if (model->dedicated)
			usnprintf(text, SETTINGS_PANEL_TEXT_LENGTH - 1, L"A dedicated server: it starts once enough players join.");
		else if (lobby_local_player_count() < MAXIMUM_LOCAL_PLAYERS)
		{
			short controller;

			for (controller = 0; controller < MAXIMUM_LOCAL_PLAYERS; controller++)
			{
				if (input_has_gamepad(controller) && !lobby_controller_playing(controller))
					usnprintf(text, SETTINGS_PANEL_TEXT_LENGTH - 1, L"Another controller: START joins.");
			}
		}
		text[SETTINGS_PANEL_TEXT_LENGTH - 1] = 0;
		/* (START GAME's words, the countdown's; a player's waiting) */
		if (model->seconds > 0)
			usnprintf(model->start, NUMBEROF(model->start) - 1, model->controls.start ? L"STARTING IN %d" :
				L"Starting in %d", model->seconds);
		else if (model->seconds == 0)
			usnprintf(model->start, NUMBEROF(model->start) - 1, model->controls.start ? L"STARTING..." : L"Starting...");
		else
			usnprintf(model->start, NUMBEROF(model->start) - 1, model->dedicated ? L"Waiting for players" :
				!model->controls.start ? L"Waiting for the host" : L"START GAME");
		model->start[NUMBEROF(model->start) - 1] = 0;
		if (model->controls.waiting)
			ustrncpy(model->waiting, model->start, NUMBEROF(model->waiting) - 1);
	}
	/* (a network game's host: the game after this one, its playlist's, and
	HOST CONTROLS: network_host_game.c; a dedicated server's admin's too:
	network_admin.c) */
	{
		struct server_entry after;
		boolean now;
		boolean hosting = game && host && !model->local && network_host_controls_available();

		model->then = hosting && network_host_game_next(&after, &now) && !now;
		if (model->then)
			network_host_game_entry_shown(&after, model->after, NUMBEROF(model->after));
		model->host_controls = hosting || (game && !host && network_admin_remote());
	}
}

/* "port lobby board update" (the list's) */
static void lobby_board_update(struct widget_instance *list)
{
	static struct lobby_board_model model;
	struct lobby_board_entry const *entries = model.entries;
	struct network_player *const *players = model.players;
	struct widget_instance *screen = screen_of(list);
	struct widget_instance *panel = named(screen, "board_panel", 0);
	struct widget_instance *bar = named(list, "board_prompts", 0);
	struct network_game *game;
	boolean players_focused = list->focused_child && !strcmp(list->focused_child->name, "board_players");
	struct lobby_board_layout layout;
	struct lobby_board_controls controls;
	wchar_t text[SETTINGS_PANEL_TEXT_LENGTH];
	short count, column, row, index;
	boolean teams, host, local;

	lobby_board_model_make(&model, LOBBY_BOARD_ROWS);
	game = model.game;
	count = model.count;
	teams = model.teams;
	host = model.host;
	local = model.local;
	layout = model.layout;
	controls = model.controls;
	(void)local;
	menu_text_box_fit(named(screen, "board_subtitle", 0), model.subtitle, 0);

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
			/* (a bot's with its skill, as wide as its name leaves room for:
			lobby_board_bot_tag) */
			if (entry->bot)
				lobby_board_bot_tag(tag, name_box);
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

		menu_text_box_fit(named(panel, "board_gametype", 0), model.gametype, 0);
		menu_text_box_fit(named(panel, "board_map", 0), model.map, 0);
		menu_text_box_fit(named(panel, "board_limits", 0), model.limits, 0);
		if ((pill = named(panel, "board_kind", 0)) != NULL)
		{
			pill->animation.current_frame_index = model.standard ? 0 : 1;
			text_set(pill, model.standard ? L"GOAT H1" : L"LEGACY");
		}
		if ((pill = named(panel, "board_tick", 0)) != NULL)
		{
			pill->animation.current_frame_index = 2;
			usnprintf(text, NUMBEROF(text) - 1, L"%ld TICK", model.standard && model.rate > 0 ? model.rate : 30L);
			text[NUMBEROF(text) - 1] = 0;
			text_set(pill, text);
		}
		if ((pill = named(panel, "board_opence", 0)) != NULL)
		{
			pill->animation.current_frame_index = 2;
			text_set(pill, L"OPENCE CAN JOIN");
		}
		visible_set(named(panel, "board_opence", 0), model.opence);
		menu_text_box_wrap(named(panel, "board_status", 0), model.status, 2);
		/* (START GAME's words with its key's picture, but on a touchscreen) */
		if (model.seconds < 0 && controls.start && !ui_widget_port_touch_menus())
			usnprintf(text, NUMBEROF(text) - 1, L"%%a-button %s", model.start);
		else
			usnprintf(text, NUMBEROF(text) - 1, L"%s", model.start);
		text[NUMBEROF(text) - 1] = 0;
		text_set(named(list, "board_start", 0), text);
		text_set(named(panel, "board_waiting", 0), text);
	}

	/* (the host's controls, a game of this machine alone's bots, the
	prompts that apply) */
	visible_set(named(list, "board_start", 0), game && controls.start);
	visible_set(named(list, "board_settings", 0), game && controls.settings);
	visible_set(named(panel, "board_waiting", 0), controls.waiting);
	for (index = 0; index < NUMBEROF(lobby_board_bots_rows); index++)
	{
		char name[32];
		struct widget_instance *bots_row, *spinner;
		struct pc_menu_setting *setting;
		/* (BOTS a matchmaking game's too, its playlist's) */
		boolean locked = controls.bots_locked || (index == 0 && bots_matchmaking_on());

		bots_row = named(list, lobby_board_bots_rows[index], 0);
		snprintf(name, sizeof(name), "%s_spinner", lobby_board_bots_rows[index]);
		spinner = named(list, name, 0);
		setting = spinner ? pc_menu_setting_get(spinner->definition_tag_index) : NULL;
		if (!bots_row || !spinner)
			continue;
		visible_set(bots_row, controls.bots);
		bots_row->disabled = locked;
		spinner->disabled = locked;
		bots_row->alpha_modifier = locked ? 0.4f : 1.0f;
		/* (the focus on a row locked goes to the players) */
		if (locked && list->focused_child == bots_row)
		{
			list->focused_child = list->child;
			list->parameters.list.selected_index = 0;
		}
		/* (BOTS' choice written as it changes; the others written to
		config.toml as they change: the host's bots follow them.
		setting_changed_save passes over a locked spinner) */
		if (index == 0)
			lobby_board_bots_choice(spinner, !controls.bots || locked);
		else if (controls.bots && setting)
			setting_changed_save(spinner, setting);
	}
	/* (a network game's host: the game after this one, its playlist's, and
	HOST CONTROLS: network_host_game.c; a dedicated server's admin's too) */
	if (model.then)
		menu_text_box_fit(named(panel, "board_after", 0), model.after, 0);
	visible_set(named(panel, "board_after", 0), model.then);
	visible_set(named(panel, "board_after_label", 0), model.then);
	visible_set(named(list, "board_host", 0), model.host_controls);
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
		(short)(x >= widget_screen_x(blue) - LOBBY_BOARD_COLUMN_GAP / 2 ? 1 : 0));
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

/* ---- the hand-written lobby (screens/lobby.xml, docs/menu_screens.md):
the same model in the new engine's tall layout. Its table (the row source
lobby.players) has a lane for each team's column, red's and blue's (in a
game without teams one list down both, under PLAYERS), each place of a lane
a row; its panel and bar the lobby.* bindings; its actions the generated
screen's functions, or these */

static void lobby_utf8(wchar_t const *wide, char *text, short size);

static struct
{
	struct lobby_board_model model;
	/* when the model was made (system_milliseconds), and the rows each lane
	has (the table's: lanes' places) */
	unsigned long made_at;
	boolean made;
	short rows;
} lobby_screen;

/* the model as this frame has it (made again once a frame: the bindings and
the table read it in turn) */
static struct lobby_board_model *lobby_screen_model(void)
{
	unsigned long now = system_milliseconds();

	if (!lobby_screen.made || lobby_screen.made_at != now)
	{
		lobby_board_model_make(&lobby_screen.model, lobby_screen.rows ? lobby_screen.rows : LOBBY_LAYOUT_ROWS);
		lobby_screen.made = TRUE;
		lobby_screen.made_at = now;
	}
	return &lobby_screen.model;
}

/* a lane's place's player (index: the lane's rows in turn), NONE for none,
and whether it is the lane's last, which says how many more */
static short lobby_screen_entry(short index, boolean *more)
{
	struct lobby_board_model *model = lobby_screen_model();
	short rows = model->layout.row_count, column = (short)(index / MAX(rows, 1)), row = (short)(index % MAX(rows, 1));

	*more = FALSE;
	if (column < 0 || column >= LOBBY_BOARD_COLUMNS || row >= rows)
		return NONE;
	*more = model->layout.rows[column][row] == NONE && row == rows - 1 && model->layout.more[column] > 0;
	return model->layout.rows[column][row];
}

static void lobby_screen_update(struct menu_table_view *view)
{
	short lanes = (short)MAX(view->lanes, 1);

	lobby_screen.rows = (short)MIN(MAX(view->shown / lanes, 1), LOBBY_LAYOUT_ROWS);
	lobby_screen.made = FALSE;
	lobby_screen_model();
	view->count = view->shown;
	view->first = 0;
	view->chosen = NONE;
	view->focus = NONE;
	/* (the speakers of the rows drawn this frame, noted as their cells are:
	pc_menu_speaker_note) */
	lobby_icons.count = 0;
	lobby_icons.updated_at = system_milliseconds();
}

static int lobby_screen_heading(short index, unsigned short *words, short size)
{
	(void)index;
	(void)words;
	(void)size;
	return FALSE;
}

static int lobby_screen_state(short index)
{
	boolean more;
	short shown = lobby_screen_entry(index, &more);

	if (shown == NONE)
		return more ? MENU_TABLE_ROW_PLAIN : MENU_TABLE_ROW_NONE;
	return lobby_screen.model.entries[shown].own ? MENU_TABLE_ROW_MARKED : MENU_TABLE_ROW_PLAIN;
}

/* a row's cells: its player's dot (its team's, or a game without teams'),
name (its controller's too, of this machine's players when it has more than
one; its speaker in voice chat) and tag (a bot's with the bots' skill where
it fits); the lane's last, how many more */
static void lobby_screen_cell(short index, char const *column, int focused, struct menu_table_cell *cell)
{
	struct lobby_board_model *model = lobby_screen_model();
	boolean more;
	short shown = lobby_screen_entry(index, &more);
	struct lobby_board_entry const *entry = shown != NONE ? &model->entries[shown] : NULL;
	wchar_t text[ROW_TEXT_LENGTH];

	(void)focused;
	text[0] = 0;
	if (!strcmp(column, "dot"))
	{
		cell->frame = (short)(!entry ? NONE : model->teams ? (entry->team == 1 ? 1 : 0) : 2);
		return;
	}
	if (more && !strcmp(column, "name"))
	{
		short lane = (short)(index / MAX(model->layout.row_count, 1));

		usnprintf(text, NUMBEROF(text) - 1, L"+%d more", model->layout.more[lane]);
	}
	else if (entry && !strcmp(column, "name"))
	{
		struct network_player const *player = model->players[shown];
		wchar_t name[NUMBEROF(player->name) + 1];

		ustrncpy(name, player->name, NUMBEROF(player->name));
		name[NUMBEROF(player->name)] = 0;
		if (entry->own && lobby_local_player_count() >= 2)
			usnprintf(text, NUMBEROF(text) - 1, L"%s (P%d)", name, player->controller_index + 1);
		else
			usnprintf(text, NUMBEROF(text) - 1, L"%s", name);
		cell->speaker = player->machine_index;
	}
	else if (entry && !strcmp(column, "tag"))
	{
		static wchar_t const *const skills[] = { L"EASY", L"NORMAL", L"HEROIC", L"LEGENDARY" };
		short letter;

		ustrncpy(text, lobby_tag_words[lobby_board_tag(entry)], NUMBEROF(text) - 1);
		if (entry->bot)
		{
			/* (BOT alone where BOT and the skill would be cut short) */
			for (letter = 0; text[letter] && letter < MENU_TABLE_TEXT_LENGTH - 1; letter++)
				cell->fallback[letter] = (unsigned short)text[letter];
			cell->fallback[letter] = 0;
			usnprintf(text, NUMBEROF(text) - 1, L"BOT \x00B7 %s", skills[PIN(bots_skill(), 0, (short)NUMBEROF(skills) - 1)]);
		}
	}
	text[NUMBEROF(text) - 1] = 0;
	{
		short letter;

		for (letter = 0; text[letter] && letter < MENU_TABLE_TEXT_LENGTH - 1; letter++)
			cell->text[letter] = (unsigned short)text[letter];
		cell->text[letter] = 0;
	}
}

struct menu_table_source const pc_menu_lobby_source =
{
	lobby_screen_update, lobby_screen_heading, lobby_screen_cell, NULL, lobby_screen_state,
};

void pc_menu_speaker_note(struct widget_instance *text, long machine)
{
	if (!text || lobby_icons.count >= LOBBY_ICON_ROWS)
		return;
	lobby_icons.texts[lobby_icons.count] = text;
	lobby_icons.machines[lobby_icons.count] = machine;
	lobby_icons.count++;
	lobby_icons.updated_at = system_milliseconds();
}

/* text (UTF-16) as a binding's value, UTF-8: nonzero if it has any */
static int lobby_screen_words(wchar_t const *text, char *value, unsigned long size)
{
	lobby_utf8(text, value, (short)MIN(size, 0x7FFF));
	return value[0] != 0;
}

/* a lobby.* binding's value (docs/menu_screens.md's table) */
int pc_menu_lobby_value(char const *name, char *value, unsigned long size)
{
	struct lobby_board_model *model = lobby_screen_model();
	struct lobby_board_controls const *controls = &model->controls;
	wchar_t text[ROW_TEXT_LENGTH];
	boolean yes = FALSE;

	if (!size)
		return 0;
	value[0] = 0;
	text[0] = 0;
	if (!strcmp(name, "lobby.title"))
		usnprintf(text, NUMBEROF(text) - 1, L"LOBBY");
	else if (!strcmp(name, "lobby.subtitle"))
		return lobby_screen_words(model->subtitle, value, size);
	/* (the lanes: RED and BLUE in a team game, each its players' count;
	else PLAYERS across both, how many of the most at its right) */
	else if (!strcmp(name, "lobby.red"))
		usnprintf(text, NUMBEROF(text) - 1, model->teams ? L"RED" : L"PLAYERS");
	else if (!strcmp(name, "lobby.blue") && model->teams)
		usnprintf(text, NUMBEROF(text) - 1, L"BLUE");
	else if ((!strcmp(name, "lobby.red.count") || !strcmp(name, "lobby.blue.count")) && model->teams)
	{
		short count = model->layout.counts[name[6] == 'b' ? 1 : 0];

		usnprintf(text, NUMBEROF(text) - 1, count == 1 ? L"1 PLAYER" : L"%d PLAYERS", count);
	}
	else if (!strcmp(name, "lobby.blue.count") && model->game)
		usnprintf(text, NUMBEROF(text) - 1, L"%d OF %d", model->count, model->game->maximum_players);
	else if (!strcmp(name, "lobby.red.tone"))
		usnprintf(text, NUMBEROF(text) - 1, model->teams ? L"0" : L"2");
	else if (!strcmp(name, "lobby.blue.tone"))
		usnprintf(text, NUMBEROF(text) - 1, model->teams ? L"1" : L"2");
	/* (the panel: the next game) */
	else if (!model->game && !strncmp(name, "lobby.", 6) && strcmp(name, "lobby.add") && strcmp(name, "lobby.invite"))
		return 0;
	else if (!strcmp(name, "lobby.gametype"))
		return lobby_screen_words(model->gametype, value, size);
	else if (!strcmp(name, "lobby.map"))
		return lobby_screen_words(model->map, value, size);
	else if (!strcmp(name, "lobby.limits"))
		return lobby_screen_words(model->limits, value, size);
	else if (!strcmp(name, "lobby.kind"))
		usnprintf(text, NUMBEROF(text) - 1, model->standard ? L"GOAT H1" : L"LEGACY");
	else if (!strcmp(name, "lobby.kind.tone"))
		usnprintf(text, NUMBEROF(text) - 1, model->standard ? L"0" : L"1");
	else if (!strcmp(name, "lobby.tick"))
		usnprintf(text, NUMBEROF(text) - 1, L"%ld TICK", model->standard && model->rate > 0 ? model->rate : 30L);
	else if (!strcmp(name, "lobby.opence") && model->opence)
		usnprintf(text, NUMBEROF(text) - 1, L"OPENCE CAN JOIN");
	else if (!strcmp(name, "lobby.after") && model->then)
		return lobby_screen_words(model->after, value, size);
	/* (the host's bots: BOTS' choice, SKILL and TEAMS, dimmed for a gametype
	the bots do not play, BOTS for a matchmaking game's too) */
	else if (!strcmp(name, "lobby.bots"))
		network_host_controls_bot_choice_words(network_host_controls_bots_chosen(), text, NUMBEROF(text));
	else if (!strcmp(name, "lobby.skill"))
	{
		static wchar_t const *const skills[] = { L"EASY", L"NORMAL", L"HEROIC", L"LEGENDARY" };

		usnprintf(text, NUMBEROF(text) - 1, L"%s", skills[PIN(bots_skill(), 0, (short)NUMBEROF(skills) - 1)]);
	}
	else if (!strcmp(name, "lobby.teams"))
		usnprintf(text, NUMBEROF(text) - 1, bots_teams() == _bots_teams_against ? L"AGAINST YOU" : L"EVEN");
	else if (!strcmp(name, "lobby.bots.shown"))
		yes = controls->bots;
	else if (!strcmp(name, "lobby.bots.locked"))
		yes = controls->bots_locked;
	else if (!strcmp(name, "lobby.bots.count.locked"))
		yes = controls->bots_locked || bots_matchmaking_on();
	/* (how the game is; START GAME's words, the countdown; a player's
	waiting) */
	else if (!strcmp(name, "lobby.status"))
		return lobby_screen_words(model->status, value, size);
	else if (!strcmp(name, "lobby.start"))
		return lobby_screen_words(model->start, value, size);
	else if (!strcmp(name, "lobby.waiting"))
		return lobby_screen_words(model->waiting, value, size);
	/* (what is offered, for ifs) */
	else if (!strcmp(name, "lobby.host"))
		yes = controls->start;
	else if (!strcmp(name, "lobby.settings"))
		yes = controls->settings;
	else if (!strcmp(name, "lobby.controls"))
		yes = model->host_controls;
	else if (!strcmp(name, "lobby.team.change"))
		yes = controls->team_change;
	else if (!strcmp(name, "lobby.invite"))
		yes = controls->invite;
	else if (!strcmp(name, "lobby.add"))
		yes = controls->add_player;
	else
		return 0;
	if (yes)
		usnprintf(text, NUMBEROF(text) - 1, L"1");
	text[NUMBEROF(text) - 1] = 0;
	return lobby_screen_words(text, value, size);
}

/* the lobby's actions of its own: a row chosen (a click on a lane: its
team, as a click on the generated screen's column; else the host's start,
as A on its players), the bots' choices stepped (BOTS' choices at their
ends no further; SKILL and TEAMS round), as the generated screen's spinners
write them */
int pc_menu_lobby_action(struct widget_instance *widget, struct event_record *event, char const *name, short row,
	int *deleted)
{
	struct lobby_board_model *model = lobby_screen_model();
	short controller = event_controller(widget, event);
	short step = (short)(strstr(name, " left") ? -1 : 1);
	short x, y;

	*deleted = FALSE;
	if (!strcmp(name, "lobby row"))
	{
		short rows = MAX(model->layout.row_count, 1);

		if (!ui_widget_port_take_click(&x, &y))
			return lobby_board_start();
		if (!model->teams || row == NONE)
			return TRUE;
		return lobby_board_team(widget, controller, (short)(row / rows == 1 ? 1 : 0));
	}
	if (!model->controls.bots || model->controls.bots_locked)
		return FALSE;
	if (!strncmp(name, "lobby bots ", 11))
	{
		short chosen = network_host_controls_bots_chosen();
		short stepped = network_host_controls_bot_choice_step(chosen, step);
		long number;
		short kind = network_host_controls_bot_choice(stepped, &number);

		return !bots_matchmaking_on() && stepped != chosen && network_host_controls_choose_bots(kind, number);
	}
	if (!strncmp(name, "lobby skill ", 12))
	{
		static char const *const skills[] = { "easy", "normal", "heroic", "legendary" };

		return config_write("bots.skill", skills[(bots_skill() + step + 4) % 4]) != 0;
	}
	if (!strncmp(name, "lobby teams ", 12))
		return config_write("bots.teams", bots_teams() == _bots_teams_against ? "even" : "against") != 0;
	return FALSE;
}

/* each frame the hand-written lobby is drawn: what the generated screen's
list did (its data inputs: the split screen's players who want to play
added, "net splitscreen prejoin players"; the model made, which opens the
map vote's screen) */
void pc_menu_lobby_frame(struct widget_instance *screen)
{
	ui_widget_game_data_function_invoke(screen, LOBBY_PREJOIN_FUNCTION);
	lobby_screen_model();
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
Tick · Slayer on Blood Gulch", "OpenH1 120 Tick · Classic · Pro Doubles on
Prisoner" for a Pro 2v2 Classic game (Legacy for a game of OpenCE's netcode; a
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
			/* (a Pro 2v2 Classic game's said so, its record's Instant Aim
			off for everyone: the 2V2 browser tells a friend's kind by it,
			doubles_friend_in_kind, both kinds at 120) */
			if (standard && TEST_FLAG(game->cooperative_flags, _network_game_no_direct_camera_bit))
				usnprintf(text, NUMBEROF(text) - 1, L"OpenH1 %ld Tick \x00B7 Classic \x00B7 ", rate > 0 ? rate : 30L);
			else if (standard)
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
