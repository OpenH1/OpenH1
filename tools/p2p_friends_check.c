/*
P2P_FRIENDS_CHECK.C

A check of friends (port/linux/src/p2p_friends.c), built by
tools/test_linux_port.py (with p2p_friends.c mutated for its negative
controls): several machines in this one process, each with its own friends'
state (swapped in and out of p2p_friends.c's) and its own folder for
friends.txt, talking through a stand-in for the signalling brokers that
delivers each message published on a topic to every other machine
subscribed to it, but to a machine out of reach (whose own messages are lost
too). Every payload is kept as an eavesdropper on the brokers sees it. The
cases, each run by its name:

- code: a code's text and back, for many keys; every character mistyped
  and every two neighbours swapped are caught (but 0 and Z swapped, which
  the check cannot tell); typing is forgiving (lower case, I, L, O, dashes,
  spaces); too few or too many characters, and a 56th bit, are no code;
- file: a new profile's identity made and written, the same code read back;
  friends, requests, requests sent, blocked codes, ignored codes and recent
  players kept through the file; its lines checked (bad hex, numbers, too
  few fields, a friend twice, this profile's own key), its text cleaned;
- request: A asks B (by code): B is told, and shows the request; B accepts:
  both are friends, and swap presence keys (HELLO), each confirming the
  other's;
- ignore: B turns A's request down: gone, and A's asking again is not shown;
- mutual: A and B ask each other: friends without an accept;
- offline: A asks B, out of reach; B, back, gets A's asking again; B accepts
  while A is away; A, back, asks again, and B's answer makes them friends;
- presence: A shares: B (a friend) sees it online, where it is (a party of
  3, hosting, its game's words) and its link; C (a stranger who has A's code)
  and the brokers read nothing of it: no name or words in any payload, and C
  can open none; A's FIRST is answered by B's presence; heard nothing for
  PRESENCE_TIMEOUT, A is offline;
- hidden: A appears offline (friends.share_status): B sees it offline at
  once, and A publishes nothing more (no presence, no HELLO) until it shares
  again;
- remove: A removes B: neither has the other; A's presence key is new, which
  C (a friend left) gets and reads, and B, with the old one, cannot;
- block: A blocks B: B is removed, its requests and asking again let go;
  unblocked, B's asking is shown again;
- invite: A invites B: B's prompt names A, and accepting joins A's party's
  link; a second invite at once is let go; declining joins nothing;
- join: B joins A (its presence's link: its party's); an offline friend, or
  a stranger, cannot be joined;
- recent: players seen: the newest first, one again moved up, at most
  P2P_FRIENDS_RECENT, this profile's own code left out, OpenCE players and
  players of no code (by name) kept as such; the list's order;
- forged: what is turned away: a request altered, sealed with another
  code's key, signed by a key not its sender's, of another format, sent
  again, a flood of requests past the checks' budget, a seventeenth request.
Prints PASS or the failures.
*/

#include "p2p_internal.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* the friends' code, its state reachable here (P2P_FRIENDS_SOURCE: a
mutated copy, for the negative controls) */
#ifndef P2P_FRIENDS_SOURCE
#define P2P_FRIENDS_SOURCE "p2p_friends.c"
#endif
#include P2P_FRIENDS_SOURCE

/* ---------- stand-ins for the rest of internet play */

pthread_mutex_t p2p_lock = PTHREAD_MUTEX_INITIALIZER;

enum
{
	MAXIMUM_MACHINES = 6,
	MAXIMUM_QUEUED = 8192,
	MAXIMUM_PAYLOAD = 600,
};

struct machine
{
	/* its friends' state, while another's is p2p_friends.c's */
	unsigned char state[sizeof(friends)];
	char folder[512];
	char topics[P2P_FRIENDS_TOPICS][P2P_TOPIC_SIZE];
	int topic_count;
	/* out of reach: neither updated nor sent to (its messages lost) */
	int away;
	int share;
	/* its party (in one, its size, its link) and spare's link, where it is,
	and the game Discord has */
	int party_active, party_count, place;
	char party_link[P2P_PARTY_LINK_SIZE], spare_link[P2P_PARTY_LINK_SIZE];
	int hosting, playing;
	char description[64];
	/* the last party link it joined, and its standby */
	char joined[P2P_PARTY_LINK_SIZE];
	int standby;
	/* what it published on its presence topic, and in all */
	int presence_published;
	int published;
};

struct queued
{
	int from;
	char topic[P2P_TOPIC_SIZE];
	int size;
	unsigned char payload[MAXIMUM_PAYLOAD];
};

static struct machine machines[MAXIMUM_MACHINES];
static int machine_count;
static int current = -1;
static unsigned long clock_now = 100000;
static long long wall_now = 1790000000;
static struct queued queue[MAXIMUM_QUEUED];
static int queue_count;
/* every payload published, as the brokers see them */
static struct queued seen[MAXIMUM_QUEUED];
static int seen_count;
static int verbose;
static int random_counter;
static char base_folder[400];
static int failures;

unsigned long p2p_now(void) { return clock_now; }
long long p2p_wall_seconds(void) { return wall_now + (long long)(clock_now - 100000) / 1000; }
unsigned long long p2p_wall_milliseconds(void)
{
	return (unsigned long long)wall_now * 1000 + (clock_now - 100000);
}

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
		bytes[index] = (unsigned char)(random_counter * 37 + index * 11 + 5 + (random_counter >> 3) * index);
}

int config_boolean(const char *name)
{
	return !strcmp(name, "friends.share_status") && current >= 0 && machines[current].share;
}

void p2p_signal_start(void) {}
int p2p_signal_connected(void) { return 1; }

