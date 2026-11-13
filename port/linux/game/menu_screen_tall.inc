/*
MENU_SCREEN_TALL.INC

A hand-written screen of the tall layout (docs/menu_screens.md): one with a
<table>, laid out as Settings' and the browsers' screens are, the screen's
whole height (tools/port_settings.py's BROWSER_TALL): its <header>'s title in
the band at the top (its detail at the band's right, its button in it), its
<tabs> under the band, the <table> under them at the left (its columns'
titles, the line, its rows; its words for no rows, and its button, in their
place) and its <panel> beside it at the right, a <search> of its own at the
top's right, and the <buttons> at the foot. On a touchscreen, its touch
layout: the tabs chips, a row two lines of its cells (each column's touch
place), no columns' titles (a <search> of the screen's at the top's right,
SORT naming the sort), the buttons a touchscreen's.

Each block's widgets are made once, as the screen is made (menu_screen.c's
screen_build), their places, widths and pictures kept here (screen_box): each
frame they are moved and made wider on a wide screen (16:9's layout: each
box's "dx dw", and past 16:9 the tall layout's past), and the table's rows,
the panel's lines and the rest are shown from the bindings and the table's
row source. Included by menu_screen.c.
*/

/* ---------- the layout, in the menus' 640 by 480 units (tools/port_settings.py's
BROWSER_TALL, _browser and _touch_browser) */

/* the band, its title's detail at its right, a button in it */
#define TALL_BAND 40
#define TALL_DETAIL_X 372
#define TALL_DETAIL_Y 11
#define TALL_DETAIL_WIDTH 240
#define TALL_BUTTON_X 276
#define TALL_BUTTON_Y 8
#define TALL_BUTTON_WIDTH 148
/* the tabs: their strip's top, the first's left, each's step and width;
their keys' prompts at the screen's margin and after the last */
#define TALL_TABS_Y 46
#define TALL_TABS_X 56
#define TALL_TAB_STEP 88
#define TALL_TAB_WIDTH 86
#define TALL_TAB_KEY_X 24
/* the table: its left and width, its titles' top, the line under them, the
rows' top, height and step, the bottom they end over (the panel's) */
#define TALL_TABLE_X 28
#define TALL_TABLE_WIDTH 404
#define TALL_TITLES_Y 74
#define TALL_LINE_Y 88
#define TALL_ROWS_Y 92
#define TALL_ROW_HEIGHT 24
#define TALL_ROWS_BOTTOM 436
#define TALL_TABLE_GROW 144
/* a row's cells: the first's left, the cells'
pictures' top in a row (16 units tall, in its middle; the gap between two
columns is theirs, <column gap>), the gap before a
pill or a lock after a cell's words, a lock's room, a ping's bars' width
and the gap before its number */
#define TALL_CELLS_X 6
#define TALL_CELL_ICON_Y 4
#define TALL_BADGE_GAP 4
#define TALL_BADGE_WIDTH 30
#define TALL_LOCK_WIDTH 14
#define TALL_BARS_WIDTH 24
#define TALL_BARS_GAP 2
/* (a title's arrow's room, and the gap before a search's glass) */
#define TALL_TITLE_ROOM 20
#define TALL_TITLE_RIGHT_ROOM 6
#define TALL_ARROW_GAP 2
#define TALL_SEARCH_GAP 2
/* the words for no rows, the button under them */
#define TALL_EMPTY_Y 220
#define TALL_EMPTY_HEIGHT 80
#define TALL_EMPTY_INSET 24
#define TALL_TABLE_BUTTON_Y 308
#define TALL_TABLE_BUTTON_WIDTH 164
/* the panel, its inner margin, its button's room at its foot */
#define TALL_PANEL_X 440
#define TALL_PANEL_Y 74
#define TALL_PANEL_WIDTH 172
#define TALL_PANEL_HEIGHT 362
#define TALL_PANEL_MOVE 38
#define TALL_PANEL_GROW 68
#define TALL_PANEL_INSET 12
#define TALL_PANEL_BUTTON_FOOT 10
#define TALL_PILL_HEIGHT 16
#define TALL_PILL_GAP 4
/* the bar: its top (the tall layout's footer: TALL_FOOTER from the foot),
its prompts' margin and the gap between them, their height and words' left */
#define TALL_BAR_Y 448
#define TALL_PROMPT_MARGIN 24
#define TALL_PROMPT_GAP 2
#define TALL_PROMPT_HEIGHT 24
#define TALL_PROMPT_TEXT_X 5
#define TALL_PROMPT_RIGHT_ROOM 4

/* a touchscreen's (_touch_browser's): a tap target's size, the gap between
two, the chips' top and left, the rows' top and width, the panel beside
them, the search at the top's right, the bar's top and its buttons' widths */
#define TOUCH_TAP 52
#define TOUCH_GAP 4
#define TOUCH_CHIPS_Y 76
#define TOUCH_CHIP_WIDTH 92
#define TOUCH_CHIP_STEP 98
#define TOUCH_CHIP_GROW 35
#define TOUCH_ROWS_Y 134
#define TOUCH_ROW_STEP 56
#define TOUCH_TABLE_WIDTH 364
#define TOUCH_ROWS_BOTTOM 410
#define TOUCH_DETAIL_X 66
#define TOUCH_DETAIL_Y 4
#define TOUCH_EMPTY_Y 230
#define TOUCH_TABLE_BUTTON_Y 316
#define TOUCH_TABLE_BUTTON_WIDTH 200
#define TOUCH_PANEL_X 400
#define TOUCH_PANEL_WIDTH 212
#define TOUCH_PANEL_HEIGHT 276
#define TOUCH_SEARCH_X 380
#define TOUCH_SEARCH_Y 18
#define TOUCH_SEARCH_WIDTH 176
#define TOUCH_SEARCH_MOVE (-102)
#define TOUCH_SEARCH_GROW 208
#define TOUCH_SEARCH_ICON 20
#define TOUCH_BAR_Y 414
#define TOUCH_PROMPT_WIDTH 136
#define TOUCH_BACK_WIDTH 110
#define TOUCH_PROMPT_GAP 6
#define TOUCH_MARGIN 28
/* (a heading's words in a touchscreen's row, on its first line) */
#define TOUCH_ROW_HEADING_Y 7

/* the panel's lines, one after another from its top (desktop, touch): the
first's gap, a heading's line, a text's line (a short text's and a long
text's), the gaps before pills (after a text), a pair (after
pills) and a text after a pair, a pill row's step, a pair's step */
#define PANEL_FIRST_GAP 12
#define TOUCH_PANEL_FIRST_GAP 10
#define PANEL_HEADING_LINE 24
#define TOUCH_PANEL_HEADING_LINE 22
#define PANEL_TEXT_LINE 22
#define PANEL_LONG_LINE 20
#define TOUCH_PANEL_TEXT_LINE 15
#define TOUCH_PANEL_LONG_LINE 21
#define PANEL_PILLS_GAP 4
#define TOUCH_PANEL_PILLS_GAP 2
#define PANEL_PAIR_GAP 3
#define PANEL_AFTER_PAIR_GAP 7
#define PANEL_PILL_STEP 20
#define PANEL_PAIR_STEP 22
#define PANEL_PAIR_LABEL_Y 3
#define PANEL_PAIR_LABEL_WIDTH 70
/* (a <spartan>'s line: the gap before and after it) */
#define PANEL_SPARTAN_GAP 6

/* (the menus' sounds: menu_functions.c's) */
#define TALL_SOUND_FORWARD 2
#define TALL_SOUND_BACK 3

/* (16:9's width past the 640, and 21:9's past 16:9's: menu_tags.c's) */
#define TALL_PAST_EXTRA 286
/* (a ping's colours by how good: menu_functions.c's) */
#define TALL_MAXIMUM_ROWS 16
#define TALL_MAXIMUM_COLUMNS 8
#define TALL_MAXIMUM_SEARCHES 4
#define TALL_MAXIMUM_TABS 8
#define TALL_MAXIMUM_LINES 16
#define TALL_MAXIMUM_BOXES 420
#define TALL_MAXIMUM_TARGETS 128
#define TALL_MAXIMUM_PROMPTS 10

/* small_ui's letters' advances (tools/port_settings.py's SMALL_UI_ADVANCES:
ui.map's font tag's), printable ASCII from the space, for the layout's
widths as the generator works them out */
static unsigned char const tall_small_advances[95] =
{
	4,3,7,10,8,13,9,6,5,5,7,8,3,5,3,6,9,6,9,9,10,9,9,8,9,9,3,3,6,7,6,8,11,10,10,9,10,8,7,10,10,4,9,9,8,11,10,10,9,
	10,10,9,9,11,9,12,9,10,9,5,6,4,8,7,5,8,8,7,9,7,5,9,9,4,5,7,4,12,8,8,8,9,6,8,6,9,7,11,7,8,7,6,4,5,8
};

static short tall_small_width(char const *text)
{
	short width = 0;

	for (; text && *text; text++)
	{
		unsigned char letter = (unsigned char)*text;

		width = (short)(width + (letter >= 32 && letter < 127 ? tall_small_advances[letter - 32] :
			tall_small_advances['W' - 32]));
	}
	return width;
}

/* ---------- the boxes: each widget's place and size, moved and grown on a
wide screen */

struct screen_box
{
	struct widget_instance *widget;
	/* its place at 4:3, and its move and growth at 16:9 (past it, as much
	again for each unit, the tall layout's past, unless centred) */
	short x, y;
	short dx, dw;
	boolean centred;
	/* its box (0: its definition's), its picture (NONE: its definition's) */
	short width, height;
	long picture;
	/* its words' colour, 0xRRGGBBAA (0: its own) */
	unsigned long color;
	/* (the box it is in, NONE: the screen; and where it is now, on the
	screen: a widget's offsets are from its parent's) */
	long parent;
	short at_x, at_y;
};

/* what a widget of the screen does when A, Start, a click or a tap is on it */
enum
{
	_target_tab,
	_target_title,
	_target_search_icon,
	_target_search_field,
	_target_search_clear,
	_target_row,
	_target_button,
};

struct screen_target
{
	struct widget_instance *widget;
	short kind;
	short index;
	char const *action;
};

struct screen_column
{
	struct menu_screen_block const *block;
	/* its place in a row and its title's, at 4:3, and their moves and
	growth at 16:9 */
	short x, width, move, grow;
	short title_x, title_width;
	struct widget_instance *title, *arrow_up, *arrow_down;
	/* (its search's: NONE for none) */
	short search;
};

struct screen_row_widgets
{
	struct widget_instance *row, *heading;
	struct widget_instance *text[TALL_MAXIMUM_COLUMNS], *badge[TALL_MAXIMUM_COLUMNS], *lock[TALL_MAXIMUM_COLUMNS],
		*picture[TALL_MAXIMUM_COLUMNS];
};

struct screen_search
{
	struct menu_screen_block const *block;
	/* its parts: the glass (or the touchscreen's bar), the field typed
	into, its X; the title it takes the place of (NULL: none) */
	struct widget_instance *icon, *field, *clear, *title;
	/* the list its parts are in */
	struct widget_instance *list;
	short field_width_base, field_grow;
};

struct screen_line
{
	struct menu_screen_block const *block;
	struct widget_instance *widget, *label;
	short width, grow;
};

struct screen_prompt
{
	struct menu_screen_block const *block;
	struct widget_instance *widget;
	char key[8];
};

