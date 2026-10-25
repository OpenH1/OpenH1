/*
NETWORK_PARTY.C

Parties made in a network game (the party itself is port/linux/src/
p2p_party.c's): a player picked on the scoreboard (game_engine.c's player
menu) may be invited to this machine's party (one made for it, this machine
its leader, if it is in none), or, in a party of their own, asked to let
this machine into theirs. The player invited sees a line at the top of the
view, "Bob invites you to their party", with the key that joins it
(controls.party_accept, J; a controller's D-pad Down: the key or the button
as the player plays, input.button_prompts), for PARTY_PROMPT_MILLISECONDS;
let go, it fades. A player asked sees "Bob wants to join your party", whose
key sends Bob the invite, which Bob's machine then takes without asking (it
asked). Joining is a party link's (p2p_party_join_link): the same as opening
halo://party/<token>. The scoreboard's menu has the accept too.

The link goes only to the player it is for, through the game's connection
(port/linux/NETCODE.md), in messages of OpenH1's own (120 and after), of a
format of their own (PARTY_FORMAT: a machine takes its own alone):

- A client tells the host (_distributed_message_party_up, reliably, over
  its own stream) whether it can party (network.online) and is in a party
  (with the party's tag: p2p_party_tag, 32 bits of its topic, by which the
  machines tell who is in the same party; never the link), when that
  changes and every PARTY_RETELL_MILLISECONDS; its invite to a player (with the party's token) and its ask, as
  its player makes them.
- The host takes them only from the machine's own stream, from one of that
  machine's players, at most PARTY_BURST at once and one each
  PARTY_REFILL_MILLISECONDS after; it sends an invite or an ask on to the
  machine of the player it is for (_distributed_message_party_down,
  reliably), or takes it itself (its own player's). It sends it only to an
  OpenH1 machine of this game (network_standard_machine_is_openh1) that
  told it it can party, so never to an OpenCE machine (nor does
  network_standard_message_goes_to let any of OpenH1's messages go to one).
- The host tells the machines that told it they can party, when it changes,
  who can (_distributed_message_party_players, reliably): each such player,
  and whether they are in a party, and its tag. A player of an OpenCE
  machine, a bot, or a machine of an older OpenH1 (which drops these kinds
  as unknown) is never in it, so no machine offers them anything.

All of this is in OpenH1's games alone (network_standard_agreed_rate): in a
Legacy game a client cannot tell whether its host, or another player, is
OpenH1's. A player muted on this machine (network_voice_machine_muted) is
not heard: their invites are let go.

The pregame lobby and the carnage report have no player menu yet: when they
do, network_party_player_actions and network_party_player_act are theirs too.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/players.h"
#include "bitmaps/bitmap_color_conversion.h"
#include "interface/hud.h"
#include "interface/hud_messaging.h"
#include "interface/ui_widget.h"
#include "main/console.h"
#include "cutscene/cinematics.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/unicode.h"
#include "network_distributed.h"
#include "network_standard.h"
#include "network_voice.h"
#include "network_chat.h"
#include "network_party.h"
#include "halo_keyboard.h"
#include "bots.h"

#include <stdio.h>

/* the platform layer's, port_config.c's */
unsigned long system_milliseconds(void);
int config_boolean(const char *name);
/* p2p_party.c's (port/linux/src/p2p.h) */
int p2p_party_invite_link(char *link, int size);
int p2p_party_join_link(const char *text);
int p2p_party_parse_link(const char *text, unsigned char *token);
unsigned long p2p_party_tag(void);
/* network_chat.c's: its on-screen keyboard is up */
unsigned char network_chat_keyboard_open(void);
/* hud_key_prompts.c's */
boolean hud_key_prompt_draw_key(char const *name, rectangle2d *bounds, rectangle2d *cursor, pixel32 color);

/* ---------- constants */

/* a party's link: this, then its token in hexadecimal (p2p.h's) */
#define PARTY_LINK_PREFIX "halo://party/"

enum
{
	/* the messages' format: a machine takes its own alone */
	PARTY_FORMAT = 1,
	/* a party's token (p2p.h's P2P_TOKEN_SIZE) and its link, with its end */
	PARTY_TOKEN_SIZE = 16,
	PARTY_LINK_SIZE = sizeof(PARTY_LINK_PREFIX) + 2 * PARTY_TOKEN_SIZE,
	PARTY_NAME_SIZE = 12,
	MAXIMUM_PARTY_MACHINES = HALO_PORT_MAXIMUM_NETWORK_MACHINES,

	/* what a client's word to the host is, and the host's sent on */
	_party_request_status = 0,
	_party_request_invite,
	_party_request_ask,
	NUMBER_OF_PARTY_REQUESTS,

