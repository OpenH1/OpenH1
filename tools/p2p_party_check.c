/*
P2P_PARTY_CHECK.C

A check of parties (port/linux/src/p2p_party.c), built by
tools/test_linux_port.py (with p2p_party.c mutated for its negative
controls): several machines in this one process, each with its own party
state (swapped in and out of p2p_party.c's) and its own Ed25519 key,
talking through a stand-in for the signalling brokers that delivers each
message published on a topic to every other machine subscribed to it, but
to a machine out of reach (whose own messages are lost too). The cases,
each run by its name:

- make: a party made, its link (halo://party/ and 32 hex digits) on the
  clipboard and parsed back, this machine its leader alone;
- join: two machines join by the link; every machine lists the three in
  the order they joined, the maker leading; the notices say so;
- follow: the leader hosts (an internet game, its invite): each member is
  asked once to join it, with the leader's invite; one says it joined (and
  is shown with the leader), one that it could not (the game was full),
  which every machine is told, and which stays in the party; the leader's
  next game asks again;
- busy: a member in another game is asked again (busy is no answer) until
  it is back in the menus;
- local: a leader in a LAN game: its members are told it cannot be
  followed, once;
- leave: a member leaves; the others list it no longer;
- handoff: the leader leaves, naming the next on its roster, which leads at
  once (a term higher), and takes a new member;
- timeout: the leader goes silent; after MEMBER_TIMEOUT the next on its
  roster still heard leads, and the others follow it; the old leader, back,
  steps down;
- regroup: after the leader's game ends (the menus), its members still in
  that game are not with it, are asked nothing, and follow its next game;
- disband: the leader ends the party: no one is in it;
- full: a party of P2P_PARTY_MAXIMUM_MEMBERS turns a seventeenth away;
- joinless: a link no one answers is given up after JOIN_WAIT;
- format: a STATE as sent (its fields at their places, its size within
  the brokers' 512 bytes), and what is turned away: a message altered,
  sealed with another party's key, signed by a key not its sender's, of
  another format (noted once), sent again (replayed), from a member gone,
  of a leader with an older term, and a malformed one;
- version: a member of another HALO_PORT_NETWORK_VERSION is shown so;
- keys: the leader names its other members by their tunnel keys' hashes
  (p2p_party_member_key_hashes, which the host of a game they join holds
  room by): each the hash of the X25519 key the machine reaches a host with,
  as p2p.c makes it from its seed, in the roster's order; a member of
  another version left out; none named by a member, nor by a leader alone;
- host: hosting for the party (matchmaking): each member tells how well
  it can host now; the leader passes the lead to the member that can host
  best, if better than itself (p2p_party_pass_host), which then leads (a
  term higher) and is asked to host the playlist once
  (p2p_party_take_host_request), the others following it; none passed when
  no member can host better, to a member that cannot host any more, nor by
  a member; a STATE without the byte (an older build's) is a member unable
  to host;
- spare: the party Discord offers out of a party (p2p_party_spare): the
  same link and id until it is made, its id the party's topic's digits (not
  the link's); a friend with its link is not answered while its topic is not
  listened on, and is, its party made, this machine its leader, while it is
  (p2p_party_standby); let in, it is made (p2p_party_spare_lead); MAKE
  PARTY makes it; the next is another.
Prints PASS or the failures.
*/

#include "p2p_internal.h"
#include "halo_port_limits.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the party's code, its state reachable here (P2P_PARTY_SOURCE: a mutated
copy, for the negative controls) */
#ifndef P2P_PARTY_SOURCE
#define P2P_PARTY_SOURCE "p2p_party.c"
#endif
#include P2P_PARTY_SOURCE

/* ---------- stand-ins for the rest of internet play */

pthread_mutex_t p2p_lock = PTHREAD_MUTEX_INITIALIZER;

enum
{
	MAXIMUM_MACHINES = P2P_PARTY_MAXIMUM_MEMBERS + 2,
	MAXIMUM_QUEUED = 4096,
	MAXIMUM_PAYLOAD = 600,
};

struct machine
{
	/* its party state, while another's is p2p_party.c's */
	unsigned char state[sizeof(party)];
	unsigned char seed[P2P_SEED_SIZE];
	unsigned char key[P2P_KEY_SIZE];
	char topic[64];
	char clipboard[128];
	/* out of reach: neither updated nor sent to (its messages lost) */
	int away;
	/* the messages it has published */
	int published;
};

struct queued
{
	int from;
	char topic[64];
	int size;
	unsigned char payload[MAXIMUM_PAYLOAD];
};

static struct machine machines[MAXIMUM_MACHINES];
static int machine_count;
static int current = -1;
static unsigned long clock_now = 100000;
static struct queued queue[MAXIMUM_QUEUED];
static int queue_count;
static int verbose;
static int random_counter;
/* the last message published (the format case reads it) */
static unsigned char last_sealed[MAXIMUM_PAYLOAD];
static int last_sealed_size;
static int brokers_up = 1;

unsigned long p2p_now(void) { return clock_now; }

void p2p_log(const char *format, ...)
{
	va_list arguments;

	if (!verbose)
		return;
	va_start(arguments, format);
	printf("  [%c] ", current >= 0 ? 'A' + current : '-');
	vprintf(format, arguments);
	printf("\n");
	va_end(arguments);
}

void p2p_hex(const unsigned char *bytes, int size, char *text)
{
	int index;

	for (index = 0; index < size; index++)
		sprintf(text + 2 * index, "%02x", bytes[index]);
	text[2 * size] = 0;
}

void posix_random_bytes(void *buffer, posix_ulong size)
{
	unsigned char *bytes = buffer;
	posix_ulong index;

	random_counter++;
	for (index = 0; index < size; index++)
		bytes[index] = (unsigned char)(random_counter * 37 + index * 11 + 5);
}

const unsigned char *p2p_signing_key(void) { return machines[current].key; }

void p2p_key_hash(const unsigned char *key, unsigned char *hash)
{
	unsigned char digest[P2P_SHA256_SIZE];

	p2p_sha256(key, P2P_KEY_SIZE, digest);
	memcpy(hash, digest, P2P_KEY_HASH_SIZE);
}

void p2p_sign(const void *message, int size, unsigned char *signature)
{
	p2p_ed25519_sign(machines[current].seed, machines[current].key, message, size, signature);
}

