/*
DELTA_PEER.C

From ChupathingyCE (https://github.com/ChupathingyCE/chupathingyce,
port/linux/src/delta_peer.c at commit 724968e), under GPL-3.0-or-later;
OpenH1 keeps the handshake and the roster (delta_peer.h), and its changes to
them are marked "OpenH1:".

Delta Peer's sessions: the host's handshakes and roster, the client's
handshake, and what each knows of the game's machines. No sockets and no
platform here: struct delta_peer_env sends, logs and draws random numbers.
*/

#include "delta_peer.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum
{
	_mode_off = 0,
	_mode_client,
	_mode_host
};

/* the capabilities this build knows (the retired md_maps and console_slots
bits never count, nor the reserved server_badge, nor the two announced on
Delta List alone) */
#define CAPABILITY(bit) ((delta_u32)1 << (bit))
#define KNOWN_CAPABILITIES \
	((CAPABILITY(NUMBER_OF_DELTA_CAPABILITIES) - 1) & ~CAPABILITY(_delta_capability_md_maps) & \
		~CAPABILITY(_delta_capability_console_slots) & ~CAPABILITY(_delta_capability_server_badge) & \
		~CAPABILITY(_delta_capability_ping) & ~CAPABILITY(_delta_capability_host_cheats))

static void say(struct delta_peer *peer, const char *format, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 2, 3)))
#endif
	;

static void say(struct delta_peer *peer, const char *format, ...)
{
	char text[256];
	va_list arguments;

	if (!peer->env.log_line)
		return;
	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	peer->env.log_line(peer->env.context, text);
}

/* (delta.h's registry, in its order: the log's names) */
static const char *const platform_names[] = {
	"unknown", "pc_windows", "pc_macos", "pc_linux", "android", "steam_deck", "xbox", "xbox360", "wiiu",
	"switch", "psp", "vita", "xbox_uwp", "pc_freebsd", "ps3", "ps4", "ps5", "n64", "pc_legacy",
	"steamlink", "ios", "tvos"
};
typedef char platform_names_check[sizeof(platform_names) / sizeof(platform_names[0]) == NUMBER_OF_DELTA_PLATFORMS ?
	1 : -1];

/* OpenH1: ... and the scoreboard's, for its badges (NULL: none shown) */
static const char *const platform_badges[] = {
	NULL, "Windows", "Mac", "Linux", "Android", "Steam Deck", "Xbox", "Xbox 360", "Wii U",
	"Switch", "PSP", "Vita", "Xbox", "FreeBSD", "PS3", "PS4", "PS5", "N64", "Legacy PC",
	"Steam Link", "iOS", "Apple TV"
};
typedef char platform_badges_check[sizeof(platform_badges) / sizeof(platform_badges[0]) ==
	NUMBER_OF_DELTA_PLATFORMS ? 1 : -1];

static const char *platform_name(int platform)
{
	return platform >= 0 && platform < NUMBER_OF_DELTA_PLATFORMS ? platform_names[platform] : "unknown";
}

static void peer_send(struct delta_peer *peer, delta_u32 ipv4, unsigned short port, const unsigned char *data, int size)
{
	if (size > 0 && peer->env.send_datagram)
		peer->env.send_datagram(peer->env.context, ipv4, port, data, size);
}

static int elapsed(delta_u32 now, delta_u32 then, delta_u32 time)
{
	return (delta_u32)(now - then) >= time;
}

static int valid_machine(int machine_index)
{
	return machine_index >= 0 && machine_index < DELTA_PEER_MAXIMUM_MACHINES;
}

delta_u32 delta_peer_agree(delta_u32 theirs, delta_u32 ours)
{
	return theirs & ours & KNOWN_CAPABILITIES;
}

/* what this machine offers */
static delta_u32 offered(const struct delta_peer *peer)
{
	return peer->local.capabilities & KNOWN_CAPABILITIES;
}

