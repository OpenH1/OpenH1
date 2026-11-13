/*
MENU_SCREEN.C

The menus' hand-written screens (docs/menu_screens.md), under
display.menu_engine "new": a screen of the menus' files that a hand-written
one takes the place of (screen_places) is opened by ui_widget.c as ever, its
stand-in kept in its history, but its widgets are made here from the
hand-written screen's file (port/assets/menus/screens, read by
port/linux/src/menu_screens.c) in place of its own: its building blocks are
the widgets of port/assets/menus/screens/blocks.xml, each given its words and
place. The engine (menu_engine.c) then draws them and takes their events as
any screen's widgets, so focus, the controller, the mouse, touch and the
menus' sounds are the game's own.

What the screens name is C's, here: an item's action (do="open multiplayer") is a
name in screen_actions, and a binding in its words ({campaign.level}) a name
in screen_bindings; the files never name the game's widgets. A file read
again (hot reload: menu_screens_poll) makes its open screen again, the focus
kept on the item of the same id.
*/

#include "cseries.h"
#include "input/input.h"
#include "interface/event_manager.h"
#include "interface/ui_widget.h"
#include "interface/ui_widget_event_handler_functions.h"
#include "math/integer_math.h"
#include "text/draw_string.h"
#include "tag_files/tag_groups.h"

#include "halo_menus.h"
#include "menu_engine.h"
#include "menu_screens.h"
#include "menu_table.h"
#include "menu_widget.h"

#include <stdio.h>
#include <string.h>

void platform_log(char const *format, ...);
char *tag_get_name(long tag_index);
long pc_menu_event_function_index(char const *name);
boolean menu_tags_open(struct widget_instance *widget, struct event_record *event, char const *screen,
	boolean *deleted);
struct widget_instance *ui_widget_port_add_child(struct widget_instance *parent, long tag_index,
	struct widget_instance *before);
/* (menu_functions.c) */
boolean pc_menu_campaign_saved(char *level, char *difficulty, long size);
void pc_menu_browser_visit(char const *tab);
int pc_menu_browser_visit_kind(void);
void pc_menu_main_menu_shown(struct widget_instance *screen);
int halo_keyboard_prompts(void);
long ui_widget_port_text_box_font(struct widget_instance *widget, short *width);
short ui_widget_port_past_move(void);
boolean ui_widget_port_pointer_focused(void);
void event_manager_post_button(short controller_index, short button_index);
short ui_widget_port_wide_scale(short value, short extra);
short ui_widget_port_past_scale(short value, short past);
short ui_widget_port_wide_extra(void);
short ui_widget_port_past_extra(void);
short ui_widget_port_foot_lift(void);
int ui_widget_port_touch_menus(void);
boolean ui_widget_port_hover_focused(void);
boolean ui_widget_port_take_click(short *x, short *y);

/* ---------- constants */

#define UI_WIDGET_DEFINITION_TAG 'DeLa'

/* the screens open at once (as the engine's: ENGINE_MAXIMUM_SCREENS), and
the most items a screen's list makes */
#define SCREEN_MAXIMUM_OPEN 8
#define SCREEN_MAXIMUM_ITEMS 16
#define SCREEN_ID_LENGTH 32
#define SCREEN_ACTION_LENGTH 48
#define SCREEN_MAXIMUM_BUTTONS 4

/* the layout, in the menus' 640 by 480 units: the title art's room at the
top (its picture TITLE_ART_TOP down in it, as the main menu has it), an
item's height and the room under it for its hint (each item's, so that the
focus moving does not move the items), the button-hint bar's room at the
foot, and the blocks' widths (blocks.xml's) */
#define SCREEN_HEIGHT 480
#define SCREEN_WIDTH 640
#define TITLE_ART_ROOM 247
#define TITLE_ART_TOP 28
#define TITLE_ART_LEAST_ROOM 160
#define ITEM_WIDTH 256
#define ITEM_HEIGHT 38
#define HINT_WIDTH 440
#define HINT_TOP 34
#define ITEM_STEP 54
/* an item with no hint: the PC version's main menu's step, and an item drawn
as a picture: the PC version's items' box (256 by 33) */
#define ITEM_STEP_PLAIN 36
#define PICTURE_HEIGHT 33
#define BITMAP_GROUP_TAG 'bitm'
/* cards (blocks.xml's screens/card: the campaign's mission card, 158 by
240): their gap, and their row's room, below them too */
#define CARD_WIDTH 158
#define CARD_GAP 24
#define CARDS_ROOM 260
/* (the card picture's own height, and the margin above and below the cards
filling their room) */
#define CARD_HEIGHT 240
#define CARD_MARGIN 10
/* a card's title drawn larger than its font (large_ui's) */
#define CARD_TITLE_SCALE 1.4f
/* an inner screen's header's room (its picture where the campaign menu's is),
or the tall layout's band's (port_settings.py's TALL_BAND) */
#define HEADER_ROOM 80
#define HEADER_ROOM_TALL 40
/* the header's words drawn larger than their font (the heading's), as the
tall layout's titles (TALL_TITLE_SCALE) */
#define HEADER_TITLE_SCALE 1.4f
/* the button bar: its room at the foot (port_settings.py's TALL_FOOTER), and
its prompts as the tall layout's footer's (Choose Mission's: port_settings.py's
_missions), from the room's top, the screen's margin (WIDE_MARGIN) at either
side, each as wide as its block (BACK's at the left, the rest's at the
right, this far apart) */
#define BUTTONS_ROOM 37
#define BUTTONS_MARGIN 24
#define PROMPTS_TOP 5
#define PROMPT_TOP 1
#define PROMPT_WIDTH 124
#define PROMPT_LEFT_WIDTH 96
#define PROMPT_GAP 8
/* an item's label drawn larger than its font (large_ui's): at 4:3, and
at 16:9 and wider */
#define ITEM_SCALE_LEAST 1.45f
#define ITEM_SCALE_MOST 1.6f
/* (menu_tags.c's WIDE_LAYOUT_EXTRA: 16:9's width past 640) */
#define WIDE_LAYOUT_EXTRA 212
/* (menu_functions.c's BUTTON_B, BUTTON_X, BUTTON_Y: the buttons a prompt presses) */
#define SCREEN_BUTTON_B 1
#define SCREEN_BUTTON_X 2
#define SCREEN_BUTTON_Y 3
/* (menu_engine.c's ENGINE_EVENT_BUTTON: the event manager's button event) */
#define SCREEN_EVENT_BUTTON 3