void p2p_signal_start(void) {}
int p2p_signal_connected(void) { return brokers_up; }

void p2p_signal_party(const char *topic)
{
	snprintf(machines[current].topic, sizeof(machines[current].topic), "%s", topic);
}

void p2p_signal_party_publish(const unsigned char *payload, int size, int urgent)
{
	(void)urgent;
	if (size > MAXIMUM_PAYLOAD || !machines[current].topic[0])
		return;
	memcpy(last_sealed, payload, (size_t)size);
	last_sealed_size = size;
	machines[current].published++;
	if (queue_count == MAXIMUM_QUEUED || machines[current].away)
		return;
	queue[queue_count].from = current;
	snprintf(queue[queue_count].topic, sizeof(queue[queue_count].topic), "%s", machines[current].topic);
	queue[queue_count].size = size;
	memcpy(queue[queue_count].payload, payload, (size_t)size);
	queue_count++;
}

void p2p_offer_clipboard(const char *text)
{
	snprintf(machines[current].clipboard, sizeof(machines[current].clipboard), "%s", text);
}

int p2p_parse_invite(const char *text, unsigned char *host_hash, unsigned char *token)
{
	const char *start = strstr(text, "halo://join/");
	int index;

	if (!start || strlen(start + 12) < 2 * (P2P_KEY_HASH_SIZE + P2P_TOKEN_SIZE))
		return 0;
	start += 12;
	for (index = 0; index < P2P_KEY_HASH_SIZE + P2P_TOKEN_SIZE; index++)
	{
		unsigned int byte;

		if (sscanf(start + 2 * index, "%2x", &byte) != 1)
			return 0;
		if (index < P2P_KEY_HASH_SIZE)
			host_hash[index] = (unsigned char)byte;
		else
			token[index - P2P_KEY_HASH_SIZE] = (unsigned char)byte;
	}
	return 1;
}

/* ---------- the machines */

static void switch_to(int index)
{
	if (index == current)
		return;
	if (current >= 0)
		memcpy(machines[current].state, &party, sizeof(party));
	memcpy(&party, machines[index].state, sizeof(party));
	current = index;
}

static int add_machine(void)
{
	struct machine *machine = &machines[machine_count];
	int index;

	memset(machine, 0, sizeof(*machine));
	for (index = 0; index < P2P_SEED_SIZE; index++)
		machine->seed[index] = (unsigned char)(machine_count * 17 + index + 1);
	p2p_ed25519_public(machine->seed, machine->key, NULL);
	return machine_count++;
}

/* each message queued to the machines subscribed to its topic, but its
sender and those away */
static void deliver(void)
{
	int read;

	for (read = 0; read < queue_count; read++)
	{
		int index;

		for (index = 0; index < machine_count; index++)
		{
			if (index == queue[read].from || machines[index].away || strcmp(machines[index].topic, queue[read].topic))
				continue;
			switch_to(index);
			p2p_party_heard(queue[read].payload, queue[read].size);
		}
	}
	queue_count = 0;
}

/* time passes, each machine's p2p thread passing every 50 ms */
static void run(unsigned long milliseconds)
{
	unsigned long end = clock_now + milliseconds;

	while (clock_now < end)
	{
		int index;

		clock_now += 50;
		for (index = 0; index < machine_count; index++)
		{
			if (machines[index].away)
				continue;
			switch_to(index);
			p2p_party_update();
		}
		deliver();
	}
}

static void status_of(int index, struct p2p_party_status *status)
{
	switch_to(index);
	p2p_party_status(status);
}

static void name(int index, const char *text)
{
	switch_to(index);
	p2p_party_set_name(text);
}

static void game(int index, int place, int kind, const char *invite)
{
	switch_to(index);
	p2p_party_set_game(place, kind, invite);
}

/* the latest notice of a kind on a machine, NULL if none */
static const struct p2p_party_notice *notice_of(int index, int kind)
{
	static struct p2p_party_status status;
	int entry;

	status_of(index, &status);
	for (entry = status.notice_count - 1; entry >= 0; entry--)
	{
		if (status.notices[entry].kind == kind)
			return &status.notices[entry];
	}
	return NULL;
}

/* the member of a name in a machine's list, NULL if none */
static const struct p2p_party_member *member_named(int index, const char *text)
{
	static struct p2p_party_status status;
	int entry;

	status_of(index, &status);
	for (entry = 0; entry < status.count; entry++)
	{
		if (!strcmp(status.members[entry].name, text))
			return &status.members[entry];
	}
	return NULL;
}

/* ---------- the checks */

static int failures;

static void check(int good, const char *what)
{
	if (!good)
	{
		failures++;
		printf("FAIL: %s\n", what);
	}
}

static const char *invite_a = "halo://join/0123456789abcdef0123456789abcdef00112233445566778899aabbccddeeff";
static const char *invite_b = "halo://join/fedcba9876543210fedcba9876543210ffeeddccbbaa99887766554433221100";

/* a party of count machines, the first its leader, named A, B, C...: the
others joined by its link, in order */
static void party_of(int count)
{
	char link[128];
	int index;

	for (index = 0; index < count; index++)
	{
		char text[2] = { (char)('A' + index), 0 };

		add_machine();
		name(index, text);
	}
	switch_to(0);
	p2p_party_make();
	snprintf(link, sizeof(link), "%s", machines[0].clipboard);
	run(500);
	for (index = 1; index < count; index++)
	{
		switch_to(index);
		check(p2p_party_join_link(link), "a party's link joins its party");
		run(1500);
	}
	run(1000);
}