struct screen_tall
{
	struct screen_box boxes[TALL_MAXIMUM_BOXES];
	long box_count;
	struct screen_target targets[TALL_MAXIMUM_TARGETS];
	long target_count;
	/* the values of the bindings, read once as the screen is made (the
	campaign's), and the rest each frame */
	struct screen_values values;
	/* the screen (the parts that take no focus), and its body (the rest) */
	struct widget_instance *owner, *body;
	/* the header's detail and the band's button */
	struct menu_screen_block const *header;
	struct widget_instance *detail;
	struct screen_prompt header_button;
	/* the tabs */
	struct menu_screen_block const *tabs;
	struct widget_instance *strip;
	struct menu_screen_block const *tab_blocks[TALL_MAXIMUM_TABS];
	struct widget_instance *tab_widgets[TALL_MAXIMUM_TABS];
	long tab_count;
	short tab_shown;
	struct widget_instance *tab_keys[2];
	/* the table */
	struct menu_screen_block const *table;
	struct menu_table_source const *source;
	struct widget_instance *titles, *line, *empty;
	struct screen_column columns[TALL_MAXIMUM_COLUMNS];
	long column_count;
	struct screen_row_widgets rows[TALL_MAXIMUM_ROWS];
	short row_count;
	short row_height, row_step, rows_y;
	struct widget_instance *table_bar;
	struct screen_prompt table_button;
	struct menu_table_view view;
	/* the searches (a column's, or the screen's), the one typed into */
	struct screen_search searches[TALL_MAXIMUM_SEARCHES];
	long search_count;
	short search_editing;
	/* the panel */
	struct menu_screen_block const *panel;
	struct screen_line lines[TALL_MAXIMUM_LINES];
	long line_count;
	struct widget_instance *panel_bar;
	struct screen_prompt panel_button;
	struct widget_instance *panel_widget;
	/* (its <spartan>: the original picture where it stands, as wide as the
	picture's shape at its height, centred in the panel) */
	struct menu_screen_block const *spartan;
	struct widget_instance *spartan_picture;
	short spartan_width;
	/* the bar's prompts */
	struct widget_instance *bar;
	struct screen_prompt prompts[TALL_MAXIMUM_PROMPTS];
	long prompt_count;
};

/* the tall screens open, by their screen_open's place */
static struct screen_tall *tall_screens[SCREEN_MAXIMUM_OPEN];
static struct screen_tall tall_storage[2];

static struct screen_tall *tall_of(struct screen_open const *open)
{
	return tall_screens[open - screens.open];
}

/* a move or growth of dx at 16:9 now (and past 16:9 as much again for each
unit, the tall layout's past: by the screen's edges' move for a move alone,
grows: by its whole width), none for a centred box */
static short tall_wide(short dx, boolean grows, boolean centred)
{
	short moved = ui_widget_port_wide_scale(dx, ui_widget_port_wide_extra());
	long past = (long)dx * TALL_PAST_EXTRA;

	if (centred || ui_widget_port_touch_menus())
		return moved;
	past = (past + (past < 0 ? -WIDE_LAYOUT_EXTRA / 2 : WIDE_LAYOUT_EXTRA / 2)) / WIDE_LAYOUT_EXTRA;
	return (short)(moved + ui_widget_port_past_scale((short)past, grows ? ui_widget_port_past_extra() :
		ui_widget_port_past_move()));
}

static struct screen_box *tall_box_of(struct screen_tall *tall, struct widget_instance const *widget)
{
	long index;

	for (index = 0; widget && index < tall->box_count; index++)
	{
		if (tall->boxes[index].widget == widget)
			return &tall->boxes[index];
	}
	return NULL;
}

/* a block's widget made in the parent at x, y (moved dx and dw wider at
16:9), as wide and tall as width and height (0: its own), kept as a box */
static struct widget_instance *tall_add(struct screen_tall *tall, struct widget_instance *parent, long tag, short x,
	short y, short dx, short dw, short width, short height)
{
	struct widget_instance *widget;
	struct screen_box *box;

	if (tag == NONE || !parent || tall->box_count >= TALL_MAXIMUM_BOXES)
		return NULL;
	widget = ui_widget_port_add_child(parent, tag, NULL);
	if (!widget)
		return NULL;
	box = &tall->boxes[tall->box_count++];
	memset(box, 0, sizeof(*box));
	box->widget = widget;
	box->x = x;
	box->y = y;
	box->dx = dx;
	box->dw = dw;
	box->width = width;
	box->height = height;
	box->picture = NONE;
	box->parent = NONE;
	{
		long index;

		for (index = 0; index < tall->box_count - 1; index++)
		{
			if (tall->boxes[index].widget == parent)
				box->parent = index;
		}
	}
	widget->horizontal_offset = x;
	widget->vertical_offset = y;
	return widget;
}

static void tall_target(struct screen_tall *tall, struct widget_instance *widget, short kind, short index,
	char const *action)
{
	if (!widget || tall->target_count >= TALL_MAXIMUM_TARGETS)
		return;
	tall->targets[tall->target_count].widget = widget;
	tall->targets[tall->target_count].kind = kind;
	tall->targets[tall->target_count].index = index;
	tall->targets[tall->target_count].action = action;
	tall->target_count++;
}

/* a picture of the menus' parts by its name and size ("main_menu/parts/
row_%dx%d"), NONE if there is none (the widget's own then) */
static long tall_picture(char const *format, short width, short height)
{
	char name[128];
	char tag_name[160];
	long index;

	snprintf(name, sizeof(name), format, width, height);
	snprintf(tag_name, sizeof(tag_name), "pc\\%s", name);
	for (index = 0; tag_name[index]; index++)
	{
		if (tag_name[index] == '/')
			tag_name[index] = '\\';
	}
	return tag_loaded(BITMAP_GROUP_TAG, tag_name);
}

static void tall_box_picture(struct screen_tall *tall, struct widget_instance *widget, char const *format, short width,
	short height)
{
	struct screen_box *box = tall_box_of(tall, widget);

	if (box)
		box->picture = tall_picture(format, width, height);
}

static void tall_centred(struct screen_tall *tall, struct widget_instance *widget)
{
	struct screen_box *box = tall_box_of(tall, widget);

	if (box)
		box->centred = TRUE;
}

/* a text's value now (UTF-8): its bindings filled; FALSE (empty) if one has
none */
static boolean tall_text(struct screen_tall *tall, char const *text, char *out, unsigned long size)
{
	if (!text)
	{
		out[0] = 0;
		return FALSE;
	}
	if (!menu_screen_text(text, screen_value, &tall->values, out, size))
	{
		out[0] = 0;
		return FALSE;
	}
	return TRUE;
}

/* a binding's value now ("" for none) */
static char const *tall_binding(struct screen_tall *tall, char const *name, char *out, unsigned long size)
{
	out[0] = 0;
	if (name && !screen_value(&tall->values, name, out, size))
		out[0] = 0;
	return out;
}

/* whether a block is shown now: its when holds, its if has a value */
static boolean tall_shown(struct screen_tall *tall, struct menu_screen_block const *block)
{
	char value[MENU_SCREEN_MAXIMUM_TEXT];

	if (!menu_screen_when(block->when))
		return FALSE;
	return !block->condition || tall_binding(tall, block->condition, value, sizeof(value))[0];
}

static void tall_text_set(struct widget_instance *widget, char const *utf8)
{
	if (widget)
		screen_text_set(widget, utf8);
}

static void visible_set_safe(struct widget_instance *widget, boolean visible)
{
	if (widget)
		widget->visible = visible;
}

/* the key's picture's name in a prompt's words ("%a-button") */
static char const *tall_key_picture(char const *key)
{
	static struct
	{
		char const *key;
		char const *picture;
	} const pictures[] =
	{
		{ "accept", "%a-button" }, { "back", "%b-button" }, { "x", "%x-button" }, { "y", "%y-button" },
		{ "lb", "%white-button" }, { "rb", "%black-button" }, { "lt", "%left-trigger" },
		{ "rt", "%right-trigger" }, { "r3", "%right-thumb" }, { "view", "%back-button" },
	};
	long index;

	for (index = 0; index < NUMBEROF(pictures); index++)
	{
		if (!strcmp(pictures[index].key, key))
			return pictures[index].picture;
	}
	return "%a-button";
}

/* the button a key is (input.h's gamepad buttons) */
static short tall_key_button(char const *key)
{
	static struct
	{
		char const *key;
		short button;
	} const buttons[] =
	{
		{ "accept", _gamepad_analog_button_a }, { "back", _gamepad_analog_button_b },
		{ "x", _gamepad_analog_button_x }, { "y", _gamepad_analog_button_y },
		{ "lb", _gamepad_analog_button_white }, { "rb", _gamepad_analog_button_black },
		{ "lt", _gamepad_analog_button_left_trigger }, { "rt", _gamepad_analog_button_right_trigger },
		{ "r3", _gamepad_binary_button_right_thumb }, { "view", _gamepad_binary_button_back },
	};
	long index;

	for (index = 0; key && index < NUMBEROF(buttons); index++)
	{
		if (!strcmp(buttons[index].key, key))
			return buttons[index].button;
	}
	return NONE;
}

/* the keyboard's key a prompt's button is drawn as (tools/port_settings.py's
BROWSER_KEYS: a screen with a table has the browsers' keys), for its width */
static char const *tall_keyboard_key(char const *key)
{
	static struct
	{
		char const *key;
		char const *name;
	} const names[] =
	{
		{ "accept", "Enter" }, { "back", "Esc" }, { "x", "Tab" }, { "y", "F" }, { "lb", "Q" }, { "rb", "E" },
		{ "lt", "L" }, { "rt", "R" }, { "r3", "/" }, { "view", "F1" },
	};
	long index;

	for (index = 0; index < NUMBEROF(names); index++)
	{
		if (!strcmp(names[index].key, key))
			return names[index].name;
	}
	return "E";
}

/* where a prompt's words end with the keyboard's key (port_settings.py's
prompt_words_end) and with the controller's picture (controller_prompt_end) */
static short tall_prompt_end(char const *key, char const *label, boolean controller)
{
	char spaced[MENU_SCREEN_MAXIMUM_TEXT];
	short advance;

	snprintf(spaced, sizeof(spaced), " %s", label);
	if (controller)
	{
		advance = !strcmp(key, "rt") ? 69 : !strcmp(key, "lt") ? 62 : !strcmp(key, "r3") ? 65 : 9;
	}
	else
	{
		short letters = tall_small_width(tall_keyboard_key(key));
		short width = (short)MAX(letters + 2 * 3, 19);

		width = (short)(width + (width - letters) % 2);
		advance = (short)(4 + width + 3);
	}
	return (short)(advance + tall_small_width(spaced));
}

/* ---------- making the screen */

static long tall_tag(char const *name)
{
	char tag_name[96];

	snprintf(tag_name, sizeof(tag_name), "pc\\screens\\%s", name);
	return tag_loaded(UI_WIDGET_DEFINITION_TAG, tag_name);
}

/* the band: its detail, its button */
static void tall_build_header(struct screen_tall *tall, struct widget_instance *screen,
	struct menu_screen const *file, long header_index)
{
	struct menu_screen_block const *header = &file->blocks[header_index];
	boolean touch = ui_widget_port_touch_menus() != 0;
	long index;

