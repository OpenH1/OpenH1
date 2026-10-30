/*
P2P_RELAY.C

Internet play's relays (p2p.c): two machines whose NATs both give each
destination a port of its own (symmetric NATs, behind which hole punching
cannot meet) reach each other through a third machine that both can reach.
There is no server of this project's: the relay is the game's host (between
two of its players), another player of the game whose network takes
connections (network.relay_for_others), or a dedicated server that says it
relays (server.relay: its listing's _stamp_relay, p2p_lobby.c, or one named
in network.relay_servers).

The relay is the tunnel's UDP socket, its packets told from the tunnel's by
their first byte (P2P_RELAY_MAGIC). Two machines whose session cannot meet
directly each bind to the relay with a token that only they have: an HMAC
of their session's secret (p2p_signal.c's, from their X25519 keys and a
nonce of each, which never travels) with a label of the relay's own
(p2p_relay_token). The relay pairs the two binds of one token, one of each
side of the session (the host's, the joiner's), and from then on forwards
between those two addresses alone, and only tunnel packets, which the two
seal with keys from the same secret under other labels: the relay reads
nothing of them, and cannot work the secret or the keys out of the token,
nor use the token for anything but this pairing.

- BIND (machine to relay): magic, 'B', version, side, the token, a nonce.
  Sent every BIND_INTERVAL until bound and paired, then every
  KEEP_ALIVE_INTERVAL (which also keeps the NATs' mappings).
- BOUND (relay to machine): magic, 'K', version, state (waiting, paired,
  refused), the bind's nonce, the pair's channel (8 random bytes). No
  larger than a bind, so a bind sent from another's address reflects
  nothing bigger to it.
- DATA (machine to relay 'D', relay to machine 'd'): magic, the type, the
  channel, then one tunnel packet (P2P_TUNNEL_MAGIC, of a tunnel packet's
  sizes). The relay forwards a 'D' only from one of the pair's two
  addresses, to the other, as a 'd'; a 'd' is never forwarded again.
- RELEASE (machine to relay): magic, 'X', version, side, the token: the
  pair ends (from its own address only).

The relay's limits (p2p_relay_limits): pairs in all and per address, binds
a second per address, bytes a second in all, per pair and per address; an
idle pair ends after IDLE_TIME, one never paired after UNPAIRED_TIME. A
player's machine relays only for pairs one side of which is a machine its
tunnel reaches (its game's: peers_only), a dedicated server for any pair.
Everything it receives is checked for its size and its fields before
anything is done with it.

The machines' side (the attempts): p2p.c asks for a relay with a peer's
session (p2p_relay_attempt_start), this file binds and keeps the binding,
and hands each tunnel packet the relay brings back to p2p.c
(p2p_relay_delivered); p2p.c sends through a paired attempt
(p2p_relay_send). Everything here runs on the p2p thread, under p2p_lock.
*/

#include "platform.h"
#include "posix.h"
#include "p2p_internal.h"

#include <string.h>

enum
{
	RELAY_VERSION = 1,
	RELAY_NONCE_SIZE = 8,
	RELAY_CHANNEL_SIZE = 8,
	/* magic, 'B', version, side, token, nonce */
	BIND_SIZE = 4 + P2P_RELAY_TOKEN_SIZE + RELAY_NONCE_SIZE,
	/* magic, 'K', version, state, nonce, channel */
	BOUND_SIZE = 4 + RELAY_NONCE_SIZE + RELAY_CHANNEL_SIZE,
	/* magic, 'X', version, side, token */
	RELEASE_SIZE = 4 + P2P_RELAY_TOKEN_SIZE,
	/* magic, 'D' or 'd', channel; then a tunnel packet */
	DATA_HEADER_SIZE = 2 + RELAY_CHANNEL_SIZE,
	MINIMUM_DATA_SIZE = DATA_HEADER_SIZE + P2P_TUNNEL_MINIMUM_PACKET,
	MAXIMUM_DATA_SIZE = DATA_HEADER_SIZE + P2P_TUNNEL_MAXIMUM_PACKET,

