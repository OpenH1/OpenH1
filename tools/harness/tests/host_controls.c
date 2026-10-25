/*
HOST_CONTROLS.C (test)

The host's controls of a network game (port/linux/game/network_host_controls.c):
who has them (the host of a network game under way; never a client, nor a
game of one machine alone, nor the menus), what each player may have done
to them (the host's own players: only a move; a bot: nothing but what the
bots allow; OpenCE's players as anyone's), Kick and Ban by the console's
own kick and ban (network_votekick_host_kick), Move team only in an OpenH1
game without OpenCE's machines (its unit killed without statistics, the
team set on the host's records and told to each OpenH1 machine, which takes
it only from the host's stream), Lock teams (a client's lobby change kept,
the host machine's own taken), End game now and Restart round (through the
scores' A and the lobby, its countdown let go), Max players, and bans.txt's
lines. test_host_controls.py takes the code (under_test.inc:
network_host_controls.c from its constants) and the constants (config.inc).
*/

#include "harness.h"

#include <stdarg.h>
#include <wchar.h>

typedef unsigned short word;
#define csmemcpy memcpy
#define csmemset memset
#define csmemcmp memcmp
#define SET_FLAG(flags, bit, value) ((value) ? ((flags) |= FLAG(bit)) : ((flags) &= ~FLAG(bit)))
#define NUMBEROF(array) ((long)(sizeof(array) / sizeof((array)[0])))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define VALID_INDEX(index, count) ((index) >= 0 && (index) < (count))
#define DATUM_INDEX_NEW(absolute_index, identifier) \
	((long)(((unsigned long)(unsigned short)(identifier) << 16) | (unsigned long)(absolute_index)))
#define error(level, ...) ((void)(level))
#define ustrncpy wcsncpy

#include "config.inc"

/* ---------- the world */

enum
{
	_game_connection_local,
	_game_connection_network_client,
	_game_connection_network_server,
	_game_connection_film_playback,
};

enum
{
	_error_log,
	MAXIMUM_TRACKED_PLAYERS = HALO_PORT_MAXIMUM_NETWORK_PLAYERS,
	NO_PLAYER = 0xFF,
	_distributed_to_host_reliably = 3,
	BOTS_FIRST_MACHINE = HALO_PORT_MAXIMUM_NETWORK_MACHINES - 15,
	SENT = 32,
};

/* the machines: the host's own (its client 0), an OpenH1 client 1, an
OpenCE client 2; a bot's */
enum
{
	OPENH1_MACHINE = 1,
	OPENCE_MACHINE = 2,
	BOT_MACHINE = BOTS_FIRST_MACHINE,
};

/* the players, by absolute index */
enum
{
	HOST_PLAYER,
	OPENH1_PLAYER,
	OPENCE_PLAYER,
	BOT_PLAYER,
	NUMBER_OF_TEST_PLAYERS,
};

struct message_header_stand_in { word size; };
typedef struct message_header_stand_in message_header;

struct distributed_message_header
{
	message_header header;
	byte type;
	byte count;
	long game_time;
};

struct network_player
{
	wchar_t name[12];
	short primary_color_index;
	short icon_index;
	char machine_index;
	char controller_index;
	char team_index;
	char player_list_index;
};

struct player_datum
{
	short identifier;
	short local_player_index;
	wchar_t name[12];
	long team_index;
	long unit_index;
	boolean quit_out_of_game;
	struct network_player network_player_data;
};

