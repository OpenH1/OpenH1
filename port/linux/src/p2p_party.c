/*
P2P_PARTY.C

Parties (p2p.h): friends grouped before any game, who go from game to game
together. A party lives on the signalling brokers alone (p2p_signal.c),
not on the tunnel: its machines need not reach each other until they play.

- A party is a link, halo://party/<token>: 16 random bytes in hexadecimal,
  which the machine that makes the party (its first leader) puts on the
  clipboard. Its machines talk on one topic, hceu/3/<HMAC(token, "party")>,
  each message sealed with HMAC(token, "party seal") and signed with the
  sender's Ed25519 key (this run's: p2p_sign) over HMAC(token, "party id")
  and the message, so that the brokers learn nothing and no one in the
  party can speak as another. Who is who is that key (a member's id is the
  first 8 bytes of its SHA-256).
- Every message (network byte order): "P", the party's format
  (PARTY_VERSION), its type, the sender's key (32) and its sequence (4: a
  sender's are taken in order, once each, whichever broker brings them),
  then the type's own, then the signature (64). A machine passes over a
  message of another format (an older or a newer build's), noting once
  that someone in the party has another version; builds without parties
  (OpenCE's, older OpenH1's) never subscribe to the topic.
  - STATE ('S'), every STATE_INTERVAL and at once on a change: flags (it
    leads; the leader has listed it), the leader's term (4) and id (8) as it
    knows them, its HALO_PORT_NETWORK_VERSION and Standard version (2
    each), where it is (the menus, joining a game, an internet game, a
    game no link reaches: a LAN game or a game of its own) and that game's
    host (its key's hash, 16), its last follow's order (4) and result
    (1), and its name (a length byte, then up to 15 characters of printable
    ASCII). The leader's adds its game, which the party follows: its order
    (4: the term, then a count of the leader's games), what it is (the
    menus, an internet game, a game no link reaches) and, for an internet
    game, its invite (the host's key hash and the token, 32); and the
    roster: the members' ids (8 each) in the order they joined, which sets
    who leads next.
  - LEAVE ('L'): the sender leaves; a leader names who leads next (8; zero:
    no one).
  - DISBAND ('D'): the leader ends the party.
  - REFUSE ('R'): the leader turns a machine away (8: its id; then why: the
    party is full).
- Joining: a machine with the link subscribes and sends its STATE until the
  leader lists it (JOIN_WAIT, else it gives up: the party has ended). The
  leader lists every new machine heard, up to P2P_PARTY_MAXIMUM_MEMBERS,
  and refuses the rest.
- Leading: the leader's STATE carries a term. A machine follows the leader
  of the highest term it hears (of equal terms, the lowest id), and steps
  down if it led. A leader that leaves names the next on its roster, who
  leads at once (a term higher); one silent for MEMBER_TIMEOUT is passed
  over: the first on the roster that is still heard (or this machine, if
  no one before it is) takes the next term.
- Following: the leader's game, as the game tells (p2p_party_set_game:
  its own invite while it hosts an internet game, the invite it joined
  with, a LAN game, or the menus), is the party's. A member not in it is
  asked to join it (p2p_party_follow, once for each of the leader's
  games), and says how that went (p2p_party_follow_result): in it, or why
  not (the host not reached, the game full, another version, a game no link
  reaches, ...), which the party shows, the member staying in the party.
  Each machine also says where it is, so that the party shows who is with
  the leader. After a game, the party is where it was: with the host, or in
  the menus, where the leader takes it on.

Voice in the party: none yet. network.voice_lobby's voice is the game's
(port/linux/game/network_voice.c), carried by its connection to the host,
which a party has not; a party's voice needs the tunnel between its
machines (p2p_reach_player's sessions, but by the party's token). The ids
and names here are what it would start from.

Everything runs under p2p_lock: the p2p thread's passes, and the game's
calls (their functions take it).
*/

#include "posix.h"
#include "p2p_internal.h"
#include "halo_port_limits.h"

#include <stdio.h>
#include <string.h>

enum
{
	PARTY_MAGIC = 'P',
	/* the party's message format: a machine takes its own alone */
	PARTY_VERSION = 1,
	_message_state = 'S',
	_message_leave = 'L',
	_message_disband = 'D',
	_message_refuse = 'R',

	/* a member's id: the first bytes of its key's SHA-256 */
	MEMBER_ID_SIZE = 8,
	/* "P", the format, the type, the key and the sequence */
	HEADER_SIZE = 3 + P2P_KEY_SIZE + 4,
	/* a STATE's fixed part after the header: flags, the term, the leader,
	the versions, the place and its host, the follow's order and result */
	STATE_FIXED_SIZE = 1 + 4 + MEMBER_ID_SIZE + 2 + 2 + 1 + P2P_KEY_HASH_SIZE + 4 + 1,
	/* an invite: its host's key hash and its token */
	INVITE_SIZE = P2P_KEY_HASH_SIZE + P2P_TOKEN_SIZE,
	/* the most a message is (p2p_signal.c takes 512) */
	MAXIMUM_PARTY_MESSAGE_SIZE = HEADER_SIZE + STATE_FIXED_SIZE + 1 + P2P_PARTY_NAME_SIZE + 4 + 1 + INVITE_SIZE +
		1 + P2P_PARTY_MAXIMUM_MEMBERS * MEMBER_ID_SIZE + P2P_SIGNATURE_SIZE,
	/* the machines heard: the members, and some besides (joiners not listed
	yet, a full party's refused) */
	MAXIMUM_HEARD = P2P_PARTY_MAXIMUM_MEMBERS + 8,
	/* machines gone, whose old messages are not taken again */
	MAXIMUM_DEPARTED = 32,

	/* a STATE's flags */
	_state_leads = 1,
	_state_listed = 2,
	/* what the menus were told of a member */
	_announced_joined = 1,
	_announced_follow = 2,
	_announced_version = 4,