/* what this machine knows of itself, as a machine of the game */
static void local_machine(const struct delta_peer *peer, struct delta_peer_machine *machine, delta_u32 now)
{
	memset(machine, 0, sizeof(*machine));
	machine->known = 1;
	machine->flags = DELTA_ROSTER_DELTA | DELTA_ROSTER_PLATFORM;
	machine->capabilities = offered(peer);
	machine->key = peer->local.key;
	memcpy(machine->build, peer->local.build, sizeof(machine->build));
	machine->seen_time = now;
}

static void forget_all(struct delta_peer *peer)
{
	memset(peer->game_machines, 0, sizeof(peer->game_machines));
	peer->game_machine_count = 0;
	memset(peer->peers, 0, sizeof(peer->peers));
	memset(peer->rates, 0, sizeof(peer->rates));
	peer->roster_dirty = 0;
	peer->roster_sent = 0;
	peer->client_state = _delta_peer_client_off;
	peer->host_ipv4 = 0;
	peer->host_port = 0;
	peer->session = 0;
	peer->machine_index = DELTA_WIRE_NO_MACHINE;
	memset(&peer->welcome, 0, sizeof(peer->welcome));
	peer->agreed = 0;
	memset(&peer->host_rate, 0, sizeof(peer->host_rate));
	peer->table_have_time = 0;
	memset(peer->machines, 0, sizeof(peer->machines));
	peer->room_capabilities = 0;
}

void delta_peer_initialize(struct delta_peer *peer, const struct delta_peer_env *env,
	const struct delta_peer_local *local)
{
	memset(peer, 0, sizeof(*peer));
	peer->env = *env;
	peer->local = *local;
	/* (only what this build knows is offered) */
	peer->local.capabilities &= KNOWN_CAPABILITIES;
	peer->local.build[DELTA_WIRE_BUILD_SIZE] = 0;
	forget_all(peer);
}

void delta_peer_stop(struct delta_peer *peer)
{
	unsigned char data[DELTA_WIRE_HEADER_SIZE];
	int index;

	if (peer->mode == _mode_client &&
		(peer->client_state == _delta_peer_client_waiting || peer->client_state == _delta_peer_client_delta))
	{
		peer_send(peer, peer->host_ipv4, peer->host_port, data,
			delta_wire_write_empty(data, DELTA_MAJOR, _delta_message_bye, peer->session));
	}
	if (peer->mode == _mode_host)
	{
		for (index = 0; index < DELTA_PEER_MAXIMUM_MACHINES; index++)
		{
			if (peer->peers[index].used)
			{
				peer_send(peer, peer->peers[index].ipv4, peer->peers[index].port, data,
					delta_wire_write_empty(data, DELTA_MAJOR, _delta_message_bye, peer->peers[index].session));
			}
		}
	}
	forget_all(peer);
	peer->mode = _mode_off;
}

static void enter_mode(struct delta_peer *peer, int mode)
{
	if (peer->mode != mode)
	{
		if (peer->mode != _mode_off)
			delta_peer_stop(peer);
		peer->mode = mode;
	}
}

/* OpenH1: "takes no tables" (TABLE_HAVE), which ChupathingyCE's machines
honour, so that none sends this one its signed legacy table: said once a
session, and again (at most once a DELTA_PEER_TABLE_HAVE_GAP) when a table
comes anyway */
static void say_no_tables(struct delta_peer *peer, delta_u32 *said_time, delta_u32 now, delta_u32 ipv4,
	unsigned short port, delta_u32 session, int again)
{
	unsigned char data[DELTA_WIRE_HEADER_SIZE + DELTA_WIRE_TABLE_HAVE_SIZE];

	if (again && *said_time && !elapsed(now, *said_time, DELTA_PEER_TABLE_HAVE_GAP))
		return;
	*said_time = now ? now : 1;
	peer_send(peer, ipv4, port, data, delta_wire_write_table_have(data, session, DELTA_WIRE_TABLE_NONE));
}

/* ---------- the host */