void p2p_signal_friends(const char (*topics)[P2P_TOPIC_SIZE], int count)
{
	memcpy(machines[current].topics, topics, sizeof(machines[current].topics[0]) * (size_t)count);
	machines[current].topic_count = count;
}

void p2p_signal_friends_publish(const char *topic, const unsigned char *payload, int size, int urgent)
{
	(void)urgent;
	if (size > MAXIMUM_PAYLOAD)
		return;
	machines[current].published++;
	if (!strcmp(topic, friends.presence_topic))
		machines[current].presence_published++;
	if (seen_count < MAXIMUM_QUEUED)
	{
		seen[seen_count].from = current;
		snprintf(seen[seen_count].topic, P2P_TOPIC_SIZE, "%s", topic);
		seen[seen_count].size = size;
		memcpy(seen[seen_count].payload, payload, (size_t)size);
		seen_count++;
	}
	if (queue_count == MAXIMUM_QUEUED || machines[current].away)
		return;
	queue[queue_count].from = current;
	snprintf(queue[queue_count].topic, P2P_TOPIC_SIZE, "%s", topic);
	queue[queue_count].size = size;
	memcpy(queue[queue_count].payload, payload, (size_t)size);
	queue_count++;
}

void p2p_party_presence(struct p2p_party_presence *presence)
{
	struct machine *machine = &machines[current];

	memset(presence, 0, sizeof(*presence));
	presence->active = machine->party_active;
	presence->count = machine->party_count;
	presence->place = machine->place;
	snprintf(presence->link, sizeof(presence->link), "%s", machine->party_active ? machine->party_link : "");
}

void p2p_party_spare(char *link, int link_size, char *id, int id_size)
{
	snprintf(link, (size_t)link_size, "%s", machines[current].spare_link);
	snprintf(id, (size_t)id_size, "spare");
}

void p2p_party_friends_standby(int on) { machines[current].standby = on; }

int p2p_party_parse_link(const char *text, unsigned char *token)
{
	const char *start = strstr(text, P2P_PARTY_LINK_PREFIX);
	int index;

	if (!start || strlen(start) < sizeof(P2P_PARTY_LINK_PREFIX) - 1 + 2 * P2P_TOKEN_SIZE)
		return 0;
	start += sizeof(P2P_PARTY_LINK_PREFIX) - 1;
	for (index = 0; index < P2P_TOKEN_SIZE; index++)
	{
		unsigned int byte;

		if (sscanf(start + 2 * index, "%2x", &byte) != 1)
			return 0;
		token[index] = (unsigned char)byte;
	}
	return 1;
}

int p2p_party_join_link_locked(const char *text)
{
	snprintf(machines[current].joined, sizeof(machines[current].joined), "%s", text);
	return 1;
}

/* (called without the lock, as the game's thread calls it) */
int p2p_party_invite_link(char *link, int size)
{
	struct machine *machine = &machines[current];

	machine->party_active = 1;
	if (!machine->party_count)
		machine->party_count = 1;
	snprintf(link, (size_t)size, "%s", machine->party_link);
	return 1;
}

void p2p_discord_game(int *hosting, int *playing, char *description, int size, int *player_count,
	int *maximum_player_count)
{
	*hosting = machines[current].hosting;
	*playing = machines[current].playing;
	snprintf(description, (size_t)size, "%s", machines[current].description);
	*player_count = machines[current].hosting ? 5 : 0;
	*maximum_player_count = machines[current].hosting ? 16 : 0;
}

/* ---------- the machines */

static void switch_to(int index)
{
	if (index == current)
		return;
	if (current >= 0)
		memcpy(machines[current].state, &friends, sizeof(friends));
	memcpy(&friends, machines[index].state, sizeof(friends));
	current = index;
}

/* a machine with a profile of its own (its folder, its friends.txt read or
made) */
static int add_machine(const char *name)
{
	struct machine *machine = &machines[machine_count];
	int index = machine_count++;
	unsigned char token[P2P_TOKEN_SIZE];
	char hex[2 * P2P_TOKEN_SIZE + 1];
	int byte;

	memset(machine, 0, sizeof(*machine));
	snprintf(machine->folder, sizeof(machine->folder), "%s/%s", base_folder, name);
	mkdir(machine->folder, 0700);
	machine->share = 1;
	for (byte = 0; byte < P2P_TOKEN_SIZE; byte++)
		token[byte] = (unsigned char)(0xA0 + index * 16 + byte);
	p2p_hex(token, P2P_TOKEN_SIZE, hex);
	snprintf(machine->party_link, sizeof(machine->party_link), "%s%s", P2P_PARTY_LINK_PREFIX, hex);
	token[0] ^= 0x5A;
	p2p_hex(token, P2P_TOKEN_SIZE, hex);
	snprintf(machine->spare_link, sizeof(machine->spare_link), "%s%s", P2P_PARTY_LINK_PREFIX, hex);
	switch_to(index);
	memset(&friends, 0, sizeof(friends));
	p2p_friends_set_profile(machine->folder, name);
	return index;
}

static void deliver(void);

/* a machine's profile read again from its file (a new start) */
static void restart(int index)
{
	char name[P2P_FRIEND_NAME_SIZE];

	switch_to(index);
	snprintf(name, sizeof(name), "%s", friends.name);
	p2p_friends_quit();
	deliver();
	switch_to(index);
	/* (a start takes a while) */
	clock_now += 2000;
	memset(&friends, 0, sizeof(friends));
	p2p_friends_set_profile(machines[index].folder, name);
}

