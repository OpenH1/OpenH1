/*
DELTA_PEER.H

From ChupathingyCE (https://github.com/ChupathingyCE/chupathingyce,
port/linux/src/delta_peer.h at commit 724968e), under GPL-3.0-or-later.
OpenH1 keeps the handshake, the roster and what each side knows of the
game's machines; ChupathingyCE's profiles, legacy table relay, maps,
moderation, chat, team changes, traits, rules and platform limits are left
out. OpenH1's changes to it are marked "OpenH1:".

Delta Peer (ChupathingyCE's docs/delta.md): messages between the machines of
one game beside OpenCE's game protocol, which stays as it is.

- A host that speaks it listens on its game's port + DELTA_PEER_PORT_OFFSET
  and puts the Delta tag in its advertisement.
- A client that joined such a host (the game's join, unchanged) sends HELLO
  once the host has given it its machine; the host answers WELCOME, or
  LEGACY for another major. No answer within DELTA_PEER_HANDSHAKE_TIME
  means legacy only for the rest of that game: the game never waits.
- Each pair uses the capabilities both have; the host sends every Delta
  client a ROSTER of every machine's claims (its platform key) and of what
  the whole room shares.

Nothing here is trusted: a peer's claims are shown as claims (the
scoreboard's badge) and never reach the game's state. The sessions have no
sockets and no platform (struct delta_peer_env sends, logs and draws
random numbers), so tools/harness runs a host and its clients in one
process; port/linux/game/delta_peer_game.c runs one over the game's sockets.
*/

#ifndef HALO_DELTA_PEER_H
#define HALO_DELTA_PEER_H

#include "delta_wire.h"

enum
{
	/* a game's machines (HALO_PORT_MAXIMUM_NETWORK_MACHINES) */
	DELTA_PEER_MAXIMUM_MACHINES = 128,

	/* milliseconds: HELLO is sent again this often until answered, and the
	client gives up (legacy only) after the handshake time */
	DELTA_PEER_HELLO_INTERVAL = 500,
	DELTA_PEER_HANDSHAKE_TIME = 4000,
	/* the host sends its roster this often, and at once (no more often than
	the gap) when it changes */
	DELTA_PEER_ROSTER_INTERVAL = 5000,
	DELTA_PEER_ROSTER_GAP = 250,
	/* a client forgets a machine the roster has not named this long */
	DELTA_PEER_ROSTER_EXPIRY = 12000,
	/* a host takes a new session (a HELLO of a new session) from a machine
	at most this often */
	DELTA_PEER_NEW_SESSION_GAP = 1000,
	/* OpenH1: "takes no tables" (TABLE_HAVE) said again, to a machine that
	sends a table anyway, at most this often */
	DELTA_PEER_TABLE_HAVE_GAP = 1000,

	/* the messages a host takes from one machine, a second (and at once);
	a client from its host */
	DELTA_PEER_HOST_RATE = 10,
	DELTA_PEER_HOST_BURST = 20,
	DELTA_PEER_CLIENT_RATE = 50,
	DELTA_PEER_CLIENT_BURST = 100,
	/* the datagrams read in one frame */
	DELTA_PEER_FRAME_DATAGRAMS = 64,

	/* OpenH1: a badge's text, its end included (delta_peer_badge) */
	DELTA_PEER_BADGE_SIZE = 16
};

/* OpenH1: what OpenH1 offers: the platform alone (the scoreboard's badges).
server_badge is ChupathingyCE's draft, which it says is never set until it
is built; the others need its site, its accounts or game changes */
#define DELTA_PEER_OPENH1_CAPABILITIES ((delta_u32)1 << _delta_capability_platform)

/* a client's handshake */
enum delta_peer_client_state
{
	_delta_peer_client_off = 0,
	/* HELLO sent, no answer yet */
	_delta_peer_client_waiting,
	_delta_peer_client_delta,
	/* the host answered LEGACY, or nothing, or said BYE, or does not speak
	Delta: the legacy protocol alone until the client leaves the game */
	_delta_peer_client_legacy
};

/* what a session asks of the machine it runs on */
struct delta_peer_env
{
	void *context;
	/* a datagram to ipv4 (host byte order) and port (host order) */
	void (*send_datagram)(void *context, delta_u32 ipv4, unsigned short port, const unsigned char *data, int size);
	/* a line for the log (never an address) */
	void (*log_line)(void *context, const char *text);
	/* a random number (sessions): never the game's random seeds */
	delta_u32 (*random_number)(void *context);
};

/* this machine */
struct delta_peer_local
{
	delta_u32 capabilities;
	/* the OpenCE network version it plays */
	unsigned short legacy_version;
	struct delta_platform_key key;
	char build[DELTA_WIRE_BUILD_SIZE + 1];
};