static void forget_peer(struct delta_peer *peer, int machine_index, const char *why)
{
	if (peer->peers[machine_index].used)
	{
		say(peer, "Delta Peer: machine %d %s", machine_index, why);
		memset(&peer->peers[machine_index], 0, sizeof(peer->peers[machine_index]));
		peer->roster_dirty = 1;
	}
}

/* the game's machine (its index) a datagram from ipv4 can be: the one of
machine_index there, or the only one there (DELTA_WIRE_NO_MACHINE); -1 if
none */
static int host_find_machine(const struct delta_peer *peer, delta_u32 ipv4, int machine_index)
{
	int found = -1;
	int index;

	for (index = 0; index < peer->game_machine_count; index++)
	{
		const struct delta_peer_game_machine *machine = &peer->game_machines[index];

		if (machine->local || machine->ipv4 != ipv4)
			continue;
		if (machine_index != DELTA_WIRE_NO_MACHINE)
		{
			if (machine->machine_index == machine_index)
				return machine_index;
		}
		else
		{
			if (found >= 0)
				return -1;
			found = machine->machine_index;
		}
	}
	return found;
}

/* the peer of a session from ipv4; -1 if none */
static int host_find_session(const struct delta_peer *peer, delta_u32 ipv4, delta_u32 session)
{
	int index;

	for (index = 0; index < DELTA_PEER_MAXIMUM_MACHINES; index++)
	{
		const struct delta_peer_host_peer *client = &peer->peers[index];

		if (client->used && client->ipv4 == ipv4 && client->session == session)
			return index;
	}
	return -1;
}

static void host_send_welcome(struct delta_peer *peer, int machine_index)
{
	struct delta_peer_host_peer const *client = &peer->peers[machine_index];
	struct delta_wire_welcome welcome;
	unsigned char data[DELTA_WIRE_MAXIMUM_DATAGRAM];
	int index;

	memset(&welcome, 0, sizeof(welcome));
	welcome.capabilities = offered(peer);
	welcome.agreed = client->agreed;
	welcome.legacy_version = peer->local.legacy_version;
	welcome.host_machine_index = DELTA_WIRE_NO_MACHINE;
	for (index = 0; index < peer->game_machine_count; index++)
	{
		if (peer->game_machines[index].local)
			welcome.host_machine_index = peer->game_machines[index].machine_index;
	}
	/* (OpenH1 has no legacy table: the handshake's serial is 0, and
	TABLE_HAVE says it takes none) */
	welcome.legacy_table_serial = 0;
	welcome.key = peer->local.key;
	memcpy(welcome.build, peer->local.build, sizeof(welcome.build));
	peer_send(peer, client->ipv4, client->port, data, delta_wire_write_welcome(data, client->session, &welcome));
}

static void host_hello(struct delta_peer *peer, delta_u32 now, delta_u32 ipv4, unsigned short port,
	const struct delta_wire_header *header, const unsigned char *payload)
{
	struct delta_wire_hello hello;
	struct delta_peer_host_peer *client;
	int machine_index;

	if (!delta_wire_read_hello(payload, header->length, &hello) || !header->session)
	{
		peer->dropped++;
		return;
	}
	/* (only a machine of the game, at its own address, and by its index) */
	machine_index = host_find_machine(peer, ipv4, hello.machine_index);
	if (!valid_machine(machine_index))
	{
		peer->dropped++;
		return;
	}
	client = &peer->peers[machine_index];
	/* (WELCOME lost: said again) */
	if (client->used && client->session == header->session && client->ipv4 == ipv4)
	{
		client->port = port;
		host_send_welcome(peer, machine_index);
		return;
	}
	if (client->used && !elapsed(now, client->session_time, DELTA_PEER_NEW_SESSION_GAP))
	{
		peer->dropped++;
		return;
	}
	memset(client, 0, sizeof(*client));
	client->used = 1;
	client->ipv4 = ipv4;
	client->port = port;
	client->session = header->session;
	client->session_time = now;
	client->hello = hello;
	client->agreed = delta_peer_agree(hello.capabilities, offered(peer));
	say(peer, "Delta Peer: machine %d speaks Delta (build %s, %s, network version %u); capabilities 0x%x, agreed 0x%x",
		machine_index, hello.build[0] ? hello.build : "?", platform_name(hello.key.platform),
		(unsigned)hello.legacy_version, (unsigned)hello.capabilities, (unsigned)client->agreed);
	host_send_welcome(peer, machine_index);
	say_no_tables(peer, &client->table_have_time, now, client->ipv4, client->port, client->session, 0);
	peer->roster_dirty = 1;
}

