/*
DELTA_PEER.C (test)

Delta Peer's handshake (port/linux/src/delta_wire.c, delta_peer.c,
ChupathingyCE's cut to it): the wire format (the header, HELLO, WELCOME,
LEGACY, the roster, the advertisement's tag), the capabilities two machines
agree to, a host and its clients in one process (the handshake, the roster,
BYE), the fallback's timing when a host does not answer, who is heard, the
scoreboard's badges, and the host's advertisement (delta_peer_game.c's
delta_peer_game_advertise). test_delta_peer.py takes the files as they are;
the clock, the addresses and the datagrams between the machines are the
test's.
*/

#include "harness.h"

#include "delta_wire.inc"
#include "delta_peer.inc"

/* delta_peer_game.c's advertisement, with the game it reads */
typedef unsigned short word;
#define csmemset memset
#include "limits.inc"
enum
{
	_role_none = 0,
	_role_client,
	_role_host
};
static struct
{
	int enabled;
	short role;
	word port;
} delta_game;
#include "advertise.inc"

#define REQUIRE(condition) CHECK(condition, "%s", "")

enum
{
	HOST = 0x0A000001,
	CLIENT = 0x0A000002,
	OTHER = 0x0A000003,
	HOST_PORT = 5160,
	CLIENT_PORT = 40000,
	MAXIMUM_SENT = 4096
};

/* one machine of the test's network */
struct side
{
	struct delta_peer peer;
	delta_u32 ipv4;
	unsigned short port;
	int log_lines;
};

struct sent
{
	struct side *from;
	delta_u32 ipv4;
	unsigned short port;
	int size;
	unsigned char data[DELTA_WIRE_MAXIMUM_DATAGRAM];
};

static struct sent sent[MAXIMUM_SENT];
static int sent_count;
static delta_u32 now = 100000;
static delta_u32 random_state = 12345;

static void test_send(void *context, delta_u32 ipv4, unsigned short port, const unsigned char *data, int size)
{
	struct sent *entry;

	if (sent_count >= MAXIMUM_SENT || size > DELTA_WIRE_MAXIMUM_DATAGRAM)
		return;
	entry = &sent[sent_count++];
	entry->from = context;
	entry->ipv4 = ipv4;
	entry->port = port;
	entry->size = size;
	memcpy(entry->data, data, (size_t)size);
}

static void test_log(void *context, const char *text)
{
	((struct side *)context)->log_lines++;
	if (getenv("DELTA_TEST_LOG"))
		fprintf(stderr, "%s\n", text);
}

static delta_u32 test_random(void *context)
{
	(void)context;
	random_state = random_state * 1103515245u + 12345u;
	return random_state;
}

static void local_of(struct delta_peer_local *local, int platform, const char *build)
{
	memset(local, 0, sizeof(*local));
	local->capabilities = DELTA_PEER_OPENH1_CAPABILITIES;
	local->legacy_version = 41;
	local->key.platform = (unsigned char)platform;
	local->key.version = DELTA_PLATFORM_KEY_VERSION;
	local->key.host_players = 128;
	local->key.join_players = 128;
	local->key.join_players_opt_in = 128;
	strncpy(local->build, build, sizeof(local->build) - 1);
}

static void side_start(struct side *side, delta_u32 ipv4, unsigned short port, int platform, const char *build)
{
	struct delta_peer_env env;
	struct delta_peer_local local;

	memset(side, 0, sizeof(*side));
	side->ipv4 = ipv4;
	side->port = port;
	memset(&env, 0, sizeof(env));
	env.context = side;
	env.send_datagram = test_send;
	env.log_line = test_log;
	env.random_number = test_random;
	local_of(&local, platform, build);
	delta_peer_initialize(&side->peer, &env, &local);
}

/* the datagrams sent, each to the side at its address and port (as from its
sender's); those to no side are lost */
static void deliver(struct side **sides, int side_count)
{
	int index;

	for (index = 0; index < sent_count; index++)
	{
		int side;

		for (side = 0; side < side_count; side++)
		{
			if (sides[side]->ipv4 == sent[index].ipv4 && sides[side]->port == sent[index].port)
			{
				delta_peer_receive(&sides[side]->peer, now, sent[index].from->ipv4, sent[index].from->port,
					sent[index].data, sent[index].size);
			}
		}
	}
	sent_count = 0;
}

static int count_sent(int type)
{
	int count = 0;
	int index;

	for (index = 0; index < sent_count; index++)
	{
		if (sent[index].size >= DELTA_WIRE_HEADER_SIZE && sent[index].data[3] == type)
			count++;
	}
	return count;
}

/* the host's game: its own machine 0, the client's machine 1 at CLIENT */
static int game_of(struct delta_peer_game_machine *machines, int with_client)
{
	memset(machines, 0, 2 * sizeof(*machines));
	machines[0].machine_index = 0;
	machines[0].local = 1;
	machines[1].machine_index = 1;
	machines[1].ipv4 = CLIENT;
	return with_client ? 2 : 1;
}

/* frames of a host and its client, every 50 ms, for milliseconds */
static void play(struct side *host, struct side *client, int milliseconds, int client_speaks)
{
	struct delta_peer_game_machine machines[2];
	struct side *sides[2];
	int count = game_of(machines, 1);
	int time;

	sides[0] = host;
	sides[1] = client;
	for (time = 0; time < milliseconds; time += 50)
	{
		delta_peer_host_frame(&host->peer, now, machines, count);
		delta_peer_client_frame(&client->peer, now, 1, HOST, HOST_PORT, client_speaks, 1);
		deliver(sides, 2);
		now += 50;
	}
}

/* ---------- the format */

