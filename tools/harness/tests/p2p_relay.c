/*
P2P_RELAY.C (test)

Internet play's relays (port/linux/src/p2p_relay.c): the relay's pairing of
two machines by their session's token, its refusals, limits and expiry,
what it forwards and to whom, the machines' attempts, malformed packets,
and the fallback order (p2p_relay_stage). test_p2p_relay.py takes the whole
file, with p2p_crypto.c's SHA-256, HMAC and comparison; the clock, the
addresses and what is sent are the test's.
*/

#include "harness.h"

#include <stdlib.h>

#include "p2p_internal.h"

/* (a check needs no words of its own: the line says which) */
#define REQUIRE(condition) CHECK(condition, "%s", "")

/* ---------- the world */

enum
{
	MAXIMUM_SENT = 4096,
	/* the relay, and machines A, B, C (addresses in network order, as the
	tunnel's; their values matter only as distinct numbers) */
	RELAY = 0x0A000001,
	A = 0x0B000002,
	B = 0x0C000003,
	C = 0x0D000004,
	PORT = 0x1234,
};

struct sent
{
	unsigned long address;
	unsigned short port;
	int size;
	unsigned char data[2048];
};

static struct sent sent[MAXIMUM_SENT];
static int sent_count;
static unsigned int random_state = 12345;
static int paired_calls, delivered_calls, released_calls;
static int delivered_index;
static unsigned char delivered[2048];
static int delivered_size;
/* the machines the tunnel reaches (peers_only) */
static unsigned long peer_addresses[4];
static int peer_count;

void posix_random_bytes(void *buffer, unsigned long size)
{
	unsigned char *bytes = buffer;
	unsigned long index;

	for (index = 0; index < size; index++)
	{
		random_state = random_state * 1103515245u + 12345u;
		bytes[index] = (unsigned char)(random_state >> 16);
	}
}

void p2p_log(const char *format, ...)
{
	(void)format;
}

void p2p_hex(const unsigned char *bytes, int size, char *text)
{
	int index;

	for (index = 0; index < size; index++)
		sprintf(text + 2 * index, "%02x", bytes[index]);
}

void p2p_relay_sendto(unsigned long address, unsigned short port, const unsigned char *data, int size)
{
	if (sent_count == MAXIMUM_SENT || size > (int)sizeof(sent[0].data))
		return;
	sent[sent_count].address = address;
	sent[sent_count].port = port;
	sent[sent_count].size = size;
	memcpy(sent[sent_count].data, data, (size_t)size);
	sent_count++;
}

void p2p_relay_paired(int index)
{
	(void)index;
	paired_calls++;
}

void p2p_relay_delivered(int index, const unsigned char *packet, int size)
{
	delivered_calls++;
	delivered_index = index;
	delivered_size = size;
	memcpy(delivered, packet, (size_t)size);
}

void p2p_relay_released(int index)
{
	(void)index;
	released_calls++;
}

int p2p_relay_peer_address(unsigned long address, unsigned short port)
{
	int index;

	(void)port;
	for (index = 0; index < peer_count; index++)
	{
		if (peer_addresses[index] == address)
			return 1;
	}
	return 0;
}

#include "under_test.inc"

/* ---------- helpers */

static unsigned long now = 1000000;

static void limits_default(struct p2p_relay_limits *limits)
{
	memset(limits, 0, sizeof(*limits));
	limits->enabled = 1;
	limits->peers_only = 0;
	limits->maximum_pairs = 64;
	limits->pairs_per_address = 4;
	limits->binds_per_second = 20;
	limits->upload = 8192 * 1024;
	limits->pair_upload = 256 * 1024;
	limits->address_upload = 512 * 1024;
}

static void relay_on(void)
{
	struct p2p_relay_limits limits;

	limits_default(&limits);
	p2p_relay_configure(&limits);
}

static void token_of(int seed, unsigned char *token)
{
	unsigned char secret[P2P_SHA256_SIZE];

	memset(secret, seed, sizeof(secret));
	p2p_relay_token(secret, token);
}

/* a bind as a machine sends it */
static int bind_packet(unsigned char *packet, int side, const unsigned char *token, int nonce)
{
	packet[0] = P2P_RELAY_MAGIC;
	packet[1] = 'B';
	packet[2] = 1;
	packet[3] = (unsigned char)side;
	memcpy(packet + 4, token, P2P_RELAY_TOKEN_SIZE);
	memset(packet + 4 + P2P_RELAY_TOKEN_SIZE, nonce, 8);
	return 4 + P2P_RELAY_TOKEN_SIZE + 8;
}

