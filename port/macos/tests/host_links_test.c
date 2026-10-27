/*
HOST_LINKS_TEST.C

port/macos/host_links.c, with SDL's event watches stood in for: the links
SDL's application delegate sends as drop events (macOS's GetURL event,
UIKit's openURLContexts) are kept for the guest (host_app_take_link),
oldest first, older builds' halo:// links among them, and nothing else is.
tools/test_macos_app.py builds and runs it.
*/

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void host_links_watch(void);
int host_app_take_link(char *link, int size);

static SDL_EventFilter watch;
static int watch_count;
static int failures;

/* (SDL's, which host_links.c calls) */
bool SDL_AddEventWatch(SDL_EventFilter filter, void *userdata)
{
	(void)userdata;
	watch = filter;
	watch_count++;
	return true;
}

void SDL_RemoveEventWatch(SDL_EventFilter filter, void *userdata)
{
	(void)userdata;
	if (watch == filter && watch_count)
		watch_count--;
}

void host_logf(int priority, const char *format, ...)
{
	va_list arguments;

	(void)priority;
	va_start(arguments, format);
	vfprintf(stdout, format, arguments);
	va_end(arguments);
	fputc('\n', stdout);
}

#define CHECK(condition, ...) \
	do \
	{ \
		if (!(condition)) \
		{ \
			printf("FAIL: " __VA_ARGS__); \
			printf("\n"); \
			failures++; \
		} \
	} while (0)

/* the system opens the app with text, as SDL's delegate sends it */
static void opened(Uint32 type, const char *text)
{
	SDL_Event event;

	memset(&event, 0, sizeof(event));
	event.type = type;
	event.drop.data = text;
	CHECK(watch && watch(NULL, &event), "the watch lets every event through");
}

static const char *taken(void)
{
	static char link[256];

	return host_app_take_link(link, sizeof(link)) ? link : NULL;
}

#define PARTY "openh1://party/00112233445566778899aabbccddeeff"
#define GAME "openh1://join/0123456789abcdef0123456789abcdef00112233445566778899aabbccddeeff"
/* an older build's (OpenCE's scheme, which the app no longer registers) */
#define OLD_GAME "halo://join/0123456789abcdef0123456789abcdef00112233445566778899aabbccddeeff"
#define PAGE "https://openh1.com/join/#party=00112233445566778899aabbccddeeff"

int main(void)
{
	char long_link[400];
	char small[8];
	const char *link;
	int index;

	host_links_watch();
	host_links_watch();
	CHECK(watch_count == 1, "one watch, however often SDL starts (%d)", watch_count);
	CHECK(!taken(), "no link before one comes");

	/* a party's link, a game's, an older build's, in any case */
	opened(SDL_EVENT_DROP_FILE, PARTY);
	opened(SDL_EVENT_DROP_FILE, GAME);
	opened(SDL_EVENT_DROP_FILE, "OpenH1://Party/00112233445566778899aabbccddeeff");
	opened(SDL_EVENT_DROP_FILE, OLD_GAME);
	link = taken();
	CHECK(link && !strcmp(link, PARTY), "the party's link first: %s", link ? link : "(none)");
	link = taken();
	CHECK(link && !strcmp(link, GAME), "the game's link next: %s", link ? link : "(none)");
	link = taken();
	CHECK(link && !strcmp(link, "OpenH1://Party/00112233445566778899aabbccddeeff"), "a link in capitals: %s",
		link ? link : "(none)");
	link = taken();
	CHECK(link && !strcmp(link, OLD_GAME), "an older build's link: %s", link ? link : "(none)");
	CHECK(!taken(), "each link taken once");

	/* not links: a file dropped on the window, another site, text dragged
	in, the drop's end, a link too long for the guest's room */
	opened(SDL_EVENT_DROP_FILE, "/Users/someone/Downloads/halo.iso");
	opened(SDL_EVENT_DROP_FILE, "https://example.com/join/#party=00112233445566778899aabbccddeeff");
	opened(SDL_EVENT_DROP_FILE, NULL);
	opened(SDL_EVENT_DROP_TEXT, PARTY);
	opened(SDL_EVENT_DROP_COMPLETE, NULL);
	memset(long_link, 'a', sizeof(long_link));
	memcpy(long_link, "openh1://party/", 15);
	long_link[sizeof(long_link) - 1] = 0;
	opened(SDL_EVENT_DROP_FILE, long_link);
	CHECK(!taken(), "nothing else is kept");

	/* the join page's address; the newest four kept of more */
	opened(SDL_EVENT_DROP_FILE, PAGE);
	link = taken();
	CHECK(link && !strcmp(link, PAGE), "the join page's address: %s", link ? link : "(none)");
	for (index = 0; index < 6; index++)
	{
		char text[64];

		snprintf(text, sizeof(text), "openh1://party/%032d", index);
		opened(SDL_EVENT_DROP_FILE, text);
	}
	for (index = 2; index < 6; index++)
	{
		char text[64];

		snprintf(text, sizeof(text), "openh1://party/%032d", index);
		link = taken();
		CHECK(link && !strcmp(link, text), "link %d of the last four: %s", index, link ? link : "(none)");
	}
	CHECK(!taken(), "the two oldest gave way");

	/* the guest's room is never overrun */
	opened(SDL_EVENT_DROP_FILE, PARTY);
	memset(small, 'x', sizeof(small));
	CHECK(host_app_take_link(small, sizeof(small)) && !strcmp(small, "openh1:"), "cut to the room: %.8s", small);
	CHECK(!host_app_take_link(NULL, 0), "no room, nothing taken");

	if (failures)
		return 1;
	printf("host_links: all checks passed\n");
	return 0;
}