static void case_header(void)
{
	unsigned char data[DELTA_WIRE_MAXIMUM_DATAGRAM + 8];
	struct delta_wire_header header;
	int size;

	memset(data, 0, sizeof(data));
	size = delta_wire_write_empty(data, DELTA_MAJOR, _delta_message_bye, 0x11223344u);
	REQUIRE(size == DELTA_WIRE_HEADER_SIZE);
	REQUIRE(data[0] == 'D' && data[1] == 'P' && data[2] == 1 && data[3] == 5);
	REQUIRE(data[4] == 0 && data[5] == 0 && data[6] == 0 && data[7] == 0);
	REQUIRE(data[8] == 0x44 && data[9] == 0x33 && data[10] == 0x22 && data[11] == 0x11);
	REQUIRE(delta_wire_read_header(data, size, &header));
	REQUIRE(header.major == 1 && header.type == 5 && header.length == 0 && header.session == 0x11223344u);
	/* (the payload length is the datagram's, exactly) */
	REQUIRE(!delta_wire_read_header(data, size + 1, &header));
	delta_wire_write_header(data, DELTA_MAJOR, _delta_message_hello, 4, 1);
	REQUIRE(!delta_wire_read_header(data, DELTA_WIRE_HEADER_SIZE, &header));
	REQUIRE(delta_wire_read_header(data, DELTA_WIRE_HEADER_SIZE + 4, &header));
	/* (too short, too long, another magic) */
	REQUIRE(!delta_wire_read_header(data, DELTA_WIRE_HEADER_SIZE - 1, &header));
	delta_wire_write_header(data, DELTA_MAJOR, _delta_message_hello, DELTA_WIRE_MAXIMUM_PAYLOAD + 1, 1);
	REQUIRE(!delta_wire_read_header(data, DELTA_WIRE_MAXIMUM_DATAGRAM + 1, &header));
	delta_wire_write_header(data, DELTA_MAJOR, _delta_message_hello, 0, 1);
	data[1] = 'Q';
	REQUIRE(!delta_wire_read_header(data, DELTA_WIRE_HEADER_SIZE, &header));
	/* (another major is still read, to be answered) */
	delta_wire_write_header(data, 7, _delta_message_hello, 0, 1);
	REQUIRE(delta_wire_read_header(data, DELTA_WIRE_HEADER_SIZE, &header) && header.major == 7);
}

static void case_hello(void)
{
	unsigned char data[DELTA_WIRE_MAXIMUM_DATAGRAM + 8];
	struct delta_wire_hello hello, back;
	struct delta_wire_header header;
	int size;

	memset(&hello, 0, sizeof(hello));
	hello.capabilities = 0x00012345u;
	hello.legacy_version = 41;
	hello.machine_index = 3;
	hello.legacy_table_serial = 0x01020304u;
	hello.key.platform = 3;
	hello.key.version = 1;
	hello.key.flags = 2;
	hello.key.host_players = 16;
	hello.key.join_players = 16;
	hello.key.join_players_opt_in = 128;
	hello.key.memory_class = 12;
	strcpy(hello.build, "OpenH1 build-1234");
	size = delta_wire_write_hello(data, 0xCAFEu, &hello);
	REQUIRE(size == DELTA_WIRE_HEADER_SIZE + DELTA_WIRE_HELLO_SIZE + 17);
	/* (the spec's offsets) */
	REQUIRE(data[12] == 0x45 && data[13] == 0x23 && data[14] == 0x01 && data[15] == 0);
	REQUIRE(data[16] == 41 && data[17] == 0 && data[18] == 3 && data[19] == 17);
	REQUIRE(data[20] == 4 && data[23] == 1);
	REQUIRE(data[24] == 3 && data[25] == 1 && data[26] == 2 && data[27] == 16 && data[30] == 12 && data[31] == 0);
	REQUIRE(!memcmp(data + 32, "OpenH1 build-1234", 17));
	REQUIRE(delta_wire_read_header(data, size, &header) && header.type == _delta_message_hello);
	REQUIRE(delta_wire_read_hello(data + DELTA_WIRE_HEADER_SIZE, header.length, &back));
	REQUIRE(back.capabilities == hello.capabilities && back.legacy_version == 41 && back.machine_index == 3);
	REQUIRE(back.legacy_table_serial == hello.legacy_table_serial && !memcmp(&back.key, &hello.key, sizeof(back.key)));
	REQUIRE(!strcmp(back.build, hello.build));
	/* (a later version's fields after the build are read past) */
	REQUIRE(delta_wire_read_hello(data + DELTA_WIRE_HEADER_SIZE, header.length + 8, &back));
	REQUIRE(!strcmp(back.build, hello.build));
	/* (short of its fields, or of the build its length says) */
	REQUIRE(!delta_wire_read_hello(data + DELTA_WIRE_HEADER_SIZE, DELTA_WIRE_HELLO_SIZE - 1, &back));
	REQUIRE(!delta_wire_read_hello(data + DELTA_WIRE_HEADER_SIZE, header.length - 1, &back));
	/* (a build longer than its field is refused, never cut) */
	data[DELTA_WIRE_HEADER_SIZE + 7] = DELTA_WIRE_BUILD_SIZE + 1;
	memset(data + DELTA_WIRE_HEADER_SIZE + DELTA_WIRE_HELLO_SIZE, 'x', DELTA_WIRE_BUILD_SIZE + 1);
	REQUIRE(!delta_wire_read_hello(data + DELTA_WIRE_HEADER_SIZE, DELTA_WIRE_HELLO_SIZE + DELTA_WIRE_BUILD_SIZE + 1,
		&back));
	/* (text that is not printable ASCII reads as '?') */
	data[DELTA_WIRE_HEADER_SIZE + 7] = 3;
	memcpy(data + DELTA_WIRE_HEADER_SIZE + DELTA_WIRE_HELLO_SIZE, "a\nb", 3);
	REQUIRE(delta_wire_read_hello(data + DELTA_WIRE_HEADER_SIZE, DELTA_WIRE_HELLO_SIZE + 3, &back));
	REQUIRE(!strcmp(back.build, "a?b"));
}