/* ---------- the registry: what the files may name */

/* an action: the function the item's own widget ran (NULL: none) and the
one before it (the mode the server browser is in), then the screen opened
if it succeeded, else the one opened if it failed (the profile asked for:
NULL, none) */
struct screen_action
{
	char const *name;
	/* (the server browser's tab for this visit: "all", "coop"; NULL, the
	player's own) */
	char const *browser_tab;
	char const *before;
	char const *run;
	/* (run once run has succeeded, before the screen opens: the campaign's
	state read, as its menu's opening read it) */
	char const *then;
	char const *open;
	char const *otherwise;
	/* (C's own, run in place of the rest: an action of a hand-written
	screen's blocks, its name given, and the row of a table it is for,
	screen_action_row) */
	boolean (*call)(struct widget_instance *widget, struct event_record *event, char const *name, boolean *deleted);
};

static boolean screen_call_search(struct widget_instance *widget, struct event_record *event, char const *name,
	boolean *deleted);
/* the row of the table an action is run for (0 up, on the table), NONE for
none (a key's, a button's) */
static short screen_action_row = NONE;

static struct screen_action const screen_actions[] =
{
	/* (the Campaign screen, screens/campaign.xml) */
	{ "open campaign", NULL, NULL, NULL, NULL, "screens/campaign", NULL },
	/* (Choose Mission, the campaign's missions as cards, as the campaign
	menu's CHOOSE MISSION opens it once the menu has read the profile's
	campaign: a profile made first if there is none, as the PC version's
	CAMPAIGN, and then Choose Mission at once: port_settings.py's
	_solo_new_profile) */
	{ "open solo", NULL, NULL, "new game if no plyr profiles", "port campaign init", "main_menu/campaign/mission_screen",
		"main_menu/campaign/solo_creating_profile" },
	/* (the server browser, as Multiplayer's SERVER BROWSER opens it: its mode
	the browser's; every game, or the co-op games, this visit) */
	{ "open multiplayer", "all", "mp type set mode", "new game if no plyr profiles", NULL,
		"main_menu/multiplayer_type_select/join_game/join_game_screen",
		"main_menu/profile_manager/multiplayer_creating_profile" },
	{ "open coop", "coop", "mp type set mode", "new game if no plyr profiles", NULL,
		"main_menu/multiplayer_type_select/join_game/join_game_screen",
		"main_menu/profile_manager/multiplayer_creating_profile" },
	/* (the browser's HOST & MORE: the PC version's Multiplayer screen, the
	hub; its HOST CO-OP: the hub's co-op page, online or split screen) */
	{ "open host", NULL, NULL, NULL, NULL, "main_menu/multiplayer_type_select/multiplayer_type_select_screen", NULL },
	{ "open host coop", NULL, NULL, NULL, NULL, "main_menu/multiplayer_type_select/hub/coop_screen", NULL },
	{ "open settings", NULL, NULL, "port settings begin", NULL,
		"main_menu/settings_select/player_setup/player_profile_edit/video_settings/video_settings_screen", NULL },
	{ "open training", NULL, NULL, NULL, NULL, "main_menu/training/training_screen", NULL },
	/* (the quit question, as QUIT and B on the main menu open it) */
	{ "quit", NULL, NULL, NULL, NULL, "main_menu/quit_select/quit_screen", NULL },
	/* (a screen's searches, <search>: the next one typed into, and every
	one's words cleared) */
	{ "search next", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_search },
	{ "search clear", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_search },
};

static char const *const screen_action_names[] =
{
	"open campaign", "open solo", "open multiplayer", "open coop", "open host", "open host coop", "open settings",
	"open training", "quit", "search next", "search clear",
};

/* a binding's value: the saved game's mission and difficulty (player 1's
profile's), none without one */
struct screen_values
{
	boolean read;
	boolean saved;
	char level[64];
	char difficulty[64];
};

static char const *const screen_binding_names[] =
{
	"campaign.level", "campaign.difficulty",
};

/* the row sources a table may name (menu_table.h), and their names */
static struct menu_table_source const *const screen_sources[] =
{
	NULL,
};

/* (none yet: a table names one of these) */
static char const *const screen_source_names[] =
{
	"",
};

static int screen_value(void *context, char const *name, char *value, unsigned long size)
{
	struct screen_values *values = context;

	if (!values->read && !strncmp(name, "campaign.", 9))
	{
		values->read = TRUE;
		values->saved = pc_menu_campaign_saved(values->level, values->difficulty, (long)sizeof(values->level));
	}
	if (!strcmp(name, "campaign.level") && values->saved)
		snprintf(value, size, "%s", values->level);
	else if (!strcmp(name, "campaign.difficulty") && values->saved)
		snprintf(value, size, "%s", values->difficulty);
	else
		return 0;
	return value[0] != 0;
}

/* a binding's value set (a search's words typed: <search value=>) */
static void screen_value_set(char const *name, char const *text)
{
	/* (none yet: each settable binding's setter here) */
	(void)name;
	(void)text;
}

static struct menu_screen_names const screen_names =
{
	screen_action_names, NUMBEROF(screen_action_names),
	screen_binding_names, NUMBEROF(screen_binding_names),
	screen_source_names, 0,
};

/* the menus' screens a hand-written one takes the place of, by its name, and
what it does each frame it is drawn (the main menu: the update and crash
questions may open over it) */
struct screen_place
{
	char const *screen;
	char const *widget;
	void (*shown)(struct widget_instance *screen);
	/* (whether it takes the screen's place this time: NULL, always) */
	int (*takes)(void);
};