	tall->header = header;
	if (header->empty)
	{
		tall->detail = touch ?
			tall_add(tall, screen, tall_tag("header_detail_left"), TOUCH_DETAIL_X, TOUCH_DETAIL_Y, -106, 0, 0, 0) :
			tall_add(tall, screen, tall_tag("header_detail"), TALL_DETAIL_X, TALL_DETAIL_Y, 106, 0, 0, 0);
	}
	for (index = 0; index < file->block_count; index++)
	{
		struct menu_screen_block const *button = &file->blocks[index];
		struct widget_instance *made;

		if (button->kind != MENU_SCREEN_BUTTON || button->parent != header_index || !menu_screen_when(button->when))
			continue;
		made = tall_add(tall, screen, screens.button_tag, TALL_BUTTON_X, TALL_BUTTON_Y, 0, 0, 0, 0);
		tall_centred(tall, made);
		tall->header_button.block = button;
		tall->header_button.widget = made;
		snprintf(tall->header_button.key, sizeof(tall->header_button.key), "%s", button->key);
		if (button->action)
			tall_target(tall, made, _target_button, NONE, button->action);
		break;
	}
}

/* the tabs: a strip of them (a touchscreen's chips), spread on a wide
screen, and their keys' prompts at either end */
static void tall_build_tabs(struct screen_tall *tall, struct widget_instance *screen, struct menu_screen const *file,
	long tabs_index)
{
	boolean touch = ui_widget_port_touch_menus() != 0;
	long index, count = 0, made_index = 0;
	short y = touch ? TOUCH_CHIPS_Y : TALL_TABS_Y;

	tall->tabs = &file->blocks[tabs_index];
	tall->tab_shown = NONE;
	tall->strip = tall_add(tall, tall->body, tall_tag("tabs"), 0, y, 0, 0, 0, touch ? TOUCH_TAP : 0);
	for (index = 0; index < file->block_count; index++)
		count += file->blocks[index].kind == MENU_SCREEN_TAB && file->blocks[index].parent == tabs_index &&
			menu_screen_when(file->blocks[index].when);
	for (index = 0; tall->strip && index < file->block_count && made_index < TALL_MAXIMUM_TABS; index++)
	{
		struct menu_screen_block const *tab = &file->blocks[index];
		struct widget_instance *made;
		char label[MENU_SCREEN_MAXIMUM_TEXT];

		if (tab->kind != MENU_SCREEN_TAB || tab->parent != tabs_index || !menu_screen_when(tab->when))
			continue;
		if (touch)
		{
			made = tall_add(tall, tall->strip, tall_tag("chip"), (short)(TOUCH_MARGIN + made_index * TOUCH_CHIP_STEP), y,
				(short)(-106 + made_index * TOUCH_CHIP_GROW), TOUCH_CHIP_GROW, 0, 0);
		}
		else
		{
			made = tall_add(tall, tall->strip, tall_tag("tab"), (short)(TALL_TABS_X + made_index * TALL_TAB_STEP), y,
				0, 0, 0, 0);
			/* (each moved by its part of 16:9's room, as Settings' tabs:
			rounded as the generator rounds them) */
			if (made)
			{
				double exact = -106.0 + 212.0 * (double)made_index / (double)(count > 1 ? count - 1 : 1);

				tall_box_of(tall, made)->dx = (short)(exact < 0 ? -(long)(-exact + 0.5) : (long)(exact + 0.5));
			}
			tall_centred(tall, made);
		}
		if (!made)
			break;
		tall_text(tall, tab->label, label, sizeof(label));
		tall_text_set(made, label);
		tall->tab_blocks[made_index] = tab;
		tall->tab_widgets[made_index] = made;
		tall_target(tall, made, _target_tab, (short)made_index, tab->action);
		made_index++;
	}
	tall->tab_count = made_index;
	/* (the keys that step them: the computer's, beside them) */
	if (!touch && made_index)
	{
		struct widget_instance *key;

		key = tall_add(tall, screen, tall_tag("tab_key"), TALL_TAB_KEY_X, y, -106, 0, 0, 0);
		tall_centred(tall, key);
		tall_text_set(key, "%white-button");
		tall->tab_keys[0] = key;
		key = tall_add(tall, screen, tall_tag("tab_key"),
			(short)(TALL_TABS_X + (made_index - 1) * TALL_TAB_STEP + TALL_TAB_WIDTH + 4), y, 106, 0, 0, 0);
		tall_centred(tall, key);
		tall_text_set(key, "%black-button");
		tall->tab_keys[1] = key;
	}
}

/* a search's parts: in a column's title (the glass after the title's
words, the field in the column's place, its X at the column's right), or
the screen's own at the top's right (a touchscreen's: its bar, its field,
its X beside them) */
static void tall_build_search(struct screen_tall *tall, struct menu_screen_block const *block,
	struct screen_column *column)
{
	struct screen_search *search;
	short index;

	if (tall->search_count >= TALL_MAXIMUM_SEARCHES || !menu_screen_when(block->when))
		return;
	index = (short)tall->search_count++;
	search = &tall->searches[index];
	memset(search, 0, sizeof(*search));
	search->block = block;
	if (column)
	{
		short y = TALL_TITLES_Y;
		short field_width = (short)(column->width - 16 - TALL_SEARCH_GAP);

		search->list = tall->titles;
		search->title = column->title;
		search->icon = tall_add(tall, tall->titles, tall_tag("search_icon"),
			(short)(TALL_TABLE_X + column->title_x + column->title_width), y, (short)(-106 + column->move), 0, 0, 0);
		search->field = tall_add(tall, tall->titles, tall_tag("search_field"), (short)(TALL_TABLE_X + column->x), y,
			(short)(-106 + column->move), column->grow, field_width, 0);
		tall_box_picture(tall, search->field, "main_menu/parts/outline_%dx%d", field_width, 16);
		search->clear = tall_add(tall, tall->titles, tall_tag("search_clear"),
			(short)(TALL_TABLE_X + column->x + column->width - 16), y, (short)(-106 + column->move + column->grow), 0,
			0, 0);
		search->field_width_base = field_width;
		search->field_grow = column->grow;
		column->search = index;
	}
	else
	{
		char label[MENU_SCREEN_MAXIMUM_TEXT];
		short icon_y = (TOUCH_TAP - TOUCH_SEARCH_ICON) / 2;

		search->list = tall_add(tall, tall->body, tall_tag("titles"), 0, TOUCH_SEARCH_Y, 0, 0, 0, TOUCH_TAP);
		search->icon = tall_add(tall, search->list, tall_tag("touch_search"), TOUCH_SEARCH_X, TOUCH_SEARCH_Y,
			TOUCH_SEARCH_MOVE, TOUCH_SEARCH_GROW, 0, 0);
		tall_add(tall, search->icon, tall_tag("touch_search_glass"), (short)(TOUCH_SEARCH_X + icon_y),
			(short)(TOUCH_SEARCH_Y + icon_y), TOUCH_SEARCH_MOVE, 0, 0, 0);
		tall_text(tall, block->label ? block->label : "SEARCH", label, sizeof(label));
		tall_text_set(search->icon, label);
		search->field = tall_add(tall, search->list, tall_tag("touch_search_field"), TOUCH_SEARCH_X, TOUCH_SEARCH_Y,
			TOUCH_SEARCH_MOVE, TOUCH_SEARCH_GROW, 0, 0);
		tall_add(tall, search->field, tall_tag("touch_search_glass"), (short)(TOUCH_SEARCH_X + icon_y),
			(short)(TOUCH_SEARCH_Y + icon_y), TOUCH_SEARCH_MOVE, 0, 0, 0);
		search->clear = tall_add(tall, search->list, tall_tag("touch_search_clear"),
			(short)(TOUCH_SEARCH_X + TOUCH_SEARCH_WIDTH + TOUCH_GAP), TOUCH_SEARCH_Y, 106, 0, 0, 0);
		tall_add(tall, search->clear, tall_tag("touch_search_cross"),
			(short)(TOUCH_SEARCH_X + TOUCH_SEARCH_WIDTH + TOUCH_GAP + icon_y), (short)(TOUCH_SEARCH_Y + icon_y), 106, 0,
			0, 0);
		search->field_width_base = TOUCH_SEARCH_WIDTH;
		search->field_grow = TOUCH_SEARCH_GROW;
	}
	tall_target(tall, search->icon, _target_search_icon, index, NULL);
	tall_target(tall, search->field, _target_search_field, index, NULL);
	tall_target(tall, search->clear, _target_search_clear, index, NULL);
	if (search->field)
		search->field->visible = FALSE;
	if (search->clear)
		search->clear->visible = FALSE;
}

