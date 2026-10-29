/*
BOTS_SIGHTINGS.C (test)

A team's shared sightings (bots_knowledge.c's bots_knowledge_team_note,
_team_pass and _fresh): a sighting reaches the team's other bots the
delay after it was seen, as a callout would; while one report is on its
way the next waits, then goes with the last sighting there is; a sighting
is of use for BOTS_KNOWLEDGE_SEEN_MEMORY_TICKS; the memory is bounded, the
oldest let go. What a bot knows (bots_knowledge_slot_known, where it goes
and looks): its own sightings and, with sharing on, the reports that reached
its team, never the team's last sighting before its report came; with
sharing off its own alone. test_bots_sightings.py takes the constants and structures
(config.inc) and the code (under_test.inc).
*/

#include "harness.h"

#define csmemset memset

#include "config.inc"

/* the game's players, as bots_knowledge_sighting_player reads them */
struct player_datum
{
	boolean quit_out_of_game;
	long unit_index;
	short team;
};

static struct player_datum players[16];
static long game_now;

static struct player_datum *player_try_and_get(long player_index)
{
	return player_index >= 0 && player_index < 16 ? &players[player_index] : NULL;
}

static short bots_knowledge_team(struct player_datum const *player)
{
	return player->team;
}

static long game_time_get(void)
{
	return game_now;
}

struct bots_knowledge_slot
{
	long player_index;
	struct bots_knowledge_sighting seen[BOTS_KNOWLEDGE_BOT_MEMORY];
};

static struct
{
	boolean share;
	struct bots_knowledge_team_sighting team_seen[2][BOTS_KNOWLEDGE_TEAM_MEMORY];
} bots_knowledge;

#include "under_test.inc"

/* where the bot (its slot, its team) knows the player was, as its
knowledge gives it; FALSE for not known */
static boolean known_position(struct bots_knowledge_slot const *slot, short team, long player_index,
	real_point3d *position)
{
	struct bots_known_enemy known[BOTS_KNOWLEDGE_MAXIMUM_KNOWN];
	short count = bots_knowledge_slot_known(slot, team, known, BOTS_KNOWLEDGE_MAXIMUM_KNOWN), index;

	for (index = 0; index < count; index++)
	{
		if (known[index].player_index == player_index)
		{
			*position = known[index].position;
			return TRUE;
		}
	}
	return FALSE;
}

#define TEAM_MEMORY 4

static struct bots_knowledge_team_sighting memory[TEAM_MEMORY];

static struct bots_knowledge_sighting sighting_of(long player_index, long tick, real x)
{
	struct bots_knowledge_sighting sighting;

	memset(&sighting, 0, sizeof(sighting));
	sighting.player_index = player_index;
	sighting.unit_index = player_index + 100;
	sighting.tick = tick;
	sighting.seen_by = 7;
	sighting.position.x = x;
	return sighting;
}

/* a tick of the game: the reports on their way passed (bots_knowledge_tick_begin's) */
static void tick(long now, long delay)
{
	long index;

	for (index = 0; index < TEAM_MEMORY; index++)
		bots_knowledge_team_pass(&memory[index], now, delay);
}

static void see(long player_index, long now, real x, long delay)
{
	struct bots_knowledge_sighting sighting = sighting_of(player_index, now, x);

	bots_knowledge_team_note(memory, TEAM_MEMORY, &sighting, now, delay);
}

static struct bots_knowledge_team_sighting *entry_of(long player_index)
{
	long index;

	for (index = 0; index < TEAM_MEMORY; index++)
	{
		if (memory[index].latest.tick > 0 && memory[index].latest.player_index == player_index)
			return &memory[index];
	}
	return NULL;
}

/* one glimpse: known to the team only once the delay has passed */
static void case_delay(void)
{
	long delay = TICKS_PER_SECOND, now;
	struct bots_knowledge_team_sighting *entry;

	see(3, 100, 5.0f, delay);
	entry = entry_of(3);
	CHECK(entry, "the sighting kept");
	CHECK(entry->latest.tick == 100, "the last sighting at once");
	for (now = 100; now < 100 + delay; now++)
	{
		tick(now, delay);
		CHECK(entry->heard.tick == 0, "heard at %ld, before the delay's end", now);
	}
	tick(100 + delay, delay);
	CHECK(entry->heard.tick == 100 && entry->heard.position.x == 5.0f, "heard as the delay ends");
	CHECK(entry->heard_tick == 100 + delay, "the report's arrival kept");
	CHECK(entry->sent.tick == 0, "nothing more on its way");
}

/* an enemy kept in sight: the team has it a delay late, a delay apart */
static void case_cadence(void)
{
	long delay = TICKS_PER_SECOND, now, updates = 0, last_heard = 0, last_heard_at = 0;
	struct bots_knowledge_team_sighting *entry;

	for (now = 1; now <= 10 * TICKS_PER_SECOND; now++)
	{
		tick(now, delay);
		/* (seen every 7 ticks, near the survey's 10, out of step with the reports) */
		if (now % 7 == 1)
			see(4, now, (real)now, delay);
		entry = entry_of(4);
		CHECK(entry, "the sighting kept");
		if (entry->heard.tick != last_heard)
		{
			CHECK(now - entry->heard.tick >= delay, "heard %ld ticks after it was seen", now - entry->heard.tick);
			/* (late by the delay, and by the sightings' period at most) */
			CHECK(now - entry->heard.tick <= delay + 7, "a report %ld ticks late", now - entry->heard.tick);
			if (last_heard_at)
				CHECK(now - last_heard_at == delay, "reports %ld ticks apart", now - last_heard_at);
			last_heard = entry->heard.tick;
			last_heard_at = now;
			updates++;
		}
	}
	CHECK(updates >= 8 && updates <= 10, "%ld reports in 10 seconds", updates);
}