static struct screen_place const screen_places[] =
{
	{ "main", "main_menu\\main_menu", pc_menu_main_menu_shown, NULL },
	{ "campaign", "screens\\campaign", NULL, NULL },
};

/* the server browser, whose screen gets a button of the hand-written
screens' (screens/button) over it: HOST & MORE, or HOST CO-OP when the
browser was opened on co-op; its key the controller's Back (F1) */
#define SCREEN_BROWSER "main_menu\\multiplayer_type_select\\join_game\\join_game_screen"
#define SCREEN_BROWSER_BUTTON_X 276
#define SCREEN_BROWSER_BUTTON_Y 8
static struct screen_place const screen_browser_place = { "browser", SCREEN_BROWSER, NULL, NULL };

/* ---------- the screens open */

struct screen_open
{
	struct widget_instance *widget;
	struct screen_place const *place;
	unsigned long generation;
	struct widget_instance *list;
	struct widget_instance *items[SCREEN_MAXIMUM_ITEMS];
	struct widget_instance *hints[SCREEN_MAXIMUM_ITEMS];
	/* (each item's picture's bitmap tag, NONE for its label; a card's, its
	picture part's) */
	long pictures[SCREEN_MAXIMUM_ITEMS];
	long card_pictures[SCREEN_MAXIMUM_ITEMS];
	char ids[SCREEN_MAXIMUM_ITEMS][SCREEN_ID_LENGTH];
	char actions[SCREEN_MAXIMUM_ITEMS][SCREEN_ACTION_LENGTH];
	long item_count;
	char back[SCREEN_ACTION_LENGTH];
	/* (a button over a generated screen, its own widgets kept: the
	browser's; and the child its focus goes back to from the button) */
	boolean extra;
	struct widget_instance *kept_focus;
	/* (its header, and the picture its file names for it, NONE for the
	block's own) */
	struct widget_instance *header;
	long header_picture;
	/* (its cards' scale, to fill their room: 0 for their own size) */
	real card_scale;
	/* (its button bar's prompts, each its key, and its place in the 640
	and its side, which past 16:9 moves it to the screen's edge each frame) */
	struct widget_instance *prompts[SCREEN_MAXIMUM_BUTTONS];
	char prompt_keys[SCREEN_MAXIMUM_BUTTONS][SCREEN_ID_LENGTH];
	short prompt_x[SCREEN_MAXIMUM_BUTTONS];
	boolean prompt_right[SCREEN_MAXIMUM_BUTTONS];
	long prompt_count;
};

static struct
{
	boolean named;
	boolean blocks_found;
	struct screen_open open[SCREEN_MAXIMUM_OPEN];
	/* each screen's item focused last, kept for its next opening (back from
	the screen its item opened) */
	char focused[NUMBEROF(screen_places)][SCREEN_ID_LENGTH];
	/* the blocks' tags, as the menus' tags were last built */
	long prompts_tag;
	long title_art_tag, list_tag, item_tag, hint_tag, prompt_left_tag, button_tag, cards_tag, card_tag, card_title_tag, card_art_tag, card_subtitle_tag, header_tag, prompt_tag, header_text_tag;
} screens = { FALSE };

static void screens_names_give(void)
{
	if (!screens.named)
	{
		screens.named = TRUE;
		menu_screens_names(&screen_names);
	}
}

static struct screen_open *screen_open_of(struct widget_instance const *widget)
{
	long index;

	for (index = 0; widget && index < SCREEN_MAXIMUM_OPEN; index++)
	{
		if (screens.open[index].widget == widget)
			return &screens.open[index];
	}
	return NULL;
}

static struct screen_place const *screen_place_of(long tag_index)
{
	char const *name = tag_index != NONE ? tag_get_name(tag_index) : NULL;
	long index;

	if (!name || strncmp(name, "pc\\", 3))
		return NULL;
	for (index = 0; index < NUMBEROF(screen_places); index++)
	{
		if (!strcmp(name + 3, screen_places[index].widget) &&
			(!screen_places[index].takes || screen_places[index].takes()))
		{
			return &screen_places[index];
		}
	}
	return NULL;
}

static long screen_block_tag(char const *name)
{
	return tag_loaded(UI_WIDGET_DEFINITION_TAG, name);
}

/* the blocks' tags found (FALSE, logged, if one is not there) */
static boolean screen_blocks_found(void)
{
	screens.title_art_tag = screen_block_tag("pc\\screens\\title_art");
	screens.list_tag = screen_block_tag("pc\\screens\\list");
	screens.item_tag = screen_block_tag("pc\\screens\\item");
	screens.hint_tag = screen_block_tag("pc\\screens\\hint");
	screens.prompt_left_tag = screen_block_tag("pc\\screens\\prompt_left");
	screens.button_tag = screen_block_tag("pc\\screens\\button");
	screens.cards_tag = screen_block_tag("pc\\screens\\cards");
	screens.card_tag = screen_block_tag("pc\\screens\\card");
	screens.card_title_tag = screen_block_tag("pc\\screens\\card_title");
	screens.card_art_tag = screen_block_tag("pc\\screens\\card_art");
	screens.header_tag = screen_block_tag("pc\\screens\\header");
	screens.card_subtitle_tag = screen_block_tag("pc\\screens\\card_subtitle");
	screens.prompt_tag = screen_block_tag("pc\\screens\\prompt");
	screens.prompts_tag = screen_block_tag("pc\\screens\\prompts");
	screens.header_text_tag = screen_block_tag("pc\\screens\\header_text");
	if (screens.title_art_tag == NONE || screens.list_tag == NONE || screens.item_tag == NONE ||
		screens.hint_tag == NONE || screens.prompt_left_tag == NONE || screens.button_tag == NONE ||
		screens.cards_tag == NONE || screens.card_tag == NONE || screens.card_title_tag == NONE ||
		screens.card_art_tag == NONE || screens.header_tag == NONE ||
		screens.card_subtitle_tag == NONE || screens.prompt_tag == NONE || screens.prompts_tag == NONE ||
		screens.header_text_tag == NONE)
	{
		platform_log("menus: the screens' blocks (screens/blocks.xml) are not all there");
		screens.blocks_found = FALSE;
		return FALSE;
	}
	screens.blocks_found = TRUE;
	return TRUE;
}