struct network_game
{
	wchar_t name[16];
	unsigned char maximum_players;
	short player_count;
	struct network_player players[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
};

struct network_game_server { int unused; };

struct data_iterator
{
	struct data_array *data;
	long index;
	long datum_index;
};

static struct
{
	short connection;
	boolean server;
	boolean in_game;
	boolean menu_loaded;
	boolean teams;
	boolean engine_running;
	boolean can_score;
	boolean postgame;
	boolean playing;
	boolean standard;
	boolean opence_machine;
	boolean living[NUMBER_OF_TEST_PLAYERS];
	/* what the code did */
	long kicked;
	boolean kick_ban;
	long killed_unit;
	short sent_count;
	long sent_machines[SENT];
	struct distributed_host_team_stand_in { byte format, player_index, team, pad; } sent[SENT];
	boolean ended;
	boolean reset;
	boolean countdown_paused;
	long countdown;
	boolean lobby_shown;
	boolean screen_loaded;
	char ban_text[4][HOST_CONTROLS_BAN_LINE_SIZE];
} world;

static struct data_array *player_data;
static struct network_game_server server_record;
static struct network_game server_game;
static struct network_game client_game;
static int client_record;

static long test_player_index(short absolute_index)
{
	struct player_datum *player = (struct player_datum *)(player_data->data + absolute_index * player_data->size);

	return DATUM_INDEX_NEW(absolute_index, player->identifier);
}

static struct player_datum *player_try_and_get(long player_index)
{
	struct player_datum *player;

	if (player_index == NONE || DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index) >= player_data->maximum_count)
		return NULL;
	player = datum_get(player_data, player_index);
	return player->identifier && (unsigned short)player->identifier == (unsigned long)player_index >> 16 ?
		player : NULL;
}

static void data_iterator_new(struct data_iterator *iterator, struct data_array *data)
{
	iterator->data = data;
	iterator->index = 0;
	iterator->datum_index = NONE;
}

static void *data_iterator_next(struct data_iterator *iterator)
{
	for (; iterator->index < iterator->data->maximum_count; iterator->index++)
	{
		struct datum_header *header =
			(struct datum_header *)(iterator->data->data + iterator->index * iterator->data->size);

		if (header->identifier)
		{
			iterator->datum_index = DATUM_INDEX_NEW(iterator->index, header->identifier);
			return iterator->data->data + iterator->index++ * iterator->data->size;
		}
	}
	return NULL;
}

static short game_connection(void) { return world.connection; }
static boolean game_in_progress(void) { return world.in_game; }
static boolean main_menu_is_loaded(void) { return world.menu_loaded; }
static void *global_network_game_server_get(void) { return world.server ? &server_record : NULL; }
static void *global_network_game_client_get(void) { return &client_record; }
static struct network_game *network_game_server_get_game(struct network_game_server *server)
{
	return server ? &server_game : NULL;
}
static struct network_game *network_game_client_get_game(void *client) { return client ? &client_game : NULL; }
static boolean network_player_is_valid(struct network_player const *player) { return player->machine_index >= 0; }
static boolean game_engine_has_teams(void) { return world.teams; }
static boolean game_engine_running(void) { return world.engine_running; }
static boolean game_engine_can_score(void) { return world.can_score; }
static boolean game_engine_showing_postgame(void) { return world.postgame; }
static void game_engine_end_game(void) { world.ended = TRUE; world.can_score = FALSE; }
static boolean network_game_server_playing(struct network_game_server *server) { (void)server; return world.playing; }
static boolean network_game_server_reset_to_pregame(struct network_game_server *server)
{
	(void)server;
	world.reset = TRUE;
	return TRUE;
}
static void network_game_server_pause_countdown(struct network_game_server *server, boolean paused)
{
	(void)server;
	world.countdown_paused = paused;
}
static void network_game_server_begin_game_start_countdown(struct network_game_server *server, long time)
{
	(void)server;
	if (!world.countdown_paused)
		world.countdown = time;
}
static void ui_widgets_close_all(void) { world.screen_loaded = FALSE; }
static void *ui_widget_load_by_name_or_tag(char const *name, long tag, void *parent, short local_player,
	long invoking, long focused_parent, short focused_child)
{
	(void)tag; (void)parent; (void)local_player; (void)invoking; (void)focused_parent; (void)focused_child;
	if (strstr(name, "lobby"))
		world.lobby_shown = TRUE;
	else
		world.screen_loaded = TRUE;
	return &world;
}
static char const *pc_menus_screen(char const *name)
{
	return strstr(name, "connected_pregame_screen") ? "pc\\main_menu\\multiplayer_type_select\\lobby\\lobby_screen" :
		name;
}
static void console_warning(const char *format, ...) { (void)format; }
static unsigned long system_milliseconds(void) { return 1000; }
static int bots_machine_is_bot(long machine_index) { return machine_index >= BOTS_FIRST_MACHINE; }
static struct player_datum *distributed_player(short player_index)
{
	return player_index >= 0 && player_index < NUMBER_OF_TEST_PLAYERS ?
		player_try_and_get(test_player_index(player_index)) : NULL;
}
static long distributed_player_machine(short player_index)
{
	switch (player_index)
	{
	case OPENH1_PLAYER: return OPENH1_MACHINE;
	case OPENCE_PLAYER: return OPENCE_MACHINE;
	default: return NONE;
	}
}
static short distributed_client_machines(long *machines, short maximum)
{
	(void)maximum;
	machines[0] = OPENH1_MACHINE;
	machines[1] = OPENCE_MACHINE;
	return world.opence_machine ? 2 : 1;
}
static boolean network_standard_hosting(void) { return world.standard; }
static boolean network_standard_machine_is_opence(long machine)
{
	return world.standard && machine == OPENCE_MACHINE && world.opence_machine;
}
static boolean network_standard_machine_is_openh1(long machine) { return world.standard && machine == OPENH1_MACHINE; }
static long network_standard_agreed_rate(void) { return world.standard ? 60 : 0; }
static boolean opence_joins = TRUE;
static boolean network_standard_opence_joins_open(void) { return opence_joins; }
static void network_standard_set_opence_joins(boolean allowed) { opence_joins = allowed; }
static byte distributed_player_to_byte(long player_index)
{
	return player_index != NONE ? (byte)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index) : NO_PLAYER;
}
static long distributed_player_from_byte(byte player_index)
{
	return player_index < NUMBER_OF_TEST_PLAYERS && distributed_player(player_index) ?
		test_player_index(player_index) : NONE;
}
static long distributed_living_unit(struct player_datum const *player)
{
	short index = (short)(player - (struct player_datum const *)player_data->data);

	return world.living[index] ? player->unit_index : NONE;
}
static void unit_kill_no_statistics(long unit_index) { world.killed_unit = unit_index; }
/* (after the code: its entry's structure) */
static void distributed_send_to_machine_reliably(long machine_index, void *message, byte type, short count,
	word size);
