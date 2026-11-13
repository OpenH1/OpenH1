/*
MENU_SCREENS.H

The menus' hand-written screens (port/assets/menus/screens,
docs/menu_screens.md): one file a screen, its building blocks and what each
does, read by the platform layer (port/linux/src/menu_screens.c) and made
into widgets by the menu engine (port/linux/game/menu_screen.c). Plain C89
types, for both.

A screen file holds <screen> (name, back), and in it, from the top of the
screen down: <title-art/>, <list> (focus, columns) holding <item>s (id,
label, state, hint or <hint>s, do, when), <space/>, and <buttons> holding
<button>s (key, label, do). An item's label and hints may hold bindings, {name}, filled from C;
its do and the screen's back are actions, names the game knows. Both are
checked as the file is read against the names the game gave
(menu_screens_names).
*/

#ifndef MENU_SCREENS_H
#define MENU_SCREENS_H

/* the most a screen has of each, and the longest of its texts */
#define MENU_SCREEN_MAXIMUM_BLOCKS 32
#define MENU_SCREEN_MAXIMUM_HINTS 4
#define MENU_SCREEN_MAXIMUM_STRINGS 256
#define MENU_SCREEN_MAXIMUM_TEXT 256
#define MENU_SCREEN_MAXIMUM_SCREENS 16

enum menu_screen_block_kind
{
	MENU_SCREEN_TITLE_ART,
	MENU_SCREEN_LIST,
	MENU_SCREEN_ITEM,
	MENU_SCREEN_SPACE,
	MENU_SCREEN_BUTTONS,
	MENU_SCREEN_BUTTON,
	/* (cards side by side: each its picture, title and subtitle) */
	MENU_SCREEN_CARDS,
	MENU_SCREEN_CARD,
	/* (an inner screen's header: its lettered picture, its title the words) */
	MENU_SCREEN_HEADER
};

/* a building block: its kind, the block it is in (-1: the screen), and what
its kind has (NULL where it has none). An item's hints are alternatives: it
shows the first whose bindings all have values */
struct menu_screen_block
{
	int kind;
	int parent;
	char const *id;
	char const *label;
	char const *action;
	char const *when;
	char const *key;
	/* (an item drawn as a picture of the menus' files: its bitmap's name,
	"main_menu/menu_versus"; its label then the words it stands for) */
	char const *picture;
	/* (a button's side of the bar: "left" or "right"; NULL, back's left
	and the others' right) */
	char const *side;
	/* (a card's picture's frame: the campaign's stills by level; -1, none) */
	long frame;
	/* (a list's item focused first, by its id) */
	char const *focus;
	/* (a list's columns: 0, one, its items the large ones under each
	other; 2 or more, a grid of small rows, each an item's label and its
	state, the focused one's hint under the grid) */
	long columns;
	/* (an item's state, at the right of its row in a grid: "{skull.iron}";
	NULL, none) */
	char const *state;
	char const *hints[MENU_SCREEN_MAXIMUM_HINTS];
	int hint_count;
	long line;
};

struct menu_screen
{
	char const *name;
	/* (B and Back: an action, NULL for the screen before) */
	char const *back;
	char const *file;
	struct menu_screen_block blocks[MENU_SCREEN_MAXIMUM_BLOCKS];
	int block_count;
	/* (the strings it owns) */
	char *strings[MENU_SCREEN_MAXIMUM_STRINGS];
	int string_count;
};

/* the names the game gives the files: its actions (a block's do, a screen's
back) and its bindings ({name} in a text); a name not among them is a
problem in the file. NULL tables: any name passes */
struct menu_screen_names
{
	char const *const *actions;
	long action_count;
	char const *const *bindings;
	long binding_count;
};

/* a binding's value as text (UTF-8) for menu_screen_text: nonzero if it has
one */
typedef int (*menu_screen_value)(void *context, char const *name, char *value, unsigned long size);

/* ---------- reading (menu_screens.c) */

/* the game's names, checked against as the files are read from now */
void menu_screens_names(struct menu_screen_names const *names);
/* a file read into *screen (nonzero), else its problem written to problem
("<file>:<line>: <what>") and nothing kept (zero) */
int menu_screen_read(char const *file, unsigned char const *data, unsigned long size, struct menu_screen *screen,
	char *problem, unsigned long problem_size);
/* what a screen read owns let go of */
void menu_screen_free(struct menu_screen *screen);
/* text with its bindings filled, in UTF-8 (nonzero if every binding had a
value; else out is as far as it got) */
int menu_screen_text(char const *text, menu_screen_value value, void *context, char *out, unsigned long size);
/* whether a when (platforms, separated by spaces: any of them) holds here
(halo_menus_platform's names); NULL holds */
int menu_screen_when(char const *when);

/* ---------- the screens (menu_screens.c) */

/* the screen of the name, its last good reading: NULL if no file has it */
struct menu_screen const *menu_screens_find(char const *name);
/* the number of the screen's reading (it changes when its file is read
again), 0 for none */
unsigned long menu_screens_generation(char const *name);
/* the watched folder looked at, at most twice a second (now: a clock in
milliseconds), and each file changed in it read again: a file with a problem
is logged and its screen keeps its last good reading */
void menu_screens_poll(unsigned long now);
/* the folder watched (debug.menu_screens, else debug.menu_files'
screens/), empty for none */
char const *menu_screens_folder(void);

#endif
