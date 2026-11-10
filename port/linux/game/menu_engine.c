/*
MENU_ENGINE.C

The port's menu engine (docs/menu_engine.md), under display.menu_engine
"new": the generated screens of the PC menus (port/assets/menus, the widgets
menu_tags.c builds) run from the menus' model (port/assets/menus/model,
halo_menus.h), the in-game pause menu and the Xbox's menus (display.menus
"xbox") still the game's.

A screen is one widget of the game's with no parent, its stand-in, which
ui_widget.c opens, keeps in its history and closes as its own (its hooks:
menu_engine_screen_opened and _closed). What is in it is the engine's: it
lays it out and draws it (menu_engine_screen_draw), each widget with the
game's own drawing (ui_widget_port_render_node: its picture, text, spinner
or list, as ui_widget.c draws them, so the look is the same), and takes its
events (menu_engine_screen_event). The widgets are the game's struct
widget_instance (menu_widget.h), so that the menus' functions
(menu_functions.c), which read and write them by name, run alike on both.

Each widget is drawn from its look in the model (model/look: its box, wide
layout, places, pictures, text and its style, and where its children are),
not its definition: ui_widget.c finds every widget's definition through
menu_engine_definition, which gives it the one menu_tags.c made from the
model (pc_menu_look_definition); what the widget does stays its own
definition's. From the model the engine also takes what the widgets cannot
say: a settings screen's rows and sections, their order and the geometry
they are laid out by (menu_engine_settings_layout, in place of
settings_rows_pack's packing), each row's help and note
(menu_engine_row_help, in place of the help found by its label's index +
1).
*/

#include "cseries.h"
#include "interface/ui_widget.h"
#include "interface/ui_widget_definitions.h"
#include "tag_files/tag_groups.h"

#include "halo_menus.h"
#include "menu_engine.h"
#include "menu_widget.h"

#include <stdio.h>
#include <string.h>

char const *config_string(char const *name);
unsigned long config_changes(void);
void *pc_menu_look_definition(long tag_index);
char *tag_get_name(long tag_index);
boolean pc_menu_tag(long tag_index);
void platform_log(char const *format, ...);

/* ui_widget.c's widget types (its enum) */
#define ENGINE_WIDGET_SPINNER 2
#define ENGINE_WIDGET_COLUMN_LIST 3

/* the screens open (one for each player's stack, and room for the screen
being opened as the one it replaces closes) */
#define ENGINE_MAXIMUM_SCREENS 8

struct engine_screen
{
	struct widget_instance *widget;
	/* its model (NULL: a screen the model has nothing for, drawn and run as
	its widgets are) */
	struct halo_menu_model_screen const *model;
};

static struct
{
	struct engine_screen screens[ENGINE_MAXIMUM_SCREENS];
	/* the screen the engine is drawing or processing an event of now (its
	stand-in's own drawing and events are ui_widget.c's) */
	struct widget_instance *drawing, *processing;
	struct halo_menu_model const *model;
} engine;

/* ---------- the model */

static boolean engine_on(void)
{
	return !strcmp(config_string("display.menu_engine"), "new");
}

/* engine_on, read again only when a setting changes: every widget's
definition asks it (menu_engine_definition) */
boolean menu_engine_looks(void)
{
	static unsigned long read_at;
	static boolean read, on;

	if (!read || read_at != config_changes())
	{
		read = TRUE;
		read_at = config_changes();
		on = engine_on();
	}
	return on;
}

void *menu_engine_definition(long tag_index)
{
	void *look = tag_index != NONE && menu_engine_looks() ? pc_menu_look_definition(tag_index) : NULL;

	return look ? look : tag_get(UI_WIDGET_DEFINITION_TAG, tag_index);
}

/* a widget's name as the menus' files have it: its tag's, without "pc\" and
with slashes */
static boolean engine_widget_name(long tag_index, char *name, size_t size)
{
	char const *tag = tag_index != NONE ? tag_get_name(tag_index) : NULL;
	size_t index;

	if (!tag || strncmp(tag, "pc\\", 3))
		return FALSE;
	for (index = 0; tag[index + 3] && index + 1 < size; index++)
		name[index] = tag[index + 3] == '\\' ? '/' : tag[index + 3];
	name[index] = 0;
	return TRUE;
}