/* the table: its columns' titles (the computer's), the line under them, its
rows (as many as fit over the panel's foot), each its cells, its words for
no rows and its button */
static void tall_build_table(struct screen_tall *tall, struct menu_screen const *file, long table_index)
{
	boolean touch = ui_widget_port_touch_menus() != 0;
	long index, column_index;
	short x = TALL_CELLS_X, move = 0;
	boolean first = TRUE;
	short table_width = touch ? TOUCH_TABLE_WIDTH : TALL_TABLE_WIDTH;

	tall->table = &file->blocks[table_index];
	for (index = 0; index < NUMBEROF(screen_source_names); index++)
	{
		if (tall->table->source && !strcmp(tall->table->source, screen_source_names[index]))
			tall->source = screen_sources[index];
	}
	/* (the columns' places: one after another, each moved by the growth of
	those before it) */
	for (index = 0; index < file->block_count && tall->column_count < TALL_MAXIMUM_COLUMNS; index++)
	{
		struct menu_screen_block const *block = &file->blocks[index];
		struct screen_column *column;
		short words;

		if (block->kind != MENU_SCREEN_COLUMN || block->parent != table_index || !menu_screen_when(block->when))
			continue;
		column = &tall->columns[tall->column_count++];
		memset(column, 0, sizeof(*column));
		if (!first)
			x = (short)(x + block->gap);
		first = FALSE;
		column->block = block;
		column->search = NONE;
		column->x = x;
		column->width = block->width;
		column->move = move;
		column->grow = block->grow;
		words = (short)(tall_small_width(block->label) + tall_small_width(block->label) % 2);
		if (block->right)
		{
			column->title_width = (short)MAX(block->width, words + TALL_TITLE_RIGHT_ROOM);
			column->title_x = (short)(x + block->width - column->title_width);
		}
		else
		{
			column->title_width = words;
			column->title_x = x;
		}
		x = (short)(x + block->width);
		move = (short)(move + block->grow);
	}
	/* (the titles, a searched column's with room for its glass) */
	if (!touch)
	{
		tall->titles = tall_add(tall, tall->body, tall_tag("titles"), 0, TALL_TITLES_Y, 0, 0, 0, 0);
		for (column_index = 0; column_index < tall->column_count; column_index++)
		{
			struct screen_column *column = &tall->columns[column_index];
			struct menu_screen_block const *block = column->block;
			long child;
			boolean searched = FALSE;
			char label[MENU_SCREEN_MAXIMUM_TEXT];

			for (child = 0; child < file->block_count; child++)
				searched |= file->blocks[child].kind == MENU_SCREEN_SEARCH && file->blocks[child].parent ==
					column->block - file->blocks && menu_screen_when(file->blocks[child].when);
			if (searched)
				column->title_width = (short)(column->title_width + TALL_TITLE_ROOM);
			if (block->title_width)
			{
				column->title_width = block->title_width;
				if (block->right)
					column->title_x = (short)(column->x + block->width - column->title_width);
			}
			if (!block->label || !block->label[0])
				continue;
			column->title = tall_add(tall, tall->titles, tall_tag(block->right ? "title_right" : "title"),
				(short)(TALL_TABLE_X + column->title_x), TALL_TITLES_Y, (short)(-106 + column->move),
				searched || block->right ? 0 : column->grow, column->title_width, 0);
			tall_text(tall, block->label, label, sizeof(label));
			tall_text_set(column->title, label);
			if (block->sort)
				tall_target(tall, column->title, _target_title, (short)column_index, block->sort);
			column->arrow_up = tall_add(tall, column->title, tall_tag("sort_up"), (short)(TALL_TABLE_X + column->title_x),
				TALL_TITLES_Y, (short)(-106 + column->move), 0, 0, 0);
			column->arrow_down = tall_add(tall, column->title, tall_tag("sort_down"),
				(short)(TALL_TABLE_X + column->title_x), TALL_TITLES_Y, (short)(-106 + column->move), 0, 0, 0);
			for (child = 0; child < file->block_count; child++)
			{
				if (file->blocks[child].kind == MENU_SCREEN_SEARCH && file->blocks[child].parent ==
					column->block - file->blocks)
				{
					tall_build_search(tall, &file->blocks[child], column);
				}
			}
		}
		tall->line = tall_add(tall, tall->owner, tall_tag("table_line"), TALL_TABLE_X, TALL_LINE_Y, -106,
			TALL_TABLE_GROW, TALL_TABLE_WIDTH, 0);
		tall_box_picture(tall, tall->line, "main_menu/parts/line_%d", TALL_TABLE_WIDTH, 0);
	}
	/* (the rows) */
	tall->rows_y = touch ? TOUCH_ROWS_Y : TALL_ROWS_Y;
	tall->row_height = touch ? TOUCH_TAP : TALL_ROW_HEIGHT;
	tall->row_step = touch ? TOUCH_ROW_STEP : TALL_ROW_HEIGHT;
	tall->row_count = (short)MIN(TALL_MAXIMUM_ROWS, ((touch ? TOUCH_ROWS_BOTTOM : TALL_ROWS_BOTTOM) - tall->rows_y +
		tall->row_step - tall->row_height) / tall->row_step);
	for (index = 0; index < tall->row_count; index++)
	{
		struct screen_row_widgets *row = &tall->rows[index];
		short y = (short)(tall->rows_y + index * tall->row_step);

		memset(row, 0, sizeof(*row));
		row->row = tall_add(tall, tall->body, tall_tag(touch ? "touch_row" : "row"), TALL_TABLE_X, y, -106,
			TALL_TABLE_GROW, table_width, 0);
		if (!row->row)
			break;
		tall_box_picture(tall, row->row, touch ? "main_menu/parts/touch_row_%dx%d" : "main_menu/parts/row_%dx%d",
			table_width, tall->row_height);
		tall_target(tall, row->row, _target_row, (short)index, tall->table->action);
		row->row->visible = FALSE;
		for (column_index = 0; column_index < tall->column_count; column_index++)
		{
			struct screen_column const *column = &tall->columns[column_index];
			struct menu_screen_block const *block = column->block;
			short cx = (short)(TALL_TABLE_X + column->x), cy = y, cw = column->width;
			short cmove = (short)(-106 + column->move), cgrow = column->grow;
			short icon_y = TALL_CELL_ICON_Y;
			boolean large = FALSE;

			if (touch)
			{
				if (!block->touch_width)
					continue;
				cx = (short)(TALL_TABLE_X + block->touch_x);
				cy = (short)(y + block->touch_y);
				cw = block->touch_width;
				cmove = (short)(-106 + block->touch_move);
				cgrow = block->touch_grow;
				icon_y = 0;
				large = block->touch_y < 20 && block->cell != MENU_SCREEN_CELL_STAR;
			}
			switch (block->cell)
			{
			case MENU_SCREEN_CELL_STAR:
				row->picture[column_index] = tall_add(tall, row->row, tall_tag("cell_star"), cx, (short)(cy + icon_y),
					cmove, 0, 0, 0);
				break;
			case MENU_SCREEN_CELL_PING:
				row->picture[column_index] = tall_add(tall, row->row, tall_tag("cell_bars"), cx,
					(short)(cy + icon_y - (touch ? 1 : 0)), cmove, 0, 0, 0);
				row->text[column_index] = tall_add(tall, row->row, tall_tag(touch ? "touch_cell_right" : "cell_right"),
					cx, cy, cmove, cgrow, cw, 0);
				break;
			default:
				row->text[column_index] = tall_add(tall, row->row, tall_tag(touch ? (large ? (block->right ?
					"touch_cell_large_right" : "touch_cell_large") : (block->right ? "touch_cell_right" : "touch_cell")) :
					block->right ? "cell_right" : "cell"), cx, cy, cmove, cgrow, cw, 0);
				row->badge[column_index] = tall_add(tall, row->row, tall_tag("cell_badge"), cx,
					(short)(cy + (touch ? 2 : icon_y)), cmove, 0, 0, 0);
				row->lock[column_index] = tall_add(tall, row->row, tall_tag("cell_lock"), cx,
					(short)(cy + (touch ? 2 : icon_y)), cmove, 0, 0, 0);
				break;
			}
		}
		/* (a heading's words, across the row from the first column of words) */
		{
			short from = 0, to = table_width;

			for (column_index = 0; column_index < tall->column_count; column_index++)
			{
				if (tall->columns[column_index].block->cell != MENU_SCREEN_CELL_STAR)
				{
					from = touch ? tall->columns[column_index].block->touch_x : tall->columns[column_index].x;
					break;
				}
			}
			if (tall->column_count && !touch)
				to = (short)(tall->columns[tall->column_count - 1].x + tall->columns[tall->column_count - 1].width);
			row->heading = touch ?
				tall_add(tall, row->row, tall_tag("touch_cell_large"), (short)(TALL_TABLE_X + from),
					(short)(y + TOUCH_ROW_HEADING_Y), -106, TALL_TABLE_GROW, (short)(table_width - 2 * from), 0) :
				tall_add(tall, row->row, tall_tag("cell"), (short)(TALL_TABLE_X + from), y, -106, TALL_TABLE_GROW,
					(short)(to - from), 0);
			if (row->heading)
				row->heading->visible = FALSE;
		}
	}
	/* (its words for none, its button under them) */
	tall->empty = tall_add(tall, tall->owner, tall_tag("table_empty"), TALL_TABLE_X + TALL_EMPTY_INSET,
		touch ? TOUCH_EMPTY_Y : TALL_EMPTY_Y, -106, TALL_TABLE_GROW, (short)(table_width - 2 * TALL_EMPTY_INSET),
		TALL_EMPTY_HEIGHT);
	if (tall->empty)
		tall->empty->visible = FALSE;
}

/* the table's button (under its words for none: CLEAR SEARCH), in a bar of
its own in the body */
static void tall_build_table_button(struct screen_tall *tall, struct menu_screen const *file, long table_index)
{
	boolean touch = ui_widget_port_touch_menus() != 0;
	short table_width = touch ? TOUCH_TABLE_WIDTH : TALL_TABLE_WIDTH;
	long index;

	for (index = 0; index < file->block_count; index++)
	{
		struct menu_screen_block const *button = &file->blocks[index];
		short width = touch ? TOUCH_TABLE_BUTTON_WIDTH : TALL_TABLE_BUTTON_WIDTH;
		short y = touch ? TOUCH_TABLE_BUTTON_Y : TALL_TABLE_BUTTON_Y;
		short x = (short)(TALL_TABLE_X + (table_width - width) / 2);
		char label[MENU_SCREEN_MAXIMUM_TEXT];
		char text[MENU_SCREEN_MAXIMUM_TEXT];

		if (button->kind != MENU_SCREEN_BUTTON || button->parent != table_index || !menu_screen_when(button->when))
			continue;
		tall->table_bar = tall_add(tall, tall->body, tall_tag("bar"), 0, y, 0, 0, 0, touch ? TOUCH_TAP : 0);
		tall->table_button.widget = tall_add(tall, tall->table_bar, tall_tag(touch ? "touch_button_do" : "table_button"),
			x, y, (short)(-106 + TALL_TABLE_GROW / 2), 0, width, 0);
		tall->table_button.block = button;
		snprintf(tall->table_button.key, sizeof(tall->table_button.key), "%s", button->key);
		tall_box_picture(tall, tall->table_button.widget, touch ? "main_menu/parts/touch_button_%dx%d" :
			"main_menu/parts/item_%dx%d", width, touch ? TOUCH_TAP : TALL_PROMPT_HEIGHT);
		tall_text(tall, button->label, label, sizeof(label));
		snprintf(text, sizeof(text), touch ? "%s%s" : "%s %s", touch ? "" : tall_key_picture(button->key), label);
		tall_text_set(tall->table_button.widget, text);
		if (button->action)
			tall_target(tall, tall->table_button.widget, _target_button, NONE, button->action);
		if (tall->table_bar)
		{
			tall->table_bar->visible = FALSE;
			tall->table_bar->focused_child = tall->table_button.widget;
		}
		break;
	}
}