	/* milliseconds */
	STATE_INTERVAL = 3000,
	/* a joiner's STATE, until the leader lists it */
	JOINING_INTERVAL = 1000,
	/* the least time between two STATEs */
	CHANGE_INTERVAL = 250,
	MEMBER_TIMEOUT = 12000,
	JOIN_WAIT = 20000,
	/* signatures checked (a STATE each few seconds from each member, through
	every broker; anyone with the link can send more): this many at once,
	one more each CHECK_INTERVAL */
	MAXIMUM_CHECKS = 48,
	CHECK_INTERVAL = 20,
};

typedef char party_message_size_assert[MAXIMUM_PARTY_MESSAGE_SIZE <= 512 ? 1 : -1];
typedef char party_link_size_assert[P2P_PARTY_LINK_SIZE == sizeof(P2P_PARTY_LINK_PREFIX) + 2 * P2P_TOKEN_SIZE ? 1 :
	-1];

struct member
{
	int used;
	unsigned char key[P2P_KEY_SIZE];
	unsigned char id[MEMBER_ID_SIZE];
	/* the last of its messages taken, and when (this machine's clock) */
	unsigned long sequence;
	unsigned long heard_time;
	char name[P2P_PARTY_NAME_SIZE];
	unsigned short network_version, standard_version;
	unsigned char listed;
	unsigned char place;
	unsigned char host[P2P_KEY_HASH_SIZE];
	unsigned long result_order;
	unsigned char result;
	/* what the menus were told of it (_announced_*) */
	unsigned char announced;
};

struct departed
{
	unsigned char id[MEMBER_ID_SIZE];
	unsigned long sequence;
};

static struct
{
	/* in a party (or joining one: not listed yet) */
	int active;
	int listed;
	unsigned char token[P2P_TOKEN_SIZE];
	unsigned char seal[P2P_SHA256_SIZE];
	unsigned char context[P2P_SHA256_SIZE];
	char topic[64];
	char link[P2P_PARTY_LINK_SIZE];
	unsigned long joined_time;

	/* this machine */
	unsigned char key[P2P_KEY_SIZE];
	unsigned char id[MEMBER_ID_SIZE];
	unsigned long sequence;
	char name[P2P_PARTY_NAME_SIZE];
	unsigned char place;
	unsigned char host[P2P_KEY_HASH_SIZE];
	int game_kind;
	unsigned char game_invite[INVITE_SIZE];
	unsigned long result_order;
	unsigned char result;
	/* the leader's game this machine was asked to join, and answered */
	unsigned long handled_order;

	/* the leader (zero: none known yet), its term, when last heard */
	unsigned char leader[MEMBER_ID_SIZE];
	unsigned long term;
	unsigned long leader_time;
	/* the leader's game, which the party follows */
	unsigned long target_order;
	int target_kind;
	unsigned char target_invite[INVITE_SIZE];
	/* (leading: the count of its games, the order's low half) */
	unsigned long game_count;
	/* the roster, in the order the members joined */
	unsigned char roster[P2P_PARTY_MAXIMUM_MEMBERS][MEMBER_ID_SIZE];
	int roster_count;

	struct member heard[MAXIMUM_HEARD];
	struct departed departed[MAXIMUM_DEPARTED];
	int departed_next;

	/* a STATE wanted at once; the last sent */
	int send_wanted;
	unsigned long sent_time;
	/* a broker was connected at the last pass (no one is timed out while
	none is) */
	int was_connected;

	int checks_left;
	unsigned long checks_time;
	/* another format's message heard (noted once a party) */
	int other_version_noted;

	/* the menus' notices, the newest last */
	struct p2p_party_notice notices[P2P_PARTY_MAXIMUM_NOTICES];
	int notice_count;
	unsigned long notice_serial;
} party;

static const unsigned char no_id[MEMBER_ID_SIZE];
/* the messages sent now are the last before the game quits */
static int party_urgent;

static int elapsed(unsigned long since, unsigned long time)
{
	return (unsigned int)(p2p_now() - since) >= (unsigned int)time;
}

static void put_long(unsigned char *bytes, unsigned long value)
{
	bytes[0] = (unsigned char)(value >> 24);
	bytes[1] = (unsigned char)(value >> 16);
	bytes[2] = (unsigned char)(value >> 8);
	bytes[3] = (unsigned char)value;
}

static unsigned long get_long(const unsigned char *bytes)
{
	return (unsigned long)bytes[0] << 24 | (unsigned long)bytes[1] << 16 | (unsigned long)bytes[2] << 8 | bytes[3];
}

static void put_short(unsigned char *bytes, unsigned short value)
{
	bytes[0] = (unsigned char)(value >> 8);
	bytes[1] = (unsigned char)value;
}

static unsigned short get_short(const unsigned char *bytes)
{
	return (unsigned short)(bytes[0] << 8 | bytes[1]);
}

static void member_id(const unsigned char *key, unsigned char *id)
{
	unsigned char digest[P2P_SHA256_SIZE];

	p2p_sha256(key, P2P_KEY_SIZE, digest);
	memcpy(id, digest, MEMBER_ID_SIZE);
}

/* a name as kept: printable ASCII alone, at most P2P_PARTY_NAME_SIZE - 1 */
static void clean_name(char *name, const char *text, int size)
{
	int read, written = 0;

	for (read = 0; read < size && text[read] && written < P2P_PARTY_NAME_SIZE - 1; read++)
	{
		if (text[read] >= 0x20 && text[read] < 0x7F)
			name[written++] = text[read];
	}
	name[written] = 0;
}

static void notice(int kind, const char *name, int result)
{
	struct p2p_party_notice *entry;

	if (party.notice_count == P2P_PARTY_MAXIMUM_NOTICES)
	{
		memmove(party.notices, party.notices + 1, sizeof(party.notices[0]) * (P2P_PARTY_MAXIMUM_NOTICES - 1));
		party.notice_count--;
	}
	entry = &party.notices[party.notice_count++];
	memset(entry, 0, sizeof(*entry));
	entry->serial = ++party.notice_serial;
	entry->kind = kind;
	entry->result = result;
	if (name)
		clean_name(entry->name, name, P2P_PARTY_NAME_SIZE);
}