static void case_welcome(void)
{
	unsigned char data[DELTA_WIRE_MAXIMUM_DATAGRAM + 8];
	struct delta_wire_welcome welcome, back;
	struct delta_wire_header header;
	int size;

	memset(&welcome, 0, sizeof(welcome));
	welcome.capabilities = 0x000FFFFFu;
	welcome.agreed = 1;
	welcome.legacy_version = 41;
	welcome.host_machine_index = DELTA_WIRE_NO_MACHINE;
	welcome.key.platform = 1;
	welcome.key.version = 1;
	strcpy(welcome.build, "ChupathingyCE 0.8.0h");
	size = delta_wire_write_welcome(data, 7, &welcome);
	REQUIRE(size == DELTA_WIRE_HEADER_SIZE + DELTA_WIRE_WELCOME_SIZE + 20);
	REQUIRE(data[3] == _delta_message_welcome && data[16] == 1 && data[22] == 0xFF && data[23] == 20);
	REQUIRE(data[28] == 1 && !memcmp(data + 36, "ChupathingyCE 0.8.0h", 20));
	REQUIRE(delta_wire_read_header(data, size, &header));
	REQUIRE(delta_wire_read_welcome(data + DELTA_WIRE_HEADER_SIZE, header.length, &back));
	REQUIRE(back.capabilities == welcome.capabilities && back.agreed == 1 && back.legacy_version == 41);
	REQUIRE(back.host_machine_index == 0xFF && back.key.platform == 1 && !strcmp(back.build, welcome.build));
	REQUIRE(!delta_wire_read_welcome(data + DELTA_WIRE_HEADER_SIZE, DELTA_WIRE_WELCOME_SIZE - 1, &back));
	REQUIRE(!delta_wire_read_welcome(data + DELTA_WIRE_HEADER_SIZE, header.length - 1, &back));
}

static void case_roster(void)
{
	static struct delta_wire_roster roster, back;
	unsigned char data[DELTA_WIRE_MAXIMUM_DATAGRAM + 64];
	struct delta_wire_header header;
	int size;
	int index;

	REQUIRE(DELTA_WIRE_MAXIMUM_ROSTER_ENTRIES == 32);
	memset(&roster, 0, sizeof(roster));
	roster.room_capabilities = 1;
	roster.room_players = 0xFF;
	roster.count = 32;
	for (index = 0; index < 32; index++)
	{
		roster.entries[index].machine_index = (unsigned char)index;
		roster.entries[index].flags = DELTA_ROSTER_DELTA | DELTA_ROSTER_PLATFORM;
		roster.entries[index].capabilities = 1;
		roster.entries[index].key.platform = (unsigned char)(index % NUMBER_OF_DELTA_PLATFORMS);
	}
	size = delta_wire_write_roster(data, 9, &roster);
	REQUIRE(size == DELTA_WIRE_HEADER_SIZE + DELTA_WIRE_ROSTER_SIZE + 32 * DELTA_WIRE_ROSTER_ENTRY_SIZE);
	REQUIRE(size <= DELTA_WIRE_MAXIMUM_DATAGRAM);
	REQUIRE(data[12] == 1 && data[16] == 0xFF && data[20] == 32);
	REQUIRE(data[24 + 36 * 5] == 5 && data[25 + 36 * 5] == 3 && data[32 + 36 * 5] == 5);
	REQUIRE(delta_wire_read_header(data, size, &header));
	REQUIRE(delta_wire_read_roster(data + DELTA_WIRE_HEADER_SIZE, header.length, &back));
	REQUIRE(back.count == 32 && back.room_capabilities == 1 && back.room_players == 0xFF);
	REQUIRE(back.entries[31].machine_index == 31 && back.entries[31].key.platform == 31 % NUMBER_OF_DELTA_PLATFORMS);
	/* (a count past the most, or past the bytes) */
	roster.count = 33;
	REQUIRE(!delta_wire_write_roster(data, 9, &roster));
	data[DELTA_WIRE_HEADER_SIZE + 8] = 33;
	REQUIRE(!delta_wire_read_roster(data + DELTA_WIRE_HEADER_SIZE, DELTA_WIRE_MAXIMUM_PAYLOAD, &back));
	data[DELTA_WIRE_HEADER_SIZE + 8] = 3;
	REQUIRE(!delta_wire_read_roster(data + DELTA_WIRE_HEADER_SIZE, DELTA_WIRE_ROSTER_SIZE + 2 * 36, &back));
	REQUIRE(delta_wire_read_roster(data + DELTA_WIRE_HEADER_SIZE, DELTA_WIRE_ROSTER_SIZE + 3 * 36, &back));
	REQUIRE(back.count == 3);
}