/* no delay: heard at once */
static void case_no_delay(void)
{
	see(5, 50, 2.0f, 0);
	CHECK(entry_of(5) && entry_of(5)->heard.tick == 50, "heard at once");
	see(5, 51, 3.0f, 0);
	CHECK(entry_of(5)->heard.tick == 51 && entry_of(5)->heard.position.x == 3.0f, "each sighting heard at once");
}

/* a sighting of use for the memory's ticks, then not */
static void case_expiry(void)
{
	struct bots_knowledge_sighting sighting = sighting_of(6, 200, 1.0f);

	CHECK(bots_knowledge_fresh(&sighting, 200), "fresh as seen");
	CHECK(bots_knowledge_fresh(&sighting, 200 + BOTS_KNOWLEDGE_SEEN_MEMORY_TICKS), "fresh to the memory's end");
	CHECK(!bots_knowledge_fresh(&sighting, 201 + BOTS_KNOWLEDGE_SEEN_MEMORY_TICKS), "forgotten after it");
	sighting.tick = 0;
	CHECK(!bots_knowledge_fresh(&sighting, 0), "none is not fresh");
}

/* more enemies than the memory: the oldest let go, the others kept */
static void case_bounded(void)
{
	long player;

	for (player = 0; player < TEAM_MEMORY; player++)
		see(player + 10, 300 + player, 0.0f, TICKS_PER_SECOND);
	see(99, 400, 0.0f, TICKS_PER_SECOND);
	CHECK(!entry_of(10), "the oldest let go");
	CHECK(entry_of(11) && entry_of(12) && entry_of(13) && entry_of(99), "the others kept");
	/* (a new enemy in a slot let go starts with no report heard) */
	CHECK(entry_of(99)->heard.tick == 0 && entry_of(99)->sent.tick == 400, "a new entry's report on its way");
}

/* what a bot knows for an order: its own sighting, or its team's report
once it came; never the team's sighting still on its way; nothing of its
team's with sharing off */
static void case_known(void)
{
	long delay = TICKS_PER_SECOND;
	struct bots_knowledge_slot slot;
	struct bots_knowledge_sighting own;
	real_point3d position;
	long index;

	memset(&slot, 0, sizeof(slot));
	memset(&bots_knowledge, 0, sizeof(bots_knowledge));
	for (index = 0; index < 16; index++)
	{
		players[index].unit_index = index + 100;
		players[index].team = index < 8 ? 0 : 1;
	}
	/* (team 0's bot 7 sees enemy 9 at tick 100; the bot asked is another
	of team 0, which saw nothing) */
	bots_knowledge.share = TRUE;
	game_now = 100;
	{
		struct bots_knowledge_sighting sighting = sighting_of(9, 100, 4.0f);

		bots_knowledge_team_note(bots_knowledge.team_seen[0], BOTS_KNOWLEDGE_TEAM_MEMORY, &sighting, 100, delay);
	}
	CHECK(!known_position(&slot, 0, 9, &position), "known before its report came");
	game_now = 100 + delay;
	for (index = 0; index < BOTS_KNOWLEDGE_TEAM_MEMORY; index++)
		bots_knowledge_team_pass(&bots_knowledge.team_seen[0][index], game_now, delay);
	CHECK(known_position(&slot, 0, 9, &position) && position.x == 4.0f, "the report not known once it came");
	CHECK(!known_position(&slot, 1, 9, &position), "an enemy's own team knows it as its enemy");
	/* (a later sighting still on its way is not known; the report is) */
	{
		struct bots_knowledge_sighting sighting = sighting_of(9, game_now, 8.0f);

		bots_knowledge_team_note(bots_knowledge.team_seen[0], BOTS_KNOWLEDGE_TEAM_MEMORY, &sighting, game_now, delay);
	}
	CHECK(known_position(&slot, 0, 9, &position) && position.x == 4.0f, "the team's sighting on its way was known");
	/* (its own later sighting wins over the report) */
	own = sighting_of(9, game_now, 6.0f);
	slot.seen[0] = own;
	CHECK(known_position(&slot, 0, 9, &position) && position.x == 6.0f, "its own later sighting not taken");
	/* (sharing off: its own alone) */
	bots_knowledge.share = FALSE;
	CHECK(known_position(&slot, 0, 9, &position) && position.x == 6.0f, "its own sighting lost with sharing off");
	slot.seen[0].tick = 0;
	CHECK(!known_position(&slot, 0, 9, &position), "a teammate's report known with sharing off");
	/* (a sighting of a life before tells nothing) */
	bots_knowledge.share = TRUE;
	players[9].unit_index = 555;
	CHECK(!known_position(&slot, 0, 9, &position), "a sighting of a life before known");
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";

	memset(memory, 0, sizeof(memory));
	if (!strcmp(name, "delay"))
		case_delay();
	else if (!strcmp(name, "cadence"))
		case_cadence();
	else if (!strcmp(name, "no-delay"))
		case_no_delay();
	else if (!strcmp(name, "expiry"))
		case_expiry();
	else if (!strcmp(name, "bounded"))
		case_bounded();
	else if (!strcmp(name, "known"))
		case_known();
	else
	{
		fprintf(stderr, "unknown case %s\n", name);
		return 2;
	}
	return 0;
}