static void case_make(void)
{
	struct p2p_party_status status;
	unsigned char token[P2P_TOKEN_SIZE];

	add_machine();
	name(0, "Keyes");
	switch_to(0);
	check(p2p_party_make(), "a party is made");
	check(!strncmp(machines[0].clipboard, "halo://party/", 13) && strlen(machines[0].clipboard) == 13 + 32,
		"its link is halo://party/ and 32 hex digits, on the clipboard");
	check(p2p_party_parse_link(machines[0].clipboard, token), "its link is a party's");
	check(p2p_party_parse_link("join me: https://example.org/?party=00112233445566778899aabbccddeeff ok", token) &&
		token[15] == 0xff, "a link's ?party= is taken in other text");
	check(!p2p_party_parse_link("halo://party/0011", token), "a short token is no link");
	check(!p2p_party_parse_link("halo://party/00112233445566778899aabbccddeeff00", token),
		"a long token is no link");
	check(!p2p_party_parse_link(invite_a, token), "a game's invite is no party's link");
	status_of(0, &status);
	check(status.state == _p2p_party_member && status.leader && status.count == 1 && status.members[0].self &&
		status.members[0].leader && !strcmp(status.members[0].name, "Keyes"), "the maker leads it alone");
	check(!strcmp(status.link, machines[0].clipboard), "its status has its link");
	check(notice_of(0, _p2p_party_notice_made) != NULL, "the menus are told it was made");
	run(5000);
	status_of(0, &status);
	check(status.count == 1 && status.leader, "alone, it stays its leader");
	/* (a second MAKE PARTY keeps it, and copies the link again) */
	{
		char before[128];

		snprintf(before, sizeof(before), "%s", status.link);
		machines[0].clipboard[0] = 0;
		switch_to(0);
		p2p_party_make();
		status_of(0, &status);
		check(!strcmp(status.link, before) && !strcmp(machines[0].clipboard, before),
			"making a party again keeps the one it leads alone");
	}
}

static void case_join(void)
{
	struct p2p_party_status status;
	int index;

	party_of(3);
	for (index = 0; index < 3; index++)
	{
		status_of(index, &status);
		check(status.state == _p2p_party_member, "every machine is a member");
		check(status.count == 3, "every machine lists the three");
		check(status.count == 3 && !strcmp(status.members[0].name, "A") && !strcmp(status.members[1].name, "B") &&
			!strcmp(status.members[2].name, "C"), "in the order they joined");
		check(status.members[0].leader && !status.members[1].leader && !status.members[2].leader,
			"the maker leads");
		check(status.leader == (index == 0), "only the maker is told it leads");
		check(!strcmp(status.leader_name, "A"), "the leader's name");
	}
	check(notice_of(1, _p2p_party_notice_joined) && !strcmp(notice_of(1, _p2p_party_notice_joined)->name, "A"),
		"a joiner is told whose party it joined");
	check(notice_of(0, _p2p_party_notice_member_joined) != NULL, "the leader is told of the joiners");
	/* (joining it again changes nothing) */
	switch_to(1);
	check(p2p_party_join_link(machines[0].clipboard), "the link again");
	run(2000);
	status_of(1, &status);
	check(status.count == 3 && status.state == _p2p_party_member, "joining one's own party again changes nothing");
}

static void case_follow(void)
{
	char invite[160];
	unsigned long order = 0, first_order;
	int kind;

	party_of(3);
	switch_to(1);
	check(p2p_party_follow(invite, sizeof(invite), &order) == _p2p_party_game_none,
		"nothing to follow while the leader is in the menus");
	game(0, _p2p_party_place_game, _p2p_party_game_invite, invite_a);
	run(1000);
	switch_to(1);
	kind = p2p_party_follow(invite, sizeof(invite), &order);
	check(kind == _p2p_party_game_invite && !strcmp(invite, invite_a), "a member is asked to join the leader's game");
	first_order = order;
	switch_to(2);
	check(p2p_party_follow(invite, sizeof(invite), &order) == _p2p_party_game_invite && order == first_order,
		"each member is");
	/* B joins it; C finds it full */
	game(1, _p2p_party_place_joining, _p2p_party_game_none, NULL);
	run(300);
	game(1, _p2p_party_place_game, _p2p_party_game_invite, invite_a);
	switch_to(1);
	p2p_party_follow_result(first_order, _p2p_party_follow_joined);
	switch_to(2);
	p2p_party_follow_result(first_order, _p2p_party_follow_full);
	run(1500);
	switch_to(1);
	check(p2p_party_follow(invite, sizeof(invite), &order) == _p2p_party_game_none,
		"a member in the leader's game is asked nothing more");
	switch_to(2);
	check(p2p_party_follow(invite, sizeof(invite), &order) == _p2p_party_game_none,
		"a member is asked once for each of the leader's games");
	check(member_named(0, "B") && member_named(0, "B")->with_leader, "the leader sees B with it");
	check(member_named(0, "C") && !member_named(0, "C")->with_leader &&
		member_named(0, "C")->result == _p2p_party_follow_full, "and why C is not");
	check(member_named(1, "C") && member_named(1, "C")->result == _p2p_party_follow_full, "B sees why too");
	check(notice_of(0, _p2p_party_notice_follow_failed) &&
		!strcmp(notice_of(0, _p2p_party_notice_follow_failed)->name, "C") &&
		notice_of(0, _p2p_party_notice_follow_failed)->result == _p2p_party_follow_full,
		"the leader is told C could not join, and why");
	{
		struct p2p_party_status status;

		status_of(2, &status);
		check(status.state == _p2p_party_member && status.count == 3, "a member that could not join stays");
	}
	/* the leader's next game: asked again */
	game(0, _p2p_party_place_game, _p2p_party_game_invite, invite_b);
	run(1000);
	switch_to(2);
	check(p2p_party_follow(invite, sizeof(invite), &order) == _p2p_party_game_invite && order != first_order &&
		!strcmp(invite, invite_b), "the leader's next game is followed");
	switch_to(1);
	check(p2p_party_follow(invite, sizeof(invite), &order) == _p2p_party_game_invite && !strcmp(invite, invite_b),
		"by those in its last game too");
	/* (JOIN LEADER: asked again) */
	p2p_party_follow_result(order, _p2p_party_follow_unreachable);
	check(p2p_party_follow(invite, sizeof(invite), &order) == _p2p_party_game_none, "answered");
	p2p_party_follow_again();
	check(p2p_party_follow(invite, sizeof(invite), &order) == _p2p_party_game_invite, "asked again on request");
}