static boolean network_votekick_host_kick(short player_index, boolean ban)
{
	world.kicked = player_index;
	world.kick_ban = ban;
	return TRUE;
}
static long latency_meter_player_ping(short player_index) { return player_index ? 40 : 0; }
static char player_name_character_ascii(wchar_t character) { return character < 128 ? (char)character : '?'; }
static short network_distributed_ban_lines(char (*lines)[HOST_CONTROLS_BAN_LINE_SIZE], short maximum)
{
	short count = 0;

	while (count < maximum && count < 4 && world.ban_text[count][0])
	{
		strcpy(lines[count], world.ban_text[count]);
		count++;
	}
	return count;
}
static boolean network_distributed_unban(char const *line) { (void)line; return TRUE; }

#include "under_test.inc"

static void distributed_send_to_machine_reliably(long machine_index, void *message, byte type, short count,
	word size)
{
	struct
	{
		struct distributed_message_header header;
		struct distributed_host_team move;
	} sent;

	CHECK(type == _distributed_message_host_team && count == 1 && size == sizeof(sent), "a team move of one entry");
	world.sent_machines[world.sent_count] = machine_index;
	memcpy(&world.sent[world.sent_count++], (byte *)message + sizeof(struct distributed_message_header), 4);
}

/* ---------- the cases */