static void host_receive(struct delta_peer *peer, delta_u32 now, delta_u32 ipv4, unsigned short port,
	const unsigned char *data, int size)
{
	struct delta_wire_header header;
	const unsigned char *payload = data + DELTA_WIRE_HEADER_SIZE;
	int first;
	int index;

	/* (only the game's machines are heard, each address at the rate of its
	first machine's) */
	first = -1;
	for (index = 0; index < peer->game_machine_count && first < 0; index++)
	{
		if (!peer->game_machines[index].local && peer->game_machines[index].ipv4 == ipv4)
			first = peer->game_machines[index].machine_index;
	}
	if (!valid_machine(first) || !delta_rate_take(&peer->rates[first], now, DELTA_PEER_HOST_RATE,
		DELTA_PEER_HOST_BURST))
	{
		peer->dropped++;
		return;
	}
	if (!delta_wire_read_header(data, size, &header))
	{
		peer->dropped++;
		return;
	}
	if (header.major != DELTA_MAJOR)
	{
		unsigned char reply[DELTA_WIRE_HEADER_SIZE];

		if (header.type == _delta_message_hello)
		{
			say(peer, "Delta Peer: a machine speaks Delta major %u (this host's is %d): legacy only",
				(unsigned)header.major, DELTA_MAJOR);
			peer_send(peer, ipv4, port, reply, delta_wire_write_empty(reply, DELTA_MAJOR, _delta_message_legacy,
				header.session));
		}
		return;
	}
	switch (header.type)
	{
	case _delta_message_hello:
		host_hello(peer, now, ipv4, port, &header, payload);
		break;
	case _delta_message_bye:
	{
		int machine_index = host_find_session(peer, ipv4, header.session);

		if (machine_index >= 0)
			forget_peer(peer, machine_index, "left Delta");
		break;
	}
	case _delta_message_table:
	{
		int machine_index = host_find_session(peer, ipv4, header.session);
		struct delta_peer_host_peer *client;

		if (machine_index < 0)
		{
			peer->dropped++;
			break;
		}
		client = &peer->peers[machine_index];
		say_no_tables(peer, &client->table_have_time, now, client->ipv4, client->port, client->session, 1);
		break;
	}
	default:
		/* (a type this build does not take: TABLE_HAVE, PROFILE, and those
		of capabilities it never agreed to) */
		break;
	}
}

/* a machine's roster entry, for a client that agreed to recipient_agreed */
static void host_roster_entry(const struct delta_peer *peer, int machine_index, delta_u32 recipient_agreed,
	struct delta_wire_roster_entry *entry)
{
	const struct delta_peer_machine *machine = &peer->machines[machine_index];

	memset(entry, 0, sizeof(*entry));
	entry->machine_index = (unsigned char)machine_index;
	if (!machine->known)
		return;
	entry->flags = DELTA_ROSTER_DELTA;
	entry->capabilities = machine->capabilities;
	if ((recipient_agreed & machine->capabilities & CAPABILITY(_delta_capability_platform)) &&
		(machine->flags & DELTA_ROSTER_PLATFORM))
	{
		entry->flags |= DELTA_ROSTER_PLATFORM;
		entry->key = machine->key;
	}
}