static void case_busy(void)
{
	char invite[160];
	unsigned long order = 0;

	party_of(2);
	game(1, _p2p_party_place_game, _p2p_party_game_invite, invite_b);
	game(0, _p2p_party_place_game, _p2p_party_game_invite, invite_a);
	run(1000);
	switch_to(1);
	check(p2p_party_follow(invite, sizeof(invite), &order) == _p2p_party_game_invite, "asked while in another game");
	p2p_party_follow_result(order, _p2p_party_follow_busy);
	run(1000);
	check(member_named(0, "B") && member_named(0, "B")->result == _p2p_party_follow_busy,
		"the leader sees B is in another game");
	check(!notice_of(0, _p2p_party_notice_follow_failed), "busy is no failure");
	switch_to(1);
	check(p2p_party_follow(invite, sizeof(invite), &order) == _p2p_party_game_invite, "busy is no answer: asked again");
	game(1, _p2p_party_place_menus, _p2p_party_game_none, NULL);
	run(500);
	switch_to(1);
	check(p2p_party_follow(invite, sizeof(invite), &order) == _p2p_party_game_invite && !strcmp(invite, invite_a),
		"and once back in the menus");
}

static void case_local(void)
{
	char invite[160];
	unsigned long order = 0;

	party_of(2);
	game(0, _p2p_party_place_local_game, _p2p_party_game_local, NULL);
	run(1000);
	switch_to(1);
	check(p2p_party_follow(invite, sizeof(invite), &order) == _p2p_party_game_local,
		"a member is told its leader's game is one no link reaches");
	p2p_party_follow_result(order, _p2p_party_follow_local);
	check(p2p_party_follow(invite, sizeof(invite), &order) == _p2p_party_game_none, "once");
	run(1000);
	check(notice_of(0, _p2p_party_notice_follow_failed) &&
		notice_of(0, _p2p_party_notice_follow_failed)->result == _p2p_party_follow_local,
		"the leader is told");
	/* (an invite that is none: a LAN game) */
	game(0, _p2p_party_place_game, _p2p_party_game_invite, "no invite here");
	run(1000);
	switch_to(1);
	check(p2p_party_follow(invite, sizeof(invite), &order) == _p2p_party_game_none ||
		p2p_party_follow(invite, sizeof(invite), &order) == _p2p_party_game_local,
		"an internet game without an invite is a local one");
}

static void case_leave(void)
{
	struct p2p_party_status status;

	party_of(3);
	switch_to(2);
	p2p_party_leave();
	status_of(2, &status);
	check(status.state == _p2p_party_none && machines[2].topic[0] == 0, "a member that leaves is in no party");
	check(notice_of(2, _p2p_party_notice_you_left) != NULL, "and is told so");
	run(1000);
	status_of(0, &status);
	check(status.count == 2 && !member_named(0, "C"), "the leader lists it no longer");
	status_of(1, &status);
	check(status.count == 2 && !member_named(1, "C"), "nor the others");
	check(notice_of(1, _p2p_party_notice_member_left) && !strcmp(notice_of(1, _p2p_party_notice_member_left)->name, "C"),
		"who are told it left");
	run(20000);
	status_of(0, &status);
	check(status.count == 2, "and it does not come back");
}

static void case_handoff(void)
{
	struct p2p_party_status status;
	unsigned long term;

	party_of(3);
	switch_to(1);
	term = party.term;
	switch_to(0);
	p2p_party_leave();
	run(300);
	status_of(1, &status);
	check(status.leader && status.members[0].self && status.members[0].leader, "the next on the roster leads at once");
	switch_to(1);
	check(party.term == term + 1, "a term higher");
	check(notice_of(1, _p2p_party_notice_leader) != NULL, "and is told it leads");
	status_of(2, &status);
	check(!status.leader && status.count == 2 && !strcmp(status.leader_name, "B"), "the others follow it");
	check(notice_of(2, _p2p_party_notice_new_leader) && !strcmp(notice_of(2, _p2p_party_notice_new_leader)->name, "B"),
		"and are told who leads");
	/* a new member, which the new leader takes */
	{
		int joiner = add_machine();

		name(joiner, "D");
		switch_to(joiner);
		check(p2p_party_join_link(machines[0].clipboard), "the first leader's link still joins the party");
		run(2000);
		status_of(joiner, &status);
		check(status.state == _p2p_party_member && status.count == 3 && !strcmp(status.leader_name, "B"),
			"the new leader takes a new member");
	}
	run(30000);
	status_of(2, &status);
	check(status.count == 3 && !strcmp(status.leader_name, "B"), "and it holds");
}

static void case_timeout(void)
{
	struct p2p_party_status status;

	party_of(4);
	machines[0].away = 1;
	run(MEMBER_TIMEOUT + 2000);
	status_of(1, &status);
	check(status.leader && status.count == 3, "after the leader's silence the next on the roster leads");
	status_of(2, &status);
	check(!status.leader && !strcmp(status.leader_name, "B") && status.count == 3, "the others follow it");
	status_of(3, &status);
	check(!status.leader && !strcmp(status.leader_name, "B"), "all of them");
	check(notice_of(3, _p2p_party_notice_member_lost) &&
		!strcmp(notice_of(3, _p2p_party_notice_member_lost)->name, "A"), "who are told the leader was lost");
	/* the old leader, back in reach (its brokers had dropped it, as it
	thinks): it hears a higher term, and follows */
	machines[0].away = 0;
	run(8000);
	status_of(0, &status);
	check(status.state == _p2p_party_member && !status.leader && !strcmp(status.leader_name, "B"),
		"the old leader, back, steps down");
	status_of(1, &status);
	check(status.leader && status.count == 4, "and the new one lists it again");
}

static void case_regroup(void)
{
	char invite[160];
	unsigned long order = 0;

	party_of(3);
	game(0, _p2p_party_place_game, _p2p_party_game_invite, invite_a);
	run(1000);
	game(1, _p2p_party_place_game, _p2p_party_game_invite, invite_a);
	game(2, _p2p_party_place_game, _p2p_party_game_invite, invite_a);
	switch_to(1);
	p2p_party_follow(invite, sizeof(invite), &order);
	p2p_party_follow_result(order, _p2p_party_follow_joined);
	switch_to(2);
	p2p_party_follow(invite, sizeof(invite), &order);
	p2p_party_follow_result(order, _p2p_party_follow_joined);
	run(1000);
	check(member_named(0, "B")->with_leader && member_named(0, "C")->with_leader, "all in the leader's game");
	/* the game ends for the leader (its host left): the menus */
	game(0, _p2p_party_place_menus, _p2p_party_game_none, NULL);
	run(1000);
	switch_to(1);
	check(p2p_party_follow(invite, sizeof(invite), &order) == _p2p_party_game_none,
		"the leader in the menus: nothing to follow");
	check(!member_named(0, "B")->with_leader, "a member still in that game is not with the leader");
	game(1, _p2p_party_place_menus, _p2p_party_game_none, NULL);
	game(2, _p2p_party_place_menus, _p2p_party_game_none, NULL);
	run(1000);
	check(member_named(0, "B")->with_leader && member_named(0, "C")->with_leader, "back together in the menus");
	{
		struct p2p_party_status status;

		status_of(2, &status);
		check(status.count == 3 && status.state == _p2p_party_member, "the party is whole after the game");
	}
	/* the leader takes it on */
	game(0, _p2p_party_place_game, _p2p_party_game_invite, invite_b);
	run(1000);
	switch_to(2);
	check(p2p_party_follow(invite, sizeof(invite), &order) == _p2p_party_game_invite && !strcmp(invite, invite_b),
		"the leader's next game is followed");
}