static int subscribed(int index, const char *topic)
{
	int at;

	for (at = 0; at < machines[index].topic_count; at++)
	{
		if (!strcmp(machines[index].topics[at], topic))
			return 1;
	}
	return 0;
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
			if (index == queue[read].from || machines[index].away || !subscribed(index, queue[read].topic))
				continue;
			switch_to(index);
			pthread_mutex_lock(&p2p_lock);
			p2p_friends_heard(queue[read].topic, queue[read].payload, queue[read].size);
			pthread_mutex_unlock(&p2p_lock);
		}
	}
	queue_count = 0;
}

static void run(unsigned long milliseconds)
{
	unsigned long end = clock_now + milliseconds;

	while (clock_now < end)
	{
		int index;

		for (index = 0; index < machine_count; index++)
		{
			if (machines[index].away)
				continue;
			switch_to(index);
			pthread_mutex_lock(&p2p_lock);
			p2p_friends_update();
			pthread_mutex_unlock(&p2p_lock);
		}
		deliver();
		clock_now += 50;
	}
}

static void check(int condition, const char *what)
{
	if (!condition)
	{
		printf("FAIL: %s\n", what);
		failures++;
	}
}

static void code_of(int index, unsigned char *code)
{
	switch_to(index);
	p2p_friends_own_code(code);
}

/* the machine's list's entry of another's code, NULL if none */
static struct p2p_friend list_buffer[P2P_FRIENDS_LIST_MAXIMUM];

static const struct p2p_friend *entry_of(int index, int other)
{
	unsigned char code[P2P_FRIEND_CODE_BYTES];
	int count, at;

	code_of(other, code);
	switch_to(index);
	count = p2p_friends_list(list_buffer, P2P_FRIENDS_LIST_MAXIMUM);
	for (at = 0; at < count; at++)
	{
		if (!memcmp(list_buffer[at].code, code, P2P_FRIEND_CODE_BYTES) && list_buffer[at].kind != _p2p_friend_recent)
			return &list_buffer[at];
	}
	return NULL;
}

static int relation(int index, int other)
{
	unsigned char code[P2P_FRIEND_CODE_BYTES];

	code_of(other, code);
	switch_to(index);
	return p2p_friends_relation(code);
}

static int add(int index, int other)
{
	unsigned char code[P2P_FRIEND_CODE_BYTES];

	code_of(other, code);
	switch_to(index);
	return p2p_friends_add(code, "");
}

/* A and B friends, through a request and its accept */
static void befriend(int first, int second)
{
	add(first, second);
	run(1000);
	add(second, first);
	run(3000);
}

static struct friend_record *record_of(int index, int other)
{
	unsigned char code[P2P_FRIEND_CODE_BYTES];

	code_of(other, code);
	switch_to(index);
	return find_friend(code);
}

/* ---------- the cases */

static void case_code(void)
{
	unsigned char key[P2P_KEY_SIZE], code[P2P_FRIEND_CODE_BYTES], back[P2P_FRIEND_CODE_BYTES];
	char text[P2P_FRIEND_CODE_TEXT_SIZE], typed[32];
	int round, place, value;
	int caught = 1, swaps = 1;

	for (round = 0; round < 200; round++)
	{
		int at;

		for (at = 0; at < P2P_KEY_SIZE; at++)
			key[at] = (unsigned char)(round * 7 + at * 13);
		p2p_friend_code_of(key, code);
		check(p2p_friend_code_format(code, text, sizeof(text)) && strlen(text) == 14 && text[4] == '-' &&
			text[9] == '-', "a code's text is 4-4-4 characters");
		check(p2p_friend_code_parse(text, back) == 1 && !memcmp(code, back, P2P_FRIEND_CODE_BYTES),
			"a code's text is the code");
		check(!(code[P2P_FRIEND_CODE_BYTES - 1] & 1), "a code has 55 bits");
		/* (every character mistyped: caught) */
		for (place = 0; place < 14 && round < 20; place++)
		{
			if (text[place] == '-')
				continue;
			for (value = 0; value < 32; value++)
			{
				int result;

				snprintf(typed, sizeof(typed), "%s", text);
				if (code_digits[value] == typed[place])
					continue;
				typed[place] = code_digits[value];
				result = p2p_friend_code_parse(typed, back);
				if (result == 1 && memcmp(back, code, P2P_FRIEND_CODE_BYTES))
					caught = 0;
			}
		}
		/* (two neighbours swapped: caught) */
		for (place = 0; place + 1 < 14 && round < 50; place++)
		{
			char first = text[place], second = text[place + 1];
			int result;

			if (first == '-' || second == '-' || first == second)
				continue;
			snprintf(typed, sizeof(typed), "%s", text);
			typed[place] = second;
			typed[place + 1] = first;
			result = p2p_friend_code_parse(typed, back);
			if (result == 1 && memcmp(back, code, P2P_FRIEND_CODE_BYTES))
				swaps = 0;
		}
	}
	check(caught, "every mistyped character is caught");
	check(swaps, "every swap of two neighbours is caught");
	/* forgiving */
	memset(key, 3, sizeof(key));
	p2p_friend_code_of(key, code);
	p2p_friend_code_format(code, text, sizeof(text));
	{
		char lower[32];
		int at, written = 0;

		for (at = 0; text[at]; at++)
		{
			char character = text[at];

			if (character == '-')
			{
				lower[written++] = ' ';
				continue;
			}
			if (character == '1')
				character = at % 2 ? 'l' : 'I';
			else if (character == '0')
				character = 'o';
			else if (character >= 'A' && character <= 'Z')
				character = (char)(character - 'A' + 'a');
			lower[written++] = character;
		}
		lower[written] = 0;
		check(p2p_friend_code_parse(lower, back) == 1 && !memcmp(back, code, P2P_FRIEND_CODE_BYTES),
			"lower case, I, L, O and spaces are taken");
	}
	check(p2p_friend_code_parse("K7QM-2XPA-9DT", back) == 0, "eleven characters are no code");
	check(p2p_friend_code_parse("K7QM-2XPA-9DTRR", back) == 0, "thirteen are none");
	check(p2p_friend_code_parse("K7QM-2XPA-9DTU", back) == 0, "U is no code's character");
	check(p2p_friend_code_parse("", back) == 0, "nothing is no code");
	/* (every code's text is a code: all 32 values in each place) */
	{
		int values[12];
		char typed_code[16];

		for (place = 0; place < 11; place++)
		{
			for (value = 0; value < 32; value++)
			{
				int at;

				memset(values, 0, sizeof(values));
				values[place] = value;
				values[11] = code_check(values);
				for (at = 0; at < 12; at++)
					typed_code[at] = code_digits[values[at]];
				typed_code[12] = 0;
				check(p2p_friend_code_parse(typed_code, back) == 1 &&
					p2p_friend_code_format(back, text, sizeof(text)) && !strncmp(text, typed_code, 4),
					"every value in every place");
			}
		}
	}
	check(p2p_friend_code_parse("0000-0000-000Z", back) == -1, "a wrong check is told apart");
}