static void host_send_rosters(struct delta_peer *peer)
{
	static struct delta_wire_roster roster;
	unsigned char data[DELTA_WIRE_MAXIMUM_DATAGRAM];
	int client_index;

	for (client_index = 0; client_index < DELTA_PEER_MAXIMUM_MACHINES; client_index++)
	{
		const struct delta_peer_host_peer *client = &peer->peers[client_index];
		int index;

		if (!client->used)
			continue;
		memset(&roster, 0, sizeof(roster));
		roster.room_capabilities = peer->room_capabilities;
		/* (OpenH1 limits no game's players by its machines' platforms) */
		roster.room_players = DELTA_WIRE_NO_LIMIT;
		for (index = 0; index <= peer->game_machine_count; index++)
		{
			if (roster.count == DELTA_WIRE_MAXIMUM_ROSTER_ENTRIES ||
				(index == peer->game_machine_count && roster.count))
			{
				peer_send(peer, client->ipv4, client->port, data, delta_wire_write_roster(data, client->session, &roster));
				roster.count = 0;
			}
			if (index < peer->game_machine_count)
			{
				host_roster_entry(peer, peer->game_machines[index].machine_index, client->agreed,
					&roster.entries[roster.count++]);
			}
		}
	}
}

void delta_peer_host_frame(struct delta_peer *peer, delta_u32 now, const struct delta_peer_game_machine *machines,
	int count)
{
	delta_u32 room_capabilities;
	int index;

	enter_mode(peer, _mode_host);
	peer->game_machine_count = 0;
	for (index = 0; index < count && peer->game_machine_count < DELTA_PEER_MAXIMUM_MACHINES; index++)
	{
		if (valid_machine(machines[index].machine_index))
			peer->game_machines[peer->game_machine_count++] = machines[index];
	}

	/* (a machine gone from the game, or another now in its place, is
	forgotten) */
	for (index = 0; index < DELTA_PEER_MAXIMUM_MACHINES; index++)
	{
		if (peer->peers[index].used && host_find_machine(peer, peer->peers[index].ipv4, index) != index)
			forget_peer(peer, index, "left the game");
	}

	memset(peer->machines, 0, sizeof(peer->machines));
	room_capabilities = offered(peer);
	for (index = 0; index < peer->game_machine_count; index++)
	{
		int machine_index = peer->game_machines[index].machine_index;
		struct delta_peer_machine *machine = &peer->machines[machine_index];
		const struct delta_peer_host_peer *client = &peer->peers[machine_index];

		if (peer->game_machines[index].local)
			local_machine(peer, machine, now);
		else if (client->used)
		{
			machine->known = 1;
			machine->flags = DELTA_ROSTER_DELTA;
			machine->capabilities = client->agreed;
			machine->key = client->hello.key;
			if (machine->capabilities & CAPABILITY(_delta_capability_platform))
				machine->flags |= DELTA_ROSTER_PLATFORM;
			memcpy(machine->build, client->hello.build, sizeof(machine->build));
			machine->seen_time = now;
		}
		/* (a machine without Delta turns every room-wide capability off) */
		room_capabilities &= machine->known ? machine->capabilities : 0;
	}
	if (room_capabilities != peer->room_capabilities)
	{
		say(peer, "Delta Peer: every machine of the game shares capabilities 0x%x", (unsigned)room_capabilities);
		peer->room_capabilities = room_capabilities;
		peer->roster_dirty = 1;
	}

	if (!peer->roster_sent ||
		(peer->roster_dirty && elapsed(now, peer->roster_time, DELTA_PEER_ROSTER_GAP)) ||
		elapsed(now, peer->roster_time, DELTA_PEER_ROSTER_INTERVAL))
	{
		host_send_rosters(peer);
		peer->roster_sent = 1;
		peer->roster_time = now;
		peer->roster_dirty = 0;
	}
}

/* ---------- the client */