	/* the relay's: its pairs and the addresses it keeps counts of */
	MAXIMUM_PAIRS = P2P_RELAY_MAXIMUM_PAIRS,
	MAXIMUM_ADDRESSES = 512,
	/* the machines' attempts: a few relays for each of a few peers */
	MAXIMUM_ATTEMPTS = P2P_RELAY_MAXIMUM_ATTEMPTS,

	/* milliseconds */
	BIND_INTERVAL = 500,
	KEEP_ALIVE_INTERVAL = 10000,
	/* an attempt not paired in this long is given up; a paired one that
	hears nothing from the relay in this long (its keep-alive answered,
	or a packet) is lost */
	ATTEMPT_TIMEOUT = 25000,
	ATTEMPT_SILENCE = 35000,
	/* a relay that refused an attempt is not asked again for this long */
	REFUSED_TIME = 10000,
	/* the relay's: a pair no bind or packet came for in this long ends;
	one never paired too; a side heard this lately keeps its address
	(another bind of its token from elsewhere is not taken: its NAT's
	mapping changes only after a silence) */
	IDLE_TIME = 30000,
	UNPAIRED_TIME = 20000,
	REBIND_TIME = 5000,
	ADDRESS_FORGET_TIME = 60000,
	/* a bucket refilled for at most this long at once (its burst) */
	BUCKET_BURST_TIME = 1000,
	/* a peer not reached in this long is tried through relays (the host,
	the game's players), at once where both NATs are known strict (hole
	punching cannot meet them); the dedicated servers' after this long, or
	as soon as neither machine has a player's to offer (p2p_relay_stage) */
	RELAY_DELAY = 4000,
	RELAY_STRICT_DELAY = 1000,
	RELAY_SERVER_DELAY = 7000,
};

enum
{
	_type_bind = 'B',
	_type_bound = 'K',
	_type_data = 'D',
	_type_delivered = 'd',
	_type_release = 'X',
};

enum
{
	_bound_waiting,
	_bound_paired,
	_bound_refused,
};

enum
{
	_attempt_binding,
	_attempt_waiting,
	_attempt_paired,
	_attempt_refused,
};

/* spent from, and refilled at rate a second (units: bytes, or binds) */
struct bucket
{
	long left;
	unsigned long time;
	int started;
};

struct side
{
	int bound;
	unsigned long address;
	unsigned short port;
	unsigned long heard_time;
	/* its latest bind's nonce, which its BOUND carries back */
	unsigned char nonce[RELAY_NONCE_SIZE];
};

struct pair
{
	int used;
	unsigned char token[P2P_RELAY_TOKEN_SIZE];
	unsigned char channel[RELAY_CHANNEL_SIZE];
	struct side sides[2];
	unsigned long created_time;
	unsigned long active_time;
	struct bucket bytes;
	unsigned long forwarded;
};

/* an address the relay hears from: its binds and bytes */
struct heard_address
{
	int used;
	unsigned long address;
	unsigned long heard_time;
	struct bucket binds;
	struct bucket bytes;
};

struct attempt
{
	int used;
	unsigned char peer[P2P_IDENTIFIER_SIZE];
	int side;
	int kind;
	unsigned char token[P2P_RELAY_TOKEN_SIZE];
	struct p2p_candidate relay;
	unsigned char relay_identifier[P2P_IDENTIFIER_SIZE];
	unsigned char nonce[RELAY_NONCE_SIZE];
	unsigned char channel[RELAY_CHANNEL_SIZE];
	int state;
	unsigned long created_time;
	unsigned long sent_time;
	unsigned long heard_time;
	/* the first bind's round trip to the relay, -1 until it answered */
	long bind_round_trip;
	unsigned long first_sent_time;
};

static struct
{
	struct p2p_relay_limits limits;
	struct pair pairs[MAXIMUM_PAIRS];
	struct heard_address addresses[MAXIMUM_ADDRESSES];
	struct bucket bytes;
	struct p2p_relay_counts counts;