static void world_reset(void)
{
	static wchar_t const *const names[NUMBER_OF_TEST_PLAYERS] = { L"Keyes", L"Johnson", L"Cortana", L"Bot" };
	static char const machines[NUMBER_OF_TEST_PLAYERS] = { 0, OPENH1_MACHINE, OPENCE_MACHINE, (char)BOT_MACHINE };
	short index;

	memset(&world, 0, sizeof(world));
	memset(&host_controls, 0, sizeof(host_controls));
	world.connection = _game_connection_network_server;
	world.server = TRUE;
	world.in_game = TRUE;
	world.teams = TRUE;
	world.engine_running = TRUE;
	world.can_score = TRUE;
	world.playing = TRUE;
	world.standard = TRUE;
	world.kicked = NONE;
	world.killed_unit = NONE;
	world.countdown = NONE;
	world.countdown_paused = TRUE;
	opence_joins = TRUE;
	if (!player_data)
		player_data = game_state_data_new("players", 16, sizeof(struct player_datum));
	memset(player_data->data, 0, (size_t)(player_data->maximum_count * player_data->size));
	player_data->count = 0;
	memset(&server_game, 0, sizeof(server_game));
	memset(&client_game, 0, sizeof(client_game));
	for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
		server_game.players[index].machine_index = client_game.players[index].machine_index = -1;
	for (index = 0; index < NUMBER_OF_TEST_PLAYERS; index++)
	{
		long player_index = datum_new(player_data);
		struct player_datum *player = datum_get(player_data, player_index);

		wcscpy(player->name, names[index]);
		player->team_index = index % 2;
		player->unit_index = 100 + index;
		player->network_player_data.machine_index = machines[index];
		player->network_player_data.player_list_index = (char)index;
		player->network_player_data.team_index = (char)(index % 2);
		server_game.players[index] = client_game.players[index] = player->network_player_data;
		world.living[index] = TRUE;
	}
	server_game.player_count = NUMBER_OF_TEST_PLAYERS;
	server_game.maximum_players = 16;
}

/* who has Host Controls: the host of a network game under way alone */
static void case_seats(void)
{
	struct host_controls_seat seat = { TRUE, _game_connection_network_server, TRUE };

	CHECK(network_host_controls_shown_for(&seat), "the host of a network game under way");
	seat.connection = _game_connection_network_client;
	CHECK(!network_host_controls_shown_for(&seat), "a client");
	seat.server = FALSE;
	CHECK(!network_host_controls_shown_for(&seat), "a client without a server");
	seat.server = TRUE;
	seat.connection = _game_connection_local;
	CHECK(!network_host_controls_shown_for(&seat), "a game of this machine alone (split screen, bots)");
	seat.connection = _game_connection_network_server;
	seat.playing = FALSE;
	CHECK(!network_host_controls_shown_for(&seat), "the host in the lobby");
	/* (and the world's) */
	world_reset();
	CHECK(network_host_controls_available(), "the host, in its game");
	world.connection = _game_connection_network_client;
	CHECK(!network_host_controls_available(), "a client");
	CHECK(!network_host_controls_player_actions(test_player_index(OPENH1_PLAYER)), "a client may do nothing");
	CHECK(!network_host_controls_kick(test_player_index(OPENH1_PLAYER), FALSE) && world.kicked == NONE,
		"a client kicks no one");
	world_reset();
	world.connection = _game_connection_local;
	CHECK(!network_host_controls_available(), "a local game");
	world_reset();
	world.menu_loaded = TRUE;
	CHECK(!network_host_controls_available(), "the menus");
	CHECK(!network_host_controls_open(NONE, 0) && !world.screen_loaded, "not opened for one who has none");
}