/* ---------- the members */

static struct member *find_member(const unsigned char *id)
{
	int index;

	for (index = 0; index < MAXIMUM_HEARD; index++)
	{
		if (party.heard[index].used && !memcmp(party.heard[index].id, id, MEMBER_ID_SIZE))
			return &party.heard[index];
	}
	return NULL;
}

static int roster_index(const unsigned char *id)
{
	int index;

	for (index = 0; index < party.roster_count; index++)
	{
		if (!memcmp(party.roster[index], id, MEMBER_ID_SIZE))
			return index;
	}
	return -1;
}

static void roster_remove(const unsigned char *id)
{
	int index = roster_index(id);

	if (index < 0)
		return;
	memmove(party.roster[index], party.roster[index + 1], (size_t)(party.roster_count - index - 1) * MEMBER_ID_SIZE);
	party.roster_count--;
}

static int is_self(const unsigned char *id)
{
	return !memcmp(id, party.id, MEMBER_ID_SIZE);
}

static int leading(void)
{
	return party.active && is_self(party.leader);
}

/* a machine gone: its messages up to the last taken are not taken again (a
copy sent again would bring it back) */
static void depart(struct member *member)
{
	struct departed *departed = &party.departed[party.departed_next];

	memcpy(departed->id, member->id, MEMBER_ID_SIZE);
	departed->sequence = member->sequence;
	party.departed_next = (party.departed_next + 1) % MAXIMUM_DEPARTED;
	roster_remove(member->id);
	memset(member, 0, sizeof(*member));
}

static int departed_sequence(const unsigned char *id, unsigned long *sequence)
{
	int index;

	for (index = 0; index < MAXIMUM_DEPARTED; index++)
	{
		if (!memcmp(party.departed[index].id, id, MEMBER_ID_SIZE) && memcmp(id, no_id, MEMBER_ID_SIZE))
		{
			*sequence = party.departed[index].sequence;
			return 1;
		}
	}
	return 0;
}

/* ---------- the messages */

/* the message signed and sealed, published on the party's topic */
static void send_message(unsigned char *message, int size)
{
	unsigned char data[P2P_SHA256_SIZE + MAXIMUM_PARTY_MESSAGE_SIZE];
	unsigned char sealed[MAXIMUM_PARTY_MESSAGE_SIZE + P2P_SEAL_OVERHEAD];

	memcpy(data, party.context, P2P_SHA256_SIZE);
	memcpy(data + P2P_SHA256_SIZE, message, (size_t)size);
	p2p_sign(data, P2P_SHA256_SIZE + size, message + size);
	size += P2P_SIGNATURE_SIZE;
	size = p2p_seal(party.seal, message, size, sealed);
	p2p_signal_party_publish(sealed, size, party_urgent);
}

static int message_header(unsigned char *message, int type)
{
	message[0] = PARTY_MAGIC;
	message[1] = PARTY_VERSION;
	message[2] = (unsigned char)type;
	memcpy(message + 3, party.key, P2P_KEY_SIZE);
	put_long(message + 3 + P2P_KEY_SIZE, ++party.sequence);
	return HEADER_SIZE;
}

static void send_state(void)
{
	unsigned char message[MAXIMUM_PARTY_MESSAGE_SIZE];
	int size = message_header(message, _message_state);
	int length = (int)strlen(party.name);

	message[size++] = (unsigned char)((leading() ? _state_leads : 0) | (party.listed ? _state_listed : 0));
	put_long(message + size, party.term);
	size += 4;
	memcpy(message + size, party.leader, MEMBER_ID_SIZE);
	size += MEMBER_ID_SIZE;
	put_short(message + size, HALO_PORT_NETWORK_VERSION);
	put_short(message + size + 2, HALO_PORT_STANDARD_NETWORK_VERSION);
	size += 4;
	message[size++] = party.place;
	memcpy(message + size, party.host, P2P_KEY_HASH_SIZE);
	size += P2P_KEY_HASH_SIZE;
	put_long(message + size, party.result_order);
	size += 4;
	message[size++] = party.result;
	message[size++] = (unsigned char)length;
	memcpy(message + size, party.name, (size_t)length);
	size += length;
	if (leading())
	{
		put_long(message + size, party.target_order);
		size += 4;
		message[size++] = (unsigned char)party.target_kind;
		if (party.target_kind == _p2p_party_game_invite)
		{
			memcpy(message + size, party.target_invite, INVITE_SIZE);
			size += INVITE_SIZE;
		}
		message[size++] = (unsigned char)party.roster_count;
		memcpy(message + size, party.roster, (size_t)party.roster_count * MEMBER_ID_SIZE);
		size += party.roster_count * MEMBER_ID_SIZE;
	}
	send_message(message, size);
	party.sent_time = p2p_now();
	party.send_wanted = 0;
}

static void send_leave(const unsigned char *successor)
{
	unsigned char message[MAXIMUM_PARTY_MESSAGE_SIZE];
	int size = message_header(message, _message_leave);

	memcpy(message + size, successor ? successor : no_id, MEMBER_ID_SIZE);
	send_message(message, size + MEMBER_ID_SIZE);
}

static void send_disband(void)
{
	unsigned char message[MAXIMUM_PARTY_MESSAGE_SIZE];

	send_message(message, message_header(message, _message_disband));
}

static void send_refuse(const unsigned char *id)
{
	unsigned char message[MAXIMUM_PARTY_MESSAGE_SIZE];
	int size = message_header(message, _message_refuse);

	memcpy(message + size, id, MEMBER_ID_SIZE);
	message[size + MEMBER_ID_SIZE] = _p2p_party_notice_full;
	send_message(message, size + MEMBER_ID_SIZE + 1);
}

/* ---------- leading */

