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

What the screens name is C's, here: an item's action (do="open versus") is a
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
#include "tag_files/tag_groups.h"

#include "halo_menus.h"
#include "menu_engine.h"
#include "menu_screens.h"
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
void pc_menu_main_menu_shown(struct widget_instance *screen);

/* ---------- constants */

#define UI_WIDGET_DEFINITION_TAG 'DeLa'

/* the screens open at once (as the engine's: ENGINE_MAXIMUM_SCREENS), and
the most items a screen's list makes */
#define SCREEN_MAXIMUM_OPEN 8
#define SCREEN_MAXIMUM_ITEMS 16
#define SCREEN_ID_LENGTH 32
#define SCREEN_ACTION_LENGTH 48

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
#define BUTTONS_ROOM 30
#define BUTTONS_TOP 4
/* an item's label drawn larger than its font (large_ui's): at 4:3, and
at 16:9 and wider */
#define ITEM_SCALE_LEAST 1.45f
#define ITEM_SCALE_MOST 1.6f
/* (menu_tags.c's WIDE_LAYOUT_EXTRA: 16:9's width past 640) */
#define WIDE_LAYOUT_EXTRA 212
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
	char const *before;
	char const *run;
	char const *open;
	char const *otherwise;
};

static struct screen_action const screen_actions[] =
{
	/* (the PC version's CAMPAIGN: a profile made first if there is none) */
	{ "open campaign", NULL, "new game if no plyr profiles", "main_menu/campaign/campaign_screen",
		"main_menu/new_campaign_creating_profile" },
	/* (the server browser, as Multiplayer's SERVER BROWSER opens it: its mode
	the browser's) */
	{ "open versus", "mp type set mode", "new game if no plyr profiles",
		"main_menu/multiplayer_type_select/join_game/join_game_screen",
		"main_menu/profile_manager/multiplayer_creating_profile" },
	{ "open settings", NULL, "port settings begin",
		"main_menu/settings_select/player_setup/player_profile_edit/video_settings/video_settings_screen", NULL },
	{ "open training", NULL, NULL, "main_menu/training/training_screen", NULL },
	/* (the quit question, as QUIT and B on the main menu open it) */
	{ "quit", NULL, NULL, "main_menu/quit_select/quit_screen", NULL },
};