static void client_send_hello(struct delta_peer *peer, delta_u32 now)
{
	struct delta_wire_hello hello;
	unsigned char data[DELTA_WIRE_MAXIMUM_DATAGRAM];

	memset(&hello, 0, sizeof(hello));
	hello.capabilities = offered(peer);
	hello.legacy_version = peer->local.legacy_version;
	hello.machine_index = peer->machine_index;
	hello.legacy_table_serial = 0;
	hello.key = peer->local.key;
	memcpy(hello.build, peer->local.build, sizeof(hello.build));
	peer_send(peer, peer->host_ipv4, peer->host_port, data, delta_wire_write_hello(data, peer->session, &hello));
	peer->hello_time = now;
}

static void client_legacy(struct delta_peer *peer, const char *why)
{
	int own = peer->machine_index;
	struct delta_peer_machine local = peer->machines[valid_machine(own) ? own : 0];

	say(peer, "Delta Peer: %s: the legacy protocol alone with this host", why);
	peer->client_state = _delta_peer_client_legacy;
	peer->agreed = 0;
	peer->room_capabilities = 0;
	memset(peer->machines, 0, sizeof(peer->machines));
	if (valid_machine(own))
		peer->machines[own] = local;
}

void delta_peer_client_frame(struct delta_peer *peer, delta_u32 now, int joined, delta_u32 host_ipv4,
	unsigned short host_port, int host_speaks_delta, unsigned char machine_index)
{
	int index;

	if (!joined)
	{
		if (peer->mode == _mode_client)
			delta_peer_stop(peer);
		return;
	}
	enter_mode(peer, _mode_client);
	if (machine_index != peer->machine_index && valid_machine(peer->machine_index))
		memset(&peer->machines[peer->machine_index], 0, sizeof(peer->machines[0]));
	peer->machine_index = machine_index;
	/* (this machine knows itself, Delta or not) */
	if (valid_machine(machine_index))
		local_machine(peer, &peer->machines[machine_index], now);

	switch (peer->client_state)
	{
	case _delta_peer_client_off:
		if (!host_speaks_delta)
		{
			say(peer, "Delta Peer: the host does not speak Delta: the legacy protocol alone");
			peer->client_state = _delta_peer_client_legacy;
			break;
		}
		/* (HELLO says which machine this is: once the host has said) */
		if (!valid_machine(machine_index))
			break;
		peer->host_ipv4 = host_ipv4;
		peer->host_port = host_port;
		do
			peer->session = peer->env.random_number ? peer->env.random_number(peer->env.context) : 1;
		while (!peer->session);
		peer->handshake_start = now;
		peer->client_state = _delta_peer_client_waiting;
		say(peer, "Delta Peer: the host speaks Delta: HELLO sent");
		client_send_hello(peer, now);
		break;
	case _delta_peer_client_waiting:
		if (elapsed(now, peer->handshake_start, DELTA_PEER_HANDSHAKE_TIME))
			client_legacy(peer, "no answer to HELLO");
		else if (elapsed(now, peer->hello_time, DELTA_PEER_HELLO_INTERVAL))
			client_send_hello(peer, now);
		break;
	case _delta_peer_client_delta:
		/* (machines the roster stopped naming, but this one and the
		host's) */
		for (index = 0; index < DELTA_PEER_MAXIMUM_MACHINES; index++)
		{
			struct delta_peer_machine *machine = &peer->machines[index];

			if (!machine->known || index == machine_index || index == peer->welcome.host_machine_index)
				continue;
			if (elapsed(now, machine->seen_time, DELTA_PEER_ROSTER_EXPIRY))
				memset(machine, 0, sizeof(*machine));
		}
		break;
	}
}

