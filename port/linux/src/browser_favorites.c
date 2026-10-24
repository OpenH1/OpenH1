/*
BROWSER_FAVORITES.C

The server browsers' favorite games (browser_favorites.h), in favorites.txt
beside config.toml: a line each, its fields separated by tabs,

	net:02a1b2c3d4e5	1729700000	0	2	Bob's Slayer	bloodgulch	Slayer

(the key, when it was last seen in seconds since 1970, its engine and kind,
its last known name, map and gametype). Lines starting with # are comments.

The file is untrusted: it is read up to BROWSER_FAVORITES_FILE_MAXIMUM
bytes, and a line whose key is not one of the two forms, or whose numbers
are not numbers, is left out; text keeps only printable ASCII, cut to its
field's size. It is written whole, to favorites.txt.new first, which then
takes its place.
*/

#include "browser_favorites.h"
#include "port_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef HALO_GUEST
#include <SDL3/SDL.h>
#endif

/* how often a favorite seen is written down, in seconds, when only its time
changed */
#define SEEN_WRITE_INTERVAL 300

static struct
{
	int loaded;
	int count;
	struct browser_favorite favorites[BROWSER_FAVORITES_MAXIMUM];
	/* the last_seen of each as last written */
	long long written_seen[BROWSER_FAVORITES_MAXIMUM];
} favorites_state;

/* ---------- keys */

void browser_favorite_key_internet(const unsigned char *identifier, char *key)
{
	static const char digits[] = "0123456789abcdef";
	int index;

	memcpy(key, "net:", 4);
	for (index = 0; index < 6; index++)
	{
		key[4 + index * 2] = digits[identifier[index] >> 4];
		key[5 + index * 2] = digits[identifier[index] & 15];
	}
	key[16] = 0;
}