static char const *const screen_action_names[] =
{
	"open campaign", "open versus", "open settings", "open training", "quit",
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

static int screen_value(void *context, char const *name, char *value, unsigned long size)
{
	struct screen_values *values = context;

	if (!values->read)
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

static struct menu_screen_names const screen_names =
{
	screen_action_names, NUMBEROF(screen_action_names),
	screen_binding_names, NUMBEROF(screen_binding_names),
};

/* the menus' screens a hand-written one takes the place of, by its name, and
what it does each frame it is drawn (the main menu: the update and crash
questions may open over it) */
struct screen_place
{
	char const *screen;
	char const *widget;
	void (*shown)(struct widget_instance *screen);
};

static struct screen_place const screen_places[] =
{
	{ "main", "main_menu\\main_menu", pc_menu_main_menu_shown },
};

/* ---------- the screens open */

struct screen_open
{
	struct widget_instance *widget;
	struct screen_place const *place;
	unsigned long generation;
	struct widget_instance *list;
	struct widget_instance *items[SCREEN_MAXIMUM_ITEMS];
	struct widget_instance *hints[SCREEN_MAXIMUM_ITEMS];
	/* (each item's picture's bitmap tag, NONE for its label) */
	long pictures[SCREEN_MAXIMUM_ITEMS];
	char ids[SCREEN_MAXIMUM_ITEMS][SCREEN_ID_LENGTH];
	char actions[SCREEN_MAXIMUM_ITEMS][SCREEN_ACTION_LENGTH];
	long item_count;
	char back[SCREEN_ACTION_LENGTH];
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
	long title_art_tag, list_tag, item_tag, hint_tag, buttons_tag;
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
		if (!strcmp(name + 3, screen_places[index].widget))
			return &screen_places[index];
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
	screens.buttons_tag = screen_block_tag("pc\\screens\\buttons");
	if (screens.title_art_tag == NONE || screens.list_tag == NONE || screens.item_tag == NONE ||
		screens.hint_tag == NONE || screens.buttons_tag == NONE)
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

/* the buttons' words: each one's key (its icon, "%a-button": the menus' key
for it while the keyboard plays) and label */
static void screen_buttons_text(struct menu_screen const *screen, long buttons, char *text, unsigned long size)
{
	long index;

	text[0] = 0;
	for (index = 0; index < screen->block_count; index++)
	{
		struct menu_screen_block const *button = &screen->blocks[index];
		char const *icon;
		size_t used = strlen(text);

		if (button->kind != MENU_SCREEN_BUTTON || button->parent != buttons)
			continue;
		icon = !strcmp(button->key, "accept") ? "%a-button" : !strcmp(button->key, "back") ? "%b-button" :
			!strcmp(button->key, "x") ? "%x-button" : "%y-button";
		snprintf(text + used, size - used, "%s%s %s", used ? "     " : "", icon, button->label);
	}
}

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
	short fixed = 0, spaces = 0, extra, top, shrink = 0;
	long index, place_index = open->place - screen_places;
	struct widget_instance *focus = NULL;
	long focus_index = 0;

	if (!screen_blocks_found())
		return FALSE;
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
		case MENU_SCREEN_ITEM:
			shown[index] = screen_item_shown(block, &values, label, sizeof(label));
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
			height = spaces ? (short)(extra / spaces) : 0;
			break;
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
			struct widget_instance *bar;

			screen_buttons_text(screen, index, text, sizeof(text));
			bar = screen_widget_add(widget, screens.buttons_tag, 0, (short)(top + BUTTONS_TOP));
			if (bar)
				screen_text_set(bar, text);
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

/* ---------- the engine's calls */

boolean menu_screen_opened(struct widget_instance *widget)
{
	struct screen_place const *place = screen_place_of(widget->definition_tag_index);
	struct menu_screen const *screen;
	long index;

	if (!place)
		return FALSE;
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

	if (open)
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
	/* (its file read again: made again, the focus on the same item) */
	menu_screens_poll(ui_widget_port_milliseconds());
	if (menu_screens_generation(open->place->screen) != open->generation)
	{
		struct menu_screen const *screen = menu_screens_find(open->place->screen);

		focused = screen_focused(open);
		if (focused != NONE)
			snprintf(screens.focused[open->place - screen_places], SCREEN_ID_LENGTH, "%s", open->ids[focused]);
		if (screen && screen_build(open, screen))
			platform_log("menus: screen %s made again", screen->name);
		else
			open->generation = menu_screens_generation(open->place->screen);
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
	for (index = 0; open && index < open->item_count; index++)
	{
		if (open->items[index] == item)
		{
			snprintf(screens.focused[open->place - screen_places], SCREEN_ID_LENGTH, "%s", open->ids[index]);
			return screen_action_run(item, event, open->actions[index], deleted);
		}
	}
	return FALSE;
}

boolean menu_screen_back(struct widget_instance *widget, struct event_record *event, boolean *deleted)
{
	struct screen_open *open = screen_open_of(widget);

	*deleted = FALSE;
	if (!open || !open->back[0] || event->type != SCREEN_EVENT_BUTTON || event->data.button.value != 1 ||
		(event->data.button.index != _gamepad_analog_button_b &&
			event->data.button.index != _gamepad_binary_button_back))
	{
		return FALSE;
	}
	if (!screen_action_run(widget, event, open->back, deleted))
		return FALSE;
	return TRUE;
}

/* a block's text scale (menu_tags.c's pc_menu_text_scale): an item's label
the larger, from 4:3 to 16:9 */
boolean menu_screen_text_scale(long definition_index, short wide_extra, real *scale)
{
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