static void case_file(void)
{
	int a = add_machine("Keyes"), b = add_machine("Johnson");
	unsigned char code[P2P_FRIEND_CODE_BYTES], again[P2P_FRIEND_CODE_BYTES], other[P2P_FRIEND_CODE_BYTES];
	static char text[FILE_MAXIMUM];
	char path[600];
	FILE *file;
	int size;

	code_of(a, code);
	snprintf(path, sizeof(path), "%s/friends.txt", machines[a].folder);
	check(access(path, F_OK) == 0, "a new profile's identity is written at once");
	befriend(a, b);
	switch_to(a);
	p2p_friends_saw_player(NULL, "Cortana", 1, "OpenH1 · Slayer on Blood Gulch");
	code[0] ^= 0x10;
	p2p_friends_block(code, "Guilty Spark");
	code[0] ^= 0x10;
	code[1] ^= 0x20;
	p2p_friends_add(code, "Foe Hammer");
	code[1] ^= 0x20;
	restart(a);
	p2p_friends_own_code(again);
	check(!memcmp(code, again, P2P_FRIEND_CODE_BYTES), "the code is the same after a restart");
	check(relation(a, b) == _p2p_friend_online, "a friend is kept");
	check(record_of(a, b) && record_of(a, b)->has_presence_key, "its presence key too");
	code[0] ^= 0x10;
	switch_to(a);
	check(p2p_friends_relation(code) == _p2p_friend_blocked, "a block is kept");
	code[0] ^= 0x10;
	code[1] ^= 0x20;
	check(p2p_friends_relation(code) == _p2p_friend_sent, "a request sent is kept");
	code[1] ^= 0x20;
	{
		int count = p2p_friends_list(list_buffer, P2P_FRIENDS_LIST_MAXIMUM);
		int at, found = 0;

		for (at = 0; at < count; at++)
		{
			if (list_buffer[at].kind == _p2p_friend_recent && !strcmp(list_buffer[at].name, "Cortana") &&
				list_buffer[at].opence && !list_buffer[at].has_code &&
				!strcmp(list_buffer[at].game, "OpenH1 · Slayer on Blood Gulch"))
			{
				found = 1;
			}
		}
		check(found, "a recent player is kept, its game's words in UTF-8");
	}
	/* (what the file is not let do) */
	size = p2p_friends_format(text, sizeof(text));
	check(size > 0 && strstr(text, "identity\t") != NULL, "the file has the identity");
	memset(&friends, 0, sizeof(friends));
	{
		char bad[4096];
		char key[2 * P2P_KEY_SIZE + 1];

		p2p_hex(machines[b].state + offsetof(__typeof__(friends), key), P2P_KEY_SIZE, key);
		snprintf(bad, sizeof(bad), "identity\tzz\nfriend\t%s\t1\t1\t-\t1\tJohnson\nfriend\t%s\t1\t1\t-\t1\tAgain\n"
			"friend\tabc\t1\t1\t-\t1\tShort\nfriend\t%s\tx\t1\t-\t1\tNumber\nfriend\t%s\t1\n"
			"request\t%s\t5\tTab\x01name\nrecent\t?\t5\t0\tNobody\tgame\n", key, key, key, key, key);
		p2p_friends_parse(bad, (int)strlen(bad));
		check(!friends.has_identity, "a bad identity is none");
		{
			int count = 0, at;

			for (at = 0; at < P2P_FRIENDS_MAXIMUM; at++)
				count += friends.friends[at].used;
			check(count == 1 && !strcmp(friends.friends[0].name, "Johnson"), "only the good friend line, once");
		}
		check(friends.requests[0].used && !strcmp(friends.requests[0].name, "Tabname"), "text is cleaned");
		check(!friends.recent[0].used, "a recent player of a bad code is left out");
	}
	switch_to(b);
	restart(b);
	code_of(b, other);
	/* (a file of this profile's own key as a friend: left out) */
	switch_to(a);
	file = fopen(path, "a");
	if (file)
	{
		char key[2 * P2P_KEY_SIZE + 1];

		restart(a);
		p2p_hex(friends.key, P2P_KEY_SIZE, key);
		fprintf(file, "friend\t%s\t1\t1\t-\t1\tMyself\n", key);
		fclose(file);
		restart(a);
		check(!find_friend(friends.code), "its own key is no friend");
	}
}