static void case_disband(void)
{
	struct p2p_party_status status;
	int index;

	party_of(3);
	switch_to(1);
	p2p_party_disband();
	status_of(1, &status);
	check(status.state == _p2p_party_member, "a member cannot end the party");
	switch_to(0);
	p2p_party_disband();
	run(500);
	for (index = 0; index < 3; index++)
	{
		status_of(index, &status);
		check(status.state == _p2p_party_none, "after the leader ends it, no one is in the party");
	}
	check(notice_of(0, _p2p_party_notice_ended) != NULL, "the leader is told it ended it");
	check(notice_of(1, _p2p_party_notice_disbanded) != NULL, "the others that the leader did");
}

static void case_full(void)
{
	struct p2p_party_status status;
	int extra;

	party_of(P2P_PARTY_MAXIMUM_MEMBERS);
	status_of(0, &status);
	check(status.count == P2P_PARTY_MAXIMUM_MEMBERS, "a party of the most members");
	extra = add_machine();
	name(extra, "Q");
	switch_to(extra);
	p2p_party_join_link(machines[0].clipboard);
	run(3000);
	status_of(extra, &status);
	check(status.state == _p2p_party_none, "one more is turned away");
	check(notice_of(extra, _p2p_party_notice_full) != NULL, "and told the party is full");
	status_of(0, &status);
	check(status.count == P2P_PARTY_MAXIMUM_MEMBERS, "the party is as it was");
	status_of(5, &status);
	check(status.count == P2P_PARTY_MAXIMUM_MEMBERS, "for every member");
}

static void case_joinless(void)
{
	struct p2p_party_status status;

	add_machine();
	switch_to(0);
	p2p_party_join_link("halo://party/00112233445566778899aabbccddeeff");
	status_of(0, &status);
	check(status.state == _p2p_party_joining && status.count == 1 && status.members[0].self,
		"a joiner shows itself while it joins");
	run(JOIN_WAIT - 2000);
	status_of(0, &status);
	check(status.state == _p2p_party_joining, "it waits for the leader");
	run(3000);
	status_of(0, &status);
	check(status.state == _p2p_party_none && notice_of(0, _p2p_party_notice_unreachable) != NULL,
		"and gives up, saying so");
	/* (out of the brokers' reach meanwhile: it waits on) */
	switch_to(0);
	p2p_party_join_link("halo://party/00112233445566778899aabbccddeeff");
	brokers_up = 0;
	run(JOIN_WAIT + 5000);
	brokers_up = 1;
	run(1000);
	status_of(0, &status);
	check(status.state == _p2p_party_joining, "the wait starts again once the brokers are back");
}

/* a STATE's plaintext, sealed by the machine (last_sealed) */
static int opened_state(int index, unsigned char *message)
{
	switch_to(index);
	send_state();
	return p2p_open(party.seal, last_sealed, last_sealed_size, message);
}

/* a message made by a machine, sealed with its party's key: its plaintext
as given, signed with a seed (NULL: its own) */
static int sealed_message(int index, const unsigned char *message, int size, const unsigned char *seed,
	unsigned char *sealed)
{
	unsigned char data[P2P_SHA256_SIZE + 600];
	unsigned char signed_message[600];
	unsigned char key[P2P_KEY_SIZE];

	switch_to(index);
	if (seed)
		p2p_ed25519_public(seed, key, NULL);
	memcpy(signed_message, message, (size_t)size);
	memcpy(data, party.context, P2P_SHA256_SIZE);
	memcpy(data + P2P_SHA256_SIZE, message, (size_t)size);
	p2p_ed25519_sign(seed ? seed : machines[index].seed, seed ? key : machines[index].key, data,
		P2P_SHA256_SIZE + size, signed_message + size);
	return p2p_seal(party.seal, signed_message, size + P2P_SIGNATURE_SIZE, sealed);
}

