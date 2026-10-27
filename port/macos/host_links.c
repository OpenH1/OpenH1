/*
HOST_LINKS.C

The links the system opens the Mac and iPhone apps with: invites
(openh1://join/...), parties' links (openh1://party/...) and the join page's
addresses of either (https://openh1.com/join/#party=..., #join=...). The
apps' Info.plist names them the openh1 scheme's handler (tools/macos_app.py,
tools/ios_app.py), and not the halo scheme's, OpenCE's, which older builds'
links have: one of those that reaches the app anyway is still taken.

The system gives a link to SDL's application delegate, at the launch it
started or while the game runs: on macOS the GetURL Apple event, on iOS
UIKit's scene:openURLContexts: (or the scene's launch options). SDL sends it
on as SDL_EVENT_DROP_FILE, whose text the guest cannot read (host_sdl.c).
The watch here (host_links_watch, from host_sdl_init, before SDL starts, so
that it sees a launch's link) keeps it, and the guest's internet play
takes it (host_app_take_link, which p2p.c's thread asks for, as Android's
reads join_link.txt), as it takes an invite handed over by a second copy
of the game on Linux and Windows: p2p_invite_received. A link that comes
before internet play starts waits for it.
*/

#include "host.h"

#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

enum
{
	/* the links kept until the guest takes them (the oldest goes first) */
	LINK_COUNT = 4,
	/* a link's room, with its end: p2p.c's (P2P_LINK_SIZE, and its
	handoff's 256) */
	LINK_SIZE = 256,
};

static pthread_mutex_t link_lock = PTHREAD_MUTEX_INITIALIZER;
static char links[LINK_COUNT][LINK_SIZE];
static int link_first, link_count;

/* whether text starts with prefix, in any case */
static int starts_with(const char *text, const char *prefix)
{
	for (; *prefix; text++, prefix++)
	{
		char letter = *text >= 'A' && *text <= 'Z' ? (char)(*text + 32) : *text;

		if (letter != *prefix)
			return 0;
	}
	return 1;
}

/* whether the system's text is one of the game's links (p2p.c reads which
game or party; anything else, a file dropped on the window, is left be) */
int host_links_is_link(const char *text)
{
	return text && (starts_with(text, "openh1://") || starts_with(text, "halo://") ||
		starts_with(text, "https://openh1.com/join/"));
}

void host_links_add(const char *text)
{
	char shown[48];
	int slot;

	if (!host_links_is_link(text) || strlen(text) >= LINK_SIZE)
		return;
	pthread_mutex_lock(&link_lock);
	if (link_count == LINK_COUNT)
	{
		link_first = (link_first + 1) % LINK_COUNT;
		link_count--;
	}
	slot = (link_first + link_count) % LINK_COUNT;
	snprintf(links[slot], LINK_SIZE, "%s", text);
	link_count++;
	pthread_mutex_unlock(&link_lock);
	/* (its start only: players post their logs, and an invite lets anyone
	in, as p2p_invite_log_text says) */
	snprintf(shown, sizeof(shown), "%.28s", text);
	host_logf(HOST_LOG_INFO, "links: the app was opened with %s...", shown);
}

/* the guest's (port/macos/guest_metal.c, port/linux/src/p2p.c): the oldest
link the app was opened with that the guest has not taken, 1; else 0 */
int host_app_take_link(char *link, int size)
{
	int taken = 0;

	if (!link || size <= 0)
		return 0;
	pthread_mutex_lock(&link_lock);
	if (link_count)
	{
		snprintf(link, (size_t)size, "%s", links[link_first]);
		link_first = (link_first + 1) % LINK_COUNT;
		link_count--;
		taken = 1;
	}
	pthread_mutex_unlock(&link_lock);
	return taken;
}

static bool SDLCALL link_event(void *userdata, SDL_Event *event)
{
	(void)userdata;
	if (event->type == SDL_EVENT_DROP_FILE && host_links_is_link(event->drop.data))
		host_links_add(event->drop.data);
	return true;
}

/* (host_sdl.c, before each SDL_Init: an SDL_Quit drops the watches, and one
added before SDL starts is kept, as it starts) */
void host_links_watch(void)
{
	SDL_RemoveEventWatch(link_event, NULL);
	SDL_AddEventWatch(link_event, NULL);
}