	struct attempt attempts[MAXIMUM_ATTEMPTS];
} relay;

static int relay_elapsed(unsigned long now, unsigned long since, unsigned long time)
{
	return (unsigned long)(now - since) >= time;
}

/* ---------- buckets */

/* whether amount can be spent at rate a second (0: none can), spending it */
static int bucket_spend(struct bucket *bucket, long rate, long amount, unsigned long now)
{
	/* (in 64 bits: a dedicated server's rate in bytes, times a thousand, is
	past a 32-bit long) */
	long burst = (long)((long long)rate * BUCKET_BURST_TIME / 1000);
	unsigned long gone;

	if (rate <= 0)
		return 0;
	if (burst < amount)
		burst = amount;
	if (!bucket->started)
	{
		bucket->left = burst;
		bucket->time = now;
		bucket->started = 1;
	}
	gone = now - bucket->time;
	/* (refilled for no more than the burst's time: no overflow) */
	if (gone > BUCKET_BURST_TIME)
		gone = BUCKET_BURST_TIME;
	bucket->left += (long)((long long)rate * (long long)gone / 1000);
	if (bucket->left > burst)
		bucket->left = burst;
	bucket->time = now;
	if (bucket->left < amount)
		return 0;
	bucket->left -= amount;
	return 1;
}

/* ---------- when */

int p2p_relay_stage(unsigned long age, int both_strict, int offered, int offered_players, int own_players)
{
	if (age < (unsigned long)(both_strict ? RELAY_STRICT_DELAY : RELAY_DELAY))
		return _p2p_relay_stage_none;
	if (age >= RELAY_SERVER_DELAY || (offered && !offered_players && !own_players))
		return _p2p_relay_stage_servers;
	return _p2p_relay_stage_players;
}

/* ---------- the token */

void p2p_relay_token(const unsigned char *secret, unsigned char *token)
{
	static const char label[] = "openh1 relay token 1";

	p2p_hmac_sha256(secret, P2P_SHA256_SIZE, label, (int)sizeof(label) - 1, token);
}

/* ---------- the relay */

void p2p_relay_configure(const struct p2p_relay_limits *limits)
{
	int changed = memcmp(&relay.limits, limits, sizeof(*limits)) != 0;

	relay.limits = *limits;
	if (relay.limits.maximum_pairs > MAXIMUM_PAIRS)
		relay.limits.maximum_pairs = MAXIMUM_PAIRS;
	if (relay.limits.maximum_pairs < 0)
		relay.limits.maximum_pairs = 0;
	/* (turned off: its pairs end at once) */
	if (changed && !relay.limits.enabled)
	{
		memset(relay.pairs, 0, sizeof(relay.pairs));
		memset(relay.addresses, 0, sizeof(relay.addresses));
	}
}

void p2p_relay_counts(struct p2p_relay_counts *counts)
{
	int index;

	*counts = relay.counts;
	counts->pairs = 0;
	counts->paired = 0;
	for (index = 0; index < MAXIMUM_PAIRS; index++)
	{
		if (!relay.pairs[index].used)
			continue;
		counts->pairs++;
		counts->paired += relay.pairs[index].sides[0].bound && relay.pairs[index].sides[1].bound;
	}
}

static struct heard_address *heard_address(unsigned long address, unsigned long now, int create)
{
	struct heard_address *free_entry = NULL;
	struct heard_address *stalest = NULL;
	int index;

	for (index = 0; index < MAXIMUM_ADDRESSES; index++)
	{
		struct heard_address *entry = &relay.addresses[index];

		if (entry->used && entry->address == address)
			return entry;
		if (!entry->used && !free_entry)
			free_entry = entry;
		else if (entry->used && (!stalest || (long)(entry->heard_time - stalest->heard_time) < 0))
			stalest = entry;
	}
	if (!create)
		return NULL;
	/* (full: the address heard from longest ago is forgotten; its counts
	start over, which only lets an address spend a little sooner) */
	if (!free_entry)
		free_entry = stalest;
	memset(free_entry, 0, sizeof(*free_entry));
	free_entry->used = 1;
	free_entry->address = address;
	free_entry->heard_time = now;
	return free_entry;
}