/* the panel: its picture beside the table, its lines from its top, its
button at its foot */
static void tall_build_panel(struct screen_tall *tall, struct menu_screen const *file, long panel_index)
{
	boolean touch = ui_widget_port_touch_menus() != 0;
	short left = touch ? TOUCH_PANEL_X : TALL_PANEL_X;
	short top = touch ? TOUCH_ROWS_Y : TALL_PANEL_Y;
	short width = touch ? TOUCH_PANEL_WIDTH : TALL_PANEL_WIDTH;
	short height = touch ? TOUCH_PANEL_HEIGHT : TALL_PANEL_HEIGHT;
	short inner_x = (short)(left + TALL_PANEL_INSET), inner_width = (short)(width - 2 * TALL_PANEL_INSET);
	short y = top, previous = NONE;
	struct widget_instance *panel;
	long index;

	tall->panel = &file->blocks[panel_index];
	panel = tall_add(tall, tall->owner, tall_tag("panel"), left, top, TALL_PANEL_MOVE, TALL_PANEL_GROW, width,
		(short)(height - (touch ? 0 : 0)));
	tall_box_picture(tall, panel, "main_menu/parts/panel_%dx%d", width, height);
	tall->panel_widget = panel;
	for (index = 0; panel && index < file->block_count; index++)
	{
		struct menu_screen_block const *block = &file->blocks[index];
		struct screen_line *line;
		short gap = 0;

		if (block->parent != panel_index || !menu_screen_when(block->when))
			continue;
		if (block->kind == MENU_SCREEN_BUTTON)
		{
			short button_height = touch ? TOUCH_TAP : TALL_PROMPT_HEIGHT;
			short button_y = (short)(top + height - button_height - TALL_PANEL_BUTTON_FOOT);
			char label[MENU_SCREEN_MAXIMUM_TEXT];
			char text[MENU_SCREEN_MAXIMUM_TEXT];

			tall->panel_bar = tall_add(tall, tall->body, tall_tag("bar"), 0, button_y, 0, 0, 0, button_height);
			tall->panel_button.widget = tall_add(tall, tall->panel_bar, tall_tag(touch ? "touch_button_large" :
				"panel_button"), inner_x, button_y, TALL_PANEL_MOVE, TALL_PANEL_GROW, inner_width, 0);
			tall_box_picture(tall, tall->panel_button.widget, touch ? "main_menu/parts/touch_button_%dx%d" :
				"main_menu/parts/button_%dx%d", inner_width, button_height);
			tall->panel_button.block = block;
			snprintf(tall->panel_button.key, sizeof(tall->panel_button.key), "%s", block->key);
			tall_text(tall, block->label, label, sizeof(label));
			snprintf(text, sizeof(text), touch ? "%s%s" : "%s %s", touch ? "" : tall_key_picture(block->key), label);
			tall_text_set(tall->panel_button.widget, text);
			if (block->action)
				tall_target(tall, tall->panel_button.widget, _target_button, NONE, block->action);
			if (tall->panel_bar)
			{
				tall->panel_bar->visible = FALSE;
				tall->panel_bar->focused_child = tall->panel_button.widget;
			}
			continue;
		}
		/* (the Spartan: a line as tall as it, the original picture in its
		place, centred each frame: tall_frame) */
		if (block->kind == MENU_SCREEN_SPARTAN)
		{
			short spartan_height = (short)block->box_height;

			y = (short)(y + (previous == NONE ? (touch ? TOUCH_PANEL_FIRST_GAP : PANEL_FIRST_GAP) : PANEL_SPARTAN_GAP));
			tall->spartan = block;
			tall->spartan_width = (short)(spartan_height * SCREEN_SPARTAN_PICTURE_WIDTH / SCREEN_SPARTAN_PICTURE_HEIGHT);
			tall->spartan_picture = tall_add(tall, tall->owner, screens.spartan_tag,
				(short)(left + (width - tall->spartan_width) / 2), y, TALL_PANEL_MOVE, 0, tall->spartan_width,
				spartan_height);
			y = (short)(y + spartan_height + PANEL_SPARTAN_GAP);
			previous = MENU_SCREEN_SPARTAN;
			continue;
		}
		/* (the gap before it, by what is before it) */
		if (previous == NONE)
			gap = touch ? TOUCH_PANEL_FIRST_GAP : PANEL_FIRST_GAP;
		else if (block->kind == MENU_SCREEN_PILLS)
			gap = previous == MENU_SCREEN_PANEL_TEXT ? (touch ? TOUCH_PANEL_PILLS_GAP : PANEL_PILLS_GAP) : 0;
		else if (block->kind == MENU_SCREEN_PAIR)
			gap = previous == MENU_SCREEN_PILLS ? PANEL_PAIR_GAP : 0;
		else if (previous == MENU_SCREEN_PAIR)
			gap = PANEL_AFTER_PAIR_GAP;
		y = (short)(y + gap);
		if (block->kind == MENU_SCREEN_PILLS)
		{
			long pill_index;
			short x = 0;

			for (pill_index = 0; pill_index < file->block_count && tall->line_count < TALL_MAXIMUM_LINES; pill_index++)
			{
				struct menu_screen_block const *pill = &file->blocks[pill_index];

				if (pill->kind != MENU_SCREEN_PILL || pill->parent != index)
					continue;
				if (x && x + pill->width > inner_width)
				{
					x = 0;
					y = (short)(y + PANEL_PILL_STEP);
				}
				line = &tall->lines[tall->line_count++];
				memset(line, 0, sizeof(*line));
				line->block = pill;
				line->widget = tall_add(tall, tall->owner, tall_tag("pill"), (short)(inner_x + x), y, TALL_PANEL_MOVE, 0,
					pill->width, 0);
				tall_box_picture(tall, line->widget, "main_menu/parts/pill_%d", pill->width, 0);
				x = (short)(x + pill->width + TALL_PILL_GAP);
			}
			y = (short)(y + PANEL_PILL_STEP);
			previous = MENU_SCREEN_PILLS;
			continue;
		}
		if (tall->line_count >= TALL_MAXIMUM_LINES)
			break;
		line = &tall->lines[tall->line_count++];
		memset(line, 0, sizeof(*line));
		line->block = block;
		line->width = inner_width;
		line->grow = TALL_PANEL_GROW;
		if (block->kind == MENU_SCREEN_PAIR)
		{
			line->widget = tall_add(tall, tall->owner, tall_tag("pair_value"), inner_x, y, TALL_PANEL_MOVE,
				TALL_PANEL_GROW, inner_width, 0);
			line->label = tall_add(tall, tall->owner, tall_tag("pair_label"), inner_x, (short)(y + PANEL_PAIR_LABEL_Y),
				TALL_PANEL_MOVE, 0, PANEL_PAIR_LABEL_WIDTH, 0);
			if (line->label)
			{
				char label[MENU_SCREEN_MAXIMUM_TEXT];

				tall_text(tall, block->label, label, sizeof(label));
				tall_text_set(line->label, label);
			}
			y = (short)(y + PANEL_PAIR_STEP);
		}
		else
		{
			boolean heading = block->kind == MENU_SCREEN_PANEL_HEADING;
			short lines = touch ? block->touch_lines : block->lines;
			short line_height = heading ? (touch ? TOUCH_PANEL_HEADING_LINE : PANEL_HEADING_LINE) :
				touch ? (lines > 2 ? TOUCH_PANEL_LONG_LINE : TOUCH_PANEL_TEXT_LINE) :
				lines > 2 ? PANEL_LONG_LINE : PANEL_TEXT_LINE;

			line->widget = tall_add(tall, tall->owner, tall_tag(heading ? "panel_heading" : "panel_text"), inner_x, y,
				TALL_PANEL_MOVE, TALL_PANEL_GROW, inner_width, (short)(lines * line_height));
			y = (short)(y + lines * line_height);
		}
		previous = (short)block->kind;
	}
}

/* the bar: its buttons as prompts (the computer's, each as wide as its
words or its width, one after another from the screen's margin, those of
one key in one place; a touchscreen's, its buttons, BACK at the right) */
static void tall_build_bar(struct screen_tall *tall, struct menu_screen const *file, long bar_index)
{
	boolean touch = ui_widget_port_touch_menus() != 0;
	short x = touch ? TOUCH_MARGIN : TALL_PROMPT_MARGIN, place;
	short y = touch ? TOUCH_BAR_Y : TALL_BAR_Y;
	short slot_x[TALL_MAXIMUM_PROMPTS], slot_width[TALL_MAXIMUM_PROMPTS], widths[TALL_MAXIMUM_PROMPTS];
	short grown[TALL_MAXIMUM_PROMPTS];
	char const *previous_key = NULL;
	long index, count = 0;
	boolean any_grown = FALSE;

	tall->bar = tall_add(tall, tall->body, tall_tag(touch ? "bar" : "prompts"), 0, y, 0, 0, 0, touch ? TOUCH_TAP : 0);
	if (!tall->bar)
		return;
	for (index = 0; index < file->block_count && count < TALL_MAXIMUM_PROMPTS; index++)
	{
		struct menu_screen_block const *button = &file->blocks[index];
		char label[MENU_SCREEN_MAXIMUM_TEXT];
		short width, needed;
		boolean shared;

		if (button->kind != MENU_SCREEN_BUTTON || button->parent != bar_index || !menu_screen_when(button->when))
			continue;
		tall_text(tall, button->label, label, sizeof(label));
		/* (a button of the key before's shares its place: shown one at a
		time, by their ifs) */
		shared = previous_key && !strcmp(previous_key, button->key);
		if (touch)
		{
			width = !strcmp(button->key, "back") ? TOUCH_BACK_WIDTH : TOUCH_PROMPT_WIDTH;
			needed = width;
		}
		else
		{
			needed = (short)(TALL_PROMPT_TEXT_X + tall_prompt_end(button->key, label, FALSE) + TALL_PROMPT_RIGHT_ROOM);
			width = (short)MAX(button->width, needed);
		}
		place = shared && count ? slot_x[count - 1] : x;
		if (touch && !strcmp(button->key, "back"))
			place = (short)(640 - TOUCH_MARGIN - width);
		slot_x[count] = place;
		widths[count] = width;
		slot_width[count] = width;
		/* (with the controller's pictures, wider on the Steam Deck's 16:10
		and on: port_settings.py's prompts_wide) */
		{
			short controller = (short)(TALL_PROMPT_TEXT_X + tall_prompt_end(button->key, label, TRUE) +
				TALL_PROMPT_RIGHT_ROOM);

			grown[count] = width;
			if (!touch && controller > width)
			{
				double deck = (768.0 - 640.0) / 212.0;
				long extra = (long)((controller - width) / deck);

				if ((double)extra < (controller - width) / deck)
					extra++;
				grown[count] = (short)(width + extra);
				any_grown = TRUE;
			}
		}
		tall->prompts[count].block = button;
		snprintf(tall->prompts[count].key, sizeof(tall->prompts[count].key), "%s", button->key);
		tall->prompts[count].widget = tall_add(tall, tall->bar, tall_tag(touch ? "touch_button" : "prompt_spread"),
			place, (short)(y + (touch ? 0 : PROMPT_TOP)), 0, 0, width, 0);
		tall_box_picture(tall, tall->prompts[count].widget, touch ? "main_menu/parts/touch_button_%dx%d" :
			"main_menu/parts/item_%dx%d", width, touch ? TOUCH_TAP : TALL_PROMPT_HEIGHT);
		if (button->action)
			tall_target(tall, tall->prompts[count].widget, _target_button, NONE, button->action);
		if (!shared)
			x = (short)(x + width + (touch ? TOUCH_PROMPT_GAP : TALL_PROMPT_GAP));
		previous_key = button->key;
		count++;
	}
	tall->prompt_count = count;
	/* (the bar's focus on its first, as a list's: A on the bar presses it) */
	if (count)
		tall->bar->focused_child = tall->prompts[0].widget;
	/* (on a wide screen: each at the edge it is nearest; or laid out again
	from the edges, each as wide as the controller's pictures need) */
	for (index = 0; index < count; index++)
	{
		struct screen_box *box = tall_box_of(tall, tall->prompts[index].widget);
		boolean left = slot_x[index] + slot_width[index] / 2 < 320;

		if (!box)
			continue;
		box->centred = TRUE;
		box->dx = (short)(left ? -106 : 106);
	}
	if (any_grown)
	{
		short cursor = (short)(slot_x[0] - 106);
		short right_end = 0;
		long place;

		/* (the left ones from the left edge, the right ones from the right) */
		for (index = 0; index < count; index++)
		{
			short slot_most = 0, slot_grown = 0;
			long other;

			if (index && slot_x[index] == slot_x[index - 1])
				continue;
			if (slot_x[index] + slot_width[index] / 2 >= 320)
				continue;
			for (other = index; other < count && slot_x[other] == slot_x[index]; other++)
				slot_grown = (short)MAX(slot_grown, grown[other]);
			for (other = index; other < count && slot_x[other] == slot_x[index]; other++)
			{
				struct screen_box *box = tall_box_of(tall, tall->prompts[other].widget);

				if (box)
				{
					box->dx = (short)(cursor - slot_x[other]);
					box->dw = (short)(grown[other] - widths[other]);
				}
			}
			(void)slot_most;
			cursor = (short)(cursor + slot_grown + TALL_PROMPT_GAP);
		}
		for (index = 0; index < count; index++)
		{
			long other;
			short most = 0;

			if (slot_x[index] + slot_width[index] / 2 < 320)
				continue;
			for (other = 0; other < count; other++)
			{
				if (slot_x[other] == slot_x[index])
					most = (short)MAX(most, widths[other]);
			}
			right_end = (short)MAX(right_end, slot_x[index] + most);
		}
		cursor = (short)(right_end + 106);
		for (place = count - 1; place >= 0; place--)
		{
			short slot_grown = 0;
			long other;

			if (slot_x[place] + slot_width[place] / 2 < 320 || (place && slot_x[place] == slot_x[place - 1]))
				continue;
			for (other = place; other < count && slot_x[other] == slot_x[place]; other++)
				slot_grown = (short)MAX(slot_grown, grown[other]);
			cursor = (short)(cursor - slot_grown);
			for (other = place; other < count && slot_x[other] == slot_x[place]; other++)
			{
				struct screen_box *box = tall_box_of(tall, tall->prompts[other].widget);

				if (box)
				{
					box->dx = (short)(cursor - slot_x[other]);
					box->dw = (short)(grown[other] - widths[other]);
				}
			}
			cursor = (short)(cursor - TALL_PROMPT_GAP);
		}
	}
}