	/* a status's flags (and the host's list's): the machine can party
	(network.online), is in a party */
	_party_status_online_bit = 0,
	_party_status_in_party_bit,
	/* an invite's: the answer to the player's ask */
	_party_invite_answer_bit = 0,

	/* milliseconds: an invite shown, and taken; an ask's answer taken
	without asking; what was done shown; this machine's party read */
	PARTY_PROMPT_MILLISECONDS = 30000,
	PARTY_ASK_MILLISECONDS = 60000,
	PARTY_NOTICE_MILLISECONDS = 5000,
	PARTY_FADE_MILLISECONDS = 1000,
	PARTY_STATUS_MILLISECONDS = 500,
	/* a client's status told again this often (the host lets a machine's
	go while it is not in the game yet) */
	PARTY_RETELL_MILLISECONDS = 5000,
	/* the host: a machine's invites and asks at once, and the time it gains
	another in */
	PARTY_BURST = 3,
	PARTY_REFILL_MILLISECONDS = 5000,
	/* the host: its list made again this often */
	PARTY_LIST_MILLISECONDS = 500,
};

/* ---------- structures */

/* a client's word to the host (_distributed_message_party_up), and an
invite or ask the host sends on (_distributed_message_party_down): the
format, what it is, the player it is from (up: one of the sender's own),
the one it is for (NO_PLAYER for a status), the flags, a status's tag (the
party's, network byte order) and an invite's token */
struct distributed_party_request
{
	byte format;
	byte kind;
	byte player_index;
	byte target_index;
	byte flags;
	byte reserved[3];
	byte tag[4];
	byte token[PARTY_TOKEN_SIZE];
};

struct distributed_party_request_message
{
	struct distributed_message_header header;
	struct distributed_party_request request;
};

/* the host's list (_distributed_message_party_players): a player who can
party, an entry each */
struct distributed_party_player
{
	byte format;
	byte player_index;
	byte flags;
	byte reserved;
	byte tag[4];
};

struct distributed_party_players_message
{
	struct distributed_message_header header;
	struct distributed_party_player players[MAXIMUM_TRACKED_PLAYERS];
};

typedef char distributed_party_request_size_assert[sizeof(struct distributed_party_request) == 28 ? 1 : -1];
typedef char distributed_party_player_size_assert[sizeof(struct distributed_party_player) == 8 ? 1 : -1];

/* a player's, as the host says (or, on the host, as their machine told) */
struct party_player
{
	byte flags;
	unsigned long tag;
};

/* ---------- globals */

static struct
{
	/* in an OpenH1 network game at the last update */
	boolean active;
	/* this machine: can party, is in one (_party_status_*), its tag; when
	read; what was told the host (a client), and whether it was */
	byte own_flags;
	unsigned long own_tag;
	unsigned long status_time;
	boolean told;
	byte told_flags;
	unsigned long told_tag;
	unsigned long told_time;
	/* the host: each machine's word (by its index in the game), its
	invites' count, and whether it has the list */
	struct
	{
		boolean heard;
		byte flags;
		unsigned long tag;
		short burst;
		unsigned long refill_time;
		boolean listed;
	} machines[MAXIMUM_PARTY_MACHINES];
	unsigned long list_time;
	struct distributed_party_player sent[MAXIMUM_TRACKED_PLAYERS];
	short sent_count;
	boolean ever_sent;
	/* every machine: each player's (by absolute index) */
	struct party_player players[MAXIMUM_TRACKED_PLAYERS];
	/* the invite (or ask) to answer: what it is (NONE: none), from whom
	(a datum index), their name, the party's token, when */
	struct
	{
		short kind;
		long player_index;
		wchar_t name[PARTY_NAME_SIZE + 1];
		byte token[PARTY_TOKEN_SIZE];
		unsigned long time;
	} prompt;
	/* the player this machine asked to join (a datum index; NONE), and when */
	long asked_player_index;
	unsigned long asked_time;
	/* what was done, shown a while */
	wchar_t notice[80];
	unsigned long notice_time;
	boolean notice_shown;
	/* the accept's key and button, held at the last update */
	boolean key_held;
	boolean button_held;
	/* a message of another format heard (noted once) */
	boolean other_format_noted;
} party_globals;

/* ---------- private code */

/* in an OpenH1 game (a Standard one, mixed or not) on the network */
static boolean party_game(
	void)
{
	short connection = game_connection();

	return (connection == _game_connection_network_client || connection == _game_connection_network_server) &&
		network_standard_agreed_rate() != 0;
}

static boolean party_host(
	void)
{
	return game_connection() == _game_connection_network_server;
}

