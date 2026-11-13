/*
FRIENDS_PANEL.H

The main menu's FRIENDS panel (docs/menu_screens.md, "<friends/>"): what it
shows, read from the friends' list (friends_menu.inc, port/linux/src/
p2p_friends.c's), and what its rows do. menu_screen.c makes and places its
widgets; friends_menu.inc says what is in them, so that the panel and the
Friends screen share one list, one join and one set of words.
*/

#ifndef FRIENDS_PANEL_H
#define FRIENDS_PANEL_H

/* the most rows the panel shows, and its texts' sizes (UTF-8, with their
ends): a friend's name (p2p.h's P2P_FRIEND_NAME_SIZE), a note, a line */
#define FRIENDS_PANEL_ROWS 6
#define FRIENDS_PANEL_CODE_BYTES 7
#define FRIENDS_PANEL_NAME_SIZE 16
#define FRIENDS_PANEL_NOTE_SIZE 24
#define FRIENDS_PANEL_LINE_SIZE 48

/* what a row is: a friend online (A joins them, or opens their entry), the
friends online past the rows (+N MORE), the friends offline folded into one
row (N OFFLINE), or the way to add some (ADD A FRIEND); each but a friend
opens the Friends screen */
enum
{
	_friends_panel_friend,
	_friends_panel_more,
	_friends_panel_offline,
	_friends_panel_add,
};

struct friends_panel_row
{
	short kind;
	/* (a friend's: their code, whether A joins them) */
	unsigned char code[FRIENDS_PANEL_CODE_BYTES];
	unsigned char joinable;
	char name[FRIENDS_PANEL_NAME_SIZE];
	/* (what they are doing, "Co-op"; a summary row's none) */
	char note[FRIENDS_PANEL_NOTE_SIZE];
};

struct friends_panel
{
	/* the header's count ("2 ONLINE", "1 REQUEST"; empty for none), and the
	line under it when no friend is online ("No friends online") */
	char count[FRIENDS_PANEL_NOTE_SIZE];
	char message[FRIENDS_PANEL_LINE_SIZE];
	short row_count;
	struct friends_panel_row rows[FRIENDS_PANEL_ROWS];
};

/* (menu_functions.c's, friends_menu.inc) the panel's contents now, in at
most rows rows */
void friends_panel_read(struct friends_panel *panel, short rows);
/* a row's A, a click or a tap: a friend whose party is offered joined, as
the Friends screen's JOIN (TRUE, its failure's sound played if it is not
reached); else FALSE, for the Friends screen to be opened, on that friend's
entry (chosen here) */
boolean friends_panel_do(struct friends_panel_row const *row);

#endif
