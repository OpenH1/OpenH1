/*
WHATS_NEW.H

What's new (whats_new.c, docs/menu_screens.md): a card over the main menu,
the first time it is shown after the game was updated to a new build, with
that build's highlights from its release notes (whats_new.txt, which a
release's build embeds with the menus' files: tools/release_notes.py), and
never again until the next update.
*/

#ifndef WHATS_NEW_H
#define WHATS_NEW_H

/* the most highlights a card shows, and the longest of each, in bytes */
#define WHATS_NEW_MAXIMUM_LINES 5
#define WHATS_NEW_LINE_LENGTH 128

/* the notes, parsed (whats_new.txt): the build they are of (0 for none, or
not notes), its release's page ("" for none) and its highlights */
struct whats_new_notes
{
	long build;
	char page[256];
	char lines[WHATS_NEW_MAXIMUM_LINES][WHATS_NEW_LINE_LENGTH];
	int line_count;
};

/* the notes' text read into *notes (nonzero if they are a build's, with
at least one highlight). The text is

	build 2290
	page https://github.com/OpenH1/goath1/releases/tag/build-2290
	- A highlight
	- Another

its lines ending in \n (or \r\n); others are left out */
int whats_new_parse(char const *text, unsigned long size, struct whats_new_notes *notes);

/* what to do at the main menu, from the notes' build and the build seen
last (0: none noted) and whether config.toml was new (a fresh install):
WHATS_NEW_SHOW the card, WHATS_NEW_NOTE the build seen without it (a
fresh install), WHATS_NEW_NOTHING */
enum
{
	WHATS_NEW_NOTHING,
	WHATS_NEW_SHOW,
	WHATS_NEW_NOTE
};
int whats_new_decide(long build, long seen, int fresh);

/* ---------- the game's (whats_new.c) */

/* between frames: once the new menu engine's main menu has been drawn,
the card opened over it if it is due (menu_functions.c's
pc_menu_whats_new_open), else the build noted as seen if it is a fresh
install's */
void whats_new_poll(void);
/* the card's build, and its index'th highlight (nonzero if it has one),
for its screen's bindings (menu_screen.c) */
long whats_new_build(void);
int whats_new_line(int index, char *text, unsigned long size);
/* the card dismissed (OK, B): its build noted as seen; read_all opens its
release's page first (READ ALL) */
void whats_new_dismiss(int read_all);

#endif