/* this machine leads from now, in this term: its game is the party's (a new
order), itself first on its roster, then the others still heard, as they
were */
static void take_lead(unsigned long term)
{
	unsigned char roster[P2P_PARTY_MAXIMUM_MEMBERS][MEMBER_ID_SIZE];
	int count = 1;
	int index;

	party.term = term;
	memcpy(party.leader, party.id, MEMBER_ID_SIZE);
	party.leader_time = p2p_now();
	party.listed = 1;
	party.game_count++;
	party.target_order = party.term << 16 | (party.game_count & 0xFFFF);
	party.target_kind = party.game_kind;
	memcpy(party.target_invite, party.game_invite, INVITE_SIZE);
	memcpy(roster[0], party.id, MEMBER_ID_SIZE);
	for (index = 0; index < party.roster_count && count < P2P_PARTY_MAXIMUM_MEMBERS; index++)
	{
		if (!is_self(party.roster[index]) && find_member(party.roster[index]))
			memcpy(roster[count++], party.roster[index], MEMBER_ID_SIZE);
	}
	memcpy(party.roster, roster, sizeof(roster));
	party.roster_count = count;
	party.send_wanted = 1;
	p2p_log("Internet play: this machine leads the party now (term %lu)", party.term);
}

/* whether a leader's claim (its term and id) wins over the one followed */
static int claim_wins(unsigned long term, const unsigned char *id)
{
	if (!memcmp(party.leader, no_id, MEMBER_ID_SIZE))
		return 1;
	if (term != party.term)
		return term > party.term;
	return memcmp(id, party.leader, MEMBER_ID_SIZE) <= 0;
}

/* the leader's game changed: a new order */
static void leader_game_changed(void)
{
	if (!leading())
		return;
	party.game_count++;
	party.target_order = party.term << 16 | (party.game_count & 0xFFFF);
	party.target_kind = party.game_kind;
	memcpy(party.target_invite, party.game_invite, INVITE_SIZE);
	party.send_wanted = 1;
}

/* a new machine heard by the leader: listed, or turned away (the party is
full) */
static void leader_admit(struct member *member)
{
	if (roster_index(member->id) >= 0)
		return;
	if (party.roster_count >= P2P_PARTY_MAXIMUM_MEMBERS)
	{
		send_refuse(member->id);
		return;
	}
	memcpy(party.roster[party.roster_count++], member->id, MEMBER_ID_SIZE);
	party.send_wanted = 1;
}

/* the leader not heard: the first on its roster still heard leads (this
machine if it is that one, or if no one before it is) */
static void leader_lost(void)
{
	int index;

	for (index = 0; index < party.roster_count; index++)
	{
		const unsigned char *id = party.roster[index];
		struct member *member;

		if (!memcmp(id, party.leader, MEMBER_ID_SIZE))
			continue;
		if (is_self(id))
			break;
		member = find_member(id);
		if (member && !elapsed(member->heard_time, MEMBER_TIMEOUT))
		{
			/* (its STATE of the next term will say so, once it sees the
			same) */
			memcpy(party.leader, id, MEMBER_ID_SIZE);
			party.term++;
			party.leader_time = p2p_now();
			return;
		}
	}
	take_lead(party.term + 1);
	notice(_p2p_party_notice_leader, NULL, 0);
}

/* ---------- what is heard */

static struct member *member_for(const unsigned char *key, const unsigned char *id)
{
	struct member *member = find_member(id);
	int index;

	if (member)
		return member;
	for (index = 0; index < MAXIMUM_HEARD; index++)
	{
		if (!party.heard[index].used)
		{
			member = &party.heard[index];
			memset(member, 0, sizeof(*member));
			member->used = 1;
			memcpy(member->key, key, P2P_KEY_SIZE);
			memcpy(member->id, id, MEMBER_ID_SIZE);
			member->heard_time = p2p_now();
			return member;
		}
	}
	return NULL;
}

/* a leader's STATE (its game and roster from offset): followed, if it leads
the highest term */
static void leader_state_heard(struct member *member, unsigned long term, const unsigned char *body, int offset,
	int size)
{
	const unsigned char *invite = NULL;
	unsigned long order;
	int kind, count, index;

	if (offset + 6 > size)
		return;
	order = get_long(body + offset);
	kind = body[offset + 4];
	offset += 5;
	if (kind > _p2p_party_game_local)
		return;
	if (kind == _p2p_party_game_invite)
	{
		if (offset + INVITE_SIZE > size)
			return;
		invite = body + offset;
		offset += INVITE_SIZE;
	}
	count = body[offset++];
	if (count > P2P_PARTY_MAXIMUM_MEMBERS || offset + count * MEMBER_ID_SIZE != size)
		return;
	if (!claim_wins(term, member->id))
	{
		/* (a leader of an older term hears this one's STATE, and follows
		it) */
		if (leading())
			party.send_wanted = 1;
		return;
	}
	if (leading())
		p2p_log("Internet play: another machine leads the party (term %lu); this one follows it", term);
	if (memcmp(party.leader, member->id, MEMBER_ID_SIZE) && party.listed)
		notice(_p2p_party_notice_new_leader, member->name, 0);
	memcpy(party.leader, member->id, MEMBER_ID_SIZE);
	party.term = term;
	party.leader_time = p2p_now();
	party.target_order = order;
	party.target_kind = kind;
	if (invite)
		memcpy(party.target_invite, invite, INVITE_SIZE);
	else
		memset(party.target_invite, 0, INVITE_SIZE);
	party.roster_count = count;
	memcpy(party.roster, body + offset, (size_t)count * MEMBER_ID_SIZE);
	for (index = 0; index < count; index++)
	{
		struct member *listed = find_member(party.roster[index]);

		if (listed)
			listed->listed = 1;
	}
	if (roster_index(party.id) >= 0 && !party.listed)
	{
		party.listed = 1;
		party.send_wanted = 1;
		notice(_p2p_party_notice_joined, member->name, 0);
		p2p_log("Internet play: joined the party (%d members)", count);
	}
	else if (roster_index(party.id) < 0 && party.listed)
	{
		/* (no longer on the leader's roster: passed over while out of
		reach) asked to be listed again */
		party.listed = 0;
		party.joined_time = p2p_now();
		party.send_wanted = 1;
	}
}