static void receive(const unsigned char *packet, int size, unsigned long address)
{
	unsigned char copy[2048];

	memcpy(copy, packet, (size_t)size);
	p2p_relay_received(copy, size, address, PORT, now);
}

static void bind_from(unsigned long address, int side, const unsigned char *token, int nonce)
{
	unsigned char packet[64];
	int size = bind_packet(packet, side, token, nonce);

	receive(packet, size, address);
}

/* the latest BOUND sent to an address: its state (-1: none), and channel */
static int bound_to(unsigned long address, unsigned char *channel)
{
	int index;

	for (index = sent_count - 1; index >= 0; index--)
	{
		if (sent[index].address == address && sent[index].size == 20 && sent[index].data[1] == 'K')
		{
			if (channel)
				memcpy(channel, sent[index].data + 12, 8);
			return sent[index].data[3];
		}
	}
	return -1;
}

/* a data packet (a tunnel packet of size bytes behind its header) */
static int data_packet(unsigned char *packet, const unsigned char *channel, int size)
{
	int index;

	packet[0] = P2P_RELAY_MAGIC;
	packet[1] = 'D';
	memcpy(packet + 2, channel, 8);
	packet[10] = P2P_TUNNEL_MAGIC;
	for (index = 1; index < size; index++)
		packet[10 + index] = (unsigned char)index;
	return 10 + size;
}

/* the data sent to an address since sent index from */
static int data_sent_to(unsigned long address, int from)
{
	int count = 0;
	int index;

	for (index = from; index < sent_count; index++)
		count += sent[index].address == address && sent[index].data[1] == 'd';
	return count;
}

static int data_sent_since(int from)
{
	int count = 0;
	int index;

	for (index = from; index < sent_count; index++)
		count += sent[index].data[0] == P2P_RELAY_MAGIC && sent[index].data[1] == 'd';
	return count;
}

/* A and B paired by one token; the channel */
static void pair_a_b(const unsigned char *token, unsigned char *channel)
{
	bind_from(A, 0, token, 1);
	bind_from(B, 1, token, 2);
	bound_to(B, channel);
}

/* ---------- cases */

static int case_pairing(void)
{
	unsigned char token[P2P_RELAY_TOKEN_SIZE], channel[8], channel_a[8], packet[2048];
	struct p2p_relay_counts counts;
	int size, before;

	relay_on();
	token_of(1, token);
	bind_from(A, 0, token, 1);
	REQUIRE(bound_to(A, NULL) == 0);
	bind_from(B, 1, token, 2);
	/* (both told: B as it binds, A at once, with its own nonce) */
	REQUIRE(bound_to(B, channel) == 1);
	REQUIRE(bound_to(A, channel_a) == 1);
	REQUIRE(!memcmp(channel, channel_a, 8));
	{
		int index, found = 0;

		for (index = 0; index < sent_count; index++)
		{
			if (sent[index].address == A && sent[index].data[1] == 'K' && sent[index].data[3] == 1)
				found |= sent[index].data[4] == 1;
		}
		REQUIRE(found);
	}
	p2p_relay_counts(&counts);
	REQUIRE(counts.paired == 1);
	/* forwarded each way, as 'd', the packet whole */
	size = data_packet(packet, channel, 100);
	before = sent_count;
	receive(packet, size, A);
	REQUIRE(data_sent_to(B, before) == 1);
	REQUIRE(data_sent_to(A, before) == 0);
	REQUIRE(sent[sent_count - 1].size == size && !memcmp(sent[sent_count - 1].data + 2, packet + 2, (size_t)(size - 2)));
	before = sent_count;
	receive(packet, size, B);
	REQUIRE(data_sent_to(A, before) == 1);
	/* a 'd' is never forwarded again (a relay's own, sent back to it) */
	before = sent_count;
	packet[1] = 'd';
	receive(packet, size, A);
	REQUIRE(data_sent_since(before) == 0);
	return 0;
}