/* the pairs a side of which is at address */
static int pairs_of_address(unsigned long address)
{
	int count = 0;
	int index;

	for (index = 0; index < MAXIMUM_PAIRS; index++)
	{
		struct pair const *pair = &relay.pairs[index];

		if (pair->used && ((pair->sides[0].bound && pair->sides[0].address == address) ||
			(pair->sides[1].bound && pair->sides[1].address == address)))
		{
			count++;
		}
	}
	return count;
}

static int pairs_used(void)
{
	int count = 0;
	int index;

	for (index = 0; index < MAXIMUM_PAIRS; index++)
		count += relay.pairs[index].used;
	return count;
}

static struct pair *pair_of_token(const unsigned char *token)
{
	int index;

	for (index = 0; index < MAXIMUM_PAIRS; index++)
	{
		if (relay.pairs[index].used && p2p_equal(relay.pairs[index].token, token, P2P_RELAY_TOKEN_SIZE))
			return &relay.pairs[index];
	}
	return NULL;
}

static struct pair *pair_of_channel(const unsigned char *channel)
{
	int index;

	for (index = 0; index < MAXIMUM_PAIRS; index++)
	{
		if (relay.pairs[index].used && !memcmp(relay.pairs[index].channel, channel, RELAY_CHANNEL_SIZE))
			return &relay.pairs[index];
	}
	return NULL;
}

static void send_bound(unsigned long address, unsigned short port, int state, const unsigned char *nonce,
	const unsigned char *channel)
{
	unsigned char packet[BOUND_SIZE];

	packet[0] = P2P_RELAY_MAGIC;
	packet[1] = _type_bound;
	packet[2] = RELAY_VERSION;
	packet[3] = (unsigned char)state;
	memcpy(packet + 4, nonce, RELAY_NONCE_SIZE);
	if (channel)
		memcpy(packet + 4 + RELAY_NONCE_SIZE, channel, RELAY_CHANNEL_SIZE);
	else
		memset(packet + 4 + RELAY_NONCE_SIZE, 0, RELAY_CHANNEL_SIZE);
	p2p_relay_sendto(address, port, packet, BOUND_SIZE);
}

static void pair_free(struct pair *pair, const char *why)
{
	if (pair->sides[0].bound && pair->sides[1].bound)
	{
		p2p_log("Internet play: relay: a pair ended (%s; %lu bytes forwarded; the relay's packets dropped by its "
			"limits so far: %lu)", why, pair->forwarded, relay.counts.dropped);
		relay.counts.ended++;
	}
	memset(pair, 0, sizeof(*pair));
}