/* a text box's words, from UTF-8 */
static void screen_text_set(struct widget_instance *widget, char const *utf8)
{
	unsigned short text[MENU_SCREEN_MAXIMUM_TEXT];
	long length = halo_menus_utf16(utf8, text, NUMBEROF(text));
	wchar_t *buffer = ui_widget_realloc(widget->parameters.text_box.text, (word)(length * (long)sizeof(wchar_t)),
		__FILE__, __LINE__);

	if (!buffer)
		return;
	csmemcpy(buffer, text, length * (long)sizeof(wchar_t));
	widget->parameters.text_box.text = buffer;
}

/* a picture of the menus' files' bitmap tag (its name as the files have it,
"main_menu/menu_versus"), NONE (logged) if there is none */
static long screen_picture_tag(char const *name)
{
	char tag_name[256];
	long index, tag_index;

	snprintf(tag_name, sizeof(tag_name), "pc\\%s", name);
	for (index = 0; tag_name[index]; index++)
	{
		if (tag_name[index] == '/')
			tag_name[index] = '\\';
	}
	tag_index = tag_loaded(BITMAP_GROUP_TAG, tag_name);
	if (tag_index == NONE)
		platform_log("menus: there is no picture %s; the item is drawn as its label", name);
	return tag_index;
}

/* a block's widget made in the parent, at x and y on the screen */
static struct widget_instance *screen_widget_add(struct widget_instance *parent, long tag_index, short x, short y)
{
	struct widget_instance *widget = ui_widget_port_add_child(parent, tag_index, NULL);

	if (widget)
	{
		widget->horizontal_offset = (short)(parent->horizontal_offset + x);
		widget->vertical_offset = (short)(parent->vertical_offset + y);
	}
	return widget;
}

/* whether an item is made: its when holds, and its label's bindings have
values */
static boolean screen_item_shown(struct menu_screen_block const *item, struct screen_values *values, char *label,
	unsigned long size)
{
	return menu_screen_when(item->when) && menu_screen_text(item->label, screen_value, values, label, size);
}

/* a button's side of the bar: back's the left, the rest's the right (or as
its side says) */
static boolean screen_button_right(struct menu_screen_block const *button)
{
	return button->side ? !strcmp(button->side, "right") : strcmp(button->key, "back") != 0;
}

static boolean tall_build(struct screen_open *open, struct menu_screen const *screen);

