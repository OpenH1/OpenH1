#include "harness.h"

#include <stdarg.h>
#include <wchar.h>

/* The server browsers' tabs, their words with no games, and the menus'
shared text fitting (port/linux/game/menu_functions.c, in Settings' look:
tools/port_settings.py's _browser): a tab lists its kind of game (OpenH1's,
Legacy, co-op) or the favorites, the favorites first and the rest sorted,
the favorites offline only on ALL and FAVORITES; a list with no games says
why, and the local network's what to do; a name too long for its column is
cut short with "..." where it fits, and one that fits is kept whole */

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define PIN(value, low, high) MIN(MAX(value, low), high)
#define NUMBEROF(array) ((long)(sizeof(array) / sizeof((array)[0])))
#define P2P_LISTING_NAME_SIZE 32
#define P2P_LISTING_MAP_SIZE 32
#define ROW_TEXT_LENGTH 64

/* the game's wide formatting: its %s a wide string */
static int usnprintf(wchar_t *text, size_t size, wchar_t const *format, ...)
{
	wchar_t wide_format[256];
	size_t from, to = 0;
	va_list arguments;
	int result;

	for (from = 0; format[from] && to < NUMBEROF(wide_format) - 2; from++)
	{
		wide_format[to++] = format[from];
		if (format[from] == '%' && format[from + 1] == '%')
			wide_format[to++] = format[++from];
		else if (format[from] == '%' && format[from + 1] == 's')
			wide_format[to++] = 'l';
	}
	wide_format[to] = 0;
	va_start(arguments, format);
	result = vswprintf(text, size + 1, wide_format, arguments);
	va_end(arguments);
	return result;
}

#include "config.inc"

static short browser_found_favorites, browser_offline_count;
static short browser_sort_column, browser_tab;
static boolean browser_sort_descending;

/* the fake font: each character 6 units wide, "." 2 */
static short settings_text_width(long font, wchar_t const *text)
{
	short width = 0;

	(void)font;
	for (; *text; text++)
		width = (short)(width + (*text == '.' ? 2 : 6));
	return width;
}

#include "under_test.inc"

static struct browser_sort_entry entry(char const *name, boolean openh1, boolean coop, short players)
{
	struct browser_sort_entry result;

	memset(&result, 0, sizeof(result));
	snprintf(result.name, sizeof(result.name), "%s", name);
	result.openh1 = openh1;
	result.coop = coop;
	result.players = players;
	result.rate = openh1 ? 60 : 30;
	return result;
}

/* the games a tab lists, their names in order, separated by spaces */
static void listed(short tab, char *names, size_t size, short *offline)
{
	struct browser_sort_entry entries[5];
	short favorite_of[5] = { NONE, 0, NONE, NONE, 1 };
	short order[5], count, index;

	entries[0] = entry("alpha", TRUE, FALSE, 3);
	entries[1] = entry("bravo", FALSE, FALSE, 9);
	entries[2] = entry("coop", FALSE, TRUE, 1);
	entries[3] = entry("delta", TRUE, FALSE, 12);
	entries[4] = entry("echo", TRUE, TRUE, 2);
	browser_tab = tab;
	browser_sort_column = _browser_sort_players;
	browser_sort_descending = TRUE;
	browser_offline_count = 2;
	count = browser_favorites_order(favorite_of, entries, 5, order);
	names[0] = 0;
	for (index = 0; index < count; index++)
	{
		strncat(names, entries[order[index]].name, size - strlen(names) - 2);
		strncat(names, " ", size - strlen(names) - 1);
	}
	*offline = browser_offline_count;
}