static void state_heard(struct member *member, const unsigned char *body, int size)
{
	unsigned long term;
	int flags, offset, length;

	if (size < STATE_FIXED_SIZE + 1)
		return;
	flags = body[0];
	term = get_long(body + 1);
	offset = 5;
	if ((flags & _state_leads) && memcmp(body + offset, member->id, MEMBER_ID_SIZE))
		return;
	offset += MEMBER_ID_SIZE;
	member->network_version = get_short(body + offset);
	member->standard_version = get_short(body + offset + 2);
	offset += 4;
	member->place = body[offset] <= _p2p_party_place_local_game ? body[offset] : _p2p_party_place_menus;
	memcpy(member->host, body + offset + 1, P2P_KEY_HASH_SIZE);
	offset += 1 + P2P_KEY_HASH_SIZE;
	member->result_order = get_long(body + offset);
	member->result = body[offset + 4];
	offset += 5;
	length = body[offset++];
	if (offset + length > size)
		return;
	clean_name(member->name, (const char *)body + offset, length);
	offset += length;
	member->listed = (flags & _state_listed) != 0;
	if (flags & _state_leads)
		leader_state_heard(member, term, body, offset, size);
	else if (leading() && roster_index(member->id) < 0)
		leader_admit(member);
	if (!party.listed || roster_index(member->id) < 0)
		return;
	/* (what the menus are told of it, once each) */
	if (member->listed && !(member->announced & _announced_joined))
	{
		member->announced |= _announced_joined;
		if (memcmp(member->id, party.leader, MEMBER_ID_SIZE))
			notice(_p2p_party_notice_member_joined, member->name, 0);
	}
	if (member->network_version != HALO_PORT_NETWORK_VERSION && !(member->announced & _announced_version))
	{
		member->announced |= _announced_version;
		notice(_p2p_party_notice_version, member->name, 0);
	}
	if (member->result_order != party.target_order)
		member->announced &= (unsigned char)~_announced_follow;
	else if (member->result != _p2p_party_follow_joined && member->result != _p2p_party_follow_none &&
		member->result != _p2p_party_follow_busy && !(member->announced & _announced_follow))
	{
		member->announced |= _announced_follow;
		notice(_p2p_party_notice_follow_failed, member->name, member->result);
	}
}

static void leave_heard(struct member *member, const unsigned char *body, int size)
{
	int was_leader;

	if (size != MEMBER_ID_SIZE)
		return;
	was_leader = !memcmp(member->id, party.leader, MEMBER_ID_SIZE);
	if (party.listed)
		notice(_p2p_party_notice_member_left, member->name, 0);
	depart(member);
	if (!was_leader)
	{
		if (leading())
			party.send_wanted = 1;
		return;
	}
	/* the leader left, naming who leads next (or no one: the next on the
	roster still heard, as when it goes silent) */
	if (is_self(body))
	{
		take_lead(party.term + 1);
		notice(_p2p_party_notice_leader, NULL, 0);
	}
	else
	{
		struct member const *next = memcmp(body, no_id, MEMBER_ID_SIZE) ? find_member(body) : NULL;

		if (next)
		{
			/* (whom the next term's STATE will name) */
			memcpy(party.leader, next->id, MEMBER_ID_SIZE);
			party.term++;
			party.leader_time = p2p_now();
			if (party.listed)
				notice(_p2p_party_notice_new_leader, next->name, 0);
		}
		else if (party.listed)
			leader_lost();
	}
}

static void leave_party(int kind);

void p2p_party_heard(const unsigned char *payload, int size)
{
	unsigned char message[MAXIMUM_PARTY_MESSAGE_SIZE + P2P_SEAL_OVERHEAD];
	unsigned char data[P2P_SHA256_SIZE + MAXIMUM_PARTY_MESSAGE_SIZE];
	unsigned char id[MEMBER_ID_SIZE];
	unsigned long sequence, departed;
	struct member *member;
	const unsigned char *body;
	int body_size;

	if (!party.active || size > (int)sizeof(message))
		return;
	size = p2p_open(party.seal, payload, size, message);
	if (size < 3 || message[0] != PARTY_MAGIC)
		return;
	if (message[1] != PARTY_VERSION)
	{
		if (!party.other_version_noted)
		{
			party.other_version_noted = 1;
			p2p_log("Internet play: someone in the party has another version of the game (party format %d, "
				"this one's %d)", message[1], PARTY_VERSION);
			notice(_p2p_party_notice_version, NULL, 0);
		}
		return;
	}
	if (size < HEADER_SIZE + P2P_SIGNATURE_SIZE || size > MAXIMUM_PARTY_MESSAGE_SIZE)
		return;
	member_id(message + 3, id);
	/* (this machine's own, which a broker without MQTT 5's No Local sends
	back) */
	if (is_self(id))
		return;
	sequence = get_long(message + 3 + P2P_KEY_SIZE);
	member = find_member(id);
	if (member && sequence <= member->sequence)
		return;
	if (!member && departed_sequence(id, &departed) && sequence <= departed)
		return;
	/* the signature, within the budget */
	{
		unsigned long earned = (p2p_now() - party.checks_time) / CHECK_INTERVAL;

		if (earned)
		{
			party.checks_left = party.checks_left + (int)earned > MAXIMUM_CHECKS ? MAXIMUM_CHECKS :
				party.checks_left + (int)earned;
			party.checks_time += earned * CHECK_INTERVAL;
		}
		if (party.checks_left <= 0)
			return;
		party.checks_left--;
	}
	size -= P2P_SIGNATURE_SIZE;
	memcpy(data, party.context, P2P_SHA256_SIZE);
	memcpy(data + P2P_SHA256_SIZE, message, (size_t)size);
	if (!p2p_ed25519_verify(message + 3, data, P2P_SHA256_SIZE + size, message + size))
		return;
	body = message + HEADER_SIZE;
	body_size = size - HEADER_SIZE;
	switch (message[2])
	{
	case _message_state:
		member = member_for(message + 3, id);
		if (!member)
			return;
		member->sequence = sequence;
		member->heard_time = p2p_now();
		state_heard(member, body, body_size);
		break;
	case _message_leave:
		if (member)
		{
			member->sequence = sequence;
			leave_heard(member, body, body_size);
		}
		break;
	case _message_disband:
		if (member && !memcmp(id, party.leader, MEMBER_ID_SIZE) && !body_size)
		{
			p2p_log("Internet play: the party's leader ended the party");
			leave_party(_p2p_party_notice_disbanded);
		}
		break;
	case _message_refuse:
		if (member && !memcmp(id, party.leader, MEMBER_ID_SIZE) && body_size == MEMBER_ID_SIZE + 1 &&
			is_self(body) && !party.listed)
		{
			p2p_log("Internet play: the party is full");
			leave_party(_p2p_party_notice_full);
		}
		break;
	}
}