static struct halo_menu_model_screen const *engine_model_screen(long tag_index)
{
	char name[256];
	long index;

	if (!engine.model)
		engine.model = halo_menus_model();
	if (!engine.model || !engine_widget_name(tag_index, name, sizeof(name)))
		return NULL;
	for (index = 0; index < engine.model->screen_count; index++)
	{
		if (!strcmp(engine.model->screens[index].name, name))
			return &engine.model->screens[index];
	}
	return NULL;
}

static struct engine_screen *engine_screen_of(struct widget_instance const *widget)
{
	short index;

	for (index = 0; widget && index < ENGINE_MAXIMUM_SCREENS; index++)
	{
		if (engine.screens[index].widget == widget)
			return &engine.screens[index];
	}
	return NULL;
}

/* a row of the screen's model by its key (NULL: none) */
static struct halo_menu_model_row const *engine_model_row(struct halo_menu_model_screen const *screen,
	char const *key)
{
	long index;

	for (index = 0; screen && key && index < screen->row_count; index++)
	{
		struct halo_menu_model_row const *row = &engine.model->rows[screen->first_row + index];

		if (!strcmp(row->key, key))
			return row;
	}
	return NULL;
}

/* ---------- laying out */

/* the child of the list named so (NULL: none, as when its platform has it
not) */
static struct widget_instance *engine_child(struct widget_instance *list, char const *prefix, char const *key)
{
	struct widget_instance *child;
	size_t length = strlen(prefix);

	for (child = list->child; child; child = child->next)
	{
		if (child->name && !strncmp(child->name, prefix, length) && !strcmp(child->name + length, key))
			return child;
	}
	return NULL;
}

/* a settings tab's or page's list (its options_menu) laid out from the model:
its rows shown, from the top, a step apart, each section's title over them
(hidden with none shown, a gap above it after rows), as
tools/port_settings.py lays them out for every row; after the list's own
function (settings_page_update) has chosen the rows shown */
static void engine_settings_layout(struct engine_screen const *screen, struct widget_instance *list)
{
	struct halo_menu_model_geometry const *geometry = &engine.model->geometry;
	struct halo_menu_model_screen const *model = screen->model;
	short y = (short)(!strcmp(model->layout, "tab") ? geometry->tab_top : geometry->page_top);
	struct widget_instance *section = NULL;
	boolean rows_above = FALSE;
	boolean section_shown = FALSE;
	short section_y = 0;
	long index;

	for (index = 0; index < model->row_count; index++)
	{
		struct halo_menu_model_row const *row = &engine.model->rows[model->first_row + index];
		struct widget_instance *widget;

		if (!strcmp(row->kind, "button"))
			continue;
		if (!strcmp(row->kind, "section"))
		{
			if (section)
				section->visible = section_shown;
			section = engine_child(list, "", row->key);
			section_shown = FALSE;
			if (section)
			{
				/* (a title the list passes over) */
				section->disabled = TRUE;
				section->visible = FALSE;
				section_y = (short)(y + (rows_above ? geometry->section_gap : 0));
			}
			continue;
		}
		widget = engine_child(list, "op_", row->key);
		if (!widget || !widget->visible)
			continue;
		if (section && !section_shown)
		{
			/* (the section's title, over its first row shown) */
			section_shown = TRUE;
			section->vertical_offset = (short)(list->vertical_offset + section_y);
			y = (short)(section_y + geometry->section_step);
		}
		widget->vertical_offset = (short)(list->vertical_offset + y);
		y = (short)(y + geometry->row_step);
		rows_above = TRUE;
	}
	if (section)
		section->visible = section_shown;
}

/* whether the model lays out what is in the widget: a settings tab's or
page's list of rows */
static boolean engine_lays_out(struct engine_screen const *screen, struct widget_instance const *widget)
{
	return screen && screen->model && !strcmp(screen->model->kind, "settings") && screen->model->layout &&
		strcmp(screen->model->layout, "options") && widget->name && !strcmp(widget->name, "options_menu");
}

/* after a widget's own drawing (its game data inputs run): what the model
lays out in it */
static void engine_layout(struct engine_screen const *screen, struct widget_instance *widget)
{
	if (engine_lays_out(screen, widget))
		engine_settings_layout(screen, widget);
}

/* ---------- drawing */