static void client_receive(struct delta_peer *peer, delta_u32 now, delta_u32 ipv4, unsigned short port,
	const unsigned char *data, int size)
{
	struct delta_wire_header header;
	const unsigned char *payload = data + DELTA_WIRE_HEADER_SIZE;

	/* (its host alone, from its address and port, of its session) */
	if ((peer->client_state != _delta_peer_client_waiting && peer->client_state != _delta_peer_client_delta) ||
		ipv4 != peer->host_ipv4 || port != peer->host_port ||
		!delta_rate_take(&peer->host_rate, now, DELTA_PEER_CLIENT_RATE, DELTA_PEER_CLIENT_BURST) ||
		!delta_wire_read_header(data, size, &header) || header.session != peer->session)
	{
		peer->dropped++;
		return;
	}
	if (header.major != DELTA_MAJOR)
	{
		if (header.type == _delta_message_legacy)
		{
			char why[64];

			snprintf(why, sizeof(why), "the host speaks Delta major %u", (unsigned)header.major);
			client_legacy(peer, why);
		}
		else
			peer->dropped++;
		return;
	}
	switch (header.type)
	{
	case _delta_message_welcome:
	{
		struct delta_wire_welcome welcome;
		struct delta_peer_machine *host;

		if (!delta_wire_read_welcome(payload, header.length, &welcome))
		{
			peer->dropped++;
			break;
		}
		if (peer->client_state == _delta_peer_client_delta)
			break;
		peer->welcome = welcome;
		/* (never more than this machine offered, whatever the host says) */
		peer->agreed = delta_peer_agree(welcome.agreed & welcome.capabilities, offered(peer));
		peer->client_state = _delta_peer_client_delta;
		say(peer, "Delta Peer: the host speaks Delta (build %s, %s, network version %u); capabilities 0x%x, agreed 0x%x",
			welcome.build[0] ? welcome.build : "?", platform_name(welcome.key.platform),
			(unsigned)welcome.legacy_version, (unsigned)welcome.capabilities, (unsigned)peer->agreed);
		if (valid_machine(welcome.host_machine_index) && welcome.host_machine_index != peer->machine_index)
		{
			host = &peer->machines[welcome.host_machine_index];
			memset(host, 0, sizeof(*host));
			host->known = 1;
			host->flags = DELTA_ROSTER_DELTA;
			host->capabilities = welcome.capabilities;
			if (peer->agreed & CAPABILITY(_delta_capability_platform))
			{
				host->flags |= DELTA_ROSTER_PLATFORM;
				host->key = welcome.key;
			}
			memcpy(host->build, welcome.build, sizeof(host->build));
			host->seen_time = now;
		}
		say_no_tables(peer, &peer->table_have_time, now, peer->host_ipv4, peer->host_port, peer->session, 0);
		break;
	}
	case _delta_message_legacy:
		client_legacy(peer, "the host answered LEGACY");
		break;
	case _delta_message_roster:
	{
		static struct delta_wire_roster roster;
		int index;

		if (peer->client_state != _delta_peer_client_delta || !delta_wire_read_roster(payload, header.length, &roster))
		{
			peer->dropped++;
			break;
		}
		peer->room_capabilities = roster.room_capabilities & peer->agreed;
		for (index = 0; index < roster.count; index++)
		{
			struct delta_wire_roster_entry *entry = &roster.entries[index];
			struct delta_peer_machine *machine;
			char build[DELTA_WIRE_BUILD_SIZE + 1];

			if (!valid_machine(entry->machine_index) || entry->machine_index == peer->machine_index)
				continue;
			machine = &peer->machines[entry->machine_index];
			/* (what this machine did not agree to is not kept) */
			if (!(peer->agreed & CAPABILITY(_delta_capability_platform)))
				entry->flags &= ~DELTA_ROSTER_PLATFORM;
			/* (said when a machine's platform is first known, or changes) */
			if ((entry->flags & DELTA_ROSTER_PLATFORM) &&
				(!(machine->flags & DELTA_ROSTER_PLATFORM) || machine->key.platform != entry->key.platform))
			{
				say(peer, "Delta Peer: machine %d is %s", entry->machine_index, platform_name(entry->key.platform));
			}
			/* (the host's build, from WELCOME, is kept: the roster has
			none) */
			memcpy(build, machine->build, sizeof(build));
			memset(machine, 0, sizeof(*machine));
			if (!(entry->flags & DELTA_ROSTER_DELTA))
				continue;
			machine->known = 1;
			machine->flags = (unsigned char)(entry->flags & (DELTA_ROSTER_DELTA | DELTA_ROSTER_PLATFORM));
			machine->capabilities = entry->capabilities;
			if (entry->flags & DELTA_ROSTER_PLATFORM)
				machine->key = entry->key;
			if (entry->machine_index == peer->welcome.host_machine_index)
				memcpy(machine->build, build, sizeof(machine->build));
			machine->seen_time = now;
		}
		break;
	}
	case _delta_message_bye:
		client_legacy(peer, "the host stopped Delta");
		break;
	case _delta_message_table:
		if (peer->client_state == _delta_peer_client_delta)
			say_no_tables(peer, &peer->table_have_time, now, peer->host_ipv4, peer->host_port, peer->session, 1);
		break;
	default:
		/* (a type this build does not take) */
		break;
	}
}