static void party_put_tag(
	byte *bytes,
	unsigned long tag)
{
	bytes[0] = (byte)(tag >> 24);
	bytes[1] = (byte)(tag >> 16);
	bytes[2] = (byte)(tag >> 8);
	bytes[3] = (byte)tag;
}

static unsigned long party_get_tag(
	byte const *bytes)
{
	return (unsigned long)bytes[0] << 24 | (unsigned long)bytes[1] << 16 | (unsigned long)bytes[2] << 8 | bytes[3];
}

static boolean party_token_empty(
	byte const *token)
{
	short index;

	for (index = 0; index < PARTY_TOKEN_SIZE; index++)
	{
		if (token[index])
			return FALSE;
	}
	return TRUE;
}

/* a player's name, ended (a player's has none at its 12 characters) */
static void party_player_name(
	struct player_datum const *player,
	wchar_t *name)
{
	short index;

	for (index = 0; index < PARTY_NAME_SIZE && player && player->name[index]; index++)
		name[index] = player->name[index];
	name[index] = 0;
}

/* ... in ASCII, for the log */
static void party_name_ascii(
	wchar_t const *name,
	char *text,
	short size)
{
	short index;

	for (index = 0; index < size - 1 && name[index]; index++)
		text[index] = name[index] >= 0x20 && name[index] < 0x7F ? (char)name[index] : '?';
	text[index] = 0;
}

static void party_notice(
	wchar_t const *format,
	wchar_t const *name)
{
	char text[80];

	usnprintf(party_globals.notice, NUMBEROF(party_globals.notice), format, name);
	party_globals.notice[NUMBEROF(party_globals.notice) - 1] = 0;
	party_globals.notice_time = system_milliseconds();
	party_globals.notice_shown = TRUE;
	party_name_ascii(party_globals.notice, text, sizeof(text));
	error(_error_log, "party: %s", text);
}

/* everything of a game let go */
static void party_reset(
	void)
{
	csmemset(&party_globals, 0, sizeof(party_globals));
	party_globals.prompt.kind = NONE;
	party_globals.prompt.player_index = NONE;
	party_globals.asked_player_index = NONE;
}

/* what a player (a datum index) of another machine may be offered, from
what this machine knows of them: none outside an OpenH1 game, for one of
its own players, a bot, an OpenCE machine's player, or a player whose
machine has not said it can party; nor while this machine cannot (or they
are in its party already) */
static unsigned long party_actions(
	boolean game,
	boolean online,
	struct player_datum const *player,
	boolean opence,
	struct party_player const *known,
	unsigned long own_tag,
	boolean prompted)
{
	unsigned long actions = 0;

	if (!game || !online || !player || player->local_player_index != NONE || opence ||
		bots_machine_is_bot(player->network_player_data.machine_index) ||
		!TEST_FLAG(known->flags, _party_status_online_bit))
	{
		return 0;
	}
	if (own_tag && TEST_FLAG(known->flags, _party_status_in_party_bit) && known->tag == own_tag)
		return 0;
	SET_FLAG(actions, _network_party_action_invite, TRUE);
	if (TEST_FLAG(known->flags, _party_status_in_party_bit))
		SET_FLAG(actions, _network_party_action_join, TRUE);
	if (prompted)
		SET_FLAG(actions, _network_party_action_accept, TRUE);
	return actions;
}

/* the client machine (the host's) of a player (absolute index), NONE for
none (or the host's own) */
static long party_player_machine(
	short player_index)
{
	long machine_indices[MAXIMUM_PARTY_MACHINES];
	short count = distributed_client_machines(machine_indices, MAXIMUM_PARTY_MACHINES);
	short index;

	for (index = 0; index < count; index++)
	{
		if (machine_indices[index] >= 0 && machine_indices[index] < MAXIMUM_PARTY_MACHINES &&
			distributed_machine_has_player(machine_indices[index], player_index))
		{
			return machine_indices[index];
		}
	}
	return NONE;
}

/* (the host) whether a machine is sent the party's messages: an OpenH1
machine of this game that told it it can party */
static boolean party_machine_told(
	long machine_index)
{
	return machine_index >= 0 && machine_index < MAXIMUM_PARTY_MACHINES &&
		party_globals.machines[machine_index].heard && network_standard_machine_is_openh1(machine_index);
}

static void party_join_token(
	byte const *token,
	wchar_t const *name)
{
	char link[PARTY_LINK_SIZE];
	short index;

	csstrcpy(link, PARTY_LINK_PREFIX);
	for (index = 0; index < PARTY_TOKEN_SIZE; index++)
		snprintf(link + sizeof(PARTY_LINK_PREFIX) - 1 + 2 * index, 3, "%02x", token[index]);
	if (p2p_party_join_link(link))
		party_notice(L"Joining %s's party", name);
}