void browser_favorite_key_address(unsigned long address, char *key)
{
	const unsigned char *bytes = (const unsigned char *)&address;

	snprintf(key, BROWSER_FAVORITE_KEY_SIZE, "lan:%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
}

int browser_favorite_key_valid(const char *key)
{
	int index;

	if (!strncmp(key, "net:", 4))
	{
		for (index = 4; index < 16; index++)
		{
			if (!((key[index] >= '0' && key[index] <= '9') || (key[index] >= 'a' && key[index] <= 'f')))
				return 0;
		}
		return key[16] == 0;
	}
	if (!strncmp(key, "lan:", 4))
	{
		const char *part = key + 4;
		int parts;

		for (parts = 0; parts < 4; parts++)
		{
			int digits = 0, value = 0;

			while (*part >= '0' && *part <= '9' && digits < 3)
			{
				value = value * 10 + (*part++ - '0');
				digits++;
			}
			if (!digits || value > 255)
				return 0;
			if (parts < 3 && *part++ != '.')
				return 0;
		}
		return *part == 0;
	}
	return 0;
}

/* ---------- the file's text */

/* text kept as printable ASCII without tabs, cut to `size` */
static void text_clean(char *destination, const char *text, size_t length, size_t size)
{
	size_t read, written = 0;

	for (read = 0; read < length && text[read] && written + 1 < size; read++)
	{
		unsigned char character = (unsigned char)text[read];

		if (character >= 0x20 && character < 0x7F)
			destination[written++] = (char)character;
	}
	destination[written] = 0;
}

/* a whole number of the file's, at most `most`; 0 if it is not one */
static int number_read(const char *text, size_t length, long long most, long long *value)
{
	size_t index;

	*value = 0;
	if (!length || length > 19)
		return 0;
	for (index = 0; index < length; index++)
	{
		if (text[index] < '0' || text[index] > '9')
			return 0;
		*value = *value * 10 + (text[index] - '0');
	}
	return *value <= most;
}

int browser_favorites_parse(const char *text, size_t size, struct browser_favorite *favorites, int maximum)
{
	size_t offset = 0;
	int count = 0;

	if (size > BROWSER_FAVORITES_FILE_MAXIMUM)
		size = BROWSER_FAVORITES_FILE_MAXIMUM;
	while (offset < size && count < maximum)
	{
		const char *fields[7];
		size_t lengths[7];
		size_t end = offset;
		size_t position = offset;
		int field;
		struct browser_favorite favorite;
		long long number;

		while (end < size && text[end] != '\n' && text[end])
			end++;
		/* (its fields, the last of them possibly empty; past the seventh
		left out) */
		for (field = 0; field < 7 && position <= end; field++)
		{
			size_t stop = position;

			while (stop < end && text[stop] != '\t')
				stop++;
			fields[field] = text + position;
			lengths[field] = stop - position;
			if (lengths[field] && fields[field][lengths[field] - 1] == '\r')
				lengths[field]--;
			position = stop + 1;
		}
		offset = end + 1;
		if (field < 7 || fields[0][0] == '#' || lengths[0] >= BROWSER_FAVORITE_KEY_SIZE)
			continue;
		memset(&favorite, 0, sizeof(favorite));
		memcpy(favorite.key, fields[0], lengths[0]);
		favorite.key[lengths[0]] = 0;
		if (!browser_favorite_key_valid(favorite.key) || browser_favorites_find_in(favorites, count, favorite.key) >= 0)
			continue;
		if (!number_read(fields[1], lengths[1], 0x7FFFFFFFFFFFLL, &favorite.last_seen))
			continue;
		if (!number_read(fields[2], lengths[2], 255, &number))
			continue;
		favorite.engine_type = (int)number;
		if (!number_read(fields[3], lengths[3], 255, &number))
			continue;
		favorite.kind = (int)number;
		text_clean(favorite.name, fields[4], lengths[4], sizeof(favorite.name));
		text_clean(favorite.map, fields[5], lengths[5], sizeof(favorite.map));
		text_clean(favorite.gametype, fields[6], lengths[6], sizeof(favorite.gametype));
		favorites[count++] = favorite;
	}
	return count;
}

size_t browser_favorites_format(const struct browser_favorite *favorites, int count, char *text, size_t size)
{
	size_t length;
	int index;

	length = (size_t)snprintf(text, size, "# The server browsers' favorite games (OpenH1): key, last seen, engine,"
		" kind, name, map, gametype\n");
	if (length >= size)
		return 0;
	for (index = 0; index < count; index++)
	{
		const struct browser_favorite *favorite = &favorites[index];
		char name[BROWSER_FAVORITE_TEXT_SIZE], map[BROWSER_FAVORITE_TEXT_SIZE], gametype[BROWSER_FAVORITE_TEXT_SIZE];
		int written;

		text_clean(name, favorite->name, sizeof(favorite->name), sizeof(name));
		text_clean(map, favorite->map, sizeof(favorite->map), sizeof(map));
		text_clean(gametype, favorite->gametype, sizeof(favorite->gametype), sizeof(gametype));
		written = snprintf(text + length, size - length, "%s\t%lld\t%d\t%d\t%s\t%s\t%s\n", favorite->key,
			favorite->last_seen, favorite->engine_type & 255, favorite->kind & 255, name, map, gametype);
		if (written < 0 || (size_t)written >= size - length)
			return 0;
		length += (size_t)written;
	}
	return length;
}

/* ---------- the favorites */

int browser_favorites_find_in(const struct browser_favorite *favorites, int count, const char *key)
{
	int index;

	for (index = 0; index < count; index++)
	{
		if (!strcmp(favorites[index].key, key))
			return index;
	}
	return -1;
}

static void favorites_path(char *path, size_t size)
{
	config_folder(path, size);
	snprintf(path + strlen(path), size - strlen(path), "favorites.txt");
}

static void favorites_load(void)
{
	char path[1100];
	size_t size = 0;
	char *text;
	int index;

	if (favorites_state.loaded)
		return;
	favorites_state.loaded = 1;
	favorites_path(path, sizeof(path));
	text = config_file_read(path, &size);
	if (!text)
		return;
	favorites_state.count = browser_favorites_parse(text, size, favorites_state.favorites, BROWSER_FAVORITES_MAXIMUM);
	free(text);
	for (index = 0; index < favorites_state.count; index++)
		favorites_state.written_seen[index] = favorites_state.favorites[index].last_seen;
}

static int file_write(const char *path, const char *text)
{
	char temporary[1104];
	int written;
#ifdef HALO_GUEST
	FILE *file;
#endif

	snprintf(temporary, sizeof(temporary), "%s.new", path);
#ifdef HALO_GUEST
	file = fopen(temporary, "wb");
	if (!file)
		return 0;
	written = fwrite(text, 1, strlen(text), file) == strlen(text);
	written = fclose(file) == 0 && written;
	if (written)
		written = rename(temporary, path) == 0;
	if (!written)
		remove(temporary);
#else
	written = SDL_SaveFile(temporary, text, strlen(text));
	if (written)
		written = SDL_RenamePath(temporary, path);
	if (!written)
		SDL_RemovePath(temporary);
#endif
	return written;
}

static void favorites_save(void)
{
	static char text[BROWSER_FAVORITES_MAXIMUM * 160 + 256];
	char path[1100];
	int index;

	if (!browser_favorites_format(favorites_state.favorites, favorites_state.count, text, sizeof(text)))
		return;
	favorites_path(path, sizeof(path));
	file_write(path, text);
	for (index = 0; index < favorites_state.count; index++)
		favorites_state.written_seen[index] = favorites_state.favorites[index].last_seen;
}

int browser_favorites_count(void)
{
	favorites_load();
	return favorites_state.count;
}

const struct browser_favorite *browser_favorites_get(int index)
{
	favorites_load();
	return index >= 0 && index < favorites_state.count ? &favorites_state.favorites[index] : NULL;
}

int browser_favorites_find(const char *key)
{
	favorites_load();
	return browser_favorites_find_in(favorites_state.favorites, favorites_state.count, key);
}

static void favorite_fill(struct browser_favorite *favorite, const struct browser_favorite *game)
{
	text_clean(favorite->name, game->name, sizeof(game->name), sizeof(favorite->name));
	text_clean(favorite->map, game->map, sizeof(game->map), sizeof(favorite->map));
	text_clean(favorite->gametype, game->gametype, sizeof(game->gametype), sizeof(favorite->gametype));
	favorite->engine_type = game->engine_type & 255;
	favorite->kind = game->kind & 255;
	favorite->last_seen = browser_favorites_now();
}

int browser_favorites_toggle(const struct browser_favorite *game)
{
	int index;

	if (!browser_favorite_key_valid(game->key))
		return -1;
	index = browser_favorites_find(game->key);
	if (index >= 0)
	{
		memmove(&favorites_state.favorites[index], &favorites_state.favorites[index + 1],
			(size_t)(favorites_state.count - index - 1) * sizeof(favorites_state.favorites[0]));
		favorites_state.count--;
		favorites_save();
		return 0;
	}
	if (favorites_state.count >= BROWSER_FAVORITES_MAXIMUM)
		return -1;
	index = favorites_state.count++;
	memset(&favorites_state.favorites[index], 0, sizeof(favorites_state.favorites[index]));
	snprintf(favorites_state.favorites[index].key, BROWSER_FAVORITE_KEY_SIZE, "%s", game->key);
	favorite_fill(&favorites_state.favorites[index], game);
	favorites_save();
	return 1;
}

void browser_favorites_seen(int index, const struct browser_favorite *game)
{
	struct browser_favorite *favorite;
	struct browser_favorite before;

	favorites_load();
	if (index < 0 || index >= favorites_state.count)
		return;
	favorite = &favorites_state.favorites[index];
	before = *favorite;
	favorite_fill(favorite, game);
	if (strcmp(before.name, favorite->name) || strcmp(before.map, favorite->map) ||
		strcmp(before.gametype, favorite->gametype) || before.engine_type != favorite->engine_type ||
		before.kind != favorite->kind ||
		favorite->last_seen - favorites_state.written_seen[index] >= SEEN_WRITE_INTERVAL)
	{
		favorites_save();
	}
}

void browser_favorites_rekey(int index, const char *key)
{
	favorites_load();
	if (index < 0 || index >= favorites_state.count || !browser_favorite_key_valid(key) ||
		browser_favorites_find(key) >= 0)
	{
		return;
	}
	snprintf(favorites_state.favorites[index].key, BROWSER_FAVORITE_KEY_SIZE, "%s", key);
	favorites_save();
}

long long browser_favorites_now(void)
{
	return (long long)time(NULL);
}

void browser_favorite_last_seen_text(long long last_seen, long long now, char *text, size_t size)
{
	long long seconds = now - last_seen;

	if (last_seen <= 0)
		snprintf(text, size, "never");
	else if (seconds < 60)
		snprintf(text, size, "just now");
	else if (seconds < 3600)
		snprintf(text, size, "%d min ago", (int)(seconds / 60));
	else if (seconds < 86400)
		snprintf(text, size, "%d h ago", (int)(seconds / 3600));
	else if (seconds < 2 * 86400)
		snprintf(text, size, "yesterday");
	else
		snprintf(text, size, "%d days ago", (int)(seconds / 86400 > 9999 ? 9999 : seconds / 86400));
}