static void case_format(void)
{
	unsigned char message[600], sealed[700];
	int size, sealed_size, offset;
	unsigned long sequence;
	struct p2p_party_status status;

	party_of(3);
	game(0, _p2p_party_place_game, _p2p_party_game_invite, invite_a);
	run(500);
	/* a leader's STATE, field by field */
	size = opened_state(0, message);
	check(size > 0, "a STATE opens with its party's key");
	check(size + P2P_SEAL_OVERHEAD <= 512 + P2P_SEAL_OVERHEAD && size <= MAXIMUM_PARTY_MESSAGE_SIZE,
		"within the brokers' 512 bytes");
	check(message[0] == 'P' && message[1] == 1 && message[2] == 'S', "\"P\", format 1, STATE");
	check(!memcmp(message + 3, machines[0].key, P2P_KEY_SIZE), "the sender's key");
	sequence = get_long(message + 35);
	offset = HEADER_SIZE;
	check(message[offset] == (_state_leads | _state_listed), "its flags: it leads, listed");
	check(get_long(message + offset + 1) == 1, "its term");
	{
		unsigned char id[MEMBER_ID_SIZE];

		member_id(machines[0].key, id);
		check(!memcmp(message + offset + 5, id, MEMBER_ID_SIZE), "the leader's id: its key's hash's first 8 bytes");
	}
	check(get_short(message + offset + 13) == HALO_PORT_NETWORK_VERSION &&
		get_short(message + offset + 15) == HALO_PORT_STANDARD_NETWORK_VERSION, "its versions");
	check(message[offset + 17] == _p2p_party_place_game, "where it is");
	check(message[offset + 18] == 0x01 && message[offset + 18 + 15] == 0xef, "its game's host (the invite's hash)");
	offset += STATE_FIXED_SIZE;
	check(message[offset] == 1 && message[offset + 1] == 'A', "its name");
	offset += 2;
	check(message[offset + 4] == _p2p_party_game_invite && message[offset + 5] == 0x01 &&
		message[offset + 5 + 16] == 0x00 && message[offset + 5 + 31] == 0xff, "its game: an invite, hash and token");
	offset += 5 + INVITE_SIZE;
	check(message[offset] == 3 && offset + 1 + 3 * MEMBER_ID_SIZE + P2P_SIGNATURE_SIZE == size,
		"its roster of three, then the signature");
	/* a member's: no game nor roster */
	size = opened_state(1, message);
	check(message[HEADER_SIZE] == _state_listed && size == HEADER_SIZE + STATE_FIXED_SIZE + 3 + P2P_SIGNATURE_SIZE,
		"a member's STATE has no game nor roster");
	check(message[size - P2P_SIGNATURE_SIZE - 1] == _p2p_party_host_unable,
		"but how well it can host, after its name (not told yet: unable)");

	/* what is turned away (B listening) */
	switch_to(0);
	size = opened_state(0, message) - P2P_SIGNATURE_SIZE;
	put_long(message + 35, sequence + 1000);
	/* altered after sealing */
	sealed_size = sealed_message(0, message, size, NULL, sealed);
	sealed[sealed_size - 5] ^= 1;
	switch_to(1);
	{
		unsigned long before = find_member(party.leader)->sequence;

		p2p_party_heard(sealed, sealed_size);
		check(find_member(party.leader)->sequence == before, "an altered message is turned away");
	}
	/* signed with another key than the sender's */
	{
		unsigned char other[P2P_SEED_SIZE] = { 99 };
		unsigned long before;

		sealed_size = sealed_message(0, message, size, other, sealed);
		switch_to(1);
		before = find_member(party.leader)->sequence;
		p2p_party_heard(sealed, sealed_size);
		check(find_member(party.leader)->sequence == before, "a message not signed by its sender's key is turned away");
	}
	/* another party's key */
	{
		unsigned char other_seal[P2P_SHA256_SIZE];
		unsigned long before;

		switch_to(0);
		memcpy(other_seal, party.seal, sizeof(other_seal));
		other_seal[0] ^= 0x80;
		{
			unsigned char data[P2P_SHA256_SIZE + 600], signed_message[600];

			memcpy(signed_message, message, (size_t)size);
			memcpy(data, party.context, P2P_SHA256_SIZE);
			memcpy(data + P2P_SHA256_SIZE, message, (size_t)size);
			p2p_ed25519_sign(machines[0].seed, machines[0].key, data, P2P_SHA256_SIZE + size, signed_message + size);
			sealed_size = p2p_seal(other_seal, signed_message, size + P2P_SIGNATURE_SIZE, sealed);
		}
		switch_to(1);
		before = find_member(party.leader)->sequence;
		p2p_party_heard(sealed, sealed_size);
		check(find_member(party.leader)->sequence == before, "another party's message is turned away");
	}
	/* a good one is taken; sent again, it is not */
	{
		sealed_size = sealed_message(0, message, size, NULL, sealed);
		switch_to(1);
		p2p_party_heard(sealed, sealed_size);
		check(find_member(party.leader)->sequence == sequence + 1000, "a good message is taken");
		find_member(party.leader)->heard_time = 0;
		p2p_party_heard(sealed, sealed_size);
		check(find_member(party.leader)->heard_time == 0, "the same message again is passed over");
		find_member(party.leader)->heard_time = clock_now;
	}
	/* another format: passed over, noted once */
	{
		int notices;

		message[1] = 2;
		put_long(message + 35, sequence + 2000);
		sealed_size = sealed_message(0, message, size, NULL, sealed);
		switch_to(1);
		status_of(1, &status);
		notices = status.notice_count;
		p2p_party_heard(sealed, sealed_size);
		check(find_member(party.leader)->sequence == sequence + 1000, "another format's message is passed over");
		status_of(1, &status);
		check(status.notice_count == notices + 1 &&
			status.notices[status.notice_count - 1].kind == _p2p_party_notice_version, "and noted");
		put_long(message + 35, sequence + 2001);
		sealed_size = sealed_message(0, message, size, NULL, sealed);
		switch_to(1);
		p2p_party_heard(sealed, sealed_size);
		status_of(1, &status);
		check(status.notice_count == notices + 1, "once");
		message[1] = 1;
	}
	/* a leader's of an older term: not followed (C claims to lead, of term
	0, then 50) */
	{
		unsigned char claim[600];
		int claim_size = opened_state(2, claim) - P2P_SIGNATURE_SIZE;
		unsigned char id[MEMBER_ID_SIZE];

		/* (made a leader's, of term 0: its game none, its roster empty; a
		member's host quality, after its name, is not a leader's) */
		claim_size -= 1;
		member_id(machines[2].key, id);
		claim[HEADER_SIZE] = _state_leads | _state_listed;
		put_long(claim + HEADER_SIZE + 1, 0);
		memcpy(claim + HEADER_SIZE + 5, id, MEMBER_ID_SIZE);
		put_long(claim + 35, get_long(claim + 35) + 500);
		put_long(claim + claim_size, 7);
		claim[claim_size + 4] = _p2p_party_game_none;
		claim[claim_size + 5] = 0;
		claim_size += 6;
		sealed_size = sealed_message(2, claim, claim_size, NULL, sealed);
		switch_to(1);
		p2p_party_heard(sealed, sealed_size);
		status_of(1, &status);
		check(!strcmp(status.leader_name, "A") && status.count == 3, "a leader of an older term is not followed");
		/* the same claim of a higher term is */
		put_long(claim + HEADER_SIZE + 1, 50);
		put_long(claim + 35, get_long(claim + 35) + 1);
		sealed_size = sealed_message(2, claim, claim_size, NULL, sealed);
		switch_to(1);
		p2p_party_heard(sealed, sealed_size);
		switch_to(1);
		check(!memcmp(party.leader, id, MEMBER_ID_SIZE) && party.term == 50,
			"a leader of a higher term is (the control)");
		/* (its roster, empty, has not B: B asks to be listed again) */
		check(!party.listed, "a member not on the leader's roster asks to be listed again");
	}
	/* malformed: a roster longer than the message */
	{
		unsigned char bad[600];
		int bad_size = opened_state(0, bad) - P2P_SIGNATURE_SIZE;
		unsigned long before;

		bad[bad_size - 3 * MEMBER_ID_SIZE - 1] = 9;
		put_long(bad + 35, get_long(bad + 35) + 900);
		put_long(bad + HEADER_SIZE + 1, 99);
		sealed_size = sealed_message(0, bad, bad_size, NULL, sealed);
		switch_to(1);
		before = party.term;
		p2p_party_heard(sealed, sealed_size);
		check(party.term == before, "a leader's STATE with a roster longer than it is turned away");
	}
	/* a member gone: its old messages do not bring it back */
	{
		unsigned char old[700];
		int old_size;

		switch_to(2);
		send_state();
		memcpy(old, last_sealed, (size_t)last_sealed_size);
		old_size = last_sealed_size;
		queue_count = 0;
		p2p_party_leave();
		run(500);
		check(!member_named(1, "C"), "C left");
		switch_to(1);
		p2p_party_heard(old, old_size);
		check(!member_named(1, "C"), "its old STATE sent again does not bring it back");
	}
}