/* what each player may have done to them */
static void case_actions(void)
{
	struct host_controls_player_view view = { FALSE, FALSE, FALSE, TRUE, TRUE };
	unsigned long actions;

	actions = network_host_controls_actions_for(&view);
	CHECK(TEST_FLAG(actions, _host_controls_kick) && TEST_FLAG(actions, _host_controls_ban) &&
		TEST_FLAG(actions, _host_controls_mute) && TEST_FLAG(actions, _host_controls_move_team), "another's player");
	view.own = TRUE;
	actions = network_host_controls_actions_for(&view);
	CHECK(!TEST_FLAG(actions, _host_controls_kick) && !TEST_FLAG(actions, _host_controls_ban) &&
		!TEST_FLAG(actions, _host_controls_mute), "the host's own players are never kicked, banned or muted");
	CHECK(TEST_FLAG(actions, _host_controls_move_team), "the host moves its own players");
	view.own = FALSE;
	view.bot = TRUE;
	actions = network_host_controls_actions_for(&view);
	CHECK(!TEST_FLAG(actions, _host_controls_kick) && !TEST_FLAG(actions, _host_controls_ban) &&
		!TEST_FLAG(actions, _host_controls_mute), "a bot the bots keep: nothing but a move");
	view.bot_removable = TRUE;
	actions = network_host_controls_actions_for(&view);
	CHECK(TEST_FLAG(actions, _host_controls_kick) && !TEST_FLAG(actions, _host_controls_ban), "a bot taken out");
	view.bot = view.bot_removable = FALSE;
	view.team_moves = FALSE;
	CHECK(!TEST_FLAG(network_host_controls_actions_for(&view), _host_controls_move_team), "no move where none may");
	view.team_moves = TRUE;
	view.teams = FALSE;
	CHECK(!TEST_FLAG(network_host_controls_actions_for(&view), _host_controls_move_team), "no move without teams");

	/* (in the world: the host's own, a client's, OpenCE's, a bot) */
	world_reset();
	actions = network_host_controls_player_actions(test_player_index(HOST_PLAYER));
	CHECK(!TEST_FLAG(actions, _host_controls_kick), "the host's own player");
	actions = network_host_controls_player_actions(test_player_index(OPENH1_PLAYER));
	CHECK(TEST_FLAG(actions, _host_controls_kick) && TEST_FLAG(actions, _host_controls_ban) &&
		TEST_FLAG(actions, _host_controls_move_team), "an OpenH1 machine's player");
	actions = network_host_controls_player_actions(test_player_index(OPENCE_PLAYER));
	CHECK(TEST_FLAG(actions, _host_controls_kick) && TEST_FLAG(actions, _host_controls_ban) &&
		TEST_FLAG(actions, _host_controls_mute), "an OpenCE machine's player is kicked as any");
	actions = network_host_controls_player_actions(test_player_index(BOT_PLAYER));
	CHECK(!TEST_FLAG(actions, _host_controls_kick) && !TEST_FLAG(actions, _host_controls_ban), "a bot");
	CHECK(!network_host_controls_kick(test_player_index(BOT_PLAYER), FALSE) && world.kicked == NONE,
		"a bot's machine, the host's, never kicked");
}

/* who may move teams: an OpenH1 game of teams without OpenCE's machines */
static void case_team_moves(void)
{
	struct host_controls_team_view view = { TRUE, TRUE, 0 };

	CHECK(network_host_controls_team_moves_for(&view), "an OpenH1 game of OpenH1 machines");
	view.opence_machines = 1;
	CHECK(!network_host_controls_team_moves_for(&view), "an OpenCE machine would not hear of it");
	view.opence_machines = 0;
	view.standard = FALSE;
	CHECK(!network_host_controls_team_moves_for(&view), "a Legacy game: nothing new is sent");
	view.standard = TRUE;
	view.teams = FALSE;
	CHECK(!network_host_controls_team_moves_for(&view), "no teams");
	world_reset();
	world.opence_machine = TRUE;
	CHECK(!network_host_controls_team_moves(), "the world's OpenCE machine");
	CHECK(!network_host_controls_move_team(test_player_index(OPENH1_PLAYER)) && world.sent_count == 0,
		"no move sent with an OpenCE machine in the game");
	world_reset();
	world.standard = FALSE;
	CHECK(!network_host_controls_move_team(test_player_index(OPENH1_PLAYER)) && world.sent_count == 0,
		"no move sent in a Legacy game");
}

/* Kick and Ban: the console's own (network_votekick_host_kick) */
static void case_kick(void)
{
	world_reset();
	CHECK(network_host_controls_kick(test_player_index(OPENH1_PLAYER), FALSE), "kicked");
	CHECK(world.kicked == OPENH1_PLAYER && !world.kick_ban, "by the console's kick, of the player's index");
	CHECK(network_host_controls_kick(test_player_index(OPENCE_PLAYER), TRUE), "banned");
	CHECK(world.kicked == OPENCE_PLAYER && world.kick_ban, "by the console's ban");
	world.kicked = NONE;
	CHECK(!network_host_controls_kick(test_player_index(HOST_PLAYER), FALSE) && world.kicked == NONE,
		"never the host's own");
	CHECK(!network_host_controls_kick(test_player_index(HOST_PLAYER), TRUE) && world.kicked == NONE,
		"never banned, the host's own");
}