static int case_secret(void)
{
	unsigned char token[P2P_RELAY_TOKEN_SIZE], other[P2P_RELAY_TOKEN_SIZE], channel[8], packet[2048];
	unsigned char secret[P2P_SHA256_SIZE], joiner_key[P2P_SHA256_SIZE], host_key[P2P_SHA256_SIZE];
	int size, before;

	relay_on();
	/* the token: the same from the same secret, another's from another,
	none of the secret or its tunnel keys (p2p.c: "joiner", "host") */
	memset(secret, 1, sizeof(secret));
	token_of(1, token);
	token_of(2, other);
	REQUIRE(memcmp(token, other, P2P_RELAY_TOKEN_SIZE));
	p2p_hmac_sha256(secret, P2P_SHA256_SIZE, "joiner", 6, joiner_key);
	p2p_hmac_sha256(secret, P2P_SHA256_SIZE, "host", 4, host_key);
	REQUIRE(memcmp(token, secret, P2P_RELAY_TOKEN_SIZE) && memcmp(token, joiner_key, P2P_RELAY_TOKEN_SIZE) &&
		memcmp(token, host_key, P2P_RELAY_TOKEN_SIZE));
	/* another token's bind pairs with no one */
	bind_from(A, 0, token, 1);
	bind_from(B, 1, other, 2);
	REQUIRE(bound_to(A, NULL) == 0 && bound_to(B, NULL) == 0);
	/* A's channel known to C, with A's token: nothing goes from C, nor to C */
	bound_to(A, channel);
	bind_from(C, 1, token, 3);
	REQUIRE(bound_to(C, NULL) == 1);
	size = data_packet(packet, channel, 64);
	before = sent_count;
	receive(packet, size, B);
	REQUIRE(data_sent_since(before) == 0);
	/* a channel no pair has */
	channel[0] ^= 0xFF;
	size = data_packet(packet, channel, 64);
	before = sent_count;
	receive(packet, size, A);
	REQUIRE(data_sent_since(before) == 0);
	return 0;
}

static int case_same_side(void)
{
	unsigned char token[P2P_RELAY_TOKEN_SIZE], channel[8], packet[2048];
	int size, before;

	relay_on();
	token_of(1, token);
	bind_from(A, 0, token, 1);
	/* the same side from elsewhere: not taken while A is heard */
	bind_from(C, 0, token, 3);
	REQUIRE(bound_to(C, NULL) == -1);
	bind_from(B, 1, token, 2);
	REQUIRE(bound_to(B, channel) == 1);
	size = data_packet(packet, channel, 64);
	before = sent_count;
	receive(packet, size, B);
	REQUIRE(data_sent_to(A, before) == 1 && data_sent_to(C, before) == 0);
	/* a pair of one address with itself: refused */
	token_of(5, packet);
	bind_from(C, 0, packet, 7);
	{
		unsigned char copy[64];
		int bind_size = bind_packet(copy, 1, packet, 8);

		p2p_relay_received(copy, bind_size, C, PORT, now);
	}
	REQUIRE(bound_to(C, NULL) == 2);
	/* a silent side's address moves after REBIND_TIME (a NAT's new mapping) */
	now += 6000;
	bind_from(C, 0, token, 4);
	REQUIRE(bound_to(C, NULL) == 1);
	return 0;
}

static int case_limits_pairs(void)
{
	struct p2p_relay_limits limits;
	unsigned char token[P2P_RELAY_TOKEN_SIZE];
	int index;

	limits_default(&limits);
	limits.maximum_pairs = 3;
	limits.pairs_per_address = 2;
	p2p_relay_configure(&limits);
	/* two from A, a third refused */
	for (index = 0; index < 3; index++)
	{
		token_of(10 + index, token);
		bind_from(A, 0, token, index);
	}
	REQUIRE(bound_to(A, NULL) == 2);
	/* the relay's most: three in all */
	token_of(20, token);
	bind_from(B, 0, token, 1);
	REQUIRE(bound_to(B, NULL) == 0);
	token_of(21, token);
	bind_from(C, 0, token, 1);
	REQUIRE(bound_to(C, NULL) == 2);
	/* (an address past its pairs does not join one either) */
	token_of(20, token);
	bind_from(A, 1, token, 9);
	REQUIRE(bound_to(A, NULL) == 2);
	return 0;
}