static void case_request(void)
{
	int a = add_machine("Keyes"), b = add_machine("Johnson");
	struct p2p_friends_notice notice_seen;
	const struct p2p_friend *entry;

	check(add(a, b) == _p2p_friends_done, "A asks B");
	check(add(a, b) == _p2p_friends_already, "not twice");
	check(add(a, a) == _p2p_friends_self, "nor itself");
	run(1000);
	entry = entry_of(b, a);
	check(entry && entry->kind == _p2p_friend_request && !strcmp(entry->name, "Keyes"), "B shows A's request");
	entry = entry_of(a, b);
	check(entry && entry->kind == _p2p_friend_sent, "A shows its request sent");
	switch_to(b);
	check(p2p_friends_notice(&notice_seen) && notice_seen.kind == _p2p_friends_notice_request &&
		!strcmp(notice_seen.name, "Keyes"), "B is told");
	check(add(b, a) == _p2p_friends_done, "B accepts");
	run(4000);
	check(relation(a, b) == _p2p_friend_online && relation(b, a) == _p2p_friend_online, "both are friends");
	switch_to(a);
	check(p2p_friends_notice(&notice_seen) && notice_seen.kind == _p2p_friends_notice_accepted, "A is told");
	/* (copies: a record is of the machine switched to) */
	{
		struct friend_record ab = *record_of(a, b), ba = *record_of(b, a);

		check(ab.has_presence_key && ba.has_presence_key && ab.generation == 1 && ba.generation == 1,
			"each has the other's key");
		check(ab.confirmed == 1 && ba.confirmed == 1, "each confirmed it has the other's");
		check(strcmp(ab.presence_topic, ba.presence_topic), "each its own presence topic");
	}
	entry = entry_of(b, a);
	check(entry && entry->kind == _p2p_friend_online, "B sees A online");
}

static void case_ignore(void)
{
	int a = add_machine("Keyes"), b = add_machine("Johnson");
	unsigned char code[P2P_FRIEND_CODE_BYTES];

	add(a, b);
	run(1000);
	code_of(a, code);
	switch_to(b);
	p2p_friends_ignore(code);
	check(!entry_of(b, a), "the request is gone");
	run(ADD_INTERVAL + 2000);
	check(!entry_of(b, a), "A's asking again is not shown");
	check(relation(a, b) == _p2p_friend_sent, "A's request is still sent");
}

static void case_mutual(void)
{
	int a = add_machine("Keyes"), b = add_machine("Johnson");

	add(a, b);
	add(b, a);
	run(3000);
	check(relation(a, b) == _p2p_friend_online && relation(b, a) == _p2p_friend_online,
		"asking each other is accepting");
}

static void case_offline(void)
{
	int a = add_machine("Keyes"), b = add_machine("Johnson");

	machines[b].away = 1;
	add(a, b);
	run(2000);
	machines[b].away = 0;
	check(!entry_of(b, a), "B, away, heard nothing");
	run(ADD_INTERVAL + 1000);
	check(entry_of(b, a) && entry_of(b, a)->kind == _p2p_friend_request, "A's asking again reaches B");
	machines[a].away = 1;
	add(b, a);
	run(2000);
	check(relation(b, a) == _p2p_friend_online, "B accepts: A is its friend");
	machines[a].away = 0;
	check(relation(a, b) == _p2p_friend_sent, "A, away, still waits");
	run(ADD_INTERVAL + 3000);
	check(relation(a, b) == _p2p_friend_online, "A's asking again is answered: friends");
}

/* whether any payload published has these bytes */
static int seen_text(const char *text)
{
	int index, at;
	int length = (int)strlen(text);

	for (index = 0; index < seen_count; index++)
	{
		for (at = 0; at + length <= seen[index].size; at++)
		{
			if (!memcmp(seen[index].payload + at, text, (size_t)length))
				return 1;
		}
	}
	return 0;
}

static void case_presence(void)
{
	int a = add_machine("Keyes"), b = add_machine("Johnson"), c = add_machine("Cortana");
	const struct p2p_friend *entry;
	int index, before;

	machines[a].party_active = 1;
	machines[a].party_count = 3;
	machines[a].hosting = 1;
	snprintf(machines[a].description, sizeof(machines[a].description), "OpenH1 120 Tick · Slayer on Blood Gulch");
	befriend(a, b);
	add(c, a);
	run(PRESENCE_INTERVAL + 1000);
	entry = entry_of(b, a);
	check(entry && entry->kind == _p2p_friend_online && entry->party_count == 3 && entry->hosting &&
		entry->joinable && entry->players == 5 && entry->maximum_players == 16 &&
		!strcmp(entry->game, "OpenH1 120 Tick · Slayer on Blood Gulch"), "B sees where A is");
	entry = entry_of(c, a);
	check(entry && entry->kind == _p2p_friend_sent, "C, a stranger, sees no more than its request");
	check(!seen_text("Slayer on Blood") && !seen_text("Keyes") && !seen_text("Johnson"),
		"the brokers see no name nor words");
	/* (C has A's code and every payload: it opens none of A's presence) */
	switch_to(a);
	{
		char topic[P2P_TOPIC_SIZE];
		int opened = 0;

		snprintf(topic, sizeof(topic), "%s", friends.presence_topic);
		check(!subscribed(c, topic), "C cannot find A's presence topic");
		for (index = 0; index < seen_count; index++)
		{
			unsigned char message[MAXIMUM_PAYLOAD_SIZE];

			if (strcmp(seen[index].topic, topic))
				continue;
			switch_to(c);
			if (p2p_open(friends.inbox_seal, seen[index].payload, seen[index].size, message) >= 0)
				opened = 1;
			pthread_mutex_lock(&p2p_lock);
			p2p_friends_heard(topic, seen[index].payload, seen[index].size);
			pthread_mutex_unlock(&p2p_lock);
		}
		check(!opened && entry_of(c, a)->kind == _p2p_friend_sent, "C opens none of it");
	}
	/* (A restarts: its FIRST answered by B's presence at once) */
	switch_to(b);
	before = machines[b].presence_published;
	restart(a);
	run(PRESENCE_ANSWER_DELAY + PRESENCE_CHANGE_INTERVAL + 1000);
	check(machines[b].presence_published > before, "B answers A's FIRST");
	entry = entry_of(a, b);
	check(entry && entry->kind == _p2p_friend_online, "so A sees B at once");
	/* (A gone without a word: offline after PRESENCE_TIMEOUT) */
	machines[a].away = 1;
	run(PRESENCE_TIMEOUT - PRESENCE_INTERVAL - 5000);
	check(entry_of(b, a)->kind == _p2p_friend_online, "A still online a while");
	run(PRESENCE_INTERVAL + 10000);
	check(entry_of(b, a)->kind == _p2p_friend_offline, "then offline");
}