/* an invite or ask for this machine's player, from the host (or the host's
own) */
static void party_heard(
	struct distributed_party_request const *request)
{
	long from = distributed_player_from_byte(request->player_index);
	long to = distributed_player_from_byte(request->target_index);
	struct player_datum *sender = from != NONE ? player_try_and_get(from) : NULL;
	unsigned long now = system_milliseconds();
	wchar_t name[PARTY_NAME_SIZE + 1];

	if (request->format != PARTY_FORMAT ||
		(request->kind != _party_request_invite && request->kind != _party_request_ask) ||
		!sender || sender->local_player_index != NONE || to == NONE || !distributed_player_is_local(to) ||
		!TEST_FLAG(party_globals.own_flags, _party_status_online_bit) ||
		network_voice_machine_muted(sender->network_player_data.machine_index))
	{
		return;
	}
	if (request->kind == _party_request_invite && party_token_empty(request->token))
		return;
	party_player_name(sender, name);
	/* (the answer to this machine's ask: joined, as it asked) */
	if (request->kind == _party_request_invite && TEST_FLAG(request->flags, _party_invite_answer_bit) &&
		party_globals.asked_player_index == from && now - party_globals.asked_time < PARTY_ASK_MILLISECONDS)
	{
		party_globals.asked_player_index = NONE;
		party_join_token(request->token, name);
		return;
	}
	party_globals.prompt.kind = request->kind;
	party_globals.prompt.player_index = from;
	csmemcpy(party_globals.prompt.name, name, sizeof(name));
	csmemcpy(party_globals.prompt.token, request->token, PARTY_TOKEN_SIZE);
	party_globals.prompt.time = now;
	party_globals.notice_shown = FALSE;
	{
		char text[PARTY_NAME_SIZE + 1];

		party_name_ascii(name, text, sizeof(text));
		error(_error_log, "party: %s %s", text, request->kind == _party_request_invite ? "invites this machine" :
			"asks to join this machine's party");
	}
}

/* (the host) an invite or ask sent on: to the machine of the player it is
for, or taken here (the host's own player's) */
static void party_host_route(
	struct distributed_party_request const *request)
{
	struct distributed_party_request_message message;
	short target = request->target_index;
	struct player_datum *player = target != NO_PLAYER ? distributed_player(target) : NULL;
	long machine_index;

	if (!player || target == request->player_index)
		return;
	if (player->local_player_index != NONE)
	{
		party_heard(request);
		return;
	}
	machine_index = party_player_machine(target);
	if (!party_machine_told(machine_index))
		return;
	csmemset(&message, 0, sizeof(message));
	message.request = *request;
	distributed_send_to_machine_reliably(machine_index, &message, _distributed_message_party_down, 1,
		sizeof(message));
}

/* a request of this machine's: to the host, or (the host) sent on */
static void party_send(
	struct distributed_party_request const *request)
{
	struct distributed_party_request_message message;

	if (party_host())
	{
		party_host_route(request);
		return;
	}
	csmemset(&message, 0, sizeof(message));
	message.request = *request;
	distributed_send(&message, _distributed_message_party_up, 1, sizeof(message), _distributed_to_host_reliably);
}

/* the first local player (absolute index), NO_PLAYER for none */
static byte party_own_player(
	void)
{
	short local_player_index = local_player_get_next(NONE);

	return local_player_index != NONE ?
		distributed_player_to_byte(local_player_get_player_index(local_player_index)) : NO_PLAYER;
}

/* this machine's invite to a player (a datum index): its party's (one made
for it if none); answer: to their ask */
static boolean party_invite(
	long player_index,
	boolean answer)
{
	struct distributed_party_request request;
	char link[PARTY_LINK_SIZE + 16];

	csmemset(&request, 0, sizeof(request));
	if (!p2p_party_invite_link(link, sizeof(link)) || !p2p_party_parse_link(link, request.token))
		return FALSE;
	request.format = PARTY_FORMAT;
	request.kind = _party_request_invite;
	request.player_index = party_own_player();
	request.target_index = distributed_player_to_byte(player_index);
	request.flags = (byte)(answer ? FLAG(_party_invite_answer_bit) : 0);
	if (request.player_index == NO_PLAYER || request.target_index == NO_PLAYER)
		return FALSE;
	party_send(&request);
	/* (in a party now: the host told at once) */
	party_globals.status_time = 0;
	return TRUE;
}

