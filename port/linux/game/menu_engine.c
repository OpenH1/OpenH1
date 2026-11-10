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
model (pc_menu_look_definition). From the model the engine also takes what
the widgets cannot say: a settings screen's rows and sections, their order
and the geometry they are laid out by (menu_engine_settings_layout, in place
of settings_rows_pack's packing), each row's help and note
(menu_engine_row_help, in place of the help found by its label's index + 1);
and how each widget takes its events and what they do, its behaviour
(model/behaviour: its flags and its <on> handlers, which menu_tags.c makes
the game's), in place of its definition's.
*/

#include "cseries.h"
#include "input/input.h"
#include "interface/event_manager.h"
#include "interface/ui_widget.h"
#include "interface/ui_widget_definitions.h"
#include "networking/network_connection.h"
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
void main_goto_main_menu(void);
/* menu_tags.c's: the model's widgets' behaviours made the game's */
long menu_tags_behaviour_flags(struct halo_menu_model_behaviour const *behaviour);
void const *menu_tags_behaviour_handlers(struct halo_menu_model const *model,
	struct halo_menu_model_behaviour const *behaviour, long *count);
short menu_tags_handler_event(void const *handlers, long index);
void *menu_tags_handler(void const *handlers, long index);

/* ui_widget.c's widget types (its enum) */
#define ENGINE_WIDGET_SPINNER 2
#define ENGINE_WIDGET_COLUMN_LIST 3

/* the event manager's kinds of event (ui_widget.c's enum of them) */
#define ENGINE_EVENT_LEFT_STICK 1
#define ENGINE_EVENT_RIGHT_STICK 2
#define ENGINE_EVENT_BUTTON 3

/* the events a widget's handlers are for past the buttons: the sticks pushed
(ui_widget.c's _widget_event_left_stick_up, ...) */
#define ENGINE_HANDLER_LEFT_STICK_UP NUMBER_OF_GAMEPAD_BUTTONS
#define ENGINE_HANDLER_RIGHT_STICK_UP (NUMBER_OF_GAMEPAD_BUTTONS + 4)

/* a widget definition's flags' bits that its events are taken by
(ui_widget.c's _widget_..._bit) */
#define ENGINE_PASS_UNHANDLED_TO_FOCUSED_CHILD_BIT 0
#define ENGINE_UP_DOWN_TABS_CHILDREN_BIT 3
#define ENGINE_LEFT_RIGHT_TABS_CHILDREN_BIT 4
#define ENGINE_UP_DOWN_TABS_ITEMS_BIT 5
#define ENGINE_LEFT_RIGHT_TABS_ITEMS_BIT 6
#define ENGINE_NO_FOCUSED_CHILD_BIT 7
#define ENGINE_PASS_UNHANDLED_TO_ALL_CHILDREN_BIT 8
#define ENGINE_PASS_HANDLED_TO_ALL_CHILDREN_BIT 10
#define ENGINE_RETURN_TO_MAIN_MENU_BIT 11

/* the sounds the menus play (ui_widget.c's _ui_audio_feedback_...) */
#define ENGINE_SOUND_NONE 0
#define ENGINE_SOUND_CURSOR 1
#define ENGINE_SOUND_BACK 3

/* a held D-pad direction repeats no faster than this (ui_widget.c's
DPAD_EVENT_REPEAT_MILLISECONDS) */
#define ENGINE_DPAD_REPEAT_MILLISECONDS 250

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

/* ---------- input

An event for a screen the engine runs is taken here: the focus moved, a
held direction repeated, the widgets' actions run, the event passed down
the focus, as the game's menus take one (ui_widget.c's
widget_instance_process_one_event_recursive), its steps the same; what the
widgets do and how they are moved through (their definitions' flags and
event handlers) read from ui_widget.c (ui_widget_port_...). */

/* how a widget takes its events: its flags that are behaviour and its event
handlers, the model's (model/behaviour: a widget it has nothing for has
neither), else its definition's (a widget not of the generated menus) */
struct engine_behaviour
{
	long flags;
	void const *handlers;
	long handler_count;
	/* (the definition's handlers, not the model's) */
	boolean definition;
};

static void engine_behaviour_of(struct widget_instance const *widget, struct engine_behaviour *behaviour)
{
	struct halo_menu_model const *model = engine.model ? engine.model : (engine.model = halo_menus_model());
	char name[256];

	memset(behaviour, 0, sizeof(*behaviour));
	if (model && engine_widget_name(widget->definition_tag_index, name, sizeof(name)))
	{
		struct halo_menu_model_behaviour const *found = halo_menus_model_behaviour(model, name);

		if (found)
		{
			behaviour->flags = menu_tags_behaviour_flags(found);
			behaviour->handlers = menu_tags_behaviour_handlers(model, found, &behaviour->handler_count);
		}
		return;
	}
	behaviour->flags = ui_widget_port_definition_flags(widget);
	behaviour->handler_count = ui_widget_port_handler_count(widget);
	behaviour->definition = TRUE;
}

static short engine_handler_event(struct widget_instance const *widget, struct engine_behaviour const *behaviour,
	long index)
{
	return behaviour->definition ? ui_widget_port_handler_event_type(widget, index) :
		menu_tags_handler_event(behaviour->handlers, index);
}

static void engine_handler_run(struct widget_instance *widget, struct engine_behaviour const *behaviour,
	struct event_record *event, long index, boolean *deleted)
{
	if (behaviour->definition)
		ui_widget_port_handler_dispatch(widget, event, index, deleted);
	else
		ui_widget_port_handler_run(widget, event, menu_tags_handler(behaviour->handlers, index), deleted);
}

/* whether a widget takes events: one enabled with handlers of its own, or
a list (ui_widget.c's widget_instance_can_handle_events) */
static boolean engine_takes_events(struct widget_instance const *widget)
{
	struct engine_behaviour behaviour;

	if (widget->disabled)
		return FALSE;
	if (widget->type == ENGINE_WIDGET_SPINNER || widget->type == ENGINE_WIDGET_COLUMN_LIST)
		return TRUE;
	engine_behaviour_of(widget, &behaviour);
	return behaviour.handler_count > 0;
}

/* whether a list's item is one its focus passes over: a label (taking no
events, with nothing in it that does), or hidden or disabled
(ui_widget.c's widget_instance_port_is_label) */
static boolean engine_is_label(struct widget_instance const *widget)
{
	return (!engine_takes_events(widget) && !widget->child) || !widget->visible || widget->disabled;
}

/* a widget that only passes events on, given the focus, focused on its first
child that takes events again (ui_widget.c's
widget_instance_refocus_passthrough) */
static void engine_refocus_passthrough(struct widget_instance *widget)
{
	struct engine_behaviour behaviour;
	struct widget_instance *child;

	if (!widget || widget->focused_child || engine_takes_events(widget))
		return;
	engine_behaviour_of(widget, &behaviour);
	if (!TEST_FLAG(behaviour.flags, ENGINE_PASS_UNHANDLED_TO_FOCUSED_CHILD_BIT) ||
		TEST_FLAG(behaviour.flags, ENGINE_NO_FOCUSED_CHILD_BIT))
	{
		return;
	}
	for (child = widget->child; child; child = child->next)
	{
		if (engine_takes_events(child))
		{
			widget->focused_child = child;
			break;
		}
	}
}

/* whether a child of the widget may take its focus: one that takes events or
passes them on, or any of a list's but a label of the generated menus' lists
(ui_widget.c's widget_instance_tab_to_next_valid_widget's test) */
static boolean engine_focusable(struct widget_instance const *widget, struct widget_instance const *child)
{
	struct engine_behaviour behaviour;
	boolean list = widget->type == ENGINE_WIDGET_SPINNER || widget->type == ENGINE_WIDGET_COLUMN_LIST;

	engine_behaviour_of(child, &behaviour);
	return (behaviour.handler_count > 0 || TEST_FLAG(behaviour.flags, ENGINE_PASS_UNHANDLED_TO_FOCUSED_CHILD_BIT) ||
		list) && !(widget->type == ENGINE_WIDGET_COLUMN_LIST && pc_menu_tag(widget->definition_tag_index) &&
		engine_is_label(child));
}

/* the focus moved to the widget's next child that may take it (or its
previous), round from the last to the first, as ui_widget.c's
widget_instance_tab_to_next_valid_widget and _previous_ move it */
static void engine_tab(struct widget_instance *widget, boolean next)
{
	struct widget_instance *child, *tail;

	for (tail = widget->child; tail && tail->next; tail = tail->next)
		;
	if (next)
		child = widget->focused_child && widget->focused_child->next ? widget->focused_child->next : widget->child;
	else if (widget->focused_child)
		child = widget->focused_child->previous ? widget->focused_child->previous : tail;
	else
		child = widget->child && widget->child->previous ? widget->child->previous : widget->child;
	while (child && child != widget->focused_child)
	{
		if (engine_focusable(widget, child))
		{
			widget->focused_child = child;
			engine_refocus_passthrough(child);
			break;
		}
		if (next)
			child = child->next ? child->next : widget->child;
		else
			child = child->previous ? child->previous : tail;
	}
}

/* the focus moved to a list's next item (or its previous), round from the
last to the first, over its labels: for a list of the generated menus whose
items are its children (not a list made in code), as ui_widget.c's
widget_event_function_list_widget_goto_next_item and _previous_item move it
there; any other list is moved by them */
static void engine_list_step(struct widget_instance *widget, boolean next, struct event_record *event,
	boolean *deleted)
{
	struct widget_instance *child = NULL;
	long item_index = 0, tries = 0;

	if (widget->type != ENGINE_WIDGET_COLUMN_LIST || !pc_menu_tag(widget->definition_tag_index) ||
		(widget->parameters.list.list_items && widget->parameters.list.number_of_items > 0) || !widget->child)
	{
		if (next)
			widget_event_function_list_widget_goto_next_item(widget, event, deleted);
		else
			widget_event_function_list_widget_goto_previous_item(widget, event, deleted);
		return;
	}
	if (widget->focused_child)
	{
		item_index = widget->parameters.list.selected_index + (next ? 1 : -1);
		child = next ? widget->focused_child->next : widget->focused_child->previous;
		if (next && item_index == widget->parameters.list.number_of_items)
			child = NULL;
	}
	if (!child)
	{
		child = widget->child;
		item_index = 0;
		while (!next && child->next)
		{
			child = child->next;
			item_index++;
		}
	}
	while (engine_is_label(child) && tries++ < 256)
	{
		child = next ? child->next : child->previous;
		item_index += next ? 1 : -1;
		if (!child)
		{
			child = widget->child;
			item_index = 0;
			while (!next && child->next)
			{
				child = child->next;
				item_index++;
			}
		}
	}
	ui_widget_port_give_focus(widget, child);
	widget->parameters.list.selected_index = (short)item_index;
	widget->parameters.list.last_list_tab_direction = next ? 15 : -15;
}

/* the direction of a D-pad button (0 up, 1 down, 2 left, 3 right), or NONE */
static short engine_dpad_direction(struct event_record const *event)
{
	if (event->type != ENGINE_EVENT_BUTTON || event->data.button.index < _gamepad_binary_button_dpad_up ||
		event->data.button.index > _gamepad_binary_button_dpad_right)
	{
		return NONE;
	}
	return (short)(event->data.button.index - _gamepad_binary_button_dpad_up);
}

/* whether the event is a press (a button's first, or a held one's repeat) */
static boolean engine_pressed(struct event_record const *event)
{
	return event->type == ENGINE_EVENT_BUTTON && event->data.button.value == 1;
}

/* whether the widget has an event handler for the event type */
static boolean engine_has_handler(struct widget_instance const *widget, struct engine_behaviour const *behaviour,
	short event_type)
{
	long index;

	for (index = 0; index < behaviour->handler_count; index++)
	{
		if (engine_handler_event(widget, behaviour, index) == event_type)
			return TRUE;
	}
	return FALSE;
}

/* the focus moved through the widget's children or its list's items, by its
flags, for a direction pressed or a stick pushed: TRUE if it was */
static boolean engine_move(struct widget_instance *widget, long flags, struct event_record *event, boolean *deleted)
{
	boolean pressed = engine_pressed(event);
	boolean stick = event->type == ENGINE_EVENT_LEFT_STICK;
	/* (-1 back, 1 on, 0 none: up and down, left and right) */
	short vertical = 0, horizontal = 0;
	boolean list = widget->type == ENGINE_WIDGET_SPINNER || widget->type == ENGINE_WIDGET_COLUMN_LIST;

	if (pressed)
	{
		switch (engine_dpad_direction(event))
		{
		case 0: vertical = -1; break;
		case 1: vertical = 1; break;
		case 2: horizontal = -1; break;
		case 3: horizontal = 1; break;
		}
	}
	else if (stick)
	{
		vertical = event->data.stick.y == SHORT_MIN ? 1 : event->data.stick.y == SHORT_MAX ? -1 : 0;
		horizontal = event->data.stick.x == SHORT_MIN ? -1 : event->data.stick.x == SHORT_MAX ? 1 : 0;
	}
	if (!vertical && !horizontal)
		return FALSE;
	if (vertical && TEST_FLAG(flags, ENGINE_UP_DOWN_TABS_CHILDREN_BIT) && widget->focused_child)
	{
		engine_tab(widget, vertical > 0);
		return TRUE;
	}
	if (horizontal && TEST_FLAG(flags, ENGINE_LEFT_RIGHT_TABS_CHILDREN_BIT) && widget->focused_child)
	{
		engine_tab(widget, horizontal > 0);
		return TRUE;
	}
	if (vertical && TEST_FLAG(flags, ENGINE_UP_DOWN_TABS_ITEMS_BIT) && list)
	{
		engine_list_step(widget, vertical > 0, event, deleted);
		return TRUE;
	}
	if (horizontal && TEST_FLAG(flags, ENGINE_LEFT_RIGHT_TABS_ITEMS_BIT) && list)
	{
		engine_list_step(widget, horizontal > 0, event, deleted);
		return TRUE;
	}
	return FALSE;
}

/* whether a handler of the widget's is for the event */
static boolean engine_handler_matches(short handler, struct event_record const *event)
{
	short base;

	switch (event->type)
	{
	case ENGINE_EVENT_LEFT_STICK:
	case ENGINE_EVENT_RIGHT_STICK:
		base = event->type == ENGINE_EVENT_LEFT_STICK ? ENGINE_HANDLER_LEFT_STICK_UP : ENGINE_HANDLER_RIGHT_STICK_UP;
		return (handler == base && event->data.stick.y == SHORT_MAX) ||
			(handler == base + 1 && event->data.stick.y == SHORT_MIN) ||
			(handler == base + 2 && event->data.stick.x == SHORT_MIN) ||
			(handler == base + 3 && event->data.stick.x == SHORT_MAX);
	case ENGINE_EVENT_BUTTON:
		return handler == event->data.button.index && event->data.button.value == 1;
	}
	return FALSE;
}

/* an event for a widget of a screen the engine runs, and on down the focus */
static void engine_event(struct widget_instance *widget, struct event_record *event, boolean *return_deleted)
{
	struct engine_behaviour behaviour;
	long flags;
	boolean handled = FALSE;
	boolean deleted = FALSE;
	boolean for_this = ui_widget_port_takes_events(widget, event->controller_index);
	short sound = ENGINE_SOUND_NONE;
	short direction = engine_dpad_direction(event);
	boolean player = event->controller_index >= 0 && event->controller_index < MAXIMUM_NUMBER_OF_LOCAL_PLAYERS;
	long index;

	engine_behaviour_of(widget, &behaviour);
	flags = behaviour.flags;
	/* (a direction pressed: the pointer is not what moved the focus last; a
	held one repeats, as a press, once it has waited long enough) */
	if (direction != NONE)
		ui_widget_port_pointer_not_last();
	if (direction != NONE && player && event->data.button.value > 1 &&
		ui_widget_port_milliseconds() - *ui_widget_port_dpad_time(event->controller_index, direction) >=
			ENGINE_DPAD_REPEAT_MILLISECONDS)
	{
		event->data.button.value = 1;
	}
	if (widget->close_if_local_player_controller_present)
	{
		short controller;

		for (controller = 0; controller < MAXIMUM_NUMBER_OF_LOCAL_PLAYERS && !deleted; controller++)
		{
			if ((widget->local_player_index == NONE || widget->local_player_index == controller) &&
				input_has_gamepad(controller))
			{
				ui_widget_delete(widget_instance_get_topmost_parent(widget));
				deleted = TRUE;
			}
		}
	}
	/* (B or Back with no handler of its own: the screen before) */
	if (for_this && !deleted && engine_pressed(event) &&
		(event->data.button.index == _gamepad_analog_button_b ||
			event->data.button.index == _gamepad_binary_button_back) &&
		!engine_has_handler(widget, &behaviour, event->data.button.index))
	{
		ui_widget_port_go_back(widget);
		sound = ENGINE_SOUND_BACK;
		deleted = TRUE;
		handled = TRUE;
	}
	if (!deleted && widget->milliseconds_to_auto_close > 0)
	{
		unsigned long now = ui_widget_port_milliseconds();

		if (now - widget->creation_time >= widget->auto_close_fade_time + widget->milliseconds_to_auto_close)
		{
			ui_widget_delete(widget_instance_get_topmost_parent(widget));
			deleted = TRUE;
		}
		else if (widget->auto_close_fade_time > 0)
		{
			long faded = (long)(now - widget->creation_time - widget->milliseconds_to_auto_close);

			if (faded > 0)
				widget->alpha_modifier = 1.0f - (real)faded / (real)widget->auto_close_fade_time;
		}
	}
	if (!deleted)
	{
		ui_widget_port_lists_update(widget);
		if (for_this && !handled && engine_move(widget, flags, event, &deleted))
		{
			sound = ENGINE_SOUND_CURSOR;
			handled = TRUE;
		}
	}
	for (index = 0; for_this && !deleted && index < behaviour.handler_count; index++)
	{
		if (engine_handler_matches(engine_handler_event(widget, &behaviour, index), event))
		{
			handled = TRUE;
			engine_handler_run(widget, &behaviour, event, index, &deleted);
		}
	}
	if ((TEST_FLAG(flags, ENGINE_PASS_HANDLED_TO_ALL_CHILDREN_BIT) || !handled) && (TEST_FLAG(flags, ENGINE_PASS_UNHANDLED_TO_FOCUSED_CHILD_BIT) || TEST_FLAG(flags, ENGINE_PASS_UNHANDLED_TO_ALL_CHILDREN_BIT)) && !deleted)
	{
		if (TEST_FLAG(flags, ENGINE_PASS_UNHANDLED_TO_ALL_CHILDREN_BIT))
		{
			struct widget_instance *child;

			for (child = widget->child; child && !deleted; child = child->next)
			{
				if (ui_widget_port_takes_events(child, event->controller_index))
					engine_event(child, event, &deleted);
			}
		}
		else if (widget->focused_child && ui_widget_port_takes_events(widget->focused_child, event->controller_index))
			engine_event(widget->focused_child, event, &deleted);
	}
	if (deleted && TEST_FLAG(flags, ENGINE_RETURN_TO_MAIN_MENU_BIT) && ui_widget_port_no_screens())
		main_goto_main_menu();
	if (direction != NONE && player && event->data.button.value == 1)
		*ui_widget_port_dpad_time(event->controller_index, direction) = ui_widget_port_milliseconds();
	ui_play_audio_feedback_sound(sound);
	*return_deleted = deleted;
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
	engine_event(screen, event, deleted);
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

struct halo_menu_model const *menu_engine_model(void)
{
	if (!engine.model)
		engine.model = halo_menus_model();
	return engine.model;
}

struct halo_menu_model_screen const *menu_engine_model_screen_of(struct widget_instance const *widget)
{
	while (widget && widget->parent)
		widget = widget->parent;
	return widget ? engine_model_screen(widget->definition_tag_index) : NULL;
}

char const *menu_engine_look_role(struct widget_instance const *widget)
{
	struct halo_menu_model_look const *look;
	char name[256];

	if (!engine.model)
		engine.model = halo_menus_model();
	if (!widget || !engine.model || !engine_widget_name(widget->definition_tag_index, name, sizeof(name)))
		return NULL;
	look = halo_menus_model_look(engine.model, name);
	return look ? look->role : NULL;
}

struct halo_menu_model_row const *menu_engine_model_row_of(struct widget_instance const *row)
{
	if (!row || !row->name || strncmp(row->name, "op_", 3))
		return NULL;
	return engine_model_row(menu_engine_model_screen_of(row), row->name + 3);
}

boolean menu_engine_lays_out(struct widget_instance *widget)
{
	struct widget_instance *screen = widget;

	while (screen && screen->parent)
		screen = screen->parent;
	return engine_lays_out(engine_screen_of(screen), widget);
}