/* ---------- the p2p thread's pass */

void p2p_party_update(void)
{
	int index;

	if (!party.active)
		return;
	p2p_signal_start();
	if (!p2p_signal_connected())
	{
		party.was_connected = 0;
		return;
	}
	/* (back in reach: no one is timed out for the time it was not) */
	if (!party.was_connected)
	{
		party.was_connected = 1;
		for (index = 0; index < MAXIMUM_HEARD; index++)
		{
			if (party.heard[index].used)
				party.heard[index].heard_time = p2p_now();
		}
		party.leader_time = p2p_now();
		if (!party.listed)
			party.joined_time = p2p_now();
		party.send_wanted = 1;
	}
	for (index = 0; index < MAXIMUM_HEARD; index++)
	{
		struct member *member = &party.heard[index];

		if (member->used && elapsed(member->heard_time, MEMBER_TIMEOUT))
		{
			if (party.listed && roster_index(member->id) >= 0)
				notice(_p2p_party_notice_member_lost, member->name, 0);
			depart(member);
			if (leading())
				party.send_wanted = 1;
		}
	}
	if (!party.listed)
	{
		if (elapsed(party.joined_time, JOIN_WAIT))
		{
			p2p_log("Internet play: no answer from the party's leader; the party may have ended");
			leave_party(_p2p_party_notice_unreachable);
			return;
		}
	}
	else if (!leading() && elapsed(party.leader_time, MEMBER_TIMEOUT))
	{
		p2p_log("Internet play: the party's leader is not heard; the next on its roster leads");
		leader_lost();
	}
	if ((party.send_wanted && elapsed(party.sent_time, CHANGE_INTERVAL)) ||
		elapsed(party.sent_time, party.listed ? STATE_INTERVAL : JOINING_INTERVAL))
	{
		send_state();
	}
}

/* ---------- the game's side */

/* out of the party: what the game said of this machine, and the notices,
kept */
static void leave_party(int kind)
{
	char name[P2P_PARTY_NAME_SIZE];
	unsigned char invite[INVITE_SIZE], host[P2P_KEY_HASH_SIZE];
	unsigned char place = party.place;
	int game_kind = party.game_kind;
	struct p2p_party_notice notices[P2P_PARTY_MAXIMUM_NOTICES];
	unsigned long serial = party.notice_serial;
	int count = party.notice_count;

	memcpy(name, party.name, sizeof(name));
	memcpy(invite, party.game_invite, INVITE_SIZE);
	memcpy(host, party.host, P2P_KEY_HASH_SIZE);
	memcpy(notices, party.notices, sizeof(notices));
	memset(&party, 0, sizeof(party));
	memcpy(party.name, name, sizeof(name));
	memcpy(party.game_invite, invite, INVITE_SIZE);
	memcpy(party.host, host, P2P_KEY_HASH_SIZE);
	party.place = place;
	party.game_kind = game_kind;
	memcpy(party.notices, notices, sizeof(notices));
	party.notice_count = count;
	party.notice_serial = serial;
	if (kind)
		notice(kind, NULL, 0);
	p2p_signal_party("");
}

/* this machine leaves the party it is in (a leader names the next on its
roster still heard); urgent: the game is quitting, the brokers' buckets
passed over */
static void leave_locked(int kind, int urgent)
{
	const unsigned char *next = NULL;
	int index;

	if (!party.active)
		return;
	for (index = 0; leading() && index < party.roster_count && !next; index++)
	{
		struct member const *member = find_member(party.roster[index]);

		if (!is_self(party.roster[index]) && member && !elapsed(member->heard_time, MEMBER_TIMEOUT))
			next = party.roster[index];
	}
	party_urgent = urgent;
	/* (twice: the brokers carry each at most once) */
	send_leave(next);
	send_leave(next);
	party_urgent = 0;
	p2p_log("Internet play: left the party");
	leave_party(kind);
}

/* a party (its token) begun on this machine: the topic and keys */
static void begin(const unsigned char *token)
{
	char hex[2 * P2P_TOKEN_SIZE + 1];
	unsigned char digest[P2P_SHA256_SIZE];

	party.active = 1;
	memcpy(party.token, token, P2P_TOKEN_SIZE);
	p2p_hmac_sha256(token, P2P_TOKEN_SIZE, "party", 5, digest);
	p2p_hex(digest, 16, hex);
	snprintf(party.topic, sizeof(party.topic), "hceu/3/%s", hex);
	p2p_hmac_sha256(token, P2P_TOKEN_SIZE, "party seal", 10, party.seal);
	p2p_hmac_sha256(token, P2P_TOKEN_SIZE, "party id", 8, party.context);
	p2p_hex(token, P2P_TOKEN_SIZE, hex);
	snprintf(party.link, sizeof(party.link), "%s%s", P2P_PARTY_LINK_PREFIX, hex);
	memcpy(party.key, p2p_signing_key(), P2P_KEY_SIZE);
	member_id(party.key, party.id);
	party.joined_time = p2p_now();
	party.leader_time = p2p_now();
	party.checks_left = MAXIMUM_CHECKS;
	party.checks_time = p2p_now();
	party.send_wanted = 1;
	p2p_signal_party(party.topic);
}