/* a machine of the game, as the game says (the host's view) */
struct delta_peer_game_machine
{
	unsigned char machine_index;
	/* the host's own */
	unsigned char local;
	/* host byte order (0 for the host's own) */
	delta_u32 ipv4;
};

/* what is known of a machine: the host's from the handshakes, a client's
from the roster (and the host's own from WELCOME) */
struct delta_peer_machine
{
	int known;
	/* DELTA_ROSTER_* */
	unsigned char flags;
	delta_u32 capabilities;
	struct delta_platform_key key;
	/* OpenH1: its build as it said it (empty where it is not known: the
	roster does not carry it, so a client knows the host's alone) */
	char build[DELTA_WIRE_BUILD_SIZE + 1];
	delta_u32 seen_time;
};

/* the host's view of a client machine's session */
struct delta_peer_host_peer
{
	int used;
	delta_u32 ipv4;
	unsigned short port;
	delta_u32 session;
	delta_u32 session_time;
	delta_u32 agreed;
	struct delta_wire_hello hello;
	/* OpenH1: when it was last told this host takes no tables */
	delta_u32 table_have_time;
};

struct delta_peer
{
	struct delta_peer_env env;
	struct delta_peer_local local;
	/* 0 off, 1 client, 2 host */
	int mode;

	/* the host's */
	struct delta_peer_game_machine game_machines[DELTA_PEER_MAXIMUM_MACHINES];
	int game_machine_count;
	struct delta_peer_host_peer peers[DELTA_PEER_MAXIMUM_MACHINES];
	struct delta_rate rates[DELTA_PEER_MAXIMUM_MACHINES];
	int roster_dirty;
	delta_u32 roster_time;
	int roster_sent;

	/* the client's */
	int client_state;
	delta_u32 host_ipv4;
	unsigned short host_port;
	delta_u32 session;
	delta_u32 handshake_start;
	delta_u32 hello_time;
	unsigned char machine_index;
	struct delta_wire_welcome welcome;
	delta_u32 agreed;
	struct delta_rate host_rate;
	delta_u32 table_have_time;

	/* both: what is known of each machine, and what the whole room
	shares */
	struct delta_peer_machine machines[DELTA_PEER_MAXIMUM_MACHINES];
	delta_u32 room_capabilities;

	/* counted for the tests and the log */
	unsigned long dropped;
};

void delta_peer_initialize(struct delta_peer *peer, const struct delta_peer_env *env,
	const struct delta_peer_local *local);

/* each frame while hosting: the game's machines (the host's own among
them); answers HELLOs, forgets the sessions of machines gone, sends the
roster */
void delta_peer_host_frame(struct delta_peer *peer, delta_u32 now, const struct delta_peer_game_machine *machines,
	int count);
/* each frame while a client: joined (in a game), the host's address and
Delta port, whether its advertisement says it speaks Delta, this machine's
index in the game (DELTA_WIRE_NO_MACHINE until the host gives one) */
void delta_peer_client_frame(struct delta_peer *peer, delta_u32 now, int joined, delta_u32 host_ipv4,
	unsigned short host_port, int host_speaks_delta, unsigned char machine_index);
/* the game left, or Delta turned off: BYE said, everything forgotten */
void delta_peer_stop(struct delta_peer *peer);
/* a datagram that came to the Delta socket, from ipv4 (host byte order) and
port */
void delta_peer_receive(struct delta_peer *peer, delta_u32 now, delta_u32 ipv4, unsigned short port,
	const unsigned char *data, int size);

/* the capabilities two machines use: what both offer, of those this build
knows (the retired and reserved bits never) */
delta_u32 delta_peer_agree(delta_u32 theirs, delta_u32 ours);
/* what is known of a machine: 1 if anything */
int delta_peer_machine(const struct delta_peer *peer, int machine_index, struct delta_peer_machine *machine);
/* whether this machine's game is played over Delta Peer: a host with a
session, or a client its host welcomed */
int delta_peer_on_delta(const struct delta_peer *peer);

/* OpenH1: a machine's badge, the scoreboard's: its platform's name where it
shares one (a number OpenH1 knows, not unknown), else its build's first word
where its build is known ("ChupathingyCE", "OpenH1"); 1 and the text
(DELTA_PEER_BADGE_SIZE) if it has one */
int delta_peer_badge(const struct delta_peer_machine *machine, char *text, int size);
/* ... the platform's name alone (NULL for unknown, or one this build does
not know) */
const char *delta_peer_platform_name(int platform);
/* ... and a machine of the game's badge as the scoreboard shows it: one
known over Delta, whose badge is not this machine's own (a badge says how a
player differs from the viewer; in a game of OpenH1's alone none shows) */
int delta_peer_machine_badge(const struct delta_peer *peer, int machine_index, char *text, int size);

#endif