static void bind_received(const unsigned char *packet, int size, unsigned long address, unsigned short port,
	unsigned long now)
{
	const unsigned char *token = packet + 4;
	const unsigned char *nonce = token + P2P_RELAY_TOKEN_SIZE;
	struct heard_address *heard;
	struct pair *pair;
	struct side *side;
	int side_index;

	if (size != BIND_SIZE || packet[2] != RELAY_VERSION || packet[3] > 1)
		return;
	side_index = packet[3];
	heard = heard_address(address, now, 1);
	heard->heard_time = now;
	/* (a few a second from an address: a bind is answered, so a flood of
	them sent in another's name reflects at most this to it) */
	if (!bucket_spend(&heard->binds, relay.limits.binds_per_second, 1, now))
	{
		relay.counts.refused++;
		return;
	}
	pair = pair_of_token(token);
	if (pair)
	{
		struct side const *other = &pair->sides[!side_index];

		side = &pair->sides[side_index];
		if (side->bound && (side->address != address || side->port != port))
		{
			/* (another address for a side heard lately is no NAT's new
			mapping: ignored) */
			if (!relay_elapsed(now, side->heard_time, REBIND_TIME))
				return;
			side->bound = 0;
		}
		if (!side->bound)
		{
			/* (never a pair of one address with itself, nor one with an
			address past its pairs) */
			if ((other->bound && other->address == address && other->port == port) ||
				pairs_of_address(address) >= relay.limits.pairs_per_address)
			{
				relay.counts.refused++;
				send_bound(address, port, _bound_refused, nonce, NULL);
				return;
			}
			/* (a player relays for its game's machines only: one side of
			the pair must be one its tunnel reaches) */
			if (relay.limits.peers_only && !p2p_relay_peer_address(address, port) &&
				!(other->bound && p2p_relay_peer_address(other->address, other->port)))
			{
				return;
			}
			side->bound = 1;
			side->address = address;
			side->port = port;
			if (pair->sides[!side_index].bound)
			{
				char text[2 * RELAY_CHANNEL_SIZE + 1];

				relay.counts.formed++;
				p2p_hex(pair->channel, 4, text);
				p2p_log("Internet play: relay: paired two machines (%s...)", text);
				send_bound(other->address, other->port, _bound_paired, other->nonce, pair->channel);
			}
		}
		memcpy(side->nonce, nonce, RELAY_NONCE_SIZE);
		side->heard_time = now;
		pair->active_time = now;
		send_bound(address, port, pair->sides[!side_index].bound ? _bound_paired : _bound_waiting, nonce,
			pair->channel);
		return;
	}
	if (relay.limits.peers_only && !p2p_relay_peer_address(address, port))
		return;
	if (pairs_used() >= relay.limits.maximum_pairs || pairs_of_address(address) >= relay.limits.pairs_per_address)
	{
		relay.counts.refused++;
		send_bound(address, port, _bound_refused, nonce, NULL);
		return;
	}
	for (pair = relay.pairs; pair < relay.pairs + MAXIMUM_PAIRS && pair->used; pair++)
		;
	if (pair == relay.pairs + MAXIMUM_PAIRS)
		return;
	memset(pair, 0, sizeof(*pair));
	pair->used = 1;
	memcpy(pair->token, token, P2P_RELAY_TOKEN_SIZE);
	/* (a channel no other pair has) */
	do
		posix_random_bytes(pair->channel, RELAY_CHANNEL_SIZE);
	while (pair_of_channel(pair->channel) != pair);
	side = &pair->sides[side_index];
	side->bound = 1;
	side->address = address;
	side->port = port;
	side->heard_time = now;
	memcpy(side->nonce, nonce, RELAY_NONCE_SIZE);
	pair->created_time = now;
	pair->active_time = now;
	send_bound(address, port, _bound_waiting, nonce, pair->channel);
}

static void data_received(unsigned char *packet, int size, unsigned long address, unsigned short port,
	unsigned long now)
{
	struct heard_address *heard;
	struct pair *pair;
	struct side *from;
	struct side const *to;

	if (size < MINIMUM_DATA_SIZE || size > MAXIMUM_DATA_SIZE || packet[DATA_HEADER_SIZE] != P2P_TUNNEL_MAGIC)
		return;
	pair = pair_of_channel(packet + 2);
	if (!pair || !pair->sides[0].bound || !pair->sides[1].bound)
		return;
	if (pair->sides[0].address == address && pair->sides[0].port == port)
		from = &pair->sides[0];
	else if (pair->sides[1].address == address && pair->sides[1].port == port)
		from = &pair->sides[1];
	else
		return;
	to = &pair->sides[from == &pair->sides[0]];
	heard = heard_address(address, now, 1);
	heard->heard_time = now;
	/* (each pair's, each address's, and the relay's bytes a second) */
	if (!bucket_spend(&pair->bytes, relay.limits.pair_upload, size, now) ||
		!bucket_spend(&heard->bytes, relay.limits.address_upload, size, now) ||
		!bucket_spend(&relay.bytes, relay.limits.upload, size, now))
	{
		relay.counts.dropped++;
		return;
	}
	from->heard_time = now;
	pair->active_time = now;
	pair->forwarded += (unsigned long)size;
	relay.counts.forwarded_bytes += (unsigned long)size;
	relay.counts.forwarded_packets++;
	packet[1] = _type_delivered;
	p2p_relay_sendto(to->address, to->port, packet, size);
}