int p2p_party_parse_link(const char *text, unsigned char *token)
{
	static const char *const prefixes[] = { P2P_PARTY_LINK_PREFIX, "?party=", "&party=" };
	const char *search;

	for (search = text; *search; search++)
	{
		int prefix;

		for (prefix = 0; prefix < (int)(sizeof(prefixes) / sizeof(*prefixes)); prefix++)
		{
			const char *start;
			int length, digits;

			for (length = 0; prefixes[prefix][length] && search[length] &&
				(search[length] | 0x20) == prefixes[prefix][length]; length++)
				;
			if (prefixes[prefix][length])
				continue;
			start = search + length;
			for (digits = 0; digits <= 2 * P2P_TOKEN_SIZE; digits++)
			{
				char digit = start[digits];

				if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f') ||
					(digit >= 'A' && digit <= 'F')))
				{
					break;
				}
			}
			if (digits != 2 * P2P_TOKEN_SIZE)
				return 0;
			for (digits = 0; digits < P2P_TOKEN_SIZE; digits++)
			{
				int high = start[2 * digits], low = start[2 * digits + 1];

				high = high <= '9' ? high - '0' : (high | 0x20) - 'a' + 10;
				low = low <= '9' ? low - '0' : (low | 0x20) - 'a' + 10;
				token[digits] = (unsigned char)(high << 4 | low);
			}
			return 1;
		}
	}
	return 0;
}

/* a party of this token made, this machine its leader */
static void lead(const unsigned char *token)
{
	leave_locked(0, 0);
	begin(token);
	take_lead(1);
	p2p_log("Internet play: made a party; friends join it with its link (it lasts while this copy of the game "
		"runs, or another member's)");
	notice(_p2p_party_notice_made, NULL, 0);
}

int p2p_party_make(void)
{
	unsigned char token[P2P_TOKEN_SIZE];

	pthread_mutex_lock(&p2p_lock);
	/* (a party of its own already, no one else in it: kept) */
	if (!leading() || party.roster_count > 1)
	{
		posix_random_bytes(token, sizeof(token));
		lead(token);
	}
	p2p_offer_clipboard(party.link);
	pthread_mutex_unlock(&p2p_lock);
	return 1;
}

int p2p_party_invite_link(char *link, int size)
{
	unsigned char token[P2P_TOKEN_SIZE];

	pthread_mutex_lock(&p2p_lock);
	if (!party.active)
	{
		posix_random_bytes(token, sizeof(token));
		lead(token);
	}
	snprintf(link, (size_t)size, "%s", party.link);
	pthread_mutex_unlock(&p2p_lock);
	return 1;
}

unsigned long p2p_party_tag(void)
{
	unsigned long tag = 0;
	const char *hex;
	int index;

	pthread_mutex_lock(&p2p_lock);
	/* (the topic's first digits, after "hceu/3/") */
	hex = strrchr(party.topic, '/');
	for (index = 0; party.active && hex && index < 8 && hex[1 + index]; index++)
	{
		char digit = hex[1 + index];

		tag = tag << 4 | (unsigned long)(digit <= '9' ? digit - '0' : (digit | 0x20) - 'a' + 10);
	}
	/* (never 0 in a party) */
	if (party.active && !tag)
		tag = 1;
	pthread_mutex_unlock(&p2p_lock);
	return tag;
}

int p2p_party_lead_link(const char *text)
{
	unsigned char token[P2P_TOKEN_SIZE];

	if (!p2p_party_parse_link(text, token))
		return 0;
	pthread_mutex_lock(&p2p_lock);
	lead(token);
	pthread_mutex_unlock(&p2p_lock);
	return 1;
}

int p2p_party_join_link_locked(const char *text)
{
	unsigned char token[P2P_TOKEN_SIZE];

	if (!p2p_party_parse_link(text, token))
		return 0;
	if (!party.active || memcmp(party.token, token, P2P_TOKEN_SIZE))
	{
		leave_locked(0, 0);
		begin(token);
		p2p_log("Internet play: joining a party");
	}
	return 1;
}

int p2p_party_join_link(const char *text)
{
	int joined;

	pthread_mutex_lock(&p2p_lock);
	joined = p2p_party_join_link_locked(text);
	pthread_mutex_unlock(&p2p_lock);
	return joined;
}

int p2p_party_active(void)
{
	return party.active;
}

void p2p_party_leave(void)
{
	pthread_mutex_lock(&p2p_lock);
	leave_locked(_p2p_party_notice_you_left, 0);
	pthread_mutex_unlock(&p2p_lock);
}

void p2p_party_disband(void)
{
	pthread_mutex_lock(&p2p_lock);
	if (leading())
	{
		/* (twice: the brokers carry each at most once) */
		send_disband();
		send_disband();
		p2p_log("Internet play: ended the party");
		leave_party(_p2p_party_notice_ended);
	}
	pthread_mutex_unlock(&p2p_lock);
}

void p2p_party_quit(void)
{
	leave_locked(0, 1);
}

int p2p_party_copy_link(void)
{
	int active;

	pthread_mutex_lock(&p2p_lock);
	active = party.active;
	if (active)
		p2p_offer_clipboard(party.link);
	pthread_mutex_unlock(&p2p_lock);
	return active;
}

void p2p_party_set_name(const char *name)
{
	char cleaned[P2P_PARTY_NAME_SIZE];

	clean_name(cleaned, name, (int)strlen(name));
	pthread_mutex_lock(&p2p_lock);
	if (strcmp(cleaned, party.name))
	{
		memcpy(party.name, cleaned, sizeof(cleaned));
		party.send_wanted = 1;
	}
	pthread_mutex_unlock(&p2p_lock);
}