static void case_tag(void)
{
	unsigned char reserved[DELTA_ADVERTISED_RESERVED_SIZE];
	struct delta_wire_id own, oldest, read_own, read_oldest;
	int major = 0;
	int index;

	REQUIRE(DELTA_ADVERTISED_TAG_OFFSET == 17);
	for (index = 0; index < DELTA_ADVERTISED_RESERVED_SIZE; index++)
		reserved[index] = (unsigned char)(0x60 + index);
	own.number = 41;
	own.letter = 'a';
	oldest.number = 28;
	oldest.letter = 'a';
	delta_wire_write_tag(reserved, (int)sizeof(reserved), DELTA_MAJOR, &own, &oldest);
	/* (the bytes before it are OpenCE's and OpenH1's: left alone) */
	for (index = 0; index < DELTA_ADVERTISED_TAG_OFFSET; index++)
		REQUIRE(reserved[index] == 0x60 + index);
	REQUIRE(reserved[17] == 'D' && reserved[18] == 'L' && reserved[19] == 1 && reserved[20] == 1);
	REQUIRE(reserved[21] == 41 && reserved[22] == 'a' && reserved[23] == 28 && reserved[24] == 'a');
	/* (ChupathingyCE's check: 0xA5 xor the eight before) */
	REQUIRE(reserved[25] == (0xA5 ^ 'D' ^ 'L' ^ 1 ^ 1 ^ 41 ^ 'a' ^ 28 ^ 'a'));
	REQUIRE(delta_wire_read_tag(reserved, (int)sizeof(reserved), &major, &read_own, &read_oldest));
	REQUIRE(major == 1 && read_own.number == 41 && read_own.letter == 'a' && read_oldest.number == 28);
	/* (a wrong check, layout version, or letters; too few bytes) */
	reserved[25] ^= 1;
	REQUIRE(!delta_wire_read_tag(reserved, (int)sizeof(reserved), &major, NULL, NULL));
	reserved[25] ^= 1;
	REQUIRE(!delta_wire_read_tag(reserved, (int)sizeof(reserved) - 1, &major, NULL, NULL));
	reserved[20] = 2;
	reserved[25] = (unsigned char)(0xA5 ^ 'D' ^ 'L' ^ 1 ^ 2 ^ 41 ^ 'a' ^ 28 ^ 'a');
	REQUIRE(!delta_wire_read_tag(reserved, (int)sizeof(reserved), &major, NULL, NULL));
	/* (another major is read: it answers LEGACY) */
	reserved[19] = 2;
	reserved[20] = 1;
	reserved[25] = (unsigned char)(0xA5 ^ 'D' ^ 'L' ^ 2 ^ 1 ^ 41 ^ 'a' ^ 28 ^ 'a');
	REQUIRE(delta_wire_read_tag(reserved, (int)sizeof(reserved), &major, NULL, NULL) && major == 2);
	/* (an oldest past its own wire is no tag: none written, none read) */
	oldest.number = 42;
	delta_wire_write_tag(reserved, (int)sizeof(reserved), DELTA_MAJOR, &own, &oldest);
	for (index = DELTA_ADVERTISED_TAG_OFFSET; index < DELTA_ADVERTISED_RESERVED_SIZE; index++)
		REQUIRE(reserved[index] == 0);
	REQUIRE(!delta_wire_read_tag(reserved, (int)sizeof(reserved), &major, NULL, NULL));
	reserved[17] = 'D';
	reserved[18] = 'L';
	reserved[19] = 1;
	reserved[20] = 1;
	reserved[21] = 41;
	reserved[22] = 'a';
	reserved[23] = 42;
	reserved[24] = 'a';
	reserved[25] = (unsigned char)(0xA5 ^ 'D' ^ 'L' ^ 1 ^ 1 ^ 41 ^ 'a' ^ 42 ^ 'a');
	REQUIRE(!delta_wire_read_tag(reserved, (int)sizeof(reserved), &major, NULL, NULL));
	/* (an OpenCE host's bytes, zeros past its fields, are no tag) */
	memset(reserved, 0, sizeof(reserved));
	reserved[0] = 41;
	reserved[2] = 0x05;
	REQUIRE(!delta_wire_read_tag(reserved, (int)sizeof(reserved), &major, NULL, NULL));
}

/* ---------- the sessions */

static void case_agree(void)
{
	/* (what both offer, of the bits this build knows) */
	REQUIRE(delta_peer_agree(0xFFFFFFFFu, DELTA_PEER_OPENH1_CAPABILITIES) == 1);
	REQUIRE(delta_peer_agree(1u << _delta_capability_profile, DELTA_PEER_OPENH1_CAPABILITIES) == 0);
	REQUIRE(delta_peer_agree(0x000FFFFFu, 0x000FFFFFu) ==
		(0x000FFFFFu & ~(1u << _delta_capability_md_maps) & ~(1u << _delta_capability_console_slots) &
			~(1u << _delta_capability_server_badge) & ~(1u << _delta_capability_ping) &
			~(1u << _delta_capability_host_cheats)));
	/* (the reserved server_badge, retired and unknown bits never) */
	REQUIRE(delta_peer_agree(1u << _delta_capability_server_badge, 1u << _delta_capability_server_badge) == 0);
	REQUIRE(delta_peer_agree(1u << _delta_capability_md_maps, 1u << _delta_capability_md_maps) == 0);
	REQUIRE(delta_peer_agree(0x80000000u, 0x80000000u) == 0);
	REQUIRE(DELTA_PEER_OPENH1_CAPABILITIES == 1u << _delta_capability_platform);
}

static void case_handshake(void)
{
	static struct side host, client;
	struct delta_peer_machine machine;

	side_start(&host, HOST, HOST_PORT, 0, "OpenH1 build-77");
	side_start(&client, CLIENT, CLIENT_PORT, 3, "ChupathingyCE 0.8.0h");
	/* (a ChupathingyCE client offers far more) */
	client.peer.local.capabilities = 0x000FFFFFu & ~(1u << 16);
	play(&host, &client, 50, 1);
	REQUIRE(client.peer.client_state == _delta_peer_client_delta);
	REQUIRE(client.peer.agreed == 1u << _delta_capability_platform);
	REQUIRE(host.peer.peers[1].used && host.peer.peers[1].agreed == 1u << _delta_capability_platform);
	REQUIRE(!strcmp(host.peer.peers[1].hello.build, "ChupathingyCE 0.8.0h"));
	REQUIRE(host.peer.peers[1].hello.legacy_version == 41 && host.peer.peers[1].hello.machine_index == 1);
	/* (what each knows of the other) */
	play(&host, &client, 500, 1);
	REQUIRE(delta_peer_machine(&host.peer, 1, &machine) && machine.key.platform == 3);
	REQUIRE((machine.flags & DELTA_ROSTER_PLATFORM) && !strcmp(machine.build, "ChupathingyCE 0.8.0h"));
	REQUIRE(delta_peer_machine(&client.peer, 0, &machine) && machine.key.platform == 0);
	REQUIRE(!strcmp(machine.build, "OpenH1 build-77"));
	REQUIRE(client.peer.welcome.host_machine_index == 0);
	REQUIRE(client.peer.room_capabilities == 1u << _delta_capability_platform);
	REQUIRE(delta_peer_on_delta(&host.peer) && delta_peer_on_delta(&client.peer));
	/* (the roster: every 5 seconds, and nothing more sent once settled) */
	{
		struct delta_peer_game_machine machines[2];
		int count = game_of(machines, 1);
		int rosters = 0;
		int hellos = 0;
		int time;

		for (time = 0; time < 10000; time += 50)
		{
			delta_peer_host_frame(&host.peer, now, machines, count);
			delta_peer_client_frame(&client.peer, now, 1, HOST, HOST_PORT, 1, 1);
			rosters += count_sent(_delta_message_roster);
			hellos += count_sent(_delta_message_hello);
			sent_count = 0;
			now += 50;
		}
		REQUIRE(rosters == 2 && hellos == 0);
	}
	/* (a client that leaves says BYE, and the host forgets it) */
	delta_peer_client_frame(&client.peer, now, 0, 0, 0, 0, DELTA_WIRE_NO_MACHINE);
	REQUIRE(count_sent(_delta_message_bye) == 1);
	{
		struct side *sides[2];

		sides[0] = &host;
		sides[1] = &client;
		deliver(sides, 2);
	}
	REQUIRE(!host.peer.peers[1].used);
	/* (a host that stops Delta says BYE: its client goes legacy) */
	side_start(&host, HOST, HOST_PORT, 0, "OpenH1 build-77");
	side_start(&client, CLIENT, CLIENT_PORT, 3, "ChupathingyCE 0.8.0h");
	play(&host, &client, 100, 1);
	REQUIRE(client.peer.client_state == _delta_peer_client_delta);
	delta_peer_stop(&host.peer);
	{
		struct side *sides[1];

		sides[0] = &client;
		deliver(sides, 1);
	}
	REQUIRE(client.peer.client_state == _delta_peer_client_legacy);
	/* (a machine gone from the game is forgotten) */
	side_start(&host, HOST, HOST_PORT, 0, "OpenH1 build-77");
	side_start(&client, CLIENT, CLIENT_PORT, 3, "ChupathingyCE 0.8.0h");
	play(&host, &client, 100, 1);
	REQUIRE(host.peer.peers[1].used);
	{
		struct delta_peer_game_machine machines[2];

		delta_peer_host_frame(&host.peer, now, machines, game_of(machines, 0));
	}
	REQUIRE(!host.peer.peers[1].used && !delta_peer_machine(&host.peer, 1, NULL));
}