/* the screen's widgets made from the hand-written screen, in place of
whatever it had: FALSE (nothing changed) if they cannot be */
static boolean screen_build(struct screen_open *open, struct menu_screen const *screen)
{
	struct widget_instance *widget = open->widget;
	struct screen_values values;
	char label[MENU_SCREEN_MAXIMUM_TEXT];
	char text[MENU_SCREEN_MAXIMUM_TEXT];
	boolean shown[MENU_SCREEN_MAXIMUM_BLOCKS];
	short heights[MENU_SCREEN_MAXIMUM_BLOCKS];
	char const *focus_id;
	short fixed = 0, spaces = 0, spaces_left, extra, top, shrink = 0;
	long index, place_index = open->place - screen_places;
	struct widget_instance *focus = NULL;
	long focus_index = 0;

	if (!screen_blocks_found())
		return FALSE;
	/* (a screen with a table: the tall layout's, menu_screen_tall.inc) */
	for (index = 0; index < screen->block_count; index++)
	{
		if (screen->blocks[index].kind == MENU_SCREEN_TABLE)
			return tall_build(open, screen);
	}
	memset(&values, 0, sizeof(values));
	/* (each block's room, from the top down: a space shares what is left) */
	for (index = 0; index < screen->block_count; index++)
	{
		struct menu_screen_block const *block = &screen->blocks[index];

		heights[index] = 0;
		shown[index] = TRUE;
		switch (block->kind)
		{
		case MENU_SCREEN_TITLE_ART:
			heights[index] = TITLE_ART_ROOM;
			break;
		case MENU_SCREEN_CARDS:
			heights[index] = CARDS_ROOM;
			break;
		case MENU_SCREEN_HEADER:
			heights[index] = block->picture ? HEADER_ROOM : HEADER_ROOM_TALL;
			break;
		case MENU_SCREEN_CARD:
		case MENU_SCREEN_ITEM:
			shown[index] = screen_item_shown(block, &values, label, sizeof(label));
			if (block->kind == MENU_SCREEN_CARD)
				break;
			if (shown[index])
				heights[block->parent] = (short)(heights[block->parent] +
					(block->hint_count ? ITEM_STEP : ITEM_STEP_PLAIN));
			break;
		case MENU_SCREEN_SPACE:
			spaces++;
			break;
		case MENU_SCREEN_BUTTONS:
			heights[index] = BUTTONS_ROOM;
			break;
		}
	}
	for (index = 0; index < screen->block_count; index++)
	{
		if (screen->blocks[index].parent < 0)
			fixed = (short)(fixed + heights[index]);
	}
	extra = (short)(SCREEN_HEIGHT - fixed);
	/* (too much for the screen: the title art's room the less) */
	if (extra < 0)
	{
		shrink = (short)MIN(-extra, TITLE_ART_ROOM - TITLE_ART_LEAST_ROOM);
		extra = 0;
	}
	/* the screen's own widgets let go of, and its own made */
	widget->focused_child = NULL;
	while (widget->child)
		ui_widget_delete(widget->child);
	open->list = NULL;
	open->item_count = 0;
	focus_id = screens.focused[place_index][0] ? screens.focused[place_index] : NULL;
	top = 0;
	spaces_left = spaces;
	for (index = 0; index < screen->block_count; index++)
	{
		struct menu_screen_block const *block = &screen->blocks[index];
		short height = heights[index];

		if (block->parent >= 0)
			continue;
		switch (block->kind)
		{
		case MENU_SCREEN_TITLE_ART:
			screen_widget_add(widget, screens.title_art_tag, 0, (short)(top + TITLE_ART_TOP - shrink));
			height = (short)(height - shrink);
			break;
		case MENU_SCREEN_SPACE:
			/* (the last the rest of it, so that a bar after it is at the
			screen's foot) */
			spaces_left--;
			height = spaces ? (short)(spaces_left ? extra / spaces : extra - (spaces - 1) * (extra / spaces)) : 0;
			break;
		case MENU_SCREEN_HEADER:
		{
			/* (its picture where the campaign menu has its own; or its
			title's words in the tall layout's band, as Settings' and the
			browsers' titles) */
			struct widget_instance *header = screen_widget_add(widget,
				block->picture ? screens.header_tag : screens.header_text_tag, 0, top);

			open->header_picture = block->picture ? screen_picture_tag(block->picture) : NONE;
			open->header = header;
			if (header && !block->picture)
			{
				menu_screen_text(block->label, screen_value, &values, label, sizeof(label));
				screen_text_set(header, label);
			}
			break;
		}
		case MENU_SCREEN_CARDS:
		{
			long card_index, count = 0;
			short x, card_width, card_gap;

			for (card_index = 0; card_index < screen->block_count; card_index++)
				count += screen->blocks[card_index].kind == MENU_SCREEN_CARD &&
					screen->blocks[card_index].parent == index && shown[card_index];
			open->list = screen_widget_add(widget, screens.cards_tag, 0, 0);
			if (!open->list)
				break;
			if (!focus_id)
				focus_id = block->focus;
			/* (at their own size: a scale, 0 for none, would draw them larger) */
			open->card_scale = 0.0f;
			card_width = CARD_WIDTH;
			card_gap = CARD_GAP;
			x = (short)((SCREEN_WIDTH - count * card_width - (count - 1) * card_gap) / 2);
			for (card_index = 0; card_index < screen->block_count; card_index++)
			{
				struct menu_screen_block const *card = &screen->blocks[card_index];
				struct widget_instance *made, *part;

				if (card->kind != MENU_SCREEN_CARD || card->parent != index || !shown[card_index] ||
					open->item_count >= SCREEN_MAXIMUM_ITEMS)
				{
					continue;
				}
				made = ui_widget_port_add_child(open->list, screens.card_tag, NULL);
				if (!made)
					break;
				open->card_pictures[open->item_count] = NONE;
				/* (its place: its parts' are in it) */
				made->horizontal_offset = (short)(open->list->horizontal_offset + x);
				made->vertical_offset = (short)(open->list->vertical_offset + top - shrink + CARD_MARGIN);
				/* (its parts: the picture, the title, the subtitle) */
				part = made->child;
				if (part)
				{
					long picture = card->picture ? screen_picture_tag(card->picture) : NONE;

					part->animation.current_frame_index = (short)(card->frame >= 0 ? card->frame : 0);
					if (picture != NONE)
						open->card_pictures[open->item_count] = picture;
					part = part->next;
				}
				menu_screen_text(card->label, screen_value, &values, label, sizeof(label));
				if (part)
				{
					screen_text_set(part, label);
					part = part->next;
				}
				if (part)
				{
					text[0] = 0;
					if (card->hint_count)
						menu_screen_text(card->hints[0], screen_value, &values, text, sizeof(text));
					screen_text_set(part, text);
				}
				open->items[open->item_count] = made;
				open->hints[open->item_count] = NULL;
				open->pictures[open->item_count] = NONE;
				snprintf(open->ids[open->item_count], SCREEN_ID_LENGTH, "%s", card->id ? card->id : "");
				snprintf(open->actions[open->item_count], SCREEN_ACTION_LENGTH, "%s", card->action);
				if (!focus || (focus_id && card->id && !strcmp(card->id, focus_id)))
				{
					focus = made;
					focus_index = open->item_count;
				}
				open->item_count++;
				x = (short)(x + card_width + card_gap);
			}
			break;
		}
		case MENU_SCREEN_LIST:
		{
			long item_index;
			short y = top;

			open->list = screen_widget_add(widget, screens.list_tag, 0, 0);
			if (!open->list)
				break;
			if (!focus_id)
				focus_id = block->focus;
			for (item_index = 0; item_index < screen->block_count; item_index++)
			{
				struct menu_screen_block const *item = &screen->blocks[item_index];
				struct widget_instance *made;
				long hint;

				if (item->kind != MENU_SCREEN_ITEM || item->parent != index || !shown[item_index] ||
					open->item_count >= SCREEN_MAXIMUM_ITEMS)
				{
					continue;
				}
				made = screen_widget_add(open->list, screens.item_tag, (short)((SCREEN_WIDTH - ITEM_WIDTH) / 2), y);
				if (!made)
					break;
				menu_screen_text(item->label, screen_value, &values, label, sizeof(label));
				open->pictures[open->item_count] = item->picture ? screen_picture_tag(item->picture) : NONE;
				/* (a picture in place of its words, which it stands for) */
				screen_text_set(made, open->pictures[open->item_count] != NONE ? "" : label);
				open->items[open->item_count] = made;
				open->hints[open->item_count] = NULL;
				snprintf(open->ids[open->item_count], SCREEN_ID_LENGTH, "%s", item->id ? item->id : "");
				snprintf(open->actions[open->item_count], SCREEN_ACTION_LENGTH, "%s", item->action);
				/* (its hint: the first whose bindings all have values) */
				for (hint = 0; hint < item->hint_count; hint++)
				{
					if (menu_screen_text(item->hints[hint], screen_value, &values, text, sizeof(text)))
					{
						struct widget_instance *line = screen_widget_add(widget, screens.hint_tag,
							(short)((SCREEN_WIDTH - HINT_WIDTH) / 2), (short)(y + HINT_TOP));

						if (line)
						{
							screen_text_set(line, text);
							line->visible = FALSE;
						}
						open->hints[open->item_count] = line;
						break;
					}
				}
				if (!focus || (focus_id && item->id && !strcmp(item->id, focus_id)))
				{
					focus = made;
					focus_index = open->item_count;
				}
				open->item_count++;
				y = (short)(y + (item->hint_count ? ITEM_STEP : ITEM_STEP_PLAIN));
			}
			break;
		}
		case MENU_SCREEN_BUTTONS:
		{
			long button;
			short left_x = BUTTONS_MARGIN, right_x = (short)(SCREEN_WIDTH - BUTTONS_MARGIN - PROMPT_WIDTH);
			struct widget_instance *bar =
				screen_widget_add(widget, screens.prompts_tag, 0, (short)(top + PROMPTS_TOP));

			/* (each a prompt, its key's picture, "%a-button": the menus' key
			for it while the keyboard plays, and its word; back at the left,
			the rest from the right, on the footer's line) */
			open->prompt_count = 0;
			for (button = 0; button < screen->block_count && open->prompt_count < SCREEN_MAXIMUM_BUTTONS; button++)
			{
				struct menu_screen_block const *block_button = &screen->blocks[button];
				boolean right;
				struct widget_instance *prompt;
				long made = open->prompt_count;

				if (!bar || block_button->kind != MENU_SCREEN_BUTTON || block_button->parent != index)
					continue;
				right = screen_button_right(block_button);
				/* (in the bar, where it is) */
				prompt = ui_widget_port_add_child(bar, right ? screens.prompt_tag : screens.prompt_left_tag, NULL);
				if (!prompt)
					continue;
				prompt->horizontal_offset = right ? right_x : left_x;
				prompt->vertical_offset = PROMPT_TOP;
				snprintf(text, sizeof(text), "%s %s", !strcmp(block_button->key, "accept") ? "%a-button" :
					!strcmp(block_button->key, "back") ? "%b-button" : !strcmp(block_button->key, "x") ? "%x-button" :
					"%y-button", block_button->label);
				screen_text_set(prompt, text);
				open->prompts[made] = prompt;
				open->prompt_x[made] = right ? right_x : left_x;
				open->prompt_right[made] = right;
				snprintf(open->prompt_keys[made], SCREEN_ID_LENGTH, "%s", block_button->key);
				open->prompt_count++;
				if (right)
					right_x = (short)(right_x - PROMPT_GAP - PROMPT_WIDTH);
				else
					left_x = (short)(left_x + PROMPT_LEFT_WIDTH + PROMPT_GAP);
			}
			break;
		}
		}
		top = (short)(top + height);
	}
	/* (the focus: the item focused last, else the list's focus, else its
	first) */
	if (open->list && focus)
	{
		widget->focused_child = open->list;
		open->list->focused_child = focus;
		open->list->parameters.list.selected_index = (short)focus_index;
	}
	snprintf(open->back, sizeof(open->back), "%s", screen->back ? screen->back : "");
	open->generation = menu_screens_generation(screen->name);
	return TRUE;
}