static void case_spare(void)
{
	char link[P2P_PARTY_LINK_SIZE], again[P2P_PARTY_LINK_SIZE], id[33], id_again[33];
	unsigned char token[P2P_TOKEN_SIZE];
	struct p2p_party_presence presence;
	struct p2p_party_status status;
	int index;

	for (index = 0; index < 5; index++)
	{
		char text[2] = { (char)('A' + index), 0 };

		add_machine();
		name(index, text);
	}
	switch_to(0);
	p2p_party_spare(link, sizeof(link), id, sizeof(id));
	p2p_party_spare(again, sizeof(again), id_again, sizeof(id_again));
	check(!strcmp(link, again) && !strcmp(id, id_again), "the spare is the same until it is made a party");
	check(p2p_party_parse_link(link, token) && strlen(id) == 32 && !strstr(link, id),
		"its link is a party's, its id 32 digits not the link's");
	p2p_party_presence(&presence);
	check(!presence.active, "the spare is no party yet");
	/* (its topic not listened on: a friend with the link is not answered) */
	switch_to(1);
	p2p_party_join_link(link);
	run(JOIN_WAIT + 2000);
	status_of(1, &status);
	check(status.state == _p2p_party_none, "not listened on, a friend with its link gives up");
	status_of(0, &status);
	check(status.state == _p2p_party_none, "... and no party is made");
	/* (listened on: made, the friend's to join) */
	switch_to(0);
	p2p_party_standby(1);
	check(!strncmp(machines[0].topic, "hceu/3/", 7) && !strcmp(machines[0].topic + 7, id),
		"listening on the spare's topic");
	switch_to(1);
	p2p_party_join_link(link);
	run(3000);
	status_of(0, &status);
	check(status.state == _p2p_party_member && status.leader && status.count == 2 &&
		!strcmp(status.members[0].name, "A") && !strcmp(status.members[1].name, "B"),
		"a friend with the link arriving makes the party, this machine its leader, the friend in it");
	status_of(1, &status);
	check(status.state == _p2p_party_member && status.count == 2 && !strcmp(status.leader_name, "A"),
		"the friend is in it");
	switch_to(0);
	p2p_party_presence(&presence);
	check(presence.active && presence.count == 2 && !strcmp(presence.link, link) && !strcmp(presence.id, id),
		"the party is the spare's: its link and its id");
	p2p_party_spare(again, sizeof(again), id_again, sizeof(id_again));
	check(strcmp(again, link) && strcmp(id_again, id), "the next spare is another");
	/* (let in: made at once) */
	switch_to(2);
	p2p_party_spare(link, sizeof(link), id, sizeof(id));
	p2p_party_spare_lead();
	status_of(2, &status);
	check(status.state == _p2p_party_member && status.leader && status.count == 1 && !strcmp(status.link, link),
		"a friend let in: the spare made a party, this machine its leader");
	switch_to(3);
	p2p_party_join_link(link);
	run(3000);
	status_of(2, &status);
	check(status.count == 2, "the friend let in joins it");
	/* (MAKE PARTY makes the spare) */
	switch_to(4);
	p2p_party_spare(link, sizeof(link), id, sizeof(id));
	p2p_party_make();
	check(!strcmp(machines[4].clipboard, link), "MAKE PARTY makes the spare's party");
}

static void case_version(void)
{
	unsigned char message[600], sealed[700];
	int size, sealed_size;

	party_of(2);
	switch_to(1);
	send_state();
	size = p2p_open(party.seal, last_sealed, last_sealed_size, message) - P2P_SIGNATURE_SIZE;
	put_short(message + HEADER_SIZE + 13, HALO_PORT_NETWORK_VERSION + 1);
	put_long(message + 35, get_long(message + 35) + 10);
	sealed_size = sealed_message(1, message, size, NULL, sealed);
	switch_to(0);
	p2p_party_heard(sealed, sealed_size);
	check(member_named(0, "B") && member_named(0, "B")->other_version, "a member of another version is shown so");
	check(notice_of(0, _p2p_party_notice_version) && !strcmp(notice_of(0, _p2p_party_notice_version)->name, "B"),
		"and the menus are told");
	check(member_named(1, "A") && !member_named(1, "A")->other_version, "one of the same version is not");
}