/* the screen made: the tall layout's blocks in their places */
static boolean tall_build(struct screen_open *open, struct menu_screen const *screen)
{
	struct widget_instance *widget = open->widget;
	struct screen_tall *tall = NULL;
	long index, table_index = NONE, open_index = open - screens.open;

	for (index = 0; index < NUMBEROF(tall_storage) && !tall; index++)
	{
		long other;
		boolean used = FALSE;

		for (other = 0; other < SCREEN_MAXIMUM_OPEN; other++)
			used |= other != open_index && tall_screens[other] == &tall_storage[index];
		if (!used)
			tall = &tall_storage[index];
	}
	if (!tall)
		return FALSE;
	/* (a search typed into ends with the screen it was on) */
	if (pc_menu_text_field_editing(NULL))
		pc_menu_text_field_end(TRUE);
	memset(tall, 0, sizeof(*tall));
	tall->search_editing = NONE;
	tall_screens[open_index] = tall;
	widget->focused_child = NULL;
	while (widget->child)
		ui_widget_delete(widget->child);
	open->list = NULL;
	open->item_count = 0;
	open->prompt_count = 0;
	open->header = NULL;
	open->has_spartan = FALSE;
	open->spartan_picture = NULL;
	/* (the parts that take no focus are the screen's, the rest the body's,
	in the order the focus steps through them) */
	tall->owner = tall_add(tall, widget, tall_tag("layer"), 0, 0, 0, 0, 0, 0);
	tall->body = tall_add(tall, widget, tall_tag("body"), 0, 0, 0, 0, 0, 0);
	if (!tall->owner || !tall->body)
		return FALSE;
	for (index = 0; index < screen->block_count; index++)
	{
		struct menu_screen_block const *block = &screen->blocks[index];

		if (block->parent >= 0)
			continue;
		switch (block->kind)
		{
		case MENU_SCREEN_HEADER:
		{
			char label[MENU_SCREEN_MAXIMUM_TEXT];

			open->header = screen_widget_add(widget, screens.header_text_tag, 0, 0);
			open->header_picture = NONE;
			tall_text(tall, block->label, label, sizeof(label));
			tall_text_set(open->header, label);
			tall_build_header(tall, widget, screen, index);
			break;
		}
		case MENU_SCREEN_TABS:
			if (menu_screen_when(block->when))
				tall_build_tabs(tall, widget, screen, index);
			break;
		case MENU_SCREEN_SEARCH:
			tall_build_search(tall, block, NULL);
			break;
		case MENU_SCREEN_TABLE:
			table_index = index;
			tall_build_table(tall, screen, index);
			break;
		case MENU_SCREEN_PANEL:
			tall_build_panel(tall, screen, index);
			break;
		}
	}
	/* (the panel's Spartan, drawn where its picture is) */
	if (tall->spartan && tall->spartan_picture)
	{
		open->has_spartan = TRUE;
		open->spartan = *tall->spartan;
		open->spartan_picture = tall->spartan_picture;
	}
	/* (the table's button after the panel's, the bar last) */
	if (table_index != NONE)
		tall_build_table_button(tall, screen, table_index);
	for (index = 0; index < screen->block_count; index++)
	{
		if (screen->blocks[index].kind == MENU_SCREEN_BUTTONS)
			tall_build_bar(tall, screen, index);
	}
	/* (the focus: the tabs, else the first row; the rows' source then
	moves it) */
	widget->focused_child = tall->body;
	tall->body->focused_child = tall->strip ? tall->strip : tall->rows[0].row;
	if (tall->strip && tall->tab_count)
		tall->strip->focused_child = tall->tab_widgets[0];
	if (tall->titles)
		tall->titles->focused_child = tall->titles->child;
	for (index = 0; index < tall->search_count; index++)
	{
		if (tall->searches[index].list && !tall->searches[index].list->focused_child)
			tall->searches[index].list->focused_child = tall->searches[index].icon;
	}
	snprintf(open->back, sizeof(open->back), "%s", screen->back ? screen->back : "");
	open->generation = menu_screens_generation(screen->name);
	return TRUE;
}

/* ---------- each frame */

/* a list's focus on its child (its index chosen too) */
static void tall_focus(struct widget_instance *list, struct widget_instance *child)
{
	struct widget_instance *each;
	short index = 0;

	for (each = list ? list->child : NULL; each; each = each->next, index++)
	{
		if (each == child)
		{
			list->focused_child = child;
			list->parameters.list.selected_index = index;
			return;
		}
	}
}

/* a bar's focus off a button hidden: on its first one shown */
static void tall_focus_off_hidden(struct widget_instance *bar)
{
	struct widget_instance *child;

	if (!bar || !bar->focused_child || bar->focused_child->visible)
		return;
	for (child = bar->child; child; child = child->next)
	{
		if (child->visible)
		{
			tall_focus(bar, child);
			return;
		}
	}
}

/* the boxes moved and grown for the layout now */
static void tall_place(struct screen_tall *tall, struct widget_instance *screen)
{
	long index;

	for (index = 0; index < tall->box_count; index++)
	{
		struct screen_box *box = &tall->boxes[index];
		struct screen_box const *parent = box->parent != NONE ? &tall->boxes[box->parent] : NULL;

		box->at_x = (short)(box->x + tall_wide(box->dx, box->dw != 0, box->centred));
		box->at_y = box->y;
		box->widget->horizontal_offset = (short)(box->at_x - (parent ? parent->at_x : 0));
		box->widget->vertical_offset = (short)(box->at_y - (parent ? parent->at_y : 0));
	}
}

/* a box's width now (its growth too) */
static short tall_width(struct screen_tall *tall, struct widget_instance *widget)
{
	struct screen_box *box = tall_box_of(tall, widget);
	short width;

	if (!box)
		return 0;
	width = box->width;
	if (!width)
		ui_widget_port_text_box_font(widget, &width);
	return (short)(width + tall_wide(box->dw, TRUE, box->centred));
}

static void tall_color(struct screen_tall *tall, struct widget_instance *widget, unsigned long color)
{
	struct screen_box *box = tall_box_of(tall, widget);

	if (box)
		box->color = color;
}

static void tall_utf16(char const *utf8, unsigned short *text, long size)
{
	halo_menus_utf16(utf8, text, size);
}

/* the tabs: the current one framed and white, the one with the focus
outlined (a tab chosen by a key takes the strip's focus) */
static void tall_tabs_frame(struct screen_tall *tall)
{
	char current[MENU_SCREEN_MAXIMUM_TEXT];
	short shown = NONE;
	boolean strip_focused;
	long index;

	if (!tall->strip)
		return;
	tall->strip->visible = tall_shown(tall, tall->tabs);
	visible_set_safe(tall->tab_keys[0], tall->strip->visible);
	visible_set_safe(tall->tab_keys[1], tall->strip->visible);
	tall_binding(tall, tall->tabs->value, current, sizeof(current));
	for (index = 0; index < tall->tab_count; index++)
	{
		if (tall->tab_blocks[index]->id && !strcmp(tall->tab_blocks[index]->id, current))
			shown = (short)index;
	}
	if (shown != NONE && shown != tall->tab_shown)
		tall_focus(tall->strip, tall->tab_widgets[shown]);
	tall->tab_shown = shown;
	strip_focused = tall->body->focused_child == tall->strip;
	for (index = 0; index < tall->tab_count; index++)
	{
		struct widget_instance *tab = tall->tab_widgets[index];

		tab->animation.current_frame_index = (short)((index == shown ? 2 : 0) +
			(strip_focused && tab == tall->strip->focused_child ? 1 : 0));
		tall_color(tall, tab, index == shown ? 0xFFFFFFFFUL : 0);
	}
}

/* the columns' titles: the one sorted by white, its arrow after its words
(past the end of a title whose words end at its right), its way */
static void tall_titles_frame(struct screen_tall *tall)
{
	char sorted[MENU_SCREEN_MAXIMUM_TEXT];
	long index;

	tall_binding(tall, tall->table->value, sorted, sizeof(sorted));
	for (index = 0; index < tall->column_count; index++)
	{
		struct screen_column *column = &tall->columns[index];
		size_t length = column->block->id ? strlen(column->block->id) : 0;
		boolean current = length && !strncmp(sorted, column->block->id, length) &&
			(sorted[length] == ' ' || !sorted[length]);
		boolean down = current && strstr(sorted + length, "down") != NULL;
		short x;

		if (!column->title)
			continue;
		tall_color(tall, column->title, current ? 0xFFFFFFFFUL : 0);
		visible_set_safe(column->arrow_up, current && !down && column->title->visible);
		visible_set_safe(column->arrow_down, current && down && column->title->visible);
		if (column->block->right)
			x = (short)(column->title_width - TALL_ARROW_GAP);
		else
			x = pc_menu_text_width(column->title, column->title->parameters.text_box.text ?
				(unsigned short const *)column->title->parameters.text_box.text : NULL);
		tall_box_of(tall, column->arrow_up)->x = (short)(TALL_TABLE_X + column->title_x + x);
		tall_box_of(tall, column->arrow_down)->x = (short)(TALL_TABLE_X + column->title_x + x);
	}
}

/* the searches: each its glass (or bar), or its field and X while it has
words or is typed into (the words typed so far its value's, at once; the
field left, its words kept, once the d-pad or a click, not the pointer
passing, took the focus off it) */
static struct screen_tall *tall_search_screen;

static void tall_search_done(char const *text)
{
	struct screen_tall *tall = tall_search_screen;

	if (tall && tall->search_editing != NONE)
		screen_value_set(tall->searches[tall->search_editing].block->value, text);
	if (tall)
		tall->search_editing = NONE;
}

static boolean tall_search_editing(struct screen_tall *tall)
{
	if (tall->search_editing != NONE && !pc_menu_text_field_editing(tall->searches[tall->search_editing].field))
		tall->search_editing = NONE;
	return tall->search_editing != NONE;
}

static void tall_search_end(struct screen_tall *tall)
{
	if (tall_search_editing(tall))
		pc_menu_text_field_end(TRUE);
	tall->search_editing = NONE;
}

static void tall_search_begin(struct screen_tall *tall, short index)
{
	struct screen_search *search = &tall->searches[index];
	char words[MENU_SCREEN_MAXIMUM_TEXT];

	if (!tall_shown(tall, search->block))
		return;
	tall_search_end(tall);
	if (pc_menu_text_field_editing(NULL))
		pc_menu_text_field_end(FALSE);
	search->field->visible = TRUE;
	tall_focus(search->list, search->field);
	tall_focus(tall->body, search->list);
	tall_search_screen = tall;
	tall->search_editing = index;
	tall_binding(tall, search->block->value, words, sizeof(words));
	pc_menu_text_field_begin(search->field, words, 31, tall_search_done);
	ui_play_audio_feedback_sound(TALL_SOUND_FORWARD);
}