static boolean screen_action_run(struct widget_instance *widget, struct event_record *event, char const *name,
	boolean *deleted);

#include "menu_screen_tall.inc"

/* ---------- the engine's calls */

/* the server browser's HOST button made over it (TRUE), where it is the
server browser (not the local network's list, nor Join by Link) */
static boolean screen_browser_opened(struct widget_instance *widget)
{
	char const *name = tag_get_name(widget->definition_tag_index);
	long index;

	if (!name || strncmp(name, "pc\\", 3) || strcmp(name + 3, SCREEN_BROWSER) || !pc_menu_browser_visit_kind() ||
		!screen_blocks_found())
	{
		return FALSE;
	}
	for (index = 0; index < SCREEN_MAXIMUM_OPEN; index++)
	{
		struct screen_open *open = &screens.open[index];
		struct widget_instance *button;
		boolean coop = pc_menu_browser_visit_kind() == 2;

		if (open->widget)
			continue;
		memset(open, 0, sizeof(*open));
		button = screen_widget_add(widget, screens.button_tag, SCREEN_BROWSER_BUTTON_X, SCREEN_BROWSER_BUTTON_Y);
		if (!button)
			return FALSE;
		open->widget = widget;
		open->place = &screen_browser_place;
		open->extra = TRUE;
		open->items[0] = button;
		open->pictures[0] = NONE;
		open->item_count = 1;
		snprintf(open->ids[0], SCREEN_ID_LENGTH, "host");
		snprintf(open->actions[0], SCREEN_ACTION_LENGTH, "%s", coop ? "open host coop" : "open host");
		screen_text_set(button, coop ? "%back-button HOST CO-OP" : "%back-button HOST & MORE");
		return TRUE;
	}
	return FALSE;
}

boolean menu_screen_opened(struct widget_instance *widget)
{
	struct screen_place const *place = screen_place_of(widget->definition_tag_index);
	struct menu_screen const *screen;
	long index;

	if (!place)
		return screen_browser_opened(widget);
	screens_names_give();
	menu_screens_poll(ui_widget_port_milliseconds());
	screen = menu_screens_find(place->screen);
	if (!screen)
		return FALSE;
	for (index = 0; index < SCREEN_MAXIMUM_OPEN; index++)
	{
		struct screen_open *open = &screens.open[index];

		if (open->widget)
			continue;
		memset(open, 0, sizeof(*open));
		open->widget = widget;
		open->place = place;
		if (!screen_build(open, screen))
		{
			memset(open, 0, sizeof(*open));
			return FALSE;
		}
		return TRUE;
	}
	return FALSE;
}

void menu_screen_closed(struct widget_instance *widget)
{
	struct screen_open *open = screen_open_of(widget);

	if (!open)
		return;
	if (tall_of(open))
	{
		struct screen_tall *tall = tall_of(open);

		if (tall_search_editing(tall))
			pc_menu_text_field_end(FALSE);
		if (tall_search_screen == tall)
			tall_search_screen = NULL;
		tall_screens[open - screens.open] = NULL;
	}
	memset(open, 0, sizeof(*open));
}

/* the item focused now (its index, NONE for none) */
static long screen_focused(struct screen_open const *open)
{
	long index;

	for (index = 0; open->list && index < open->item_count; index++)
	{
		if (open->list->focused_child == open->items[index])
			return index;
	}
	return NONE;
}

