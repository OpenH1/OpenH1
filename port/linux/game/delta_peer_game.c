/*
DELTA_PEER_GAME.C

The game's Delta Peer (delta_peer_game.h; port/linux/NETCODE.md, "Delta
Peer"), after ChupathingyCE's port/linux/src/delta_peer_game.c (at commit
724968e, under GPL-3.0-or-later), with OpenH1's own hooks.

One session (port/linux/src/delta_peer.c) over a UDP socket of the game's
own Winsock layer (xnet.c), so that it goes where the game's traffic goes:
network.address binds it to the machine's loopback alias or interface, and
an invite's peers reach it through the invite tunnel (p2p.c learns the
socket's port when it is bound, as it does the game's, and a datagram to a
peer's virtual address goes onto the tunnel at once). A host listens on its
game's port + DELTA_PEER_PORT_OFFSET (5160 beside 5150); a client sends from
a port the system picks, to its host's game port + the offset.

It runs from the main loop's frame, beside the game, never inside a tick:
it reads which machines are in the game and the scoreboard reads what it
learnt, and nothing it hears reaches the game's state. Its random numbers
are the system's, never the game's seeds. network.delta_peer = false turns
it off: no socket, no tag, plain OpenCE behaviour.
*/

/* ---------- headers */

#include "cseries.h"
#include "bungie_net/network/transport.h"
#include "bungie_net/network/transport_endpoint_winsock.h"
#include "bungie_net/network/transport_address_constants.h"
#include "networking/network_game_globals.h"
#include "networking/network_messages.h"
#include "networking/network_game_protocol.h"
#include "networking/network_client_manager.h"
#include "networking/network_connection.h"
#include "halo_port_limits.h"
#include "delta_peer_game.h"
#include "../src/delta_peer.h"

#include <stdio.h>
#include <string.h>

/* port_config.c's, network_server_manager.c's, cseries_windows.c's,
the platform's (posix.h), server_mode.c's */
int config_boolean(const char *name);
unsigned long network_game_server_machine_address(long machine_index);
unsigned long system_milliseconds(void);
void posix_random_bytes(void *buffer, unsigned int size);
const char *server_mode_build(void);
int halo_server_mode(void);

/* (the tag fits where OpenH1 writes nothing: its bytes are past every
field of OpenCE's and OpenH1's, halo_port_limits.h) */
typedef char delta_peer_tag_clear_of_openh1[HALO_PORT_ADVERTISED_BOTS_OFFSET < DELTA_ADVERTISED_TAG_OFFSET ? 1 : -1];
/* (the wire's number is a byte) */
typedef char delta_peer_wire_number_fits[HALO_PORT_NETWORK_VERSION >= 1 && HALO_PORT_NETWORK_VERSION <= 255 ? 1 : -1];

enum
{
	_role_none = 0,
	_role_client,
	_role_host
};

static struct
{
	boolean ready;
	boolean enabled;
	struct delta_peer peer;
	short role;
	SOCKET socket;
	/* the socket's port (host order; 0: none open) */
	word port;
	/* a host whose port was taken said so once */
	boolean port_refused;
	/* the advertisement of the game this client joins says Delta */
	boolean host_speaks_delta;
} delta_game;

/* ---------- the session's needs */

static void game_send(
	void *context,
	delta_u32 ipv4,
	unsigned short port,
	unsigned char const *data,
	int size)
{
	struct sockaddr_in address;

	(void)context;
	if (!delta_game.port)
		return;
	csmemset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(ipv4);
	/* (lost as a datagram may be: the handshake says HELLO again) */
	sendto(delta_game.socket, (char const *)data, size, 0, (struct sockaddr *)&address, sizeof(address));
}

static void game_log(
	void *context,
	char const *text)
{
	(void)context;
	network_event("%s", text);
}

static delta_u32 game_random(
	void *context)
{
	delta_u32 value = 0;

	(void)context;
	posix_random_bytes(&value, sizeof(value));
	return value;
}