static void release_received(const unsigned char *packet, int size, unsigned long address, unsigned short port)
{
	struct pair *pair;
	struct side const *side;

	if (size != RELEASE_SIZE || packet[2] != RELAY_VERSION || packet[3] > 1)
		return;
	pair = pair_of_token(packet + 4);
	if (!pair)
		return;
	side = &pair->sides[packet[3]];
	if (side->bound && side->address == address && side->port == port)
		pair_free(pair, "released");
}

/* ---------- the machines' attempts */

static struct attempt *attempt_of_bound(unsigned long address, unsigned short port, const unsigned char *nonce)
{
	int index;

	for (index = 0; index < MAXIMUM_ATTEMPTS; index++)
	{
		struct attempt *attempt = &relay.attempts[index];

		if (attempt->used && attempt->relay.address == address && attempt->relay.port == port &&
			!memcmp(attempt->nonce, nonce, RELAY_NONCE_SIZE))
		{
			return attempt;
		}
	}
	return NULL;
}

static void attempt_free(struct attempt *attempt)
{
	int index = (int)(attempt - relay.attempts);

	memset(attempt, 0, sizeof(*attempt));
	p2p_relay_released(index);
}

static void bound_received(const unsigned char *packet, int size, unsigned long address, unsigned short port,
	unsigned long now)
{
	struct attempt *attempt;
	int state;

	if (size != BOUND_SIZE || packet[2] != RELAY_VERSION)
		return;
	attempt = attempt_of_bound(address, port, packet + 4);
	if (!attempt)
		return;
	state = packet[3];
	attempt->heard_time = now;
	if (attempt->bind_round_trip < 0)
		attempt->bind_round_trip = (long)(now - attempt->first_sent_time);
	if (state == _bound_refused)
	{
		if (attempt->state != _attempt_paired)
		{
			attempt->state = _attempt_refused;
			attempt->sent_time = now;
		}
		return;
	}
	if (state != _bound_waiting && state != _bound_paired)
		return;
	/* (the channel is the pair's, which a bind of the same token keeps) */
	memcpy(attempt->channel, packet + 4 + RELAY_NONCE_SIZE, RELAY_CHANNEL_SIZE);
	if (state == _bound_paired && attempt->state != _attempt_paired)
	{
		attempt->state = _attempt_paired;
		p2p_relay_paired((int)(attempt - relay.attempts));
	}
	else if (state == _bound_waiting)
	{
		attempt->state = _attempt_waiting;
	}
}

static void delivered_received(const unsigned char *packet, int size, unsigned long address, unsigned short port,
	unsigned long now)
{
	int index;

	if (size < MINIMUM_DATA_SIZE || size > MAXIMUM_DATA_SIZE || packet[DATA_HEADER_SIZE] != P2P_TUNNEL_MAGIC)
		return;
	for (index = 0; index < MAXIMUM_ATTEMPTS; index++)
	{
		struct attempt *attempt = &relay.attempts[index];

		if (attempt->used && attempt->state == _attempt_paired && attempt->relay.address == address &&
			attempt->relay.port == port && !memcmp(attempt->channel, packet + 2, RELAY_CHANNEL_SIZE))
		{
			attempt->heard_time = now;
			p2p_relay_delivered(index, packet + DATA_HEADER_SIZE, size - DATA_HEADER_SIZE);
			return;
		}
	}
}

static void send_bind(struct attempt *attempt, unsigned long now)
{
	unsigned char packet[BIND_SIZE];

	packet[0] = P2P_RELAY_MAGIC;
	packet[1] = _type_bind;
	packet[2] = RELAY_VERSION;
	packet[3] = (unsigned char)attempt->side;
	memcpy(packet + 4, attempt->token, P2P_RELAY_TOKEN_SIZE);
	memcpy(packet + 4 + P2P_RELAY_TOKEN_SIZE, attempt->nonce, RELAY_NONCE_SIZE);
	p2p_relay_sendto(attempt->relay.address, attempt->relay.port, packet, BIND_SIZE);
	attempt->sent_time = now;
}