void menu_screen_frame(struct widget_instance *widget)
{
	struct screen_open *open = screen_open_of(widget);
	long focused, index;

	if (!open)
		return;
	/* (the browser's button: the focus back to the browser's own widgets
	once the pointer is not what gave the button it, the d-pad its) */
	if (open->extra)
	{
		if (widget->focused_child != open->items[0])
			open->kept_focus = widget->focused_child;
		else if (open->kept_focus && !ui_widget_port_pointer_focused())
			widget->focused_child = open->kept_focus;
		return;
	}
	/* (its file read again: made again, the focus on the same item) */
	menu_screens_poll(ui_widget_port_milliseconds());
	if (menu_screens_generation(open->place->screen) != open->generation)
	{
		struct menu_screen const *screen = menu_screens_find(open->place->screen);

		focused = screen_focused(open);
		if (focused != NONE)
			snprintf(screens.focused[open->place - screen_places], SCREEN_ID_LENGTH, "%s", open->ids[focused]);
		if (tall_of(open))
			tall_screens[open - screens.open] = NULL;
		if (screen && screen_build(open, screen))
			platform_log("menus: screen %s made again", screen->name);
		else
			open->generation = menu_screens_generation(open->place->screen);
	}
	/* (the tall layout's: its blocks each frame) */
	if (tall_of(open))
	{
		tall_frame(open);
		if (open->place->shown)
			open->place->shown(widget);
		return;
	}
	/* (past 16:9, the band's title and the bar's prompts to the screen's
	edges, as the tall layout's titles and footer's prompts) */
	if (open->header && open->header_picture == NONE)
		open->header->horizontal_offset = (short)(widget->horizontal_offset - ui_widget_port_past_move() / 2);
	for (index = 0; index < open->prompt_count; index++)
	{
		open->prompts[index]->horizontal_offset = (short)(open->prompt_x[index] +
			(open->prompt_right[index] ? 1 : -1) * (ui_widget_port_past_move() / 2));
	}
	focused = screen_focused(open);
	for (index = 0; index < open->item_count; index++)
	{
		if (open->hints[index])
			open->hints[index]->visible = index == focused;
		/* (a picture's frames: lit while focused, as the PC version's items) */
		open->items[index]->animation.current_frame_index = (short)(index == focused);
	}
	if (focused != NONE)
		snprintf(screens.focused[open->place - screen_places], SCREEN_ID_LENGTH, "%s", open->ids[focused]);
	if (open->place->shown)
		open->place->shown(widget);
}

/* an action run from the widget (an item, or the screen) */
static boolean screen_action_run(struct widget_instance *widget, struct event_record *event, char const *name,
	boolean *deleted)
{
	struct screen_action const *action = NULL;
	boolean succeeded = TRUE;
	char const *open;
	long index;

	*deleted = FALSE;
	for (index = 0; index < NUMBEROF(screen_actions); index++)
	{
		if (!strcmp(screen_actions[index].name, name))
			action = &screen_actions[index];
	}
	if (!action)
	{
		platform_log("menus: there is no action \"%s\"", name);
		return FALSE;
	}
	if (action->call)
		return action->call(widget, event, name, deleted);
	if (action->browser_tab || action->before)
		pc_menu_browser_visit(action->browser_tab);
	if (action->before)
	{
		index = pc_menu_event_function_index(action->before);
		if (index != NONE)
			ui_widget_event_handler_function_invoke(widget, event, (word)index, deleted);
		if (*deleted)
			return TRUE;
	}
	if (action->run)
	{
		index = pc_menu_event_function_index(action->run);
		/* (a failure that opens its other screen is expected, not warned
		of, as a handler's with a conditional) */
		ui_widget_function_failure_expected = action->otherwise != NULL;
		succeeded = index != NONE && ui_widget_event_handler_function_invoke(widget, event, (word)index, deleted);
		ui_widget_function_failure_expected = FALSE;
		if (*deleted)
			return TRUE;
	}
	if (succeeded && action->then)
	{
		index = pc_menu_event_function_index(action->then);
		if (index != NONE)
			ui_widget_event_handler_function_invoke(widget, event, (word)index, deleted);
		if (*deleted)
			return TRUE;
	}
	open = succeeded ? action->open : action->otherwise;
	if (open && !menu_tags_open(widget, event, open, deleted))
		platform_log("menus: action \"%s\": there is no screen %s", name, open);
	return TRUE;
}

boolean menu_screen_do(struct widget_instance *item, struct event_record *event, boolean *deleted)
{
	struct screen_open *open = screen_open_of(widget_instance_get_topmost_parent(item));
	long index;

	*deleted = FALSE;
	if (open && tall_of(open))
		return tall_do(open, item, event, deleted);
	for (index = 0; open && index < open->item_count; index++)
	{
		if (open->items[index] == item)
		{
			if (!open->extra)
				snprintf(screens.focused[open->place - screen_places], SCREEN_ID_LENGTH, "%s", open->ids[index]);
			return screen_action_run(item, event, open->actions[index], deleted);
		}
	}
	return FALSE;
}

boolean menu_screen_key(struct widget_instance *prompt, struct event_record *event, boolean *deleted, short controller)
{
	struct screen_open *open = screen_open_of(widget_instance_get_topmost_parent(prompt));
	long index;

	*deleted = FALSE;
	if (open && tall_of(open))
		return tall_prompt(open, prompt, event, deleted, controller);
	for (index = 0; open && index < open->prompt_count; index++)
	{
		char const *key = open->prompt_keys[index];

		if (open->prompts[index] != prompt)
			continue;
		/* (accept: the item the list has chosen, as A on it; the others their
		button pressed, which the screen takes as it takes the controller's) */
		if (!strcmp(key, "accept"))
			return open->list && open->list->focused_child && menu_screen_do(open->list->focused_child, event, deleted);
		event_manager_post_button(controller, !strcmp(key, "back") ? SCREEN_BUTTON_B :
			!strcmp(key, "x") ? SCREEN_BUTTON_X : SCREEN_BUTTON_Y);
		return TRUE;
	}
	return FALSE;
}