static int case_rate_bytes(void)
{
	struct p2p_relay_limits limits;
	unsigned char token[P2P_RELAY_TOKEN_SIZE], channel[8], packet[2048];
	int size, before, index, passed;

	/* a dedicated server's limits (8 MB/s in all): a burst of a game's
	packets at once all pass */
	relay_on();
	token_of(1, token);
	pair_a_b(token, channel);
	size = data_packet(packet, channel, 1400);
	before = sent_count;
	for (index = 0; index < 50; index++)
		receive(packet, size, A);
	REQUIRE(data_sent_since(before) == 50);
	limits_default(&limits);
	limits.pair_upload = 10000;
	p2p_relay_configure(&limits);
	now += 2000;
	size = data_packet(packet, channel, 990);
	/* a second's worth at once (1000 bytes a packet), then nothing */
	before = sent_count;
	for (index = 0; index < 50; index++)
		receive(packet, size, A);
	passed = data_sent_since(before);
	REQUIRE(passed == 10);
	/* half a second later, half as much again */
	now += 500;
	before = sent_count;
	for (index = 0; index < 50; index++)
		receive(packet, size, A);
	REQUIRE(data_sent_since(before) == 5);
	/* the relay's own most, over its pairs */
	limits_default(&limits);
	limits.upload = 3000;
	p2p_relay_configure(&limits);
	now += 2000;
	before = sent_count;
	for (index = 0; index < 50; index++)
		receive(packet, size, B);
	REQUIRE(data_sent_since(before) <= 3);
	/* an address's most, over its pairs */
	limits_default(&limits);
	limits.address_upload = 2000;
	p2p_relay_configure(&limits);
	now += 2000;
	before = sent_count;
	for (index = 0; index < 50; index++)
		receive(packet, size, A);
	REQUIRE(data_sent_since(before) <= 2);
	return 0;
}

static int case_rate_binds(void)
{
	unsigned char token[P2P_RELAY_TOKEN_SIZE];
	int index, before, answered = 0;

	relay_on();
	token_of(1, token);
	/* 100 binds in one moment from one address: 20 answered */
	before = sent_count;
	for (index = 0; index < 100; index++)
		bind_from(A, 0, token, index);
	for (index = before; index < sent_count; index++)
		answered += sent[index].address == A;
	REQUIRE(answered == 20);
	/* an answer is no larger than the bind (nothing amplified) */
	REQUIRE(sent[before].size <= 4 + P2P_RELAY_TOKEN_SIZE + 8);
	return 0;
}

static int case_idle(void)
{
	unsigned char token[P2P_RELAY_TOKEN_SIZE], channel[8], packet[2048];
	struct p2p_relay_counts counts;
	int size, before;

	relay_on();
	token_of(1, token);
	pair_a_b(token, channel);
	/* one waiting alone: ended after UNPAIRED_TIME */
	token_of(2, packet);
	bind_from(C, 0, packet, 1);
	now += 21000;
	p2p_relay_update(now);
	p2p_relay_counts(&counts);
	REQUIRE(counts.pairs == 1);
	/* the pair, idle: ended after IDLE_TIME */
	now += 10000;
	p2p_relay_update(now);
	p2p_relay_counts(&counts);
	REQUIRE(counts.pairs == 0);
	size = data_packet(packet, channel, 64);
	before = sent_count;
	receive(packet, size, A);
	REQUIRE(data_sent_since(before) == 0);
	/* a pair in use lives on */
	pair_a_b(token, channel);
	size = data_packet(packet, channel, 64);
	for (before = 0; before < 10; before++)
	{
		now += 5000;
		receive(packet, size, A);
		p2p_relay_update(now);
	}
	p2p_relay_counts(&counts);
	REQUIRE(counts.paired == 1);
	return 0;
}