static void case_takes_no_tables(void)
{
	static struct side host, client;
	unsigned char data[64];
	int index;
	int found = 0;

	side_start(&host, HOST, HOST_PORT, 0, "OpenH1 build-77");
	side_start(&client, CLIENT, CLIENT_PORT, 3, "ChupathingyCE 0.8.0h");
	{
		struct delta_peer_game_machine machines[2];

		delta_peer_host_frame(&host.peer, now, machines, game_of(machines, 1));
		delta_peer_client_frame(&client.peer, now, 1, HOST, HOST_PORT, 1, 1);
		REQUIRE(count_sent(_delta_message_hello) == 1);
		delta_peer_receive(&host.peer, now, CLIENT, CLIENT_PORT, sent[0].data, sent[0].size);
	}
	/* (WELCOME, then TABLE_HAVE of none) */
	for (index = 0; index < sent_count; index++)
	{
		if (sent[index].size == DELTA_WIRE_HEADER_SIZE + 4 && sent[index].data[3] == _delta_message_table_have)
		{
			REQUIRE(sent[index].data[12] == 0xFF && sent[index].data[13] == 0xFF && sent[index].data[14] == 0xFF &&
				sent[index].data[15] == 0xFF);
			found++;
		}
	}
	REQUIRE(found == 1);
	sent_count = 0;
	/* (a table sent anyway: said again, at most once a second) */
	delta_wire_write_header(data, DELTA_MAJOR, _delta_message_table, 4, host.peer.peers[1].session);
	delta_peer_receive(&host.peer, now, CLIENT, CLIENT_PORT, data, DELTA_WIRE_HEADER_SIZE + 4);
	REQUIRE(count_sent(_delta_message_table_have) == 0);
	now += 1000;
	delta_peer_receive(&host.peer, now, CLIENT, CLIENT_PORT, data, DELTA_WIRE_HEADER_SIZE + 4);
	delta_peer_receive(&host.peer, now, CLIENT, CLIENT_PORT, data, DELTA_WIRE_HEADER_SIZE + 4);
	REQUIRE(count_sent(_delta_message_table_have) == 1);
	sent_count = 0;
}

static void case_legacy(void)
{
	static struct side host, client;
	unsigned char data[DELTA_WIRE_MAXIMUM_DATAGRAM];
	struct delta_peer_game_machine machines[2];
	int size;

	/* (a HELLO of another major is answered LEGACY, of this major, empty) */
	side_start(&host, HOST, HOST_PORT, 0, "OpenH1 build-77");
	delta_peer_host_frame(&host.peer, now, machines, game_of(machines, 1));
	sent_count = 0;
	delta_wire_write_header(data, 2, _delta_message_hello, 0, 0x5150u);
	delta_peer_receive(&host.peer, now, CLIENT, CLIENT_PORT, data, DELTA_WIRE_HEADER_SIZE);
	REQUIRE(sent_count == 1 && sent[0].ipv4 == CLIENT && sent[0].port == CLIENT_PORT);
	REQUIRE(sent[0].size == DELTA_WIRE_HEADER_SIZE && sent[0].data[2] == DELTA_MAJOR &&
		sent[0].data[3] == _delta_message_legacy && sent[0].data[8] == 0x50 && sent[0].data[9] == 0x51);
	REQUIRE(!host.peer.peers[1].used);
	sent_count = 0;
	/* (and nothing else of another major) */
	delta_wire_write_header(data, 2, _delta_message_bye, 0, 0x5150u);
	delta_peer_receive(&host.peer, now, CLIENT, CLIENT_PORT, data, DELTA_WIRE_HEADER_SIZE);
	REQUIRE(sent_count == 0);
	/* (a client answered LEGACY, of its major or another, is legacy) */
	side_start(&client, CLIENT, CLIENT_PORT, 0, "OpenH1 build-77");
	delta_peer_client_frame(&client.peer, now, 1, HOST, HOST_PORT, 1, 1);
	REQUIRE(client.peer.client_state == _delta_peer_client_waiting);
	sent_count = 0;
	size = delta_wire_write_empty(data, 3, _delta_message_legacy, client.peer.session);
	delta_peer_receive(&client.peer, now, HOST, HOST_PORT, data, size);
	REQUIRE(client.peer.client_state == _delta_peer_client_legacy);
	delta_peer_client_frame(&client.peer, now + 600, 1, HOST, HOST_PORT, 1, 1);
	REQUIRE(sent_count == 0);
	side_start(&client, CLIENT, CLIENT_PORT, 0, "OpenH1 build-77");
	delta_peer_client_frame(&client.peer, now, 1, HOST, HOST_PORT, 1, 1);
	size = delta_wire_write_empty(data, DELTA_MAJOR, _delta_message_legacy, client.peer.session);
	delta_peer_receive(&client.peer, now, HOST, HOST_PORT, data, size);
	REQUIRE(client.peer.client_state == _delta_peer_client_legacy);
	sent_count = 0;
}