void p2p_party_set_game(int place, int kind, const char *invite)
{
	unsigned char hash[P2P_KEY_HASH_SIZE], token[P2P_TOKEN_SIZE];
	unsigned char game_invite[INVITE_SIZE];

	memset(game_invite, 0, INVITE_SIZE);
	if (kind == _p2p_party_game_invite && invite && p2p_parse_invite(invite, hash, token) > 0)
	{
		memcpy(game_invite, hash, P2P_KEY_HASH_SIZE);
		memcpy(game_invite + P2P_KEY_HASH_SIZE, token, P2P_TOKEN_SIZE);
	}
	else if (kind == _p2p_party_game_invite)
	{
		kind = _p2p_party_game_local;
		if (place == _p2p_party_place_game)
			place = _p2p_party_place_local_game;
	}
	pthread_mutex_lock(&p2p_lock);
	if (party.game_kind != kind || memcmp(party.game_invite, game_invite, INVITE_SIZE))
	{
		party.game_kind = kind;
		memcpy(party.game_invite, game_invite, INVITE_SIZE);
		memcpy(party.host, game_invite, P2P_KEY_HASH_SIZE);
		party.send_wanted = 1;
		leader_game_changed();
	}
	if (party.place != place)
	{
		party.place = (unsigned char)place;
		party.send_wanted = 1;
	}
	pthread_mutex_unlock(&p2p_lock);
}

int p2p_party_follow(char *invite, int size, unsigned long *order)
{
	int kind = _p2p_party_game_none;

	pthread_mutex_lock(&p2p_lock);
	if (party.active && party.listed && !leading() && party.target_order &&
		party.target_order != party.handled_order && party.target_kind != _p2p_party_game_none &&
		!(party.target_kind == _p2p_party_game_invite && party.game_kind == _p2p_party_game_invite &&
		!memcmp(party.host, party.target_invite, P2P_KEY_HASH_SIZE)))
	{
		kind = party.target_kind;
		*order = party.target_order;
		if (kind == _p2p_party_game_invite)
		{
			char hex[2 * INVITE_SIZE + 1];

			p2p_hex(party.target_invite, INVITE_SIZE, hex);
			snprintf(invite, (size_t)size, "halo://join/%s", hex);
		}
		else if (size > 0)
			invite[0] = 0;
	}
	pthread_mutex_unlock(&p2p_lock);
	return kind;
}

void p2p_party_follow_result(unsigned long order, int result)
{
	pthread_mutex_lock(&p2p_lock);
	if (party.active && (party.result_order != order || party.result != result))
	{
		party.result_order = order;
		party.result = (unsigned char)result;
		party.send_wanted = 1;
	}
	if (result != _p2p_party_follow_busy && result != _p2p_party_follow_none)
		party.handled_order = order;
	pthread_mutex_unlock(&p2p_lock);
}

void p2p_party_follow_again(void)
{
	pthread_mutex_lock(&p2p_lock);
	party.handled_order = 0;
	pthread_mutex_unlock(&p2p_lock);
}

/* whether a member (or this machine: member NULL) is with the leader: in
its game, or in the menus with it */
static int with_leader(const struct member *member)
{
	int place = member ? member->place : party.place;
	const unsigned char *host = member ? member->host : party.host;

	if (member ? !memcmp(member->id, party.leader, MEMBER_ID_SIZE) : leading())
		return 1;
	switch (party.target_kind)
	{
	case _p2p_party_game_none:
		return place == _p2p_party_place_menus;
	case _p2p_party_game_invite:
		return place == _p2p_party_place_game && !memcmp(host, party.target_invite, P2P_KEY_HASH_SIZE);
	}
	return 0;
}

static void member_status(struct p2p_party_member *shown, const struct member *member)
{
	unsigned long result_order = member ? member->result_order : party.result_order;

	memset(shown, 0, sizeof(*shown));
	snprintf(shown->name, sizeof(shown->name), "%s", member ? member->name : party.name);
	shown->self = member == NULL;
	shown->leader = member ? !memcmp(member->id, party.leader, MEMBER_ID_SIZE) : leading();
	shown->place = member ? member->place : party.place;
	shown->with_leader = with_leader(member);
	shown->other_version = member && member->network_version != HALO_PORT_NETWORK_VERSION;
	shown->result = result_order == party.target_order ? (member ? member->result : party.result) :
		_p2p_party_follow_none;
}

void p2p_party_status(struct p2p_party_status *status)
{
	int index;

	memset(status, 0, sizeof(*status));
	pthread_mutex_lock(&p2p_lock);
	status->notice_count = party.notice_count;
	memcpy(status->notices, party.notices, sizeof(party.notices[0]) * (size_t)party.notice_count);
	if (!party.active)
	{
		pthread_mutex_unlock(&p2p_lock);
		return;
	}
	status->state = party.listed ? _p2p_party_member : _p2p_party_joining;
	status->leader = leading();
	status->target_kind = party.target_kind;
	snprintf(status->link, sizeof(status->link), "%s", party.link);
	/* (the roster's order, this machine where it is; first while it is not
	listed yet) */
	if (!party.listed)
		member_status(&status->members[status->count++], NULL);
	for (index = 0; index < party.roster_count && status->count < P2P_PARTY_MAXIMUM_MEMBERS; index++)
	{
		struct member const *member = find_member(party.roster[index]);

		if (is_self(party.roster[index]))
			member_status(&status->members[status->count++], NULL);
		else if (member)
			member_status(&status->members[status->count++], member);
	}
	for (index = 0; index < status->count; index++)
	{
		if (status->members[index].leader)
			snprintf(status->leader_name, sizeof(status->leader_name), "%s", status->members[index].name);
	}
	pthread_mutex_unlock(&p2p_lock);
}