static int case_only_tunnel(void)
{
	unsigned char token[P2P_RELAY_TOKEN_SIZE], channel[8], packet[2048];
	int size, before;

	relay_on();
	token_of(1, token);
	pair_a_b(token, channel);
	/* not a tunnel packet: not forwarded */
	size = data_packet(packet, channel, 64);
	packet[10] = 0x00;
	before = sent_count;
	receive(packet, size, A);
	REQUIRE(data_sent_since(before) == 0);
	/* shorter than a tunnel packet, or longer than the largest */
	size = data_packet(packet, channel, P2P_TUNNEL_MINIMUM_PACKET - 1);
	receive(packet, size, A);
	size = data_packet(packet, channel, P2P_TUNNEL_MAXIMUM_PACKET + 1);
	receive(packet, size, A);
	REQUIRE(data_sent_since(before) == 0);
	/* the smallest and the largest pass */
	size = data_packet(packet, channel, P2P_TUNNEL_MINIMUM_PACKET);
	receive(packet, size, A);
	size = data_packet(packet, channel, P2P_TUNNEL_MAXIMUM_PACKET);
	receive(packet, size, A);
	REQUIRE(data_sent_since(before) == 2);
	/* a bind one byte off its size is passed over */
	{
		unsigned char bind[64];
		int bind_size = bind_packet(bind, 0, token, 9);

		before = sent_count;
		receive(bind, bind_size - 1, C);
		receive(bind, bind_size + 1, C);
		bind[3] = 2;
		receive(bind, bind_size, C);
		bind[3] = 0;
		bind[2] = 2;
		receive(bind, bind_size, C);
		REQUIRE(sent_count == before);
	}
	return 0;
}

static int case_peers_only(void)
{
	struct p2p_relay_limits limits;
	unsigned char token[P2P_RELAY_TOKEN_SIZE], channel[8];

	limits_default(&limits);
	limits.peers_only = 1;
	p2p_relay_configure(&limits);
	token_of(1, token);
	/* two strangers: nothing */
	bind_from(A, 0, token, 1);
	bind_from(B, 1, token, 2);
	REQUIRE(bound_to(A, NULL) == -1 && bound_to(B, NULL) == -1);
	/* a stranger joins a pair its game's machine made */
	peer_addresses[peer_count++] = A;
	bind_from(B, 1, token, 2);
	REQUIRE(bound_to(B, NULL) == -1);
	bind_from(A, 0, token, 1);
	REQUIRE(bound_to(A, NULL) == 0);
	bind_from(B, 1, token, 2);
	REQUIRE(bound_to(B, channel) == 1);
	/* off: nothing at all */
	limits.enabled = 0;
	p2p_relay_configure(&limits);
	token_of(3, token);
	bind_from(A, 0, token, 5);
	REQUIRE(bound_to(A, NULL) == 1);
	return 0;
}

static int case_attempt(void)
{
	struct p2p_relay_offered offered;
	struct p2p_relay_attempt_state state;
	unsigned char secret[P2P_SHA256_SIZE], token[P2P_RELAY_TOKEN_SIZE], packet[2048], peer[6] = { 1, 2, 3, 4, 5, 6 };
	unsigned char channel[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };
	int index, size, before;

	memset(secret, 7, sizeof(secret));
	p2p_relay_token(secret, token);
	memset(&offered, 0, sizeof(offered));
	offered.relay.address = RELAY;
	offered.relay.port = PORT;
	offered.kind = _p2p_relay_server;
	index = p2p_relay_attempt_start(peer, secret, 1, &offered, now);
	REQUIRE(index >= 0);
	/* the same relay again: the same attempt */
	REQUIRE(p2p_relay_attempt_start(peer, secret, 1, &offered, now) == index);
	/* its bind: side 1, the token */
	REQUIRE(sent_count == 1 && sent[0].address == RELAY && sent[0].size == 44 && sent[0].data[1] == 'B' &&
		sent[0].data[3] == 1 && !memcmp(sent[0].data + 4, token, P2P_RELAY_TOKEN_SIZE));
	/* not paired: nothing sent through it, and binds again */
	REQUIRE(!p2p_relay_send(index, packet, 64));
	now += 600;
	p2p_relay_update(now);
	REQUIRE(sent_count == 2 && sent[1].data[1] == 'B');
	/* a BOUND from elsewhere, or with another nonce: passed over */
	packet[0] = P2P_RELAY_MAGIC;
	packet[1] = 'K';
	packet[2] = 1;
	packet[3] = 1;
	memcpy(packet + 4, sent[0].data + 4 + P2P_RELAY_TOKEN_SIZE, 8);
	memcpy(packet + 12, channel, 8);
	p2p_relay_received(packet, 20, C, PORT, now);
	REQUIRE(!paired_calls);
	packet[4] ^= 1;
	p2p_relay_received(packet, 20, RELAY, PORT, now);
	REQUIRE(!paired_calls);
	packet[4] ^= 1;
	p2p_relay_received(packet, 20, RELAY, PORT, now);
	REQUIRE(paired_calls == 1);
	REQUIRE(p2p_relay_attempt(index, &state) && state.paired && state.bind_round_trip == 600);
	/* sent through it: wrapped with the channel */
	memset(packet, 0x69, 64);
	before = sent_count;
	REQUIRE(p2p_relay_send(index, packet, 64));
	REQUIRE(sent_count == before + 1 && sent[before].data[1] == 'D' && !memcmp(sent[before].data + 2, channel, 8) &&
		sent[before].size == 74);
	/* delivered: from the relay, with the channel, a tunnel packet */
	size = data_packet(packet, channel, 64);
	packet[1] = 'd';
	p2p_relay_received(packet, size, C, PORT, now);
	packet[2] ^= 1;
	p2p_relay_received(packet, size, RELAY, PORT, now);
	packet[2] ^= 1;
	REQUIRE(!delivered_calls);
	p2p_relay_received(packet, size, RELAY, PORT, now);
	REQUIRE(delivered_calls == 1 && delivered_index == index && delivered_size == 64 && delivered[0] == 0x69);
	/* given up: released at the relay */
	before = sent_count;
	p2p_relay_forget_peer(peer, -1);
	REQUIRE(released_calls == 1 && sent_count == before + 1 && sent[before].data[1] == 'X');
	REQUIRE(!p2p_relay_attempt(index, NULL));
	return 0;
}