/* this machine's party, read; told the host (a client) when it changed */
static void party_status_update(
	unsigned long now)
{
	if (party_globals.status_time && now - party_globals.status_time < PARTY_STATUS_MILLISECONDS)
		return;
	party_globals.status_time = now | 1;
	party_globals.own_flags = 0;
	party_globals.own_tag = 0;
	if (config_boolean("network.online"))
	{
		SET_FLAG(party_globals.own_flags, _party_status_online_bit, TRUE);
		party_globals.own_tag = p2p_party_tag();
		SET_FLAG(party_globals.own_flags, _party_status_in_party_bit, party_globals.own_tag != 0);
	}
	if (!party_host() &&
		(!party_globals.told || party_globals.told_flags != party_globals.own_flags ||
			party_globals.told_tag != party_globals.own_tag ||
			now - party_globals.told_time >= PARTY_RETELL_MILLISECONDS))
	{
		struct distributed_party_request request;

		csmemset(&request, 0, sizeof(request));
		request.format = PARTY_FORMAT;
		request.kind = _party_request_status;
		request.player_index = party_own_player();
		request.target_index = NO_PLAYER;
		request.flags = party_globals.own_flags;
		party_put_tag(request.tag, party_globals.own_tag);
		party_send(&request);
		party_globals.told = TRUE;
		party_globals.told_flags = party_globals.own_flags;
		party_globals.told_tag = party_globals.own_tag;
		party_globals.told_time = now;
	}
}

/* (the host) who can party, made again; sent on to the machines that told
it they can, when it changed (and to a machine new to it) */
static void party_host_list(
	unsigned long now)
{
	struct distributed_party_players_message message;
	long machine_indices[MAXIMUM_PARTY_MACHINES];
	boolean present[MAXIMUM_PARTY_MACHINES];
	short machine_count;
	short count = 0;
	short player_index;
	short index;
	boolean changed;

	if (party_globals.list_time && now - party_globals.list_time < PARTY_LIST_MILLISECONDS)
		return;
	party_globals.list_time = now | 1;
	/* (a machine gone has its word let go: a machine new to its slot tells
	its own) */
	machine_count = distributed_client_machines(machine_indices, MAXIMUM_PARTY_MACHINES);
	csmemset(present, 0, sizeof(present));
	for (index = 0; index < machine_count; index++)
	{
		if (machine_indices[index] >= 0 && machine_indices[index] < MAXIMUM_PARTY_MACHINES)
			present[machine_indices[index]] = TRUE;
	}
	for (index = 0; index < MAXIMUM_PARTY_MACHINES; index++)
	{
		if (!present[index])
			csmemset(&party_globals.machines[index], 0, sizeof(party_globals.machines[index]));
	}
	csmemset(&message, 0, sizeof(message));
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		struct player_datum *player = distributed_player(player_index);
		struct party_player *known = &party_globals.players[player_index];
		long machine_index;

		known->flags = 0;
		known->tag = 0;
		if (!player || bots_machine_is_bot(player->network_player_data.machine_index))
			continue;
		if (player->local_player_index != NONE)
		{
			known->flags = party_globals.own_flags;
			known->tag = party_globals.own_tag;
		}
		else if (party_machine_told(machine_index = party_player_machine(player_index)))
		{
			known->flags = party_globals.machines[machine_index].flags;
			known->tag = party_globals.machines[machine_index].tag;
		}
		if (!TEST_FLAG(known->flags, _party_status_online_bit))
			continue;
		message.players[count].format = PARTY_FORMAT;
		message.players[count].player_index = (byte)player_index;
		message.players[count].flags = known->flags;
		party_put_tag(message.players[count].tag, known->tag);
		count++;
	}
	changed = !party_globals.ever_sent || count != party_globals.sent_count ||
		csmemcmp(message.players, party_globals.sent, count * sizeof(message.players[0]));
	csmemcpy(party_globals.sent, message.players, count * sizeof(message.players[0]));
	party_globals.sent_count = count;
	party_globals.ever_sent = TRUE;
	for (index = 0; index < machine_count; index++)
	{
		long machine_index = machine_indices[index];

		if (!party_machine_told(machine_index) || (!changed && party_globals.machines[machine_index].listed))
			continue;
		party_globals.machines[machine_index].listed = TRUE;
		distributed_send_to_machine_reliably(machine_index, &message, _distributed_message_party_players, count,
			(word)(sizeof(message.header) + count * sizeof(message.players[0])));
	}
}