/* a move: the unit killed, the team set on the host's records, each OpenH1
machine told; a client takes it from the host's stream alone */
static void case_move(void)
{
	struct player_datum *player;
	struct
	{
		struct distributed_message_header header;
		byte move[4];
	} message;
	/* (the header and one entry, as the host's) */
	word size = (word)(sizeof(struct distributed_message_header) + sizeof(struct distributed_host_team));

	world_reset();
	player = player_try_and_get(test_player_index(OPENH1_PLAYER));
	CHECK(player->team_index == 1, "starts blue");
	CHECK(network_host_controls_move_team(test_player_index(OPENH1_PLAYER)), "moved");
	CHECK(player->team_index == 0 && player->network_player_data.team_index == 0, "on red");
	CHECK(server_game.players[OPENH1_PLAYER].team_index == 0, "in the host's record");
	CHECK(world.killed_unit == 100 + OPENH1_PLAYER, "its living unit killed without statistics");
	CHECK(world.sent_count == 1 && world.sent_machines[0] == OPENH1_MACHINE, "the OpenH1 machine told");
	CHECK(world.sent[0].format == 1 && world.sent[0].player_index == OPENH1_PLAYER && world.sent[0].team == 0,
		"the player and team told");
	/* (a dead player is not killed again) */
	world.living[OPENH1_PLAYER] = FALSE;
	world.killed_unit = NONE;
	CHECK(network_host_controls_move_team(test_player_index(OPENH1_PLAYER)) && player->team_index == 1 &&
		world.killed_unit == NONE, "a dead player moved, no unit killed");
	/* (a client: the host's move from its stream) */
	world_reset();
	world.connection = _game_connection_network_client;
	player = player_try_and_get(test_player_index(HOST_PLAYER));
	memset(&message, 0, sizeof(message));
	message.header.type = _distributed_message_host_team;
	message.header.count = 1;
	message.move[0] = 1;
	message.move[1] = HOST_PLAYER;
	message.move[2] = 1;
	CHECK(network_host_controls_handles_message((word const *)&message, size), "its message");
	network_host_controls_handle_message(OPENH1_MACHINE, (word const *)&message, size, TRUE);
	CHECK(player->team_index == 0, "not from a client");
	network_host_controls_handle_message(NONE, (word const *)&message, size, FALSE);
	CHECK(player->team_index == 0, "not from a datagram");
	message.move[0] = 2;
	network_host_controls_handle_message(NONE, (word const *)&message, size, TRUE);
	CHECK(player->team_index == 0, "not of another format");
	message.move[0] = 1;
	message.move[2] = 2;
	network_host_controls_handle_message(NONE, (word const *)&message, size, TRUE);
	CHECK(player->team_index == 0, "not to a team of none");
	message.move[2] = 1;
	world.standard = FALSE;
	network_host_controls_handle_message(NONE, (word const *)&message, size, TRUE);
	CHECK(player->team_index == 0, "not in a Legacy game");
	world.standard = TRUE;
	network_host_controls_handle_message(NONE, (word const *)&message, size, TRUE);
	CHECK(player->team_index == 1 && client_game.players[HOST_PLAYER].team_index == 1, "the host's, taken");
	CHECK(world.killed_unit == NONE, "a client kills nothing: the host's kill reaches it");
}

/* Lock teams: a client's lobby change kept, the host machine's taken */
static void case_lock(void)
{
	world_reset();
	CHECK(network_host_controls_lobby_team(FALSE, 1, 0) == 1, "unlocked: a player switches");
	network_host_controls_lock_teams(TRUE);
	CHECK(network_host_controls_teams_locked(), "locked");
	CHECK(network_host_controls_lobby_team(FALSE, 1, 0) == 0, "locked: a client's player stays");
	CHECK(network_host_controls_lobby_team(TRUE, 1, 0) == 1, "locked: the host's own switch");
	world.server = FALSE;
	network_host_controls_update();
	world.server = TRUE;
	CHECK(!network_host_controls_teams_locked(), "a new server starts unlocked");
}

