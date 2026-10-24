/*
BROWSER_FAVORITES.H

The server browsers' favorite games (browser_favorites.c): kept per machine
in favorites.txt, beside config.toml. A favorite is known by its server's
identity, not its name: an internet game's by its host's internet-play
identifier ("net:" and twelve hex digits), a local network game's by its
host's address ("lan:a.b.c.d"). Its last known name, map and gametype, and
when it was last seen, are kept for showing it while it is offline.

Only this machine's browsers read it: nothing is sent.
*/

#ifndef BROWSER_FAVORITES_H
#define BROWSER_FAVORITES_H

#include <stddef.h>

enum
{
	BROWSER_FAVORITES_MAXIMUM = 64,
	BROWSER_FAVORITE_KEY_SIZE = 24,
	BROWSER_FAVORITE_TEXT_SIZE = 33,
	/* the file read at most */
	BROWSER_FAVORITES_FILE_MAXIMUM = 64 * 1024,
};

struct browser_favorite
{
	char key[BROWSER_FAVORITE_KEY_SIZE];
	/* printable ASCII, without tabs */
	char name[BROWSER_FAVORITE_TEXT_SIZE];
	char map[BROWSER_FAVORITE_TEXT_SIZE];
	char gametype[BROWSER_FAVORITE_TEXT_SIZE];
	/* the game's engine and OpenCE kind, as the browsers show its type */
	int engine_type, kind;
	/* seconds since 1970, 0 if never */
	long long last_seen;
};

/* an internet game's key, from its host's six-byte identifier */
void browser_favorite_key_internet(const unsigned char *identifier, char *key);
/* a local network game's key, from its host's IPv4 address (in network
order, as an in_addr's s_addr) */
void browser_favorite_key_address(unsigned long address, char *key);
/* whether a key is one of the two forms */
int browser_favorite_key_valid(const char *key);

/* the favorites (read from the file the first time) */
int browser_favorites_count(void);
const struct browser_favorite *browser_favorites_get(int index);
/* a favorite's index by its key, else -1 */
int browser_favorites_find(const char *key);
/* the game seen now: made a favorite if it is not one (1), else no longer
one (0); -1 if it cannot be (its key is not valid, or there are as many
favorites as are kept) */
int browser_favorites_toggle(const struct browser_favorite *game);
/* a favorite seen now (its key the game's): its name, map, gametype and
time kept; written down when they change, or every few minutes */
void browser_favorites_seen(int index, const struct browser_favorite *game);
/* a favorite's key changed: an internet game's host has a new identifier
each time it starts, so the browsers find a favorite again by its last
known name (menu_functions.c) and give it the new one */
void browser_favorites_rekey(int index, const char *key);

/* "just now", "5 min ago", "3 h ago", "2 days ago", "never" */
void browser_favorite_last_seen_text(long long last_seen, long long now, char *text, size_t size);
long long browser_favorites_now(void);

/* the file's text read into favorites (at most `maximum`), skipping what is
not valid; returns how many */
int browser_favorites_parse(const char *text, size_t size, struct browser_favorite *favorites, int maximum);
/* a key's index in favorites, else -1 */
int browser_favorites_find_in(const struct browser_favorite *favorites, int count, const char *key);
/* favorites written as the file's text, NUL terminated; returns its length,
or 0 if it does not fit */
size_t browser_favorites_format(const struct browser_favorite *favorites, int count, char *text, size_t size);

#endif