/* (the host) a client's word: its status, or an invite or ask of one of
its players' */
static void party_host_heard(
	long machine_index,
	struct distributed_party_request const *request,
	unsigned long now)
{
	if (machine_index < 0 || machine_index >= MAXIMUM_PARTY_MACHINES ||
		!network_standard_machine_is_openh1(machine_index))
	{
		return;
	}
	if (request->kind == _party_request_status)
	{
		party_globals.machines[machine_index].heard = TRUE;
		party_globals.machines[machine_index].flags = request->flags;
		party_globals.machines[machine_index].tag = party_get_tag(request->tag);
		party_globals.list_time = 0;
		return;
	}
	if (request->player_index >= MAXIMUM_TRACKED_PLAYERS ||
		!distributed_machine_has_player(machine_index, request->player_index))
	{
		return;
	}
	/* (counted: PARTY_BURST at once, one more each PARTY_REFILL_MILLISECONDS) */
	{
		short *burst = &party_globals.machines[machine_index].burst;
		unsigned long *refill_time = &party_globals.machines[machine_index].refill_time;

		if (!*refill_time)
		{
			*burst = PARTY_BURST;
			*refill_time = now;
		}
		while (*burst < PARTY_BURST && now - *refill_time >= PARTY_REFILL_MILLISECONDS)
		{
			(*burst)++;
			*refill_time += PARTY_REFILL_MILLISECONDS;
		}
		if (*burst >= PARTY_BURST)
			*refill_time = now;
		if (*burst <= 0)
			return;
		(*burst)--;
	}
	party_host_route(request);
}

/* (a client) the host's list of who can party */
static void party_list_heard(
	struct distributed_party_player const *players,
	short count)
{
	short index;

	csmemset(party_globals.players, 0, sizeof(party_globals.players));
	for (index = 0; index < count; index++)
	{
		if (players[index].format != PARTY_FORMAT || players[index].player_index >= MAXIMUM_TRACKED_PLAYERS)
			continue;
		party_globals.players[players[index].player_index].flags = players[index].flags;
		party_globals.players[players[index].player_index].tag = party_get_tag(players[index].tag);
	}
}

/* the invite to answer, if it still is */
static boolean party_prompted(
	unsigned long now)
{
	if (party_globals.prompt.kind == NONE)
		return FALSE;
	if (now - party_globals.prompt.time >= PARTY_PROMPT_MILLISECONDS ||
		!player_try_and_get(party_globals.prompt.player_index))
	{
		party_globals.prompt.kind = NONE;
		party_globals.prompt.player_index = NONE;
		return FALSE;
	}
	return TRUE;
}

/* the accept's key (controls.party_accept) or the controller's D-pad Down
pressed, in the game (not while a line is typed, a menu or the console is
up, or the chat's on-screen keyboard is) */
static void party_keys_update(
	unsigned long now)
{
	boolean key = (halo_keyboard_actions(0) & FLAG(HALO_KEYBOARD_PARTY_ACCEPT)) != 0;
	boolean button = (halo_chat_pad_buttons() & FLAG(HALO_CHAT_PAD_DOWN)) != 0;
	boolean pressed = (key && !party_globals.key_held) || (button && !party_globals.button_held);

	party_globals.key_held = key;
	party_globals.button_held = button;
	if (pressed && party_prompted(now) && game_in_progress() && !ui_widgets_active() && !console_is_active() &&
		!network_chat_typing() && !network_chat_keyboard_open())
	{
		network_party_accept();
	}
}

/* ---------- public code */

void network_party_update(
	void)
{
	unsigned long now = system_milliseconds();

	if (!party_game())
	{
		if (party_globals.active)
			party_reset();
		return;
	}
	if (!party_globals.active)
	{
		party_reset();
		party_globals.active = TRUE;
	}
	party_status_update(now);
	if (party_host())
		party_host_list(now);
	party_keys_update(now);
}

unsigned long network_party_player_actions(
	long player_index)
{
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;
	short absolute_index = player ? (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index) : NONE;
	boolean prompted = party_prompted(system_milliseconds()) && party_globals.prompt.player_index == player_index;

	if (!player || absolute_index < 0 || absolute_index >= MAXIMUM_TRACKED_PLAYERS || !party_globals.active)
		return 0;
	return party_actions(party_game(), TEST_FLAG(party_globals.own_flags, _party_status_online_bit), player,
		network_standard_player_is_opence(player_index) ||
			(party_host() && network_standard_machine_is_opence(party_player_machine(absolute_index))),
		&party_globals.players[absolute_index], party_globals.own_tag, prompted);
}

void network_party_action_label(
	long player_index,
	short action,
	wchar_t *label,
	short size)
{
	wchar_t const *text = L"";

	switch (action)
	{
	case _network_party_action_invite:
		text = L"Invite to party";
		break;
	case _network_party_action_join:
		text = L"Join their party";
		break;
	case _network_party_action_accept:
		text = party_globals.prompt.kind == _party_request_ask ? L"Let into your party" : L"Accept party invite";
		break;
	}
	usnprintf(label, size, L"%s", text);
	label[size - 1] = 0;
}