static boolean contains(wchar_t const *text, wchar_t const *part)
{
	return wcsstr(text, part) != NULL;
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	CASE("tabs")
	{
		char names[128];
		short offline;

		/* (the favorites first, bravo then echo by players; the rest by
		players) */
		listed(_browser_tab_all, names, sizeof(names), &offline);
		CHECK(!strcmp(names, "bravo echo delta alpha coop "), "ALL lists %s", names);
		CHECK(offline == 2, "ALL lists no favorites offline");
		listed(_browser_tab_openh1, names, sizeof(names), &offline);
		CHECK(!strcmp(names, "echo delta alpha "), "OPENH1 lists %s", names);
		CHECK(offline == 0, "OPENH1 lists the favorites offline");
		listed(_browser_tab_legacy, names, sizeof(names), &offline);
		CHECK(!strcmp(names, "bravo coop "), "LEGACY lists %s", names);
		listed(_browser_tab_coop, names, sizeof(names), &offline);
		CHECK(!strcmp(names, "echo coop "), "CO-OP lists %s", names);
		CHECK(offline == 0, "CO-OP lists the favorites offline");
		listed(_browser_tab_favorites, names, sizeof(names), &offline);
		CHECK(!strcmp(names, "bravo echo "), "FAVORITES lists %s", names);
		CHECK(offline == 2, "FAVORITES lists no favorites offline");
		return 0;
	}
	CASE("empty")
	{
		wchar_t text[256];

		/* the local network's: what to do */
		browser_empty_text(_multiplayer_mode_lan, _browser_tab_all, TRUE, TRUE, FALSE, 0, text, NUMBEROF(text));
		CHECK(contains(text, L"No games on your network yet.") && contains(text, L"Host one from Create Game."),
			"the local network's empty list says %ls", text);
		browser_empty_text(_multiplayer_mode_lan, _browser_tab_openh1, TRUE, TRUE, FALSE, 0, text, NUMBEROF(text));
		CHECK(contains(text, L"No OpenH1 games on your network."), "OPENH1's says %ls", text);
		browser_empty_text(_multiplayer_mode_lan, _browser_tab_favorites, TRUE, TRUE, FALSE, 0, text,
			NUMBEROF(text));
		CHECK(contains(text, L"favorites") && contains(text, L"%y-button"), "FAVORITES' says %ls", text);
		/* the server browser's: still looking, off, none of a kind, newer */
		browser_empty_text(_multiplayer_mode_server_browser, _browser_tab_all, TRUE, TRUE, TRUE, 0, text,
			NUMBEROF(text));
		CHECK(contains(text, L"Looking for public games"), "looking says %ls", text);
		browser_empty_text(_multiplayer_mode_server_browser, _browser_tab_all, FALSE, TRUE, FALSE, 0, text,
			NUMBEROF(text));
		CHECK(contains(text, L"Internet play is off"), "offline says %ls", text);
		browser_empty_text(_multiplayer_mode_server_browser, _browser_tab_coop, TRUE, TRUE, FALSE, 0, text,
			NUMBEROF(text));
		CHECK(contains(text, L"No co-op games"), "CO-OP's says %ls", text);
		browser_empty_text(_multiplayer_mode_server_browser, _browser_tab_all, TRUE, TRUE, FALSE, 3, text,
			NUMBEROF(text));
		CHECK(contains(text, L"3 games are on a newer version"), "newer says %ls", text);
		browser_empty_text(_multiplayer_mode_server_browser, _browser_tab_all, TRUE, TRUE, FALSE, 0, text,
			NUMBEROF(text));
		CHECK(contains(text, L"No public games found"), "none says %ls", text);
		/* Join by Link's: how to paste a link (its footer's prompt) */
		browser_empty_text(_multiplayer_mode_direct_link, _browser_tab_all, TRUE, TRUE, FALSE, 0, text,
			NUMBEROF(text));
		CHECK(contains(text, L"PASTE LINK"), "Join by Link's says %ls", text);
		return 0;
	}
	CASE("ping")
	{
		wchar_t text[16];

		/* the ping's column: its milliseconds, a faded "..." while it is
		measured, a dash while it is not known */
		browser_ping_cell(_browser_ping_measured, 38, text, NUMBEROF(text));
		CHECK(!wcscmp(text, L"38"), "38 ms shown as %ls", text);
		browser_ping_cell(_browser_ping_measuring, -1, text, NUMBEROF(text));
		CHECK(!wcscmp(text, L"\x2026"), "a ping being measured shown as %ls", text);
		browser_ping_cell(_browser_ping_locked, -1, text, NUMBEROF(text));
		CHECK(!wcscmp(text, L"\x2013"), "a game with a password's ping shown as %ls", text);
		browser_ping_cell(_browser_ping_failed, -1, text, NUMBEROF(text));
		CHECK(!wcscmp(text, L"\x2013"), "a host not reached's ping shown as %ls", text);
		browser_ping_cell(_browser_ping_local, -1, text, NUMBEROF(text));
		CHECK(!wcscmp(text, L"\x2013"), "a local network game's ping shown as %ls", text);
		/* (the bars' frame by how good: none lit, poor, fair, good) */
		CHECK(browser_ping_bars(-1) == 0 && browser_ping_bars(38) == 3 && browser_ping_bars(PING_GOOD) == 2 &&
			browser_ping_bars(PING_FAIR) == 1, "the bars do not follow the ping");
		return 0;
	}
	CASE("ellipsis")
	{
		wchar_t fitted[ROW_TEXT_LENGTH];

		/* (6 units a character: "FOOZY BG SNIPER" is 90) */
		menu_text_fit(1, L"FOOZY BG SNIPER", fitted, NUMBEROF(fitted), 90);
		CHECK(!wcscmp(fitted, L"FOOZY BG SNIPER"), "a name that fits cut: %ls", fitted);
		menu_text_fit(1, L"FOOZY BG SNIPER", fitted, NUMBEROF(fitted), 60);
		CHECK(!wcscmp(fitted, L"FOOZY BG..."), "cut to 60 units: %ls", fitted);
		CHECK(settings_text_width(1, fitted) <= 60, "the cut text is wider than its column: %ls", fitted);
		/* (the most that fits: one more character would not) */
		menu_text_fit(1, L"Assault on the Control Room", fitted, NUMBEROF(fitted), 84);
		CHECK(!wcscmp(fitted, L"Assault on th..."), "cut to 84 units, exactly its width: %ls", fitted);
		menu_text_fit(1, L"ABCDEFGHIJ", fitted, NUMBEROF(fitted), 4);
		CHECK(!wcscmp(fitted, L"..."), "nothing fits: %ls", fitted);
		/* (a font not known: all of it) */
		menu_text_fit(NONE, L"FOOZY BG SNIPER", fitted, NUMBEROF(fitted), 10);
		CHECK(!wcscmp(fitted, L"FOOZY BG SNIPER"), "cut without its font: %ls", fitted);
		return 0;
	}
	CHECK(FALSE, "no such case: %s", case_name);
	return 1;
}