static void case_hidden(void)
{
	int a = add_machine("Keyes"), b = add_machine("Johnson");
	int presence, published;

	befriend(a, b);
	run(2000);
	check(entry_of(b, a)->kind == _p2p_friend_online, "A online");
	machines[a].share = 0;
	run(1500);
	check(entry_of(b, a)->kind == _p2p_friend_offline, "A appearing offline is offline at once");
	presence = machines[a].presence_published;
	published = machines[a].published;
	run(2 * PRESENCE_INTERVAL + HELLO_RETRY);
	check(machines[a].presence_published == presence && machines[a].published == published,
		"A publishes nothing while hidden");
	check(entry_of(a, b)->kind == _p2p_friend_online, "A still sees B");
	machines[a].share = 1;
	run(2000);
	check(entry_of(b, a)->kind == _p2p_friend_online, "shared again, online");
}

static void case_remove(void)
{
	int a = add_machine("Keyes"), b = add_machine("Johnson"), c = add_machine("Cortana");
	unsigned char code[P2P_FRIEND_CODE_BYTES];
	unsigned long generation;
	char old_topic[P2P_TOPIC_SIZE];
	struct friend_record kept;

	befriend(a, b);
	befriend(a, c);
	run(5000);
	switch_to(a);
	generation = friends.generation;
	snprintf(old_topic, sizeof(old_topic), "%s", friends.presence_topic);
	kept = *record_of(b, a);
	code_of(b, code);
	switch_to(a);
	p2p_friends_remove(code);
	run(5000);
	check(relation(a, b) == _p2p_friend_none && relation(b, a) == _p2p_friend_none, "neither has the other");
	switch_to(a);
	check(friends.generation == generation + 1 && strcmp(old_topic, friends.presence_topic),
		"A's presence key and topic are new");
	run(PRESENCE_INTERVAL + 5000);
	check(record_of(c, a)->generation == generation + 1, "C has A's new key");
	check(entry_of(c, a)->kind == _p2p_friend_online, "and sees A");
	/* (B, with A's old key, reads nothing on the new topic) */
	{
		int index, read = 0;

		switch_to(a);
		snprintf(old_topic, sizeof(old_topic), "%s", friends.presence_topic);
		for (index = 0; index < seen_count; index++)
		{
			unsigned char message[MAXIMUM_PAYLOAD_SIZE];

			if (!strcmp(seen[index].topic, old_topic) &&
				p2p_open(kept.presence_seal, seen[index].payload, seen[index].size, message) >= 0)
			{
				read = 1;
			}
		}
		check(!read, "B's old key opens none of A's new presence");
	}
}

static void case_block(void)
{
	int a = add_machine("Keyes"), b = add_machine("Johnson");
	unsigned char code[P2P_FRIEND_CODE_BYTES];

	befriend(a, b);
	code_of(b, code);
	switch_to(a);
	check(p2p_friends_block(code, NULL), "A blocks B");
	run(3000);
	check(relation(a, b) == _p2p_friend_blocked && relation(b, a) == _p2p_friend_none, "B is no friend");
	check(entry_of(a, b) && !strcmp(entry_of(a, b)->name, "Johnson"), "the block keeps its name");
	add(b, a);
	run(ADD_INTERVAL + 2000);
	check(relation(a, b) == _p2p_friend_blocked, "B's asking is let go");
	switch_to(a);
	p2p_friends_unblock(code);
	run(ADD_INTERVAL + 2000);
	check(relation(a, b) == _p2p_friend_request, "unblocked, B's asking is shown");
}