static void send_release(struct attempt const *attempt)
{
	unsigned char packet[RELEASE_SIZE];

	packet[0] = P2P_RELAY_MAGIC;
	packet[1] = _type_release;
	packet[2] = RELAY_VERSION;
	packet[3] = (unsigned char)attempt->side;
	memcpy(packet + 4, attempt->token, P2P_RELAY_TOKEN_SIZE);
	p2p_relay_sendto(attempt->relay.address, attempt->relay.port, packet, RELEASE_SIZE);
}

int p2p_relay_attempt_start(const unsigned char *peer, const unsigned char *secret, int side,
	const struct p2p_relay_offered *offered, unsigned long now)
{
	struct attempt *free_attempt = NULL;
	int index;

	if ((side != 0 && side != 1) || !offered->relay.address || !offered->relay.port)
		return -1;
	for (index = 0; index < MAXIMUM_ATTEMPTS; index++)
	{
		struct attempt *attempt = &relay.attempts[index];

		if (attempt->used && !memcmp(attempt->peer, peer, P2P_IDENTIFIER_SIZE) &&
			attempt->relay.address == offered->relay.address && attempt->relay.port == offered->relay.port)
		{
			return index;
		}
		if (!attempt->used && !free_attempt)
			free_attempt = attempt;
	}
	if (!free_attempt)
		return -1;
	memset(free_attempt, 0, sizeof(*free_attempt));
	free_attempt->used = 1;
	memcpy(free_attempt->peer, peer, P2P_IDENTIFIER_SIZE);
	free_attempt->side = side;
	free_attempt->kind = offered->kind;
	free_attempt->relay = offered->relay;
	memcpy(free_attempt->relay_identifier, offered->identifier, P2P_IDENTIFIER_SIZE);
	p2p_relay_token(secret, free_attempt->token);
	posix_random_bytes(free_attempt->nonce, RELAY_NONCE_SIZE);
	free_attempt->state = _attempt_binding;
	free_attempt->created_time = now;
	free_attempt->heard_time = now;
	free_attempt->bind_round_trip = -1;
	free_attempt->first_sent_time = now;
	send_bind(free_attempt, now);
	return (int)(free_attempt - relay.attempts);
}

void p2p_relay_forget_peer(const unsigned char *peer, int keep)
{
	int index;

	for (index = 0; index < MAXIMUM_ATTEMPTS; index++)
	{
		struct attempt *attempt = &relay.attempts[index];

		if (!attempt->used || index == keep || memcmp(attempt->peer, peer, P2P_IDENTIFIER_SIZE))
			continue;
		if (attempt->state == _attempt_waiting || attempt->state == _attempt_paired)
			send_release(attempt);
		attempt_free(attempt);
	}
}

int p2p_relay_send(int index, const unsigned char *packet, int size)
{
	unsigned char data[MAXIMUM_DATA_SIZE];
	struct attempt const *attempt;

	if (index < 0 || index >= MAXIMUM_ATTEMPTS || size < P2P_TUNNEL_MINIMUM_PACKET ||
		size > P2P_TUNNEL_MAXIMUM_PACKET)
	{
		return 0;
	}
	attempt = &relay.attempts[index];
	if (!attempt->used || attempt->state != _attempt_paired)
		return 0;
	data[0] = P2P_RELAY_MAGIC;
	data[1] = _type_data;
	memcpy(data + 2, attempt->channel, RELAY_CHANNEL_SIZE);
	memcpy(data + DATA_HEADER_SIZE, packet, (size_t)size);
	p2p_relay_sendto(attempt->relay.address, attempt->relay.port, data, DATA_HEADER_SIZE + size);
	return 1;
}