/* End game now, and Restart round through the scores and the lobby */
static void case_end(void)
{
	short action;
	struct host_controls_restart_view view = { TRUE, FALSE, FALSE, FALSE };

	world_reset();
	CHECK(network_host_controls_end_game() && world.ended, "ended as its time limit ends it");
	CHECK(!network_host_controls_end_game(), "not twice");
	world_reset();
	world.connection = _game_connection_network_client;
	CHECK(!network_host_controls_end_game() && !world.ended, "never a client's");
	/* (the steps) */
	CHECK(network_host_controls_restart_step(_host_controls_restart_ending, &view, &action) ==
		_host_controls_restart_ending && action == _host_controls_restart_wait, "waits for the scores");
	view.postgame = TRUE;
	CHECK(network_host_controls_restart_step(_host_controls_restart_ending, &view, &action) ==
		_host_controls_restart_lobby && action == _host_controls_restart_to_lobby, "the scores: to the lobby");
	CHECK(network_host_controls_restart_step(_host_controls_restart_lobby, &view, &action) ==
		_host_controls_restart_lobby && action == _host_controls_restart_wait, "waits for the lobby");
	view.menu_loaded = view.pregame = TRUE;
	CHECK(network_host_controls_restart_step(_host_controls_restart_lobby, &view, &action) ==
		_host_controls_restart_none && action == _host_controls_restart_start, "the lobby: started");
	view.server = FALSE;
	CHECK(network_host_controls_restart_step(_host_controls_restart_ending, &view, &action) ==
		_host_controls_restart_none && action == _host_controls_restart_wait, "the server gone: nothing");
	/* (in the world) */
	world_reset();
	CHECK(network_host_controls_restart() && world.ended && network_host_controls_restarting(), "restarting");
	network_host_controls_update();
	CHECK(!world.reset, "not before the scores");
	world.postgame = TRUE;
	network_host_controls_update();
	CHECK(world.reset, "the scores' A, the host's");
	world.postgame = FALSE;
	world.playing = FALSE;
	world.menu_loaded = TRUE;
	network_host_controls_update();
	CHECK(world.lobby_shown && !world.countdown_paused && world.countdown > 0, "the lobby, its countdown going");
	CHECK(!network_host_controls_restarting(), "done");
}

/* Max players: the list's next either way, never fewer than are in it */
static void case_maximum(void)
{
	static short const values[] = { 2, 4, 8, 12, 16, 24, 32 };

	CHECK(network_host_controls_maximum_step(values, NUMBEROF(values), 16, 3, 1) == 24, "up");
	CHECK(network_host_controls_maximum_step(values, NUMBEROF(values), 16, 3, -1) == 12, "down");
	CHECK(network_host_controls_maximum_step(values, NUMBEROF(values), 8, 5, -1) == 8, "no fewer than are in it");
	CHECK(network_host_controls_maximum_step(values, NUMBEROF(values), 12, 5, -1) == 8, "down to those in it");
	CHECK(network_host_controls_maximum_step(values, NUMBEROF(values), 32, 3, 1) == 32, "no more than the most");
	CHECK(network_host_controls_maximum_step(values, NUMBEROF(values), 10, 3, 1) == 12, "from a value not listed");
}