void network_party_player_act(
	long player_index,
	short action)
{
	struct player_datum *player = player_try_and_get(player_index);
	wchar_t name[PARTY_NAME_SIZE + 1];

	if (!player || !TEST_FLAG(network_party_player_actions(player_index), action))
		return;
	party_player_name(player, name);
	switch (action)
	{
	case _network_party_action_invite:
		if (party_invite(player_index, FALSE))
			party_notice(L"Invited %s to your party", name);
		break;
	case _network_party_action_join:
	{
		struct distributed_party_request request;

		csmemset(&request, 0, sizeof(request));
		request.format = PARTY_FORMAT;
		request.kind = _party_request_ask;
		request.player_index = party_own_player();
		request.target_index = distributed_player_to_byte(player_index);
		if (request.player_index == NO_PLAYER || request.target_index == NO_PLAYER)
			break;
		party_send(&request);
		party_globals.asked_player_index = player_index;
		party_globals.asked_time = system_milliseconds();
		party_notice(L"Asked %s to let you into their party", name);
		break;
	}
	case _network_party_action_accept:
		network_party_accept();
		break;
	}
}

boolean network_party_accept(
	void)
{
	wchar_t name[PARTY_NAME_SIZE + 1];
	short kind;
	long player_index;

	if (!party_prompted(system_milliseconds()))
		return FALSE;
	kind = party_globals.prompt.kind;
	player_index = party_globals.prompt.player_index;
	csmemcpy(name, party_globals.prompt.name, sizeof(name));
	party_globals.prompt.kind = NONE;
	party_globals.prompt.player_index = NONE;
	if (kind == _party_request_invite)
	{
		party_join_token(party_globals.prompt.token, name);
		return TRUE;
	}
	if (party_invite(player_index, TRUE))
	{
		party_notice(L"Letting %s into your party", name);
		return TRUE;
	}
	return FALSE;
}

boolean network_party_handles_message(
	word const *message,
	word size)
{
	struct distributed_message_header header;

	if (size < sizeof(header))
		return FALSE;
	csmemcpy(&header, message, sizeof(header));
	return header.type == _distributed_message_party_up || header.type == _distributed_message_party_down ||
		header.type == _distributed_message_party_players;
}

void network_party_handle_message(
	long machine_index,
	word const *message,
	word size,
	boolean from_stream)
{
	struct distributed_message_header header;
	byte const *entries = (byte const *)message + sizeof(header);

	csmemcpy(&header, message, sizeof(header));
	if (!from_stream || !party_globals.active || !party_game())
		return;
	switch (header.type)
	{
	case _distributed_message_party_up:
	case _distributed_message_party_down:
	{
		struct distributed_party_request request;

		if (header.count != 1 || size != sizeof(header) + sizeof(request))
			return;
		csmemcpy(&request, entries, sizeof(request));
		if (request.format != PARTY_FORMAT || request.kind >= NUMBER_OF_PARTY_REQUESTS)
		{
			if (!party_globals.other_format_noted)
				error(_error_log, "party: a message of another format (%d), not taken", request.format);
			party_globals.other_format_noted = TRUE;
			return;
		}
		/* (up: the host's, from a client; down: a client's, from the host) */
		if (header.type == _distributed_message_party_up && party_host() && machine_index != NONE)
			party_host_heard(machine_index, &request, system_milliseconds());
		else if (header.type == _distributed_message_party_down && !party_host() && machine_index == NONE)
			party_heard(&request);
		break;
	}
	case _distributed_message_party_players:
		if (party_host() || machine_index != NONE || header.count > MAXIMUM_TRACKED_PLAYERS ||
			size != sizeof(header) + header.count * sizeof(struct distributed_party_player))
		{
			return;
		}
		{
			struct distributed_party_player players[MAXIMUM_TRACKED_PLAYERS];

			csmemcpy(players, entries, header.count * sizeof(players[0]));
			party_list_heard(players, header.count);
		}
		break;
	}
}

/* ---------- drawing */

/* how much of a line shows (fading out at its end) */
static real party_line_alpha(
	unsigned long elapsed,
	unsigned long shown)
{
	if (elapsed >= shown)
		return 0.0f;
	if (shown - elapsed < PARTY_FADE_MILLISECONDS)
		return (real)(shown - elapsed) / PARTY_FADE_MILLISECONDS;
	return 1.0f;
}