/* a search's words cleared (left if typed into), its glass with the focus
again if its field had it */
static void tall_search_clear(struct screen_tall *tall, short index)
{
	struct screen_search *search = &tall->searches[index];

	if (tall_search_editing(tall) && tall->search_editing == index)
	{
		pc_menu_text_field_end(FALSE);
		tall->search_editing = NONE;
	}
	screen_value_set(search->block->value, "");
	if (search->list && (search->list->focused_child == search->field || search->list->focused_child == search->clear))
	{
		search->icon->visible = TRUE;
		tall_focus(search->list, search->icon);
	}
}

static void tall_searches_frame(struct screen_tall *tall)
{
	long index;

	if (tall_search_editing(tall))
	{
		struct screen_search *search = &tall->searches[tall->search_editing];

		if (!ui_widget_port_hover_focused() &&
			(tall->body->focused_child != search->list || search->list->focused_child != search->field))
		{
			tall_search_end(tall);
		}
	}
	for (index = 0; index < tall->search_count; index++)
	{
		struct screen_search *search = &tall->searches[index];
		char words[MENU_SCREEN_MAXIMUM_TEXT];
		boolean editing = tall->search_editing == index;
		boolean shown;

		boolean usable = tall_shown(tall, search->block);

		tall_binding(tall, search->block->value, words, sizeof(words));
		shown = usable && (editing || words[0]);
		visible_set_safe(search->title, !shown);
		visible_set_safe(search->icon, usable && !shown);
		if (search->list && search->list != tall->titles)
			search->list->visible = usable;
		/* (a touchscreen's bar, lit while the focus is on it) */
		if (search->icon && search->icon->animation.number_of_sprite_frames > 2)
			search->icon->animation.current_frame_index = (short)(tall->body->focused_child == search->list &&
				search->list->focused_child == search->icon);
		visible_set_safe(search->field, shown);
		visible_set_safe(search->clear, shown);
		if (!search->field || !shown)
			continue;
		pc_menu_text_field_show(search->field, words, editing);
		/* (the words typed so far: its value's, filtering as each is typed) */
		if (editing && tall_search_editing(tall))
			screen_value_set(search->block->value, pc_menu_text_field_words());
		/* (words longer than the field: their end shown) */
		{
			wchar_t *text = search->field->parameters.text_box.text;
			short width = (short)(search->field_width_base + tall_wide(search->field_grow, TRUE, FALSE) - 8);

			while (text && text[0] && text[1] && pc_menu_text_width(search->field, (unsigned short const *)text) > width)
				{
				long length = 0;

				while (text[length])
					length++;
				memmove(text, text + 1, length * sizeof(wchar_t));
			}
			search->field->animation.current_frame_index = (short)editing;
		}
	}
	tall_focus_off_hidden(tall->titles);
	for (index = 0; index < tall->search_count; index++)
		tall_focus_off_hidden(tall->searches[index].list);
}

/* the rows: the source's, from its first, each its cells or a heading;
each row's picture normal, focused or chosen */
static void tall_rows_frame(struct screen_tall *tall, struct widget_instance *screen)
{
	struct menu_table_view *view = &tall->view;
	boolean touch = ui_widget_port_touch_menus() != 0;
	long index, column_index;
	short focused = NONE;

	for (index = 0; index < tall->row_count; index++)
	{
		if (tall->rows[index].row && tall->body->focused_child == tall->rows[index].row)
			focused = (short)index;
	}
	memset(view, 0, sizeof(*view));
	view->screen = screen;
	view->shown = tall->row_count;
	view->focused = focused;
	view->hover = ui_widget_port_hover_focused();
	view->titles = tall->titles && tall->titles->visible;
	view->chosen = NONE;
	view->focus = NONE;
	if (tall->source)
		tall->source->update(view);
	if (view->focus != NONE && view->focus >= 0 && view->focus < tall->row_count && tall->rows[view->focus].row)
	{
		tall->rows[view->focus].row->visible = TRUE;
		tall_focus(tall->body, tall->rows[view->focus].row);
	}
	if (view->focus_titles && tall->titles && tall->titles->visible)
		tall_focus(tall->body, tall->titles);
	for (index = 0; index < tall->row_count; index++)
	{
		struct screen_row_widgets *row = &tall->rows[index];
		short row_index = (short)(view->first + index);
		unsigned short words[MENU_TABLE_TEXT_LENGTH];
		boolean heading;
		boolean row_focused = tall->body->focused_child == row->row;

		if (!row->row)
			continue;
		row->row->visible = row_index < view->count;
		if (!row->row->visible)
			continue;
		heading = tall->source && tall->source->heading(row_index, words, MENU_TABLE_TEXT_LENGTH);
		visible_set_safe(row->heading, heading);
		row->row->animation.current_frame_index = (short)(heading ? 0 : row_index == view->chosen ? 2 :
			row_focused ? 1 : 0);
		if (heading)
		{
			pc_menu_text_fit(row->heading, words, tall_width(tall, row->heading));
			tall_color(tall, row->heading, 0xFFFFFFFFUL);
		}
		for (column_index = 0; column_index < tall->column_count; column_index++)
		{
			struct screen_column const *column = &tall->columns[column_index];
			struct menu_table_cell cell;
			short width, drawn, room = 0, x;
			struct screen_box *box;

			if (heading)
			{
				visible_set_safe(row->text[column_index], FALSE);
				visible_set_safe(row->badge[column_index], FALSE);
				visible_set_safe(row->lock[column_index], FALSE);
				visible_set_safe(row->picture[column_index], FALSE);
				continue;
			}
			memset(&cell, 0, sizeof(cell));
			cell.frame = NONE;
			if (tall->source)
				tall->source->cell(row_index, column->block->id, row_focused, &cell);
			if (row->picture[column_index])
			{
				row->picture[column_index]->visible = cell.frame != NONE;
				row->picture[column_index]->animation.current_frame_index = (short)MAX(cell.frame, 0);
			}
			if (!row->text[column_index])
				continue;
			row->text[column_index]->visible = TRUE;
			width = tall_width(tall, row->text[column_index]);
			/* (room after the words for its pill and its lock) */
			if (row->badge[column_index] && cell.badge[0])
				room = (short)(TALL_BADGE_WIDTH + TALL_BADGE_GAP);
			if (cell.lock)
				room = (short)(room + TALL_LOCK_WIDTH + TALL_BADGE_GAP);
			drawn = pc_menu_text_fit(row->text[column_index], cell.text, (short)(width - room));
			tall_color(tall, row->text[column_index], cell.color ? cell.color :
				column->block->cell == MENU_SCREEN_CELL_STRONG ? 0xFFFFFFFFUL : 0);
			box = tall_box_of(tall, row->text[column_index]);
			x = (short)(box->x + drawn + TALL_BADGE_GAP);
			if (row->badge[column_index])
			{
				char badge[MENU_TABLE_BADGE_LENGTH * 3];
				long letter;

				for (letter = 0; letter < MENU_TABLE_BADGE_LENGTH - 1 && cell.badge[letter]; letter++)
					badge[letter] = (char)cell.badge[letter];
				badge[letter] = 0;
				row->badge[column_index]->visible = room && cell.badge[0];
				tall_text_set(row->badge[column_index], badge);
				tall_box_of(tall, row->badge[column_index])->x = x;
				row->badge[column_index]->animation.current_frame_index = 0;
				if (cell.badge[0])
					x = (short)(x + TALL_BADGE_WIDTH + TALL_BADGE_GAP);
			}
			if (row->lock[column_index])
			{
				row->lock[column_index]->visible = cell.lock != 0;
				tall_box_of(tall, row->lock[column_index])->x = (short)(x - 2);
				row->lock[column_index]->animation.current_frame_index = 1;
			}
			/* (a ping's bars before its number, if both fit) */
			if (column->block->cell == MENU_SCREEN_CELL_PING && row->picture[column_index])
				row->picture[column_index]->visible = cell.frame != NONE &&
					drawn + TALL_BARS_GAP + TALL_BARS_WIDTH <= width;
		}
		(void)touch;
	}
	/* (the words for no rows) */
	if (tall->empty)
	{
		char text[MENU_SCREEN_MAXIMUM_TEXT * 2];
		unsigned short wide_text[MENU_SCREEN_MAXIMUM_TEXT * 2];

		tall->empty->visible = view->count == 0;
		if (!view->count)
		{
			tall_text(tall, tall->table->empty, text, sizeof(text));
			tall_utf16(text, wide_text, NUMBEROF(wide_text));
			pc_menu_text_wrap(tall->empty, wide_text, tall_width(tall, tall->empty), 3);
		}
	}
	/* (its button: shown as its if says, the focus on it as it comes, as
	there is no row, but for the titles' while a search is typed into) */
	if (tall->table_bar)
	{
		boolean shown = tall_shown(tall, tall->table_button.block);
		struct widget_instance *bar = tall->table_bar;

		if (shown && !bar->visible && tall->body->focused_child != tall->titles &&
			!(tall->search_count && tall->body->focused_child == tall->searches[0].list))
			tall_focus(tall->body, bar);
		bar->visible = shown;
		bar->disabled = !shown;
		if (!shown && tall->body->focused_child == bar)
			tall->body->focused_child = NULL;
		if (bar->child)
			bar->child->animation.current_frame_index = (short)(tall->body->focused_child == bar);
	}
}

/* the panel's lines: each its binding's words (a heading's and a text's
wrapped to their lines), shown while they have some */
static void tall_panel_frame(struct screen_tall *tall)
{
	boolean touch = ui_widget_port_touch_menus() != 0;
	long index;

	for (index = 0; index < tall->line_count; index++)
	{
		struct screen_line *line = &tall->lines[index];
		struct menu_screen_block const *block = line->block;
		char text[MENU_SCREEN_MAXIMUM_TEXT * 2];
		unsigned short wide_text[MENU_SCREEN_MAXIMUM_TEXT * 2];
		boolean shown;

		if (!line->widget)
			continue;
		shown = tall_text(tall, block->kind == MENU_SCREEN_PAIR ? block->hints[0] : block->label, text, sizeof(text)) &&
			text[0];
		line->widget->visible = shown;
		if (line->label)
			line->label->visible = shown;
		if (!shown)
			continue;
		tall_utf16(text, wide_text, NUMBEROF(wide_text));
		switch (block->kind)
		{
		case MENU_SCREEN_PANEL_HEADING:
		case MENU_SCREEN_PANEL_TEXT:
			pc_menu_text_wrap(line->widget, wide_text, tall_width(tall, line->widget),
				touch ? block->touch_lines : block->lines);
			break;
		case MENU_SCREEN_PILL:
		{
			char tone[16];

			tall_text_set(line->widget, text);
			line->widget->animation.current_frame_index = 0;
			if (block->tone && block->tone[0] >= '0' && block->tone[0] <= '9')
				line->widget->animation.current_frame_index = (short)atoi(block->tone);
			else if (block->tone)
				line->widget->animation.current_frame_index = (short)atoi(tall_binding(tall, block->tone, tone,
					sizeof(tone)));
			break;
		}
		default:
			tall_text_set(line->widget, text);
			break;
		}
	}
	if (tall->panel_bar)
	{
		boolean shown = tall_shown(tall, tall->panel_button.block);

		tall->panel_bar->visible = shown;
		tall->panel_bar->disabled = !shown;
	}
}

