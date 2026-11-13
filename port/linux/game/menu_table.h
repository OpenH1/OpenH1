/*
MENU_TABLE.H

A hand-written screen's live table (docs/menu_screens.md, <table>): its rows
come from a row source of C's, named in the screen's file (source=), which
the screens' registry (port/linux/game/menu_screen.c) gives the table. The
table is the view: its row widgets, the focus among them, their cells drawn;
the source is what is listed: how many rows, which is first on the table
(the rows scroll), which is chosen, and each row's cells. The server
browser's and Select Profile's are menu_functions.c's (pc_menu_browser_source,
pc_menu_profile_source).
*/

#ifndef MENU_TABLE_H
#define MENU_TABLE_H

struct widget_instance;
struct event_record;

#define MENU_TABLE_TEXT_LENGTH 64
#define MENU_TABLE_BADGE_LENGTH 8

/* a frame's look of the table, which the source reads and answers */
struct menu_table_view
{
	/* (in) the screen; the rows the table has; the row focused (0 up, on
	the table: NONE, none), and whether the pointer resting on it put the
	focus there (not the d-pad, which scrolls at the ends); whether the
	columns' titles are shown, above the first row */
	struct widget_instance *screen;
	short shown;
	short focused;
	short hover;
	short titles;
	/* (out) the rows in all; the first on the table; the row chosen (its
	details the panel's: NONE, none); the row of the table to give the focus
	(NONE: as it is), or the titles */
	short count;
	short first;
	short chosen;
	short focus;
	short focus_titles;
};

/* a row's cell, as the source says it is */
struct menu_table_cell
{
	/* its words (UTF-16), cut short with "..." to its column by the table */
	unsigned short text[MENU_TABLE_TEXT_LENGTH];
	/* its words' colour, 0xRRGGBBAA (0: the column's own) */
	unsigned long color;
	/* a picture cell's frame (a ping's bars, a star): NONE, not shown */
	short frame;
	/* a pill after the words (empty: none), and a lock after them */
	unsigned short badge[MENU_TABLE_BADGE_LENGTH];
	short lock;
};

struct menu_table_source
{
	/* the frame's view read and answered (each frame the table is drawn) */
	void (*update)(struct menu_table_view *view);
	/* whether the row is a heading across the table (its words), not a row
	of cells */
	int (*heading)(short index, unsigned short *words, short size);
	/* the row's cell of the column (by its id; focused: the row has the
	focus) */
	void (*cell)(short index, char const *column, int focused, struct menu_table_cell *cell);
	/* the table's screen opened (not made again by a hot reload): NULL,
	nothing to do */
	void (*opened)(void);
};

/* ---------- the server browser's (menu_functions.c) */

extern struct menu_table_source const pc_menu_browser_source;
/* a binding's value, UTF-8 ("browser.<name>": nonzero if it has one) */
int pc_menu_browser_value(char const *name, char *value, unsigned long size);
/* a search's words set ("browser.search.<column>") */
void pc_menu_browser_search_set(char const *name, char const *text);

/* ---------- Select Profile's (menu_functions.c) */

extern struct menu_table_source const pc_menu_profile_source;
/* a binding's value, UTF-8 ("profile.<name>": nonzero if it has one) */
int pc_menu_profile_value(char const *name, char *value, unsigned long size);
/* the row's profile (NONE: the chosen one's) made player 1's: zero for the
row that makes one */
int pc_menu_profile_choose(struct widget_instance *widget, struct event_record *event, short row);

/* ---------- the menus' text fields and text (menu_functions.c), for the
hand-written screens' searches */

/* a field typed into (its words to begin with; done: its words when it is
left with them kept), Enter ending it */
void pc_menu_text_field_begin(struct widget_instance *field, char const *text, short maximum,
	void (*done)(char const *text));
/* whether the field is typed into (NULL: any) */
int pc_menu_text_field_editing(struct widget_instance *field);
/* the field typed into left (keep: its words kept, its done told) */
void pc_menu_text_field_end(int keep);
/* the field shown (its caret blinking while it is typed into), the keys
typed taken; the words typed so far */
void pc_menu_text_field_show(struct widget_instance *field, char const *text, int editing);
char const *pc_menu_text_field_words(void);
/* a text box's words fitted to width (cut short with "..."), returning how
wide they are drawn; wrapped to width in lines at most */
short pc_menu_text_fit(struct widget_instance *box, unsigned short const *text, short width);
void pc_menu_text_wrap(struct widget_instance *box, unsigned short const *text, short width, short lines);
/* text's width in a text box's font (0: not known) */
short pc_menu_text_width(struct widget_instance *box, unsigned short const *text);

#endif