static void case_fallback(void)
{
	static struct side client;
	delta_u32 start;
	int hellos = 0;
	int time;

	/* (a host that does not speak Delta: nothing sent, legacy at once) */
	side_start(&client, CLIENT, CLIENT_PORT, 0, "OpenH1 build-77");
	sent_count = 0;
	delta_peer_client_frame(&client.peer, now, 1, HOST, HOST_PORT, 0, 1);
	REQUIRE(client.peer.client_state == _delta_peer_client_legacy && sent_count == 0);
	for (time = 0; time < 10000; time += 50)
		delta_peer_client_frame(&client.peer, now + (delta_u32)time, 1, HOST, HOST_PORT, 1, 1);
	REQUIRE(sent_count == 0);
	/* (no HELLO before the host gives this machine its index) */
	side_start(&client, CLIENT, CLIENT_PORT, 0, "OpenH1 build-77");
	delta_peer_client_frame(&client.peer, now, 1, HOST, HOST_PORT, 1, DELTA_WIRE_NO_MACHINE);
	REQUIRE(client.peer.client_state == _delta_peer_client_off && sent_count == 0);
	/* (one that says Delta but never answers: HELLO every 500 ms, legacy
	at 4 s, then silence) */
	start = now;
	for (time = 0; time < 10000; time += 10)
	{
		delta_peer_client_frame(&client.peer, start + (delta_u32)time, 1, HOST, HOST_PORT, 1, 1);
		if (count_sent(_delta_message_hello))
		{
			REQUIRE(time < DELTA_PEER_HANDSHAKE_TIME);
			REQUIRE(time % DELTA_PEER_HELLO_INTERVAL == 0);
			hellos++;
		}
		if (time < DELTA_PEER_HANDSHAKE_TIME)
			REQUIRE(client.peer.client_state == _delta_peer_client_waiting);
		else
			REQUIRE(client.peer.client_state == _delta_peer_client_legacy);
		sent_count = 0;
	}
	REQUIRE(DELTA_PEER_HANDSHAKE_TIME == 4000 && DELTA_PEER_HELLO_INTERVAL == 500);
	REQUIRE(hellos == DELTA_PEER_HANDSHAKE_TIME / DELTA_PEER_HELLO_INTERVAL);
	/* (legacy for the rest of that game; a new game starts again) */
	delta_peer_client_frame(&client.peer, start + 20000, 0, 0, 0, 0, DELTA_WIRE_NO_MACHINE);
	sent_count = 0;
	delta_peer_client_frame(&client.peer, start + 20010, 1, HOST, HOST_PORT, 1, 1);
	REQUIRE(client.peer.client_state == _delta_peer_client_waiting && count_sent(_delta_message_hello) == 1);
	sent_count = 0;
	now = start + 30000;
}

static void case_who_is_heard(void)
{
	static struct side host, client, other;
	struct delta_peer_game_machine machines[3];
	struct delta_wire_hello hello;
	unsigned char data[DELTA_WIRE_MAXIMUM_DATAGRAM];
	int size;
	int index;

	side_start(&host, HOST, HOST_PORT, 0, "OpenH1 build-77");
	memset(machines, 0, sizeof(machines));
	game_of(machines, 1);
	machines[2].machine_index = 2;
	machines[2].ipv4 = OTHER;
	delta_peer_host_frame(&host.peer, now, machines, 2);
	sent_count = 0;
	memset(&hello, 0, sizeof(hello));
	hello.capabilities = 1;
	hello.machine_index = 1;
	strcpy(hello.build, "X");
	/* (an address no machine of the game has) */
	size = delta_wire_write_hello(data, 77, &hello);
	delta_peer_receive(&host.peer, now, OTHER, CLIENT_PORT, data, size);
	REQUIRE(sent_count == 0 && !host.peer.peers[1].used);
	/* (a machine of the game claiming another's index) */
	delta_peer_host_frame(&host.peer, now, machines, 3);
	delta_peer_receive(&host.peer, now, OTHER, CLIENT_PORT, data, size);
	REQUIRE(sent_count == 0 && !host.peer.peers[1].used && !host.peer.peers[2].used);
	/* (the host's own machine is never a client) */
	hello.machine_index = 0;
	size = delta_wire_write_hello(data, 77, &hello);
	delta_peer_receive(&host.peer, now, 0, CLIENT_PORT, data, size);
	REQUIRE(sent_count == 0 && !host.peer.peers[0].used);
	/* (a session of 0) */
	hello.machine_index = 1;
	size = delta_wire_write_hello(data, 0, &hello);
	delta_peer_receive(&host.peer, now, CLIENT, CLIENT_PORT, data, size);
	REQUIRE(sent_count == 0 && !host.peer.peers[1].used);
	/* (its own: answered) */
	size = delta_wire_write_hello(data, 77, &hello);
	delta_peer_receive(&host.peer, now, CLIENT, CLIENT_PORT, data, size);
	REQUIRE(host.peer.peers[1].used && count_sent(_delta_message_welcome) == 1);
	sent_count = 0;
	/* (a new session from it no sooner than a second on) */
	size = delta_wire_write_hello(data, 78, &hello);
	delta_peer_receive(&host.peer, now + 10, CLIENT, CLIENT_PORT, data, size);
	REQUIRE(host.peer.peers[1].session == 77 && sent_count == 0);
	delta_peer_receive(&host.peer, now + 1000, CLIENT, CLIENT_PORT, data, size);
	REQUIRE(host.peer.peers[1].session == 78);
	sent_count = 0;
	/* (the host takes at most its burst at once from a machine) */
	now += 5000;
	for (index = 0; index < 100; index++)
		delta_peer_receive(&host.peer, now, CLIENT, CLIENT_PORT, data, size);
	REQUIRE(count_sent(_delta_message_welcome) == DELTA_PEER_HOST_BURST);
	sent_count = 0;

	/* (a client hears its host alone: its address, its port, its session) */
	side_start(&client, CLIENT, CLIENT_PORT, 0, "OpenH1 build-77");
	side_start(&other, OTHER, HOST_PORT, 0, "OpenH1 build-77");
	(void)other;
	delta_peer_client_frame(&client.peer, now, 1, HOST, HOST_PORT, 1, 1);
	sent_count = 0;
	{
		struct delta_wire_welcome welcome;

		memset(&welcome, 0, sizeof(welcome));
		welcome.capabilities = 1;
		welcome.agreed = 1;
		welcome.host_machine_index = 0;
		size = delta_wire_write_welcome(data, client.peer.session, &welcome);
		delta_peer_receive(&client.peer, now, OTHER, HOST_PORT, data, size);
		REQUIRE(client.peer.client_state == _delta_peer_client_waiting);
		delta_peer_receive(&client.peer, now, HOST, HOST_PORT + 1, data, size);
		REQUIRE(client.peer.client_state == _delta_peer_client_waiting);
		size = delta_wire_write_welcome(data, client.peer.session + 1, &welcome);
		delta_peer_receive(&client.peer, now, HOST, HOST_PORT, data, size);
		REQUIRE(client.peer.client_state == _delta_peer_client_waiting);
		size = delta_wire_write_welcome(data, client.peer.session, &welcome);
		delta_peer_receive(&client.peer, now, HOST, HOST_PORT, data, size);
		REQUIRE(client.peer.client_state == _delta_peer_client_delta);
	}
	/* (and never agrees past what it offers, whatever the host says) */
	REQUIRE(client.peer.agreed == 1);
	sent_count = 0;
}