static int case_attempt_expiry(void)
{
	struct p2p_relay_offered offered;
	unsigned char secret[P2P_SHA256_SIZE], peer[6] = { 1, 2, 3, 4, 5, 6 };
	int index;

	memset(secret, 7, sizeof(secret));
	memset(&offered, 0, sizeof(offered));
	offered.relay.address = RELAY;
	offered.relay.port = PORT;
	index = p2p_relay_attempt_start(peer, secret, 0, &offered, now);
	REQUIRE(index >= 0);
	/* never paired: given up after its time */
	now += 26000;
	p2p_relay_update(now);
	REQUIRE(!p2p_relay_attempt(index, NULL) && released_calls == 1);
	/* an address of none, or a side of neither: not tried */
	offered.relay.address = 0;
	REQUIRE(p2p_relay_attempt_start(peer, secret, 0, &offered, now) < 0);
	offered.relay.address = RELAY;
	REQUIRE(p2p_relay_attempt_start(peer, secret, 2, &offered, now) < 0);
	return 0;
}

/* two machines and a relay in one: packets passed between them by their
addresses; A's attempts and B's on separate relays' channels */
static int case_through(void)
{
	unsigned char token[P2P_RELAY_TOKEN_SIZE], channel[8], packet[2048];
	int size, before, index;

	relay_on();
	token_of(1, token);
	pair_a_b(token, channel);
	/* both ways, many times, every packet once */
	for (index = 0; index < 200; index++)
	{
		size = data_packet(packet, channel, 32 + index);
		before = sent_count;
		receive(packet, size, index & 1 ? A : B);
		REQUIRE(data_sent_to(index & 1 ? B : A, before) == 1 && sent_count == before + 1);
		REQUIRE(sent[before].size == size);
		now += 3;
	}
	return 0;
}