static void delta_peer_game_ready(
	void)
{
	struct delta_peer_env env;
	struct delta_peer_local local;
	char const *build = server_mode_build();

	if (delta_game.ready)
		return;
	delta_game.ready = TRUE;
	delta_game.socket = INVALID_SOCKET;
	delta_game.enabled = config_boolean("network.delta_peer") != 0;
	if (!delta_game.enabled)
		network_event("Delta Peer: off (network.delta_peer = false): the legacy protocol alone");
	csmemset(&env, 0, sizeof(env));
	env.send_datagram = game_send;
	env.log_line = game_log;
	env.random_number = game_random;
	csmemset(&local, 0, sizeof(local));
	local.capabilities = DELTA_PEER_OPENH1_CAPABILITIES;
	local.legacy_version = HALO_PORT_NETWORK_VERSION;
	local.key.platform = DELTA_OPENH1_PLATFORM;
	local.key.version = DELTA_PLATFORM_KEY_VERSION;
	local.key.host_players = DELTA_OPENH1_HOST_PLAYERS;
	local.key.join_players = DELTA_OPENH1_JOIN_PLAYERS;
	local.key.join_players_opt_in = DELTA_OPENH1_JOIN_PLAYERS;
	local.key.memory_class = DELTA_OPENH1_MEMORY_CLASS;
	if (halo_server_mode())
		local.key.flags |= DELTA_PLATFORM_KEY_DEDICATED;
	/* (the client and its build: "OpenH1 build-1234", a local build's
	"OpenH1 local") */
	snprintf(local.build, sizeof(local.build), "OpenH1 %s", strcmp(build, "build-0") ? build : "local");
	delta_peer_initialize(&delta_game.peer, &env, &local);
}

/* ---------- the socket */

static void close_socket(
	void)
{
	if (delta_game.socket != INVALID_SOCKET)
		closesocket(delta_game.socket);
	delta_game.socket = INVALID_SOCKET;
	delta_game.port = 0;
}

/* the socket open on the port (0: any); TRUE if it is */
static boolean open_socket(
	word port)
{
	struct sockaddr_in address;
	int length = sizeof(address);
	unsigned long nonblocking = 1;

	if (delta_game.socket != INVALID_SOCKET && (!port || delta_game.port == port))
		return TRUE;
	close_socket();
	delta_game.socket = socket(AF_INET, SOCK_DGRAM, 0);
	if (delta_game.socket == INVALID_SOCKET)
		return FALSE;
	csmemset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = INADDR_ANY;
	/* (bound through xnet.c: network.address, and the tunnel told the port) */
	if (ioctlsocket(delta_game.socket, FIONBIO, &nonblocking) != 0 ||
		bind(delta_game.socket, (struct sockaddr *)&address, sizeof(address)) != 0 ||
		getsockname(delta_game.socket, (struct sockaddr *)&address, &length) != 0)
	{
		close_socket();
		return FALSE;
	}
	delta_game.port = ntohs(address.sin_port);
	return delta_game.port != 0;
}

static void receive(
	void)
{
	unsigned char data[DELTA_WIRE_MAXIMUM_DATAGRAM + 1];
	long count;

	for (count = 0; count < DELTA_PEER_FRAME_DATAGRAMS && delta_game.socket != INVALID_SOCKET; count++)
	{
		struct sockaddr_in from;
		int length = sizeof(from);
		int size = recvfrom(delta_game.socket, (char *)data, (int)sizeof(data), 0, (struct sockaddr *)&from,
			&length);

		if (size < 0)
			break;
		if (length < (int)sizeof(from) || from.sin_family != AF_INET)
			continue;
		/* (one too big is dropped by the session: read as one byte more) */
		delta_peer_receive(&delta_game.peer, (delta_u32)system_milliseconds(), (delta_u32)ntohl(from.sin_addr.s_addr),
			ntohs(from.sin_port), data, size);
	}
}

static void stop(
	void)
{
	delta_peer_stop(&delta_game.peer);
	close_socket();
	delta_game.role = _role_none;
}

/* ---------- the host and the client */

/* this machine's Delta port as a host: its game's port + the offset (none
for a game port too high for one) */
static word host_port(
	unsigned short game_port)
{
	return (word)(game_port <= 0xFFFF - DELTA_PEER_PORT_OFFSET ? game_port + DELTA_PEER_PORT_OFFSET : 0);
}

static void host_frame(
	void)
{
	static struct delta_peer_game_machine machines[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
	short local_machine = network_game_client_get_local_machine_index();
	word port = host_port(NETWORK_GAME_SERVER_PORT);
	long count = 0;
	long index;

	if (delta_game.role != _role_host)
	{
		stop();
		delta_game.role = _role_host;
		delta_game.port_refused = FALSE;
	}
	if (!port || !open_socket(port))
	{
		/* (another copy on this address has it: this game is OpenCE's alone,
		and its advertisement says so) */
		if (!delta_game.port_refused)
			network_event("Delta Peer: cannot listen on port %d (taken?): this game is hosted without Delta", port);
		delta_game.port_refused = TRUE;
		return;
	}
	if (delta_game.port_refused)
		network_event("Delta Peer: listening on port %d", port);
	else if (!delta_game.peer.roster_sent)
		network_event("Delta Peer: hosting with Delta on port %d", port);
	delta_game.port_refused = FALSE;
	/* (the game's machines: the host's own, and each joined client's at its
	address, as the game's sockets see it) */
	for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_MACHINES; index++)
	{
		unsigned long address = network_game_server_machine_address(index);

		if (index == local_machine)
		{
			machines[count].machine_index = (unsigned char)index;
			machines[count].local = 1;
			machines[count].ipv4 = 0;
			count++;
		}
		else if (address)
		{
			machines[count].machine_index = (unsigned char)index;
			machines[count].local = 0;
			machines[count].ipv4 = (delta_u32)address;
			count++;
		}
	}
	receive();
	delta_peer_host_frame(&delta_game.peer, (delta_u32)system_milliseconds(), machines, (int)count);
}