boolean menu_screen_back(struct widget_instance *widget, struct event_record *event, boolean *deleted)
{
	struct screen_open *open = screen_open_of(widget);

	*deleted = FALSE;
	/* (a tall screen's keys: its buttons' and its tabs') */
	if (open && tall_of(open) && tall_key(open, widget, event, deleted))
		return TRUE;
	if (open && open->extra)
	{
		if (event->type != SCREEN_EVENT_BUTTON || event->data.button.value != 1 ||
			event->data.button.index != _gamepad_binary_button_back)
		{
			return FALSE;
		}
		return screen_action_run(widget, event, open->actions[0], deleted);
	}
	if (!open || !open->back[0] || event->type != SCREEN_EVENT_BUTTON || event->data.button.value != 1 ||
		(event->data.button.index != _gamepad_analog_button_b &&
			event->data.button.index != _gamepad_binary_button_back))
	{
		return FALSE;
	}
	/* (a tall screen whose band has a button of Back's: that, not back) */
	if (tall_of(open) && event->data.button.index == _gamepad_binary_button_back && tall_of(open)->header_button.widget)
		return FALSE;
	if (!screen_action_run(widget, event, open->back, deleted))
		return FALSE;
	return TRUE;
}

/* a block's text scale (menu_tags.c's pc_menu_text_scale): an item's label
the larger, from 4:3 to 16:9 */
boolean menu_screen_text_scale(long definition_index, short wide_extra, real *scale)
{
	if (screens.blocks_found && definition_index == screens.header_text_tag)
	{
		*scale = HEADER_TITLE_SCALE;
		return TRUE;
	}
	if (screens.blocks_found && (definition_index == screens.card_title_tag ||
		definition_index == screens.card_subtitle_tag))
	{
		real card_scale = 1.0f;
		long open_index;

		for (open_index = 0; open_index < SCREEN_MAXIMUM_OPEN; open_index++)
		{
			if (screens.open[open_index].widget && screens.open[open_index].card_scale > 0.0f)
				card_scale = screens.open[open_index].card_scale;
		}
		*scale = (definition_index == screens.card_title_tag ? CARD_TITLE_SCALE : 1.0f) * card_scale;
		return TRUE;
	}
	if (!screens.blocks_found || definition_index != screens.item_tag)
		return FALSE;
	*scale = ITEM_SCALE_LEAST + (ITEM_SCALE_MOST - ITEM_SCALE_LEAST) * (real)MIN(wide_extra, WIDE_LAYOUT_EXTRA) /
		(real)WIDE_LAYOUT_EXTRA;
	return TRUE;
}

/* a block's text colour (menu_functions.c's pc_menu_text_color): a focused
item's label white, as the main menu's lit pictures were */
boolean menu_screen_text_color(struct widget_instance const *widget, real_argb_color *color)
{
	if (screens.blocks_found && tall_color_of(widget, color))
		return TRUE;
	if (!screens.blocks_found || widget->definition_tag_index != screens.item_tag || !widget->parent ||
		widget->parent->focused_child != widget)
	{
		return FALSE;
	}
	color->red = color->green = color->blue = 1.0f;
	return TRUE;
}

boolean menu_screen_item_picture(struct widget_instance const *widget, long *bitmap_tag, rectangle2d *bounds)
{
	long open_index, index;

	/* (a tall screen's blocks: their places' widths and pictures) */
	if (screens.blocks_found && tall_picture_of(widget, bitmap_tag, bounds))
		return TRUE;
	/* (a header: the picture its file names) */
	if (screens.blocks_found && widget->definition_tag_index == screens.header_tag)
	{
		for (open_index = 0; open_index < SCREEN_MAXIMUM_OPEN; open_index++)
		{
			struct screen_open const *open = &screens.open[open_index];

			if (open->widget && open->header == widget && open->header_picture != NONE)
			{
				*bitmap_tag = open->header_picture;
				return TRUE;
			}
		}
		return FALSE;
	}
	/* (a card and its parts: scaled to the cards' room, from the card's
	corner; its picture part, its card's picture where its file names one) */
	if (screens.blocks_found && widget->parent && (widget->definition_tag_index == screens.card_tag ||
		widget->definition_tag_index == screens.card_art_tag || widget->definition_tag_index == screens.card_title_tag ||
		widget->definition_tag_index == screens.card_subtitle_tag))
	{
		struct widget_instance const *card = widget->definition_tag_index == screens.card_tag ? widget : widget->parent;

		for (open_index = 0; open_index < SCREEN_MAXIMUM_OPEN; open_index++)
		{
			struct screen_open const *open = &screens.open[open_index];

			for (index = 0; open->widget && index < open->item_count; index++)
			{
				real scale = open->card_scale;

				if (open->items[index] != card)
					continue;
				*bitmap_tag = widget->definition_tag_index == screens.card_art_tag ? open->card_pictures[index] : NONE;
				if (scale > 0.0f)
				{
					bounds->x0 = (short)(bounds->x0 * scale);
					bounds->y0 = (short)(bounds->y0 * scale);
					bounds->x1 = (short)(bounds->x1 * scale + 0.5f);
					bounds->y1 = (short)(bounds->y1 * scale + 0.5f);
				}
				return scale > 0.0f || *bitmap_tag != NONE;
			}
		}
		return FALSE;
	}
	if (!screens.blocks_found || widget->definition_tag_index != screens.item_tag)
		return FALSE;
	for (open_index = 0; open_index < SCREEN_MAXIMUM_OPEN; open_index++)
	{
		struct screen_open const *open = &screens.open[open_index];

		for (index = 0; open->widget && index < open->item_count; index++)
		{
			if (open->items[index] == widget && open->pictures[index] != NONE)
			{
				*bitmap_tag = open->pictures[index];
				bounds->x0 = 0;
				bounds->y0 = 0;
				bounds->x1 = ITEM_WIDTH;
				bounds->y1 = PICTURE_HEIGHT;
				return TRUE;
			}
		}
	}
	return FALSE;
}