/* bans.txt's lines: those that ban, their parts */
static void case_bans(void)
{
	struct host_controls_ban ban;
	struct host_controls_ban bans[8];
	char lines[8][HOST_CONTROLS_BAN_LINE_SIZE];

	CHECK(network_host_controls_ban_parse("2026-10-20 19:42:11\tip=203.0.113.24\thwid=9f1c\tdiscord_username=none\t"
		"discord_id=none\tplayers=Griefer, Griefer(2)\treason=banned by the host", &ban), "a ban");
	CHECK(!strcmp(ban.when, "2026-10-20 19:42:11") && !strcmp(ban.address, "203.0.113.24") &&
		!strcmp(ban.hardware_id, "9f1c") && !strcmp(ban.players, "Griefer, Griefer(2)") &&
		!strcmp(ban.reason, "banned by the host"), "its parts");
	CHECK(network_host_controls_ban_parse("ip=192.0.2.55", &ban) && !ban.when[0] && !strcmp(ban.address, "192.0.2.55"),
		"one written by hand");
	CHECK(!network_host_controls_ban_parse("# a note", &ban), "a line that bans no one");
	CHECK(!network_host_controls_ban_parse("2026-10-20\tip=\thwid=none\tplayers=x", &ban), "no address, no id");
	CHECK(network_host_controls_ban_parse("x\tskip=1\thwid=abcd\tplayers=y", &ban) && !ban.address[0] &&
		!strcmp(ban.hardware_id, "abcd"), "a hardware id alone");
	CHECK(network_host_controls_ban_parse("when\tvip=9.9.9.9\tip=1.2.3.4", &ban) && !strcmp(ban.address, "1.2.3.4"),
		"a part's whole key");
	world_reset();
	strcpy(world.ban_text[0], "# kept by hand");
	strcpy(world.ban_text[1], "2026-10-20 19:42:11\tip=203.0.113.24\tplayers=Griefer");
	strcpy(world.ban_text[2], "ip=192.0.2.55");
	CHECK(network_host_controls_bans(bans, lines, 8) == 2, "the lines that ban");
	CHECK(!strcmp(bans[0].players, "Griefer") && !strcmp(lines[0], world.ban_text[1]), "each with its line");
	CHECK(!strcmp(lines[1], "ip=192.0.2.55"), "in their order");
	world.server = FALSE;
	CHECK(network_host_controls_bans(bans, lines, 8) == 0 && !network_host_controls_unban(lines[0]),
		"none without a server");
}

/* the players listed, their tags */
static void case_players(void)
{
	struct host_controls_player players[8];
	short count;

	world_reset();
	player_try_and_get(test_player_index(BOT_PLAYER))->quit_out_of_game = FALSE;
	count = network_host_controls_players(players, 8);
	CHECK(count == NUMBER_OF_TEST_PLAYERS, "every player");
	CHECK(players[HOST_PLAYER].tag == _host_controls_tag_host && players[OPENH1_PLAYER].tag == _host_controls_tag_openh1 &&
		players[BOT_PLAYER].tag == _host_controls_tag_bot, "tagged");
	world.opence_machine = TRUE;
	count = network_host_controls_players(players, 8);
	CHECK(players[OPENCE_PLAYER].tag == _host_controls_tag_opence, "OpenCE's");
	world.standard = FALSE;
	count = network_host_controls_players(players, 8);
	CHECK(players[OPENH1_PLAYER].tag == _host_controls_tag_none && players[OPENCE_PLAYER].tag == _host_controls_tag_none,
		"a Legacy game's: not told apart");
	player_try_and_get(test_player_index(OPENCE_PLAYER))->quit_out_of_game = TRUE;
	CHECK(network_host_controls_players(players, 8) == NUMBER_OF_TEST_PLAYERS - 1, "not those who quit");
	CHECK(network_host_controls_open_command("players pa") && world.screen_loaded, "opened");
	{
		long player_index;
		short tab;

		CHECK(network_host_controls_take_opening(&player_index, &tab) && tab == 0 &&
			player_index == test_player_index(OPENH1_PLAYER), "on Players, the player named");
		CHECK(!network_host_controls_take_opening(&player_index, &tab), "taken once");
	}
	CHECK(!network_host_controls_open_command("players zz"), "no such player");
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	CASE("seats") case_seats();
	else CASE("actions") case_actions();
	else CASE("team_moves") case_team_moves();
	else CASE("kick") case_kick();
	else CASE("move") case_move();
	else CASE("lock") case_lock();
	else CASE("end") case_end();
	else CASE("maximum") case_maximum();
	else CASE("bans") case_bans();
	else CASE("players") case_players();
	else
	{
		fprintf(stderr, "no case %s\n", case_name);
		return 2;
	}
	printf("%s: passed\n", case_name);
	return 0;
}