static void client_frame(
	void)
{
	struct network_game_client *client = global_network_game_client_get();
	struct network_connection *connection = client ? network_game_client_get_connection(client) : NULL;
	short machine_index = network_game_client_get_local_machine_index();
	struct transport_address reliable;
	struct transport_address unreliable;
	boolean joined = FALSE;
	boolean speaks_delta = delta_game.host_speaks_delta;

	csmemset(&reliable, 0, sizeof(reliable));
	if (connection && machine_index != NONE && network_connection_connected(connection))
	{
		network_connection_get_address(connection, &reliable, &unreliable);
		joined = reliable.address_length == IPV4_ADDRESS_LENGTH && reliable.address.long_words[0];
	}
	if (!joined)
	{
		if (delta_game.role == _role_client)
			stop();
		return;
	}
	if (delta_game.role != _role_client)
	{
		stop();
		delta_game.role = _role_client;
	}
	if (speaks_delta && (!host_port(reliable.port) || !open_socket(0)))
		speaks_delta = FALSE;
	receive();
	delta_peer_client_frame(&delta_game.peer, (delta_u32)system_milliseconds(), TRUE,
		(delta_u32)reliable.address.long_words[0], host_port(reliable.port), speaks_delta,
		(unsigned char)(machine_index >= 0 && machine_index < DELTA_PEER_MAXIMUM_MACHINES ? machine_index :
			DELTA_WIRE_NO_MACHINE));
}

void delta_peer_game_update(
	void)
{
	delta_peer_game_ready();
	if (!delta_game.enabled)
		return;
	/* (a host's own client is the host's machine: the host's session has
	it; a split screen game has no other machine) */
	if (global_network_game_server_get() && !network_game_is_splitscreen_local())
		host_frame();
	else if (global_network_game_client_get() && !global_network_game_server_get())
		client_frame();
	else if (delta_game.role != _role_none)
		stop();
}

/* ---------- the advertisement */

void delta_peer_game_advertise(
	byte *reserved,
	long size)
{
	word version;
	struct delta_wire_id wire;

	if (size < DELTA_ADVERTISED_RESERVED_SIZE)
		return;
	csmemset(reserved + DELTA_ADVERTISED_TAG_OFFSET, 0, DELTA_ADVERTISED_TAG_SIZE);
	/* (only a game OpenCE's machines may join, a Legacy or mixed game's:
	a ChupathingyCE client joins a tagged host by its wire, whatever its
	number, so a Standard game's would let it into a game it cannot play) */
	version = (word)(reserved[HALO_PORT_ADVERTISED_VERSION_OFFSET] |
		(reserved[HALO_PORT_ADVERTISED_VERSION_OFFSET + 1] << 8));
	if (!delta_game.enabled || delta_game.role != _role_host || !delta_game.port)
		return;
	if (version != HALO_PORT_NETWORK_VERSION)
		return;
	wire.number = HALO_PORT_NETWORK_VERSION;
	wire.letter = DELTA_OPENH1_WIRE_LETTER;
	delta_wire_write_tag(reserved, (int)size, DELTA_MAJOR, &wire, &wire);
}

boolean delta_peer_game_advertised(
	byte const *reserved,
	long size)
{
	delta_peer_game_ready();
	return delta_game.enabled && delta_wire_read_tag(reserved, (int)size, NULL, NULL, NULL);
}

void delta_peer_game_joining(
	boolean host_speaks_delta)
{
	delta_game.host_speaks_delta = host_speaks_delta;
}

/* ---------- the scoreboard */

boolean delta_peer_game_badge(
	long machine_index,
	wchar_t *text,
	long count)
{
	char badge[DELTA_PEER_BADGE_SIZE];
	long index;

	if (count <= 0)
		return FALSE;
	text[0] = 0;
	if (!delta_game.enabled || delta_game.role == _role_none ||
		!delta_peer_machine_badge(&delta_game.peer, (int)machine_index, badge, (int)sizeof(badge)))
	{
		return FALSE;
	}
	/* (printable ASCII already: delta_wire.c's) */
	for (index = 0; index < count - 1 && badge[index]; index++)
		text[index] = (wchar_t)(unsigned char)badge[index];
	text[index] = 0;
	return index > 0;
}