/* ---------- the badges */

static void case_badge(void)
{
	static struct side host, client;
	struct delta_peer_machine machine;
	char text[DELTA_PEER_BADGE_SIZE];

	/* (a platform's name where it shares one, else its build's first word) */
	memset(&machine, 0, sizeof(machine));
	machine.known = 1;
	machine.flags = DELTA_ROSTER_DELTA | DELTA_ROSTER_PLATFORM;
	machine.key.platform = _delta_platform_pc_linux;
	strcpy(machine.build, "ChupathingyCE 0.8.0h");
	REQUIRE(delta_peer_badge(&machine, text, (int)sizeof(text)) && !strcmp(text, "Linux"));
	machine.key.platform = _delta_platform_xbox;
	REQUIRE(delta_peer_badge(&machine, text, (int)sizeof(text)) && !strcmp(text, "Xbox"));
	machine.key.platform = _delta_platform_unknown;
	REQUIRE(delta_peer_badge(&machine, text, (int)sizeof(text)) && !strcmp(text, "ChupathingyCE"));
	machine.key.platform = 200;
	REQUIRE(delta_peer_badge(&machine, text, (int)sizeof(text)) && !strcmp(text, "ChupathingyCE"));
	/* (a platform not shared is not shown) */
	machine.key.platform = _delta_platform_pc_linux;
	machine.flags = DELTA_ROSTER_DELTA;
	REQUIRE(delta_peer_badge(&machine, text, (int)sizeof(text)) && !strcmp(text, "ChupathingyCE"));
	/* (none for an unknown platform without a build, nor a machine without
	Delta) */
	machine.build[0] = 0;
	REQUIRE(!delta_peer_badge(&machine, text, (int)sizeof(text)) && !text[0]);
	machine.known = 0;
	strcpy(machine.build, "ChupathingyCE");
	REQUIRE(!delta_peer_badge(&machine, text, (int)sizeof(text)));
	/* (cut to its field) */
	machine.known = 1;
	strcpy(machine.build, "AVeryLongClientNameIndeed 1");
	REQUIRE(delta_peer_badge(&machine, text, (int)sizeof(text)) && strlen(text) == DELTA_PEER_BADGE_SIZE - 1);
	REQUIRE(delta_peer_platform_name(_delta_platform_unknown) == NULL);
	REQUIRE(delta_peer_platform_name(NUMBER_OF_DELTA_PLATFORMS) == NULL);
	REQUIRE(!strcmp(delta_peer_platform_name(_delta_platform_tvos), "Apple TV"));

	/* (in a game: the others' badges, not the viewer's own; an OpenH1 host
	shows a ChupathingyCE Linux machine's, its client the host's port) */
	side_start(&host, HOST, HOST_PORT, 0, "OpenH1 build-77");
	side_start(&client, CLIENT, CLIENT_PORT, _delta_platform_pc_linux, "ChupathingyCE 0.8.0h");
	REQUIRE(!delta_peer_machine_badge(&host.peer, 1, text, (int)sizeof(text)));
	play(&host, &client, 500, 1);
	REQUIRE(delta_peer_machine_badge(&host.peer, 1, text, (int)sizeof(text)) && !strcmp(text, "Linux"));
	REQUIRE(!delta_peer_machine_badge(&host.peer, 0, text, (int)sizeof(text)));
	REQUIRE(delta_peer_machine_badge(&client.peer, 0, text, (int)sizeof(text)) && !strcmp(text, "OpenH1"));
	REQUIRE(!delta_peer_machine_badge(&client.peer, 1, text, (int)sizeof(text)));
	REQUIRE(!delta_peer_machine_badge(&client.peer, 2, text, (int)sizeof(text)));
	REQUIRE(!delta_peer_machine_badge(&client.peer, -1, text, (int)sizeof(text)));
	REQUIRE(!delta_peer_machine_badge(&client.peer, DELTA_PEER_MAXIMUM_MACHINES, text, (int)sizeof(text)));
	/* (OpenH1 with OpenH1: none) */
	side_start(&host, HOST, HOST_PORT, 0, "OpenH1 build-77");
	side_start(&client, CLIENT, CLIENT_PORT, 0, "OpenH1 build-78");
	play(&host, &client, 500, 1);
	REQUIRE(delta_peer_on_delta(&host.peer));
	REQUIRE(!delta_peer_machine_badge(&host.peer, 1, text, (int)sizeof(text)));
	REQUIRE(!delta_peer_machine_badge(&client.peer, 0, text, (int)sizeof(text)));
	/* (a client gone legacy shows none) */
	side_start(&client, CLIENT, CLIENT_PORT, 0, "OpenH1 build-78");
	delta_peer_client_frame(&client.peer, now, 1, HOST, HOST_PORT, 0, 1);
	REQUIRE(!delta_peer_machine_badge(&client.peer, 0, text, (int)sizeof(text)));
	sent_count = 0;
}