/* the bar's prompts: shown as their ifs say (their words again, which may
have bindings) */
static void tall_bar_frame(struct screen_tall *tall)
{
	boolean touch = ui_widget_port_touch_menus() != 0;
	long index;

	for (index = 0; index < tall->prompt_count; index++)
	{
		struct screen_prompt *prompt = &tall->prompts[index];
		char label[MENU_SCREEN_MAXIMUM_TEXT];
		char text[MENU_SCREEN_MAXIMUM_TEXT];

		if (!prompt->widget)
			continue;
		prompt->widget->visible = tall_shown(tall, prompt->block);
		tall_text(tall, prompt->block->label, label, sizeof(label));
		snprintf(text, sizeof(text), touch ? "%s%s" : "%s %s", touch ? "" : tall_key_picture(prompt->key), label);
		tall_text_set(prompt->widget, text);
	}
	tall_focus_off_hidden(tall->bar);
	if (tall->header_button.widget)
	{
		char label[MENU_SCREEN_MAXIMUM_TEXT];
		char text[MENU_SCREEN_MAXIMUM_TEXT];

		tall->header_button.widget->visible = tall_shown(tall, tall->header_button.block);
		tall_text(tall, tall->header_button.block->label, label, sizeof(label));
		snprintf(text, sizeof(text), "%s %s", tall_key_picture(tall->header_button.key), label);
		tall_text_set(tall->header_button.widget, text);
	}
}

static void tall_frame(struct screen_open *open)
{
	struct screen_tall *tall = tall_of(open);
	struct widget_instance *screen = open->widget;
	struct widget_instance *focused;

	if (!tall)
		return;
	tall_place(tall, screen);
	if (tall->detail)
	{
		char text[MENU_SCREEN_MAXIMUM_TEXT];

		tall_text(tall, tall->header->empty, text, sizeof(text));
		tall_text_set(tall->detail, text);
	}
	if (open->header)
	{
		char text[MENU_SCREEN_MAXIMUM_TEXT];

		tall_text(tall, tall->header->label, text, sizeof(text));
		tall_text_set(open->header, text);
		open->header->horizontal_offset = (short)(screen->horizontal_offset - ui_widget_port_past_move() / 2);
	}
	tall_tabs_frame(tall);
	tall_searches_frame(tall);
	if (tall->table)
	{
		tall_titles_frame(tall);
		tall_rows_frame(tall, screen);
	}
	tall_panel_frame(tall);
	tall_bar_frame(tall);
	/* (the band's button: the focus back to the body once the pointer is
	not what gave the button it, as the browser's HOST over its generated
	screen) */
	if (screen->focused_child != tall->body && !ui_widget_port_pointer_focused())
		screen->focused_child = tall->body;
	/* (the focus off what cannot have it: the first row, else the bar) */
	focused = tall->body->focused_child;
	if (!focused || !focused->visible || focused->disabled)
	{
		if (tall->rows[0].row && tall->rows[0].row->visible)
			tall_focus(tall->body, tall->rows[0].row);
		else if (tall->bar)
			tall_focus(tall->body, tall->bar);
	}
	tall_place(tall, screen);
	/* (the panel's Spartan: in the middle of the panel as wide as it is now,
	at its line's foot, in its color now) */
	if (open->has_spartan && tall->spartan_picture && tall->panel_widget)
	{
		struct screen_box *panel = tall_box_of(tall, tall->panel_widget);
		struct screen_box *picture = tall_box_of(tall, tall->spartan_picture);

		if (panel && picture)
		{
			picture->at_x = (short)(panel->at_x + (tall_width(tall, tall->panel_widget) - tall->spartan_width) / 2);
			tall->spartan_picture->horizontal_offset = (short)(picture->at_x -
				(picture->parent != NONE ? tall->boxes[picture->parent].at_x : 0));
			open->spartan.box_x = picture->at_x;
			open->spartan.box_y = picture->at_y;
			open->spartan.box_width = tall->spartan_width;
			open->spartan.box_height = tall->spartan->box_height;
			screen_spartan_frame(open, NONE);
		}
	}
}

/* ---------- events */

/* a target's action run: a row's (the table's do, or a cell's of a column
with its own do, clicked), a title's (its sort), a tab's, a button's; a
search's part begun, ended or cleared */
static boolean tall_do(struct screen_open *open, struct widget_instance *widget, struct event_record *event,
	boolean *deleted)
{
	struct screen_tall *tall = tall_of(open);
	long index;

	*deleted = FALSE;
	for (index = 0; tall && index < tall->target_count; index++)
	{
		struct screen_target const *target = &tall->targets[index];
		boolean result;

		if (target->widget != widget)
			continue;
		switch (target->kind)
		{
		case _target_search_icon:
		case _target_search_field:
			/* (typed into, or if it is, left with its words; a touchscreen's
			typed into again, its keyboard back) */
			if (target->kind == _target_search_field && tall_search_editing(tall) &&
				tall->search_editing == target->index && !ui_widget_port_touch_menus())
			{
				tall_search_end(tall);
				return TRUE;
			}
			tall_search_begin(tall, target->index);
			return TRUE;
		case _target_search_clear:
			tall_search_clear(tall, target->index);
			ui_play_audio_feedback_sound(TALL_SOUND_BACK);
			return TRUE;
		case _target_row:
		{
			long column_index;
			short x, y;

			/* (a click on a cell of a column of its own do: that) */
			for (column_index = 0; column_index < tall->column_count; column_index++)
			{
				struct widget_instance *cell = tall->rows[target->index].picture[column_index];

				if (!tall->columns[column_index].block->action || !cell || !cell->visible)
					continue;
				if (ui_widget_port_take_click(&x, &y))
				{
					short left = tall_box_of(tall, cell)->at_x;

					if (x >= left - 4 && x < left + 20)
					{
						screen_action_row = target->index;
						result = screen_action_run(widget, event, tall->columns[column_index].block->action, deleted);
						screen_action_row = NONE;
						return result;
					}
				}
				break;
			}
			if (!target->action)
				return FALSE;
			screen_action_row = target->index;
			result = screen_action_run(widget, event, target->action, deleted);
			screen_action_row = NONE;
			return result;
		}
		default:
			return target->action && screen_action_run(widget, event, target->action, deleted);
		}
	}
	return FALSE;
}

/* a key pressed on the screen: a button's of its own do (the bar's, the
band's, a panel's or a table's: the key whose button is shown), LB and RB
the tabs either side (their do), B while a search is typed into its words
cleared; FALSE for what the screen leaves to its widgets */
static boolean tall_key(struct screen_open *open, struct widget_instance *screen, struct event_record *event,
	boolean *deleted)
{
	struct screen_tall *tall = tall_of(open);
	short button;
	long index;

	*deleted = FALSE;
	if (!tall || event->type != SCREEN_EVENT_BUTTON || event->data.button.value != 1)
		return FALSE;
	button = event->data.button.index;
	if (tall_search_editing(tall) && (button == _gamepad_analog_button_b || button == _gamepad_binary_button_back))
	{
		tall_search_clear(tall, tall->search_editing);
		ui_play_audio_feedback_sound(TALL_SOUND_BACK);
		return TRUE;
	}
	if (tall->strip && tall->strip->visible && tall->tab_count &&
		(button == _gamepad_analog_button_white || button == _gamepad_analog_button_black))
	{
		short step = button == _gamepad_analog_button_black ? 1 : -1;
		short at = (short)((tall->tab_shown == NONE ? 0 : tall->tab_shown) + step);

		at = (short)((at + tall->tab_count) % tall->tab_count);
		return screen_action_run(screen, event, tall->tab_blocks[at]->action, deleted);
	}
	{
		struct screen_prompt *prompts[TALL_MAXIMUM_PROMPTS + 3];
		long count = 0;

		for (index = 0; index < tall->prompt_count; index++)
			prompts[count++] = &tall->prompts[index];
		prompts[count++] = &tall->header_button;
		prompts[count++] = &tall->panel_button;
		prompts[count++] = &tall->table_button;
		for (index = 0; index < count; index++)
		{
			struct screen_prompt *prompt = prompts[index];

			if (!prompt->widget || !prompt->block || !prompt->block->action || !prompt->widget->visible ||
				(prompt->widget->parent && !prompt->widget->parent->visible) ||
				tall_key_button(prompt->key) != button || !strcmp(prompt->key, "accept"))
			{
				continue;
			}
			return screen_action_run(screen, event, prompt->block->action, deleted);
		}
	}
	return FALSE;
}

/* a prompt of the bar chosen (A, a click or a tap on it): its do, else its
key pressed */
static boolean tall_prompt(struct screen_open *open, struct widget_instance *widget, struct event_record *event,
	boolean *deleted, short controller)
{
	struct screen_tall *tall = tall_of(open);
	long index;

	*deleted = FALSE;
	for (index = 0; tall && index < tall->prompt_count; index++)
	{
		struct screen_prompt *prompt = &tall->prompts[index];

		if (prompt->widget != widget)
			continue;
		if (prompt->block->action)
			return screen_action_run(widget, event, prompt->block->action, deleted);
		if (!strcmp(prompt->key, "accept"))
		{
			struct widget_instance *focused = tall->body->focused_child;

			return focused && focused != tall->bar && tall_do(open, focused, event, deleted);
		}
		event_manager_post_button(controller, tall_key_button(prompt->key));
		return TRUE;
	}
	return FALSE;
}

/* "search next" (the next of the screen's searches typed into, from the
first) and "search clear" (every one's words cleared, the focus on the
first row) */
static boolean screen_call_search(struct widget_instance *widget, struct event_record *event, char const *name,
	boolean *deleted)
{
	struct screen_open *open = screen_open_of(widget_instance_get_topmost_parent(widget));
	struct screen_tall *tall = open ? tall_of(open) : NULL;
	short index;

	(void)event;
	*deleted = FALSE;
	if (!tall || !tall->search_count)
		return FALSE;
	if (!strcmp(name, "search next"))
	{
		short next = tall_search_editing(tall) ? (short)((tall->search_editing + 1) % tall->search_count) : 0;

		tall_search_begin(tall, next);
		return TRUE;
	}
	for (index = 0; index < tall->search_count; index++)
		tall_search_clear(tall, index);
	ui_play_audio_feedback_sound(TALL_SOUND_BACK);
	return TRUE;
}

/* the hooks' boxes: a block's width and picture where its screen says */
static boolean tall_picture_of(struct widget_instance const *widget, long *bitmap_tag, rectangle2d *bounds)
{
	long open_index;

	for (open_index = 0; open_index < SCREEN_MAXIMUM_OPEN; open_index++)
	{
		struct screen_tall *tall = tall_screens[open_index];
		struct screen_box const *box;

		if (!tall || !screens.open[open_index].widget)
			continue;
		box = tall_box_of(tall, widget);
		if (!box)
			continue;
		*bitmap_tag = box->picture;
		if (box->width)
			bounds->x1 = (short)(bounds->x0 + box->width);
		bounds->x1 = (short)(bounds->x1 + tall_wide(box->dw, TRUE, box->centred));
		if (box->height)
			bounds->y1 = (short)(bounds->y0 + box->height);
		return box->width || box->height || box->dw || box->picture != NONE;
	}
	return FALSE;
}

static boolean tall_color_of(struct widget_instance const *widget, real_argb_color *color)
{
	long open_index;

	for (open_index = 0; open_index < SCREEN_MAXIMUM_OPEN; open_index++)
	{
		struct screen_tall *tall = tall_screens[open_index];
		struct screen_box const *box;

		if (!tall || !screens.open[open_index].widget)
			continue;
		box = tall_box_of(tall, widget);
		if (!box || !box->color)
			continue;
		color->red = (real)((box->color >> 24) & 0xFF) / 255.0f;
		color->green = (real)((box->color >> 16) & 0xFF) / 255.0f;
		color->blue = (real)((box->color >> 8) & 0xFF) / 255.0f;
		return TRUE;
	}
	return FALSE;
}