static void case_invite(void)
{
	int a = add_machine("Keyes"), b = add_machine("Johnson"), c = add_machine("Cortana");
	unsigned char code[P2P_FRIEND_CODE_BYTES];
	char name[P2P_FRIEND_NAME_SIZE];
	unsigned long serial;

	befriend(a, b);
	befriend(a, c);
	code_of(b, code);
	switch_to(a);
	check(p2p_friends_invite(code), "A invites B");
	run(1000);
	switch_to(b);
	serial = p2p_friends_invite_prompt(name, sizeof(name));
	check(serial && !strcmp(name, "Keyes"), "B's prompt names A");
	check(entry_of(b, a)->inviting, "B's list shows the invite");
	switch_to(a);
	p2p_friends_invite(code);
	run(1000);
	switch_to(b);
	check(p2p_friends_invite_prompt(name, sizeof(name)) == serial, "a second invite at once is let go");
	p2p_friends_answer(serial, 1);
	check(!strcmp(machines[b].joined, machines[a].party_link), "accepting joins A's party");
	check(!p2p_friends_invite_prompt(name, sizeof(name)), "answered");
	code_of(c, code);
	switch_to(a);
	p2p_friends_invite(code);
	run(1000);
	switch_to(c);
	serial = p2p_friends_invite_prompt(name, sizeof(name));
	p2p_friends_answer(serial, 0);
	check(serial && !machines[c].joined[0], "declining joins nothing");
	/* (a stranger cannot invite) */
	{
		int d = add_machine("Foe");

		code_of(c, code);
		switch_to(d);
		check(!p2p_friends_invite(code), "a stranger cannot invite");
	}
}

static void case_join(void)
{
	int a = add_machine("Keyes"), b = add_machine("Johnson"), c = add_machine("Cortana");
	unsigned char code[P2P_FRIEND_CODE_BYTES];

	machines[a].party_active = 1;
	machines[a].party_count = 2;
	befriend(a, b);
	run(PRESENCE_CHANGE_INTERVAL + 1000);
	code_of(a, code);
	switch_to(b);
	check(p2p_friends_join(code) && !strcmp(machines[b].joined, machines[a].party_link), "B joins A's party");
	/* (out of a party: its spare's link, which friends' standby listens on) */
	machines[a].party_active = 0;
	run(PRESENCE_CHANGE_INTERVAL + 1000);
	switch_to(b);
	check(p2p_friends_join(code) && !strcmp(machines[b].joined, machines[a].spare_link),
		"out of a party, A's spare");
	check(machines[a].standby, "which A listens on");
	machines[a].away = 1;
	run(PRESENCE_TIMEOUT + 1000);
	machines[b].joined[0] = 0;
	switch_to(b);
	check(!p2p_friends_join(code) && !machines[b].joined[0], "an offline friend cannot be joined");
	switch_to(c);
	check(!p2p_friends_join(code), "nor a stranger");
}

static void case_recent(void)
{
	int a = add_machine("Keyes");
	unsigned char code[P2P_FRIEND_CODE_BYTES], own[P2P_FRIEND_CODE_BYTES];
	int index, count;

	switch_to(a);
	p2p_friends_own_code(own);
	memset(code, 0x44, sizeof(code));
	code[6] = 0;
	p2p_friends_saw_player(code, "Johnson", 0, "Team Slayer");
	wall_now += 100;
	p2p_friends_saw_player(NULL, "Cortana", 1, "Slayer");
	wall_now += 100;
	p2p_friends_saw_player(own, "Keyes", 0, "Slayer");
	count = p2p_friends_list(list_buffer, P2P_FRIENDS_LIST_MAXIMUM);
	check(count == 2 && !strcmp(list_buffer[0].name, "Cortana") && list_buffer[0].opence && !list_buffer[0].has_code,
		"the newest first, an OpenCE player of no code");
	check(!strcmp(list_buffer[1].name, "Johnson") && list_buffer[1].has_code &&
		list_buffer[1].relation == _p2p_friend_none, "a player of a code; its own code left out");
	wall_now += 100;
	p2p_friends_saw_player(code, "Johnson", 0, "CTF");
	count = p2p_friends_list(list_buffer, P2P_FRIENDS_LIST_MAXIMUM);
	check(count == 2 && !strcmp(list_buffer[0].name, "Johnson") && !strcmp(list_buffer[0].game, "CTF"),
		"seen again: moved up, once");
	for (index = 0; index < P2P_FRIENDS_RECENT + 10; index++)
	{
		code[0] = (unsigned char)index;
		wall_now += 100;
		p2p_friends_saw_player(code, "Grunt", 0, NULL);
	}
	count = p2p_friends_list(list_buffer, P2P_FRIENDS_LIST_MAXIMUM);
	check(count == P2P_FRIENDS_RECENT, "at most P2P_FRIENDS_RECENT");
	/* (the list's order) */
	{
		struct p2p_friend first, second;

		memset(&first, 0, sizeof(first));
		memset(&second, 0, sizeof(second));
		first.kind = _p2p_friend_request;
		second.kind = _p2p_friend_online;
		check(p2p_friends_compare(&second, &first) < 0, "online friends before requests");
		second.kind = _p2p_friend_offline;
		check(p2p_friends_compare(&first, &second) < 0, "requests before offline friends");
		first.kind = _p2p_friend_recent;
		check(p2p_friends_compare(&second, &first) < 0, "offline friends before recent players");
		second.kind = _p2p_friend_recent;
		first.time = 5;
		second.time = 9;
		check(p2p_friends_compare(&second, &first) < 0, "recent players newest first");
		first.kind = second.kind = _p2p_friend_online;
		snprintf(first.name, sizeof(first.name), "Avery");
		snprintf(second.name, sizeof(second.name), "Buck");
		check(p2p_friends_compare(&first, &second) < 0, "friends by name");
	}
}

/* a request from a machine to another's inbox, made by hand (its fields
set as given), for the forged case */
static int forged_request(int from, int to, int version, int resign_wrong, unsigned char *payload)
{
	unsigned char message[MAXIMUM_MESSAGE_SIZE];
	unsigned char code[P2P_FRIEND_CODE_BYTES], seal[P2P_SHA256_SIZE], context[P2P_SHA256_SIZE];
	char topic[P2P_TOPIC_SIZE];
	unsigned char body[1 + P2P_FRIEND_NAME_SIZE];
	int size;

	int body_size;

	code_of(to, code);
	switch_to(from);
	size = message_header(message, _message_add);
	message[1] = (unsigned char)version;
	body_size = name_put(body, "Forger");
	memcpy(message + size, body, (size_t)body_size);
	size += body_size;
	inbox_of(code, topic, seal, context);
	size = sign(context, message, size);
	if (resign_wrong)
		message[size - 1] ^= 1;
	payload[0] = _outer_request;
	return 1 + p2p_seal(seal, message, size, payload + 1);
}