/* ---------- the host's advertisement */

static void case_advertise(void)
{
	unsigned char reserved[DELTA_ADVERTISED_RESERVED_SIZE];
	int major;
	struct delta_wire_id own, oldest;
	int index;

	/* (a Legacy or mixed game: OpenCE's network version) */
	memset(reserved, 0, sizeof(reserved));
	reserved[HALO_PORT_ADVERTISED_VERSION_OFFSET] = (byte)(HALO_PORT_NETWORK_VERSION & 0xFF);
	reserved[HALO_PORT_ADVERTISED_VERSION_OFFSET + 1] = (byte)(HALO_PORT_NETWORK_VERSION >> 8);
	reserved[2] = 0x05;
	reserved[3] = 2;
	reserved[4] = 120;
	reserved[5] = 19;
	reserved[6] = 3;
	delta_game.enabled = 1;
	delta_game.role = _role_host;
	delta_game.port = 5160;
	delta_peer_game_advertise(reserved, (long)sizeof(reserved));
	REQUIRE(delta_wire_read_tag(reserved, (int)sizeof(reserved), &major, &own, &oldest));
	REQUIRE(major == DELTA_MAJOR && own.number == HALO_PORT_NETWORK_VERSION && own.letter == 'a');
	REQUIRE(oldest.number == own.number && oldest.letter == own.letter);
	/* (OpenCE's and OpenH1's bytes as they were) */
	REQUIRE(reserved[0] == (HALO_PORT_NETWORK_VERSION & 0xFF) && reserved[2] == 0x05 && reserved[3] == 2 &&
		reserved[4] == 120 && reserved[5] == 19 && reserved[6] == 3);
	for (index = 7; index < DELTA_ADVERTISED_TAG_OFFSET; index++)
		REQUIRE(reserved[index] == 0);
	REQUIRE(HALO_PORT_ADVERTISED_BOTS_OFFSET < DELTA_ADVERTISED_TAG_OFFSET);
	/* (a Standard game, which OpenCE's machines do not join: none) */
	reserved[HALO_PORT_ADVERTISED_VERSION_OFFSET] = (byte)(HALO_PORT_STANDARD_ADVERTISED_VERSION & 0xFF);
	reserved[HALO_PORT_ADVERTISED_VERSION_OFFSET + 1] = (byte)(HALO_PORT_STANDARD_ADVERTISED_VERSION >> 8);
	delta_peer_game_advertise(reserved, (long)sizeof(reserved));
	REQUIRE(!delta_wire_read_tag(reserved, (int)sizeof(reserved), &major, NULL, NULL));
	for (index = DELTA_ADVERTISED_TAG_OFFSET; index < DELTA_ADVERTISED_RESERVED_SIZE; index++)
		REQUIRE(reserved[index] == 0);
	/* (no socket, not hosting, or Delta off: none) */
	reserved[HALO_PORT_ADVERTISED_VERSION_OFFSET] = (byte)(HALO_PORT_NETWORK_VERSION & 0xFF);
	reserved[HALO_PORT_ADVERTISED_VERSION_OFFSET + 1] = (byte)(HALO_PORT_NETWORK_VERSION >> 8);
	delta_game.port = 0;
	delta_peer_game_advertise(reserved, (long)sizeof(reserved));
	REQUIRE(!delta_wire_read_tag(reserved, (int)sizeof(reserved), &major, NULL, NULL));
	delta_game.port = 5160;
	delta_game.role = _role_client;
	delta_peer_game_advertise(reserved, (long)sizeof(reserved));
	REQUIRE(!delta_wire_read_tag(reserved, (int)sizeof(reserved), &major, NULL, NULL));
	delta_game.role = _role_host;
	delta_game.enabled = 0;
	delta_peer_game_advertise(reserved, (long)sizeof(reserved));
	REQUIRE(!delta_wire_read_tag(reserved, (int)sizeof(reserved), &major, NULL, NULL));
	delta_game.enabled = 1;
	delta_peer_game_advertise(reserved, (long)sizeof(reserved));
	REQUIRE(delta_wire_read_tag(reserved, (int)sizeof(reserved), &major, NULL, NULL));
}

int main(int argc, char **argv)
{
	static const struct
	{
		const char *name;
		void (*run)(void);
	} cases[] = {
		{ "header", case_header },
		{ "hello", case_hello },
		{ "welcome", case_welcome },
		{ "roster", case_roster },
		{ "tag", case_tag },
		{ "agree", case_agree },
		{ "handshake", case_handshake },
		{ "takes-no-tables", case_takes_no_tables },
		{ "legacy", case_legacy },
		{ "fallback", case_fallback },
		{ "who-is-heard", case_who_is_heard },
		{ "badge", case_badge },
		{ "advertise", case_advertise },
	};
	size_t index;

	if (argc != 2)
		return 2;
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
	{
		if (!strcmp(argv[1], cases[index].name))
		{
			cases[index].run();
			return 0;
		}
	}
	fprintf(stderr, "no case %s\n", argv[1]);
	return 2;
}