void network_party_draw(
	short local_player_index)
{
	long font_index = hud_get_font_index();
	unsigned long now = system_milliseconds();
	rectangle2d window = render.camera.window_bounds;
	rectangle2d bounds, cursor, measured;
	real_argb_color color;
	wchar_t text[96];
	wchar_t const *action = NULL;
	struct font_header *font;
	short line_height;
	short width;
	real alpha;
	boolean prompt;

	if (!party_globals.active || font_index == NONE || local_player_index != local_player_get_next(NONE) ||
		ui_widgets_active() || cinematic_in_progress())
	{
		return;
	}
	prompt = party_prompted(now);
	if (prompt)
	{
		alpha = party_line_alpha(now - party_globals.prompt.time, PARTY_PROMPT_MILLISECONDS);
		usnprintf(text, NUMBEROF(text), party_globals.prompt.kind == _party_request_ask ?
			L"%s wants to join your party   " : L"%s invites you to their party   ", party_globals.prompt.name);
		action = party_globals.prompt.kind == _party_request_ask ? L" Let in" : L" Join";
	}
	else if (party_globals.notice_shown)
	{
		alpha = party_line_alpha(now - party_globals.notice_time, PARTY_NOTICE_MILLISECONDS);
		usnprintf(text, NUMBEROF(text), L"%s", party_globals.notice);
	}
	else
	{
		return;
	}
	text[NUMBEROF(text) - 1] = 0;
	if (alpha <= 0.0f)
		return;
	offset_rectangle2d(&window, -render.camera.viewport_bounds.x0, -render.camera.viewport_bounds.y0);
	font = font_definition_get(font_index);
	line_height = (short)(font->ascending_height + font->descending_height + font->leading_height);
	if (line_height <= 0)
		return;
	color.alpha = alpha;
	color.red = 0.81f;
	color.green = 0.89f;
	color.blue = 1.0f;
	draw_string_set_draw_mode(font_index, NONE, 0 /* left */, 0, &color);
	/* (centered, a fifth of the way down the view: the text measured, and
	room for the key and its word) */
	measured = window;
	cursor = window;
	draw_string_set_indents(0, 0);
	draw_unicode_string_compute_bounds(&measured, text, &bounds, &cursor);
	width = (short)(bounds.x1 - bounds.x0 + (action ? 5 * line_height : 0));
	bounds.x0 = (short)MAX(window.x0, (window.x0 + window.x1 - width) / 2);
	bounds.x1 = window.x1;
	bounds.y0 = (short)(window.y0 + (window.y1 - window.y0) / 5);
	bounds.y1 = (short)(bounds.y0 + 2 * line_height);
	{
		rectangle2d panel;
		long panel_alpha = (long)(160 * alpha);

		panel.x0 = (short)(bounds.x0 - line_height / 2);
		panel.x1 = (short)MIN(window.x1, bounds.x0 + width + line_height / 2);
		panel.y0 = (short)(bounds.y0 - line_height / 4);
		panel.y1 = (short)(bounds.y0 + line_height + line_height / 4);
		draw_quad(&panel, ((pixel32)panel_alpha << 24) | 0x00061430);
	}
	if (!action)
	{
		rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, text);
		return;
	}
	/* (the key as the player plays: the keyboard's, else the controller's
	D-pad Down, input.button_prompts) */
	{
		char key[32];
		pixel32 key_color = real_argb_color_to_pixel32(&color);

		cursor = bounds;
		draw_string_set_indents(0, 0);
		draw_unicode_string_compute_bounds(&bounds, text, &measured, &cursor);
		cursor.x0 -= 3;
		measured.x0 = bounds.x0;
		rasterizer_draw_unicode_string(&measured, NULL, NULL, 0, text);
		if (halo_keyboard_action_name(HALO_KEYBOARD_PARTY_ACCEPT, key, sizeof(key)) &&
			hud_key_prompt_draw_key(key, &bounds, &cursor, key_color))
		{
			rectangle2d rest = bounds;

			draw_string_set_draw_mode(font_index, NONE, 0 /* left */, 0, &color);
			draw_string_set_indents((short)(cursor.x0 - bounds.x0), 0);
			draw_unicode_string_compute_bounds(&bounds, action, &rest, &cursor);
			rest.x0 = bounds.x0;
			rasterizer_draw_unicode_string(&rest, NULL, NULL, 0, action);
		}
		else
		{
			wchar_t button[32];
			rectangle2d rest = bounds;

			usnprintf(button, NUMBEROF(button), L"%%dpad-down%s", action);
			button[NUMBEROF(button) - 1] = 0;
			rest.x0 = cursor.x0;
			draw_string_set_draw_mode(font_index, NONE, 0 /* left */, 0, &color);
			draw_string_and_hack_in_icons(&rest, NULL, NULL, 0, button, FALSE);
		}
	}
	draw_string_set_indents(0, 0);
}