static void hear(int index, int to, const unsigned char *payload, int size)
{
	unsigned char code[P2P_FRIEND_CODE_BYTES];
	char topic[P2P_TOPIC_SIZE];
	unsigned char seal[P2P_SHA256_SIZE], context[P2P_SHA256_SIZE];

	code_of(to, code);
	inbox_of(code, topic, seal, context);
	switch_to(index);
	pthread_mutex_lock(&p2p_lock);
	p2p_friends_heard(topic, payload, size);
	pthread_mutex_unlock(&p2p_lock);
}

static int requests_of(int index)
{
	int count = 0, at;

	switch_to(index);
	for (at = 0; at < P2P_FRIENDS_REQUESTS; at++)
		count += friends.requests[at].used;
	return count;
}

static void case_forged(void)
{
	int a = add_machine("Keyes"), b = add_machine("Johnson"), c = add_machine("Cortana");
	unsigned char payload[MAXIMUM_PAYLOAD_SIZE], copy[MAXIMUM_PAYLOAD_SIZE];
	int size, index;

	size = forged_request(a, b, FRIENDS_VERSION, 0, payload);
	memcpy(copy, payload, (size_t)size);
	copy[size - 3] ^= 1;
	hear(b, b, copy, size);
	check(requests_of(b) == 0, "an altered request is let go");
	/* (sealed with C's inbox key, heard on B's) */
	{
		int other = forged_request(a, c, FRIENDS_VERSION, 0, copy);

		hear(b, b, copy, other);
		check(requests_of(b) == 0, "one sealed for another code is let go");
	}
	size = forged_request(a, b, FRIENDS_VERSION, 1, copy);
	hear(b, b, copy, size);
	check(requests_of(b) == 0, "one signed wrong is let go");
	size = forged_request(a, b, FRIENDS_VERSION + 1, 0, copy);
	hear(b, b, copy, size);
	check(requests_of(b) == 0, "one of another format is let go");
	size = forged_request(a, b, FRIENDS_VERSION, 0, payload);
	hear(b, b, payload, size);
	check(requests_of(b) == 1, "a good one is taken");
	/* (the same again, as a broker could send it: its time is old) */
	switch_to(b);
	snprintf(friends.requests[0].name, sizeof(friends.requests[0].name), "Renamed");
	hear(b, b, payload, size);
	switch_to(b);
	check(requests_of(b) == 1 && !strcmp(friends.requests[0].name, "Renamed"), "the same again is not taken");
	/* (a flood: past the checks' budget, let go; at most
	P2P_FRIENDS_REQUESTS kept) */
	{
		int flood = add_machine("Grunt");

		switch_to(b);
		memset(friends.requests, 0, sizeof(friends.requests));
		for (index = 0; index < 60; index++)
		{
			switch_to(flood);
			memset(&friends, 0, sizeof(friends));
			p2p_friends_set_profile("", "Grunt");
			posix_random_bytes(friends.seed, P2P_SEED_SIZE);
			identity_derive();
			size = forged_request(flood, b, FRIENDS_VERSION, 0, payload);
			hear(b, b, payload, size);
		}
		check(requests_of(b) == P2P_FRIENDS_REQUESTS, "a flood fills the requests, no more");
		switch_to(b);
		check(friends.checks_left <= 0, "and spends the checks' budget");
	}
	/* (sent again: the same message twice is taken once) */
	{
		int d = add_machine("Foe");
		unsigned char again[MAXIMUM_PAYLOAD_SIZE];
		struct request_record *request;

		befriend(c, d);
		size = forged_request(d, c, FRIENDS_VERSION, 0, again);
		hear(c, c, again, size);
		switch_to(c);
		request = find_request(friends.friends[0].code);
		check(!request, "a friend's ADD is no request");
	}
}

int main(int argc, char **argv)
{
	static const struct
	{
		const char *name;
		void (*run)(void);
	} cases[] = {
		{ "code", case_code }, { "file", case_file }, { "request", case_request }, { "ignore", case_ignore },
		{ "mutual", case_mutual }, { "offline", case_offline }, { "presence", case_presence },
		{ "hidden", case_hidden }, { "remove", case_remove }, { "block", case_block }, { "invite", case_invite },
		{ "join", case_join }, { "recent", case_recent }, { "forged", case_forged },
	};
	int index;

	if (argc < 2)
	{
		printf("usage: p2p_friends_check <case> [folder] [-v]\n");
		return 2;
	}
	verbose = argc > 3 && !strcmp(argv[3], "-v");
	if (argc > 2 && argv[2][0] != '-')
		snprintf(base_folder, sizeof(base_folder), "%s", argv[2]);
	else
	{
		char made[] = "/tmp/p2p_friends_check_XXXXXX";

		if (!mkdtemp(made))
			return 2;
		snprintf(base_folder, sizeof(base_folder), "%s", made);
	}
	for (index = 0; index < (int)(sizeof(cases) / sizeof(cases[0])); index++)
	{
		if (!strcmp(argv[1], cases[index].name))
		{
			cases[index].run();
			printf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
			return failures ? 1 : 0;
		}
	}
	printf("no case %s\n", argv[1]);
	return 2;
}