/* a widget and what is in it, as ui_widget.c's render does: the widget by
the game's drawing, then each child from where its parent puts them, the
focused one drawn focused (and with the plasma, in a list) */
static void engine_draw(struct engine_screen const *screen, struct widget_instance *widget, rectangle2d *clip,
	point2d offset, boolean focus, boolean nifty)
{
	struct widget_instance *child;
	point2d children_offset;
	boolean children;

	if (!ui_widget_port_render_node(widget, clip, offset, focus, nifty, &children_offset, &children))
		return;
	engine_layout(screen, widget);
	if (!children)
		return;
	for (child = widget->child; child; child = child->next)
	{
		boolean child_focus = child == widget->focused_child;

		engine_draw(screen, child, clip, children_offset, child_focus, child_focus &&
			(widget->type == ENGINE_WIDGET_SPINNER || widget->type == ENGINE_WIDGET_COLUMN_LIST));
	}
}

/* ---------- ui_widget.c's calls */

void menu_engine_screen_opened(struct widget_instance *screen)
{
	short index;

	if (!engine_on() || !pc_menu_tag(screen->definition_tag_index) || engine_screen_of(screen))
		return;
	for (index = 0; index < ENGINE_MAXIMUM_SCREENS; index++)
	{
		if (!engine.screens[index].widget)
		{
			engine.screens[index].widget = screen;
			engine.screens[index].model = engine_model_screen(screen->definition_tag_index);
			platform_log("menu engine: %s%s", screen->name ? screen->name : "?",
				engine.screens[index].model ? " (its model's)" : "");
			return;
		}
	}
	platform_log("menu engine: more than %d screens open; %s is the game's", ENGINE_MAXIMUM_SCREENS,
		screen->name ? screen->name : "?");
}

void menu_engine_screen_closed(struct widget_instance *screen)
{
	struct engine_screen *owned = engine_screen_of(screen);

	if (owned)
		memset(owned, 0, sizeof(*owned));
}

boolean menu_engine_screen_draw(struct widget_instance *screen, rectangle2d *clip, point2d offset, boolean focus,
	boolean nifty)
{
	struct engine_screen *owned = engine_screen_of(screen);
	struct widget_instance *previous = engine.drawing;

	if (!owned || engine.drawing == screen)
		return FALSE;
	engine.drawing = screen;
	engine_draw(owned, screen, clip, offset, focus, nifty);
	engine.drawing = previous;
	return TRUE;
}

boolean menu_engine_screen_event(struct widget_instance *screen, struct event_record *event, boolean *deleted)
{
	struct engine_screen *owned = engine_screen_of(screen);
	struct widget_instance *previous = engine.processing;

	if (!owned || engine.processing == screen)
		return FALSE;
	engine.processing = screen;
	*deleted = FALSE;
	ui_widget_port_process_event(screen, event, deleted);
	engine.processing = previous;
	return TRUE;
}

/* ---------- the menus' functions' (menu_functions.c) */

/* a settings row's help and its note after a "|" (as the menus' help
strings have them), from the model, in UTF-16 (*found: the engine runs the
row's screen and the model has the row; NULL if it has no help, or its
label is the game's to write, which has no help string either) */
wchar_t const *menu_engine_row_help(struct widget_instance *row, boolean *found)
{
	static wchar_t help[1024];
	struct widget_instance *screen = row;
	struct engine_screen *owned;
	struct halo_menu_model_row const *model_row;
	char text[2048];

	*found = FALSE;
	while (screen && screen->parent)
		screen = screen->parent;
	owned = engine_screen_of(screen);
	if (!owned || !owned->model || !row->name || strncmp(row->name, "op_", 3))
		return NULL;
	model_row = engine_model_row(owned->model, row->name + 3);
	if (!model_row)
		return NULL;
	*found = TRUE;
	if (model_row->named)
		return NULL;
	snprintf(text, sizeof(text), "%s%s%s", model_row->help ? model_row->help : "", model_row->note ? "|" : "",
		model_row->note ? model_row->note : "");
	halo_menus_utf16(text, (unsigned short *)help, NUMBEROF(help));
	return help;
}

boolean menu_engine_lays_out(struct widget_instance *widget)
{
	struct widget_instance *screen = widget;

	while (screen && screen->parent)
		screen = screen->parent;
	return engine_lays_out(engine_screen_of(screen), widget);
}