static void case_keys(void)
{
	unsigned char hashes[P2P_PARTY_MAXIMUM_MEMBERS * P2P_KEY_HASH_SIZE];
	unsigned char secret[P2P_KEY_SIZE], tunnel_key[P2P_KEY_SIZE], expected[P2P_KEY_HASH_SIZE];
	unsigned char id[MEMBER_ID_SIZE];
	struct member *member;
	int count, index;

	party_of(4);
	switch_to(0);
	count = p2p_party_member_key_hashes(hashes, P2P_PARTY_MAXIMUM_MEMBERS);
	check(count == 3, "the leader names its three other members");
	for (index = 1; index < 4 && count == 3; index++)
	{
		/* (the key the machine reaches a host with: p2p.c's, of its seed) */
		p2p_ed25519_public(machines[index].seed, tunnel_key, secret);
		p2p_x25519(tunnel_key, secret, NULL);
		p2p_key_hash(tunnel_key, expected);
		check(!memcmp(hashes + (index - 1) * P2P_KEY_HASH_SIZE, expected, P2P_KEY_HASH_SIZE),
			"each by the hash of the tunnel key it reaches a host with, in the roster's order");
	}
	check(p2p_party_member_key_hashes(hashes, 2) == 2, "no more than asked for");
	/* (D of another version: it cannot join the leader's games) */
	member_id(machines[3].key, id);
	member = find_member(id);
	check(member != NULL, "the leader hears D");
	if (member)
		member->network_version = HALO_PORT_NETWORK_VERSION + 1;
	check(p2p_party_member_key_hashes(hashes, P2P_PARTY_MAXIMUM_MEMBERS) == 2,
		"a member of another version is not named");
	switch_to(1);
	check(p2p_party_member_key_hashes(hashes, P2P_PARTY_MAXIMUM_MEMBERS) == 0, "a member names no one");
	memset(&party, 0, sizeof(party));
	memset(machines, 0, sizeof(machines));
	machine_count = 0;
	current = -1;
	party_of(1);
	check(p2p_party_member_key_hashes(hashes, P2P_PARTY_MAXIMUM_MEMBERS) == 0, "a leader alone names no one");
}

static void quality(int index, int value)
{
	switch_to(index);
	p2p_party_set_host_quality(value);
}

static void case_host(void)
{
	struct p2p_party_status status;
	unsigned char id[MEMBER_ID_SIZE];
	int index;

	party_of(4);
	quality(0, _p2p_party_host_strict);
	quality(1, _p2p_party_host_strict);
	quality(2, _p2p_party_host_open);
	quality(3, _p2p_party_host_open);
	run(1000);
	/* (an ask to host from a member, not the leader: not taken) */
	member_id(machines[1].key, id);
	switch_to(3);
	send_host(id, 2);
	run(1500);
	status_of(1, &status);
	check(!strcmp(status.leader_name, "A"), "an ask to host from a member is not taken");
	switch_to(1);
	check(!p2p_party_pass_host(1), "a member passes no lead");
	switch_to(0);
	check(!p2p_party_pass_host(0), "nor a leader for no playlist");
	check(p2p_party_pass_host(3), "a leader of a strict network passes the lead to one that can host better");
	run(1500);
	for (index = 0; index < 4; index++)
	{
		status_of(index, &status);
		check(!strcmp(status.leader_name, "C"), "the first on the roster of the best, C, leads now");
	}
	switch_to(2);
	check(party.term == 2, "a term higher");
	check(p2p_party_take_host_request() == 3, "and is asked to host the playlist");
	check(p2p_party_take_host_request() == 0, "once");
	switch_to(3);
	check(p2p_party_take_host_request() == 0, "the others are not asked");
	switch_to(0);
	check(p2p_party_take_host_request() == 0 && !party.host_request, "nor the old leader");
	/* (C leads: none can host better than C) */
	switch_to(2);
	check(!p2p_party_pass_host(3), "none passed when no member can host better");
	/* a member that can no longer host (in a game) when asked: not taken */
	quality(2, _p2p_party_host_strict);
	quality(0, _p2p_party_host_open);
	run(1000);
	quality(0, _p2p_party_host_unable);
	switch_to(2);
	check(p2p_party_pass_host(1), "passed to A, which told it could host");
	run(1500);
	status_of(1, &status);
	check(!strcmp(status.leader_name, "C"), "A, unable now, did not take the lead");
	switch_to(0);
	check(p2p_party_take_host_request() == 0, "nor was it asked to host");
	/* an older build's STATE (no host byte): unable to host */
	switch_to(1);
	send_state();
	{
		unsigned char message[600], sealed[700];
		int size = p2p_open(party.seal, last_sealed, last_sealed_size, message) - P2P_SIGNATURE_SIZE;
		int sealed_size;

		message[size - 1] = _p2p_party_host_open;
		put_long(message + 35, get_long(message + 35) + 10);
		sealed_size = sealed_message(1, message, size - 1, NULL, sealed);
		switch_to(2);
		p2p_party_heard(sealed, sealed_size);
		member_id(machines[1].key, id);
		check(find_member(id) && find_member(id)->host_quality == _p2p_party_host_unable,
			"a STATE without how well it can host is a member unable to");
		/* (and with it, the byte read) */
		put_long(message + 35, get_long(message + 35) + 1);
		sealed_size = sealed_message(1, message, size, NULL, sealed);
		switch_to(2);
		p2p_party_heard(sealed, sealed_size);
		check(find_member(id) && find_member(id)->host_quality == _p2p_party_host_open, "a member's byte is read");
	}
}

static const struct
{
	const char *name;
	void (*run)(void);
} cases[] =
{
	{ "make", case_make },
	{ "join", case_join },
	{ "follow", case_follow },
	{ "busy", case_busy },
	{ "local", case_local },
	{ "leave", case_leave },
	{ "handoff", case_handoff },
	{ "timeout", case_timeout },
	{ "regroup", case_regroup },
	{ "disband", case_disband },
	{ "full", case_full },
	{ "joinless", case_joinless },
	{ "format", case_format },
	{ "version", case_version },
	{ "keys", case_keys },
	{ "host", case_host },
	{ "spare", case_spare },
};

int main(int argc, char **argv)
{
	int index, ran = 0;

	verbose = getenv("PARTY_CHECK_VERBOSE") != NULL;
	for (index = 0; index < (int)(sizeof(cases) / sizeof(*cases)); index++)
	{
		if (argc > 1 && strcmp(argv[1], cases[index].name))
			continue;
		memset(&party, 0, sizeof(party));
		memset(machines, 0, sizeof(machines));
		machine_count = 0;
		current = -1;
		queue_count = 0;
		brokers_up = 1;
		cases[index].run();
		ran++;
	}
	if (!ran)
	{
		printf("no case %s\n", argc > 1 ? argv[1] : "");
		return 2;
	}
	if (failures)
		return 1;
	printf("PASS\n");
	return 0;
}