void delta_peer_receive(struct delta_peer *peer, delta_u32 now, delta_u32 ipv4, unsigned short port,
	const unsigned char *data, int size)
{
	if (!data || size <= 0 || size > DELTA_WIRE_MAXIMUM_DATAGRAM)
	{
		peer->dropped++;
		return;
	}
	if (peer->mode == _mode_host)
		host_receive(peer, now, ipv4, port, data, size);
	else if (peer->mode == _mode_client)
		client_receive(peer, now, ipv4, port, data, size);
	else
		peer->dropped++;
}

/* ---------- what is known */

int delta_peer_machine(const struct delta_peer *peer, int machine_index, struct delta_peer_machine *machine)
{
	if (!valid_machine(machine_index) || !peer->machines[machine_index].known)
		return 0;
	if (machine)
		*machine = peer->machines[machine_index];
	return 1;
}

int delta_peer_on_delta(const struct delta_peer *peer)
{
	int index;

	if (peer->mode == _mode_client)
		return peer->client_state == _delta_peer_client_delta;
	if (peer->mode != _mode_host)
		return 0;
	for (index = 0; index < DELTA_PEER_MAXIMUM_MACHINES; index++)
	{
		if (peer->peers[index].used)
			return 1;
	}
	return 0;
}

const char *delta_peer_platform_name(int platform)
{
	return platform > 0 && platform < NUMBER_OF_DELTA_PLATFORMS ? platform_badges[platform] : NULL;
}

int delta_peer_badge(const struct delta_peer_machine *machine, char *text, int size)
{
	const char *name = NULL;
	int length = 0;

	if (size <= 0)
		return 0;
	text[0] = 0;
	if (!machine || !machine->known)
		return 0;
	if (machine->flags & DELTA_ROSTER_PLATFORM)
		name = delta_peer_platform_name(machine->key.platform);
	if (name)
	{
		while (name[length] && length < size - 1)
		{
			text[length] = name[length];
			length++;
		}
	}
	else
	{
		/* (the build's first word, its client's name: printable already,
		delta_wire.c's) */
		while (machine->build[length] && machine->build[length] != ' ' && length < size - 1 &&
			length < DELTA_WIRE_BUILD_SIZE)
		{
			text[length] = machine->build[length];
			length++;
		}
	}
	text[length] = 0;
	return length > 0;
}

int delta_peer_machine_badge(const struct delta_peer *peer, int machine_index, char *text, int size)
{
	struct delta_peer_machine own;
	char own_badge[DELTA_PEER_BADGE_SIZE];

	if (size > 0)
		text[0] = 0;
	if (!delta_peer_on_delta(peer) || !valid_machine(machine_index) ||
		!delta_peer_badge(&peer->machines[machine_index], text, size))
	{
		return 0;
	}
	local_machine(peer, &own, 0);
	if (delta_peer_badge(&own, own_badge, (int)sizeof(own_badge)) && !strcmp(own_badge, text))
	{
		text[0] = 0;
		return 0;
	}
	return 1;
}