/* anything at all: no crash, and nothing sent to an address that bound to
nothing, nor anything larger than what came */
static int case_malformed(void)
{
	unsigned char token[P2P_RELAY_TOKEN_SIZE], channel[8], packet[2048], valid[2048];
	struct p2p_relay_offered offered;
	unsigned char secret[P2P_SHA256_SIZE], peer[6] = { 1, 2, 3, 4, 5, 6 };
	/* (the addresses that sent binds, damaged or not: a side whose address
	went silent takes a new one, a NAT's new mapping, and two damaged alike
	pair) */
	unsigned long bound[70000];
	int bound_count = 0;
	int round;

	relay_on();
	token_of(1, token);
	pair_a_b(token, channel);
	memset(secret, 7, sizeof(secret));
	memset(&offered, 0, sizeof(offered));
	offered.relay.address = RELAY;
	offered.relay.port = PORT;
	p2p_relay_attempt_start(peer, secret, 0, &offered, now);
	for (round = 0; round < 200000; round++)
	{
		unsigned long from = round % 3 == 0 ? A : round % 3 == 1 ? B : (unsigned long)rand();
		int size = rand() % 1600;
		int kind = rand() % 4;
		int index, before = sent_count;

		if (kind == 0)
		{
			for (index = 0; index < size; index++)
				packet[index] = (unsigned char)rand();
		}
		else
		{
			/* a valid packet of each type, damaged */
			int valid_size = kind == 1 ? bind_packet(valid, rand() % 2, token, rand()) :
				kind == 2 ? data_packet(valid, channel, 32 + rand() % 1400) : 20;

			if (kind == 3)
			{
				valid[0] = P2P_RELAY_MAGIC;
				valid[1] = rand() % 2 ? 'K' : 'd';
				valid[2] = 1;
			}
			memcpy(packet, valid, (size_t)valid_size);
			size = valid_size + rand() % 5 - 2;
			if (size < 0)
				size = 0;
			for (index = 0; index < rand() % 4; index++)
				packet[rand() % (size ? size : 1)] ^= (unsigned char)(1 << (rand() % 8));
		}
		if (size > 0)
			packet[0] = rand() % 8 ? P2P_RELAY_MAGIC : packet[0];
		if (kind == 1 && bound_count < (int)(sizeof(bound) / sizeof(*bound)))
			bound[bound_count++] = from;
		p2p_relay_received(packet, size, from, PORT, now);
		for (index = before; index < sent_count; index++)
		{
			int known = sent[index].address == A || sent[index].address == B || sent[index].address == from;
			int other;

			/* (what is sent goes to a side that bound, or answers its
			sender, and is never larger than what came) */
			for (other = 0; other < bound_count; other++)
				known |= sent[index].address == bound[other];
			REQUIRE(known);
			REQUIRE(sent[index].size <= size);
		}
		if (sent_count > MAXIMUM_SENT - 64)
			sent_count = 0;
		now += rand() % 5;
		if (round % 1000 == 0)
			p2p_relay_update(now);
	}
	return 0;
}

/* the order: directly first, then the game's machines, then the dedicated
servers'; at once where both NATs are strict */
static int case_fallback_order(void)
{
	REQUIRE(p2p_relay_stage(0, 0, 0, 0, 0) == _p2p_relay_stage_none);
	REQUIRE(p2p_relay_stage(3999, 0, 1, 0, 0) == _p2p_relay_stage_none);
	REQUIRE(p2p_relay_stage(4000, 0, 0, 0, 1) == _p2p_relay_stage_players);
	REQUIRE(p2p_relay_stage(6999, 0, 1, 1, 0) == _p2p_relay_stage_players);
	REQUIRE(p2p_relay_stage(7000, 0, 1, 1, 1) == _p2p_relay_stage_servers);
	/* both strict: hole punching cannot meet them */
	REQUIRE(p2p_relay_stage(999, 1, 0, 0, 0) == _p2p_relay_stage_none);
	REQUIRE(p2p_relay_stage(1000, 1, 0, 0, 1) == _p2p_relay_stage_players);
	/* neither has a player's to offer (the peer said so): servers at once */
	REQUIRE(p2p_relay_stage(4000, 0, 1, 0, 0) == _p2p_relay_stage_servers);
	/* the peer has not said yet: the players' first */
	REQUIRE(p2p_relay_stage(4000, 0, 0, 0, 0) == _p2p_relay_stage_players);
	return 0;
}

int main(int argc, char **argv)
{
	static const struct
	{
		const char *name;
		int (*run)(void);
	} cases[] =
	{
		{ "pairing", case_pairing },
		{ "secret", case_secret },
		{ "same-side", case_same_side },
		{ "limits-pairs", case_limits_pairs },
		{ "rate-bytes", case_rate_bytes },
		{ "rate-binds", case_rate_binds },
		{ "idle", case_idle },
		{ "only-tunnel", case_only_tunnel },
		{ "peers-only", case_peers_only },
		{ "attempt", case_attempt },
		{ "attempt-expiry", case_attempt_expiry },
		{ "through", case_through },
		{ "malformed", case_malformed },
		{ "fallback-order", case_fallback_order },
	};
	size_t index;

	if (argc != 2)
		return 2;
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
	{
		if (!strcmp(argv[1], cases[index].name))
			return cases[index].run();
	}
	return 2;
}