int p2p_relay_attempt(int index, struct p2p_relay_attempt_state *state)
{
	struct attempt const *attempt;

	if (index < 0 || index >= MAXIMUM_ATTEMPTS)
		return 0;
	attempt = &relay.attempts[index];
	if (!attempt->used)
		return 0;
	if (state)
	{
		memcpy(state->peer, attempt->peer, P2P_IDENTIFIER_SIZE);
		memcpy(state->relay_identifier, attempt->relay_identifier, P2P_IDENTIFIER_SIZE);
		state->relay = attempt->relay;
		state->kind = attempt->kind;
		state->side = attempt->side;
		state->paired = attempt->state == _attempt_paired;
		state->refused = attempt->state == _attempt_refused;
		state->bind_round_trip = attempt->bind_round_trip;
	}
	return 1;
}

int p2p_relay_peer_attempts(const unsigned char *peer, int *indices, int maximum)
{
	int count = 0;
	int index;

	for (index = 0; index < MAXIMUM_ATTEMPTS && count < maximum; index++)
	{
		if (relay.attempts[index].used && !memcmp(relay.attempts[index].peer, peer, P2P_IDENTIFIER_SIZE))
			indices[count++] = index;
	}
	return count;
}

/* ---------- both */

int p2p_relay_received(unsigned char *packet, int size, unsigned long address, unsigned short port,
	unsigned long now)
{
	if (size < 2 || packet[0] != P2P_RELAY_MAGIC)
		return 0;
	switch (packet[1])
	{
	case _type_bind:
		if (relay.limits.enabled)
			bind_received(packet, size, address, port, now);
		break;
	case _type_data:
		if (relay.limits.enabled)
			data_received(packet, size, address, port, now);
		break;
	case _type_release:
		if (relay.limits.enabled)
			release_received(packet, size, address, port);
		break;
	case _type_bound:
		bound_received(packet, size, address, port, now);
		break;
	case _type_delivered:
		delivered_received(packet, size, address, port, now);
		break;
	}
	return 1;
}

void p2p_relay_update(unsigned long now)
{
	int index;

	for (index = 0; index < MAXIMUM_PAIRS; index++)
	{
		struct pair *pair = &relay.pairs[index];

		if (!pair->used)
			continue;
		if (!(pair->sides[0].bound && pair->sides[1].bound) && relay_elapsed(now, pair->created_time, UNPAIRED_TIME))
			pair_free(pair, "never paired");
		else if (relay_elapsed(now, pair->active_time, IDLE_TIME))
			pair_free(pair, "idle");
	}
	for (index = 0; index < MAXIMUM_ADDRESSES; index++)
	{
		struct heard_address *entry = &relay.addresses[index];

		if (entry->used && relay_elapsed(now, entry->heard_time, ADDRESS_FORGET_TIME) &&
			!pairs_of_address(entry->address))
		{
			entry->used = 0;
		}
	}
	for (index = 0; index < MAXIMUM_ATTEMPTS; index++)
	{
		struct attempt *attempt = &relay.attempts[index];

		if (!attempt->used)
			continue;
		if (attempt->state == _attempt_paired)
		{
			if (relay_elapsed(now, attempt->heard_time, ATTEMPT_SILENCE))
				attempt_free(attempt);
			else if (relay_elapsed(now, attempt->sent_time, KEEP_ALIVE_INTERVAL))
				send_bind(attempt, now);
		}
		else if (relay_elapsed(now, attempt->created_time, ATTEMPT_TIMEOUT))
		{
			attempt_free(attempt);
		}
		else if (attempt->state == _attempt_refused)
		{
			/* (asked again once, after a while: a pair may have ended) */
			if (relay_elapsed(now, attempt->sent_time, REFUSED_TIME))
			{
				attempt->state = _attempt_binding;
				send_bind(attempt, now);
			}
		}
		else if (relay_elapsed(now, attempt->sent_time, BIND_INTERVAL))
		{
			send_bind(attempt, now);
		}
	}
}
