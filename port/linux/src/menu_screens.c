/*
MENU_SCREENS.C

The menus' hand-written screens (menu_screens.h, docs/menu_screens.md): the
files of port/assets/menus/screens (embedded, or a menus folder's of the same
name, as the menus' files are: menu_files.c) read with Expat into screens of
building blocks, for the menu engine (port/linux/game/menu_screen.c), which
makes them into widgets.

A file is checked whole as it is read: its elements and attributes, what an
item must have, the ids a list's focus names, and its actions and bindings
against the names the game gave (menu_screens_names). A file with a problem
is logged with its line, and its screen is not used: the one read before it,
if any, stays.

Hot reload: a folder (debug.menu_screens, else debug.menu_files' screens/)
is looked at twice a second (menu_screens_poll), and a file changed in it is
read again; the engine rebuilds a screen that is open when its reading
changes (menu_screens_generation).
*/

#include "halo_menus.h"
#include "menu_screens.h"
#include "platform.h"
#include "port_config.h"

#include "expat.h"

#ifndef HALO_ANDROID
#include <SDL3/SDL.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the folder in the menus' files that holds the screens, and its file of
the building blocks' widgets (read by menu_files.c, not here) */
#define SCREENS_FOLDER "screens/"
#define SCREENS_BLOCKS "screens/blocks.xml"
/* the watched folder looked at no more often than this */
#define SCREENS_POLL_MILLISECONDS 500
#define SCREENS_MAXIMUM_DEPTH 4

/* ---------- reading */

enum reader_element
{
	_element_screen,
	_element_title_art,
	_element_list,
	_element_item,
	_element_hint,
	_element_space,
	_element_buttons,
	_element_button,
	_element_cards,
	_element_card,
	_element_header
};

struct reader
{
	struct menu_screen *screen;
	XML_Parser parser;
	int failed;
	char *problem;
	unsigned long problem_size;
	int depth;
	int elements[SCREENS_MAXIMUM_DEPTH];
	/* (the block each open element is: -1 for none) */
	int blocks[SCREENS_MAXIMUM_DEPTH];
	/* the open <hint>'s text */
	char text[MENU_SCREEN_MAXIMUM_TEXT];
	unsigned long text_length;
	int text_overflow;
};

static struct menu_screen_names const *screen_names;

void menu_screens_names(struct menu_screen_names const *names)
{
	screen_names = names;
}

static void reader_problem(struct reader *reader, char const *format, char const *detail)
{
	char message[256];

	if (reader->failed)
		return;
	reader->failed = 1;
	snprintf(message, sizeof(message), format, detail ? detail : "");
	snprintf(reader->problem, reader->problem_size, "%s:%lu: %s", reader->screen->file ? reader->screen->file : "?",
		reader->parser ? (unsigned long)XML_GetCurrentLineNumber(reader->parser) : 0UL, message);
	if (reader->parser)
		XML_StopParser(reader->parser, XML_FALSE);
}

static char const *reader_string(struct reader *reader, char const *text)
{
	struct menu_screen *screen = reader->screen;
	char *copy;

	if (screen->string_count >= MENU_SCREEN_MAXIMUM_STRINGS)
	{
		reader_problem(reader, "more than %s texts", "256");
		return NULL;
	}
	copy = strdup(text);
	if (!copy)
	{
		reader_problem(reader, "out of memory", NULL);
		return NULL;
	}
	screen->strings[screen->string_count++] = copy;
	return copy;
}

static int name_known(char const *const *names, long count, char const *name, unsigned long length)
{
	long index;

	if (!names)
		return 1;
	for (index = 0; index < count; index++)
	{
		if (strlen(names[index]) == length && !strncmp(names[index], name, length))
			return 1;
	}
	return 0;
}

/* a text's bindings, each a name the game knows */
static void text_check(struct reader *reader, char const *text)
{
	char const *at = text;

	while (!reader->failed && (at = strchr(at, '{')) != NULL)
	{
		char const *end = strchr(at, '}');
		char name[64];

		if (!end)
		{
			reader_problem(reader, "a { with no } in \"%s\"", text);
			return;
		}
		if (end == at + 1 || (unsigned long)(end - at - 1) >= sizeof(name))
		{
			reader_problem(reader, "a binding with no name, or too long, in \"%s\"", text);
			return;
		}
		snprintf(name, sizeof(name), "%.*s", (int)(end - at - 1), at + 1);
		if (screen_names && !name_known(screen_names->bindings, screen_names->binding_count, name, strlen(name)))
		{
			reader_problem(reader, "there is no binding {%s}", name);
			return;
		}
		at = end + 1;
	}
	if (!reader->failed && strchr(text, '}') && !strchr(text, '{'))
		reader_problem(reader, "a } with no { in \"%s\"", text);
}

static void action_check(struct reader *reader, char const *action)
{
	if (screen_names && !name_known(screen_names->actions, screen_names->action_count, action, strlen(action)))
		reader_problem(reader, "there is no action \"%s\"", action);
}

/* a when: platforms the menus' files know (halo_menus_platform) */
static void when_check(struct reader *reader, char const *when)
{
	char const *name = when;

	while (*name && !reader->failed)
	{
		unsigned long length = (unsigned long)strcspn(name, " ");

		if (length && halo_menus_platform(name, length) < 0)
			reader_problem(reader, "when=\"%s\" names one that is not desktop, android, vr, hdr, touch, pointer or "
				"computer", when);
		name += length;
		while (*name == ' ')
			name++;
	}
}

/* an element's attributes: each one of the names (NULL-ended) it may have,
into the values (NULL where not written) */
static void attributes_read(struct reader *reader, char const *element, XML_Char const **attributes,
	char const *const *names, char const **values)
{
	int index, name;

	for (name = 0; names[name]; name++)
		values[name] = NULL;
	for (index = 0; attributes[index] && !reader->failed; index += 2)
	{
		for (name = 0; names[name] && strcmp(names[name], attributes[index]); name++)
			;
		if (!names[name])
		{
			char detail[96];

			snprintf(detail, sizeof(detail), "<%s> has no attribute %s", element, attributes[index]);
			reader_problem(reader, "%s", detail);
			return;
		}
		values[name] = reader_string(reader, attributes[index + 1]);
	}
}

static int block_add(struct reader *reader, int kind, int parent)
{
	struct menu_screen *screen = reader->screen;
	struct menu_screen_block *block;

	if (screen->block_count >= MENU_SCREEN_MAXIMUM_BLOCKS)
	{
		reader_problem(reader, "more than %s blocks", "32");
		return -1;
	}
	block = &screen->blocks[screen->block_count];
	memset(block, 0, sizeof(*block));
	block->frame = -1;
	block->kind = kind;
	block->parent = parent;
	block->line = (long)XML_GetCurrentLineNumber(reader->parser);
	return screen->block_count++;
}

static int kind_count(struct menu_screen const *screen, int kind)
{
	int index, count = 0;

	for (index = 0; index < screen->block_count; index++)
		count += screen->blocks[index].kind == kind;
	return count;
}

static void XMLCALL element_start(void *data, XML_Char const *name, XML_Char const **attributes)
{
	struct reader *reader = data;
	struct menu_screen *screen = reader->screen;
	int parent_element = reader->depth ? reader->elements[reader->depth - 1] : -1;
	int parent_block = reader->depth ? reader->blocks[reader->depth - 1] : -1;
	int element = -1;
	int block = -1;
	char const *values[8];

	if (reader->failed)
		return;
	if (reader->depth >= SCREENS_MAXIMUM_DEPTH)
	{
		reader_problem(reader, "<%s> is too deep", name);
		return;
	}
	if (!strcmp(name, "screen") && parent_element == -1)
	{
		static char const *const names[] = { "name", "back", NULL };

		element = _element_screen;
		attributes_read(reader, name, attributes, names, values);
		screen->name = values[0];
		screen->back = values[1];
		if (!reader->failed && (!screen->name || !screen->name[0]))
			reader_problem(reader, "<screen> has no name", NULL);
		if (!reader->failed && screen->back)
			action_check(reader, screen->back);
	}
	else if (parent_element == _element_screen && (!strcmp(name, "title-art") || !strcmp(name, "space")))
	{
		static char const *const names[] = { NULL };

		element = !strcmp(name, "space") ? _element_space : _element_title_art;
		attributes_read(reader, name, attributes, names, values);
		if (element == _element_title_art && kind_count(screen, MENU_SCREEN_TITLE_ART))
			reader_problem(reader, "a screen has one <title-art>", NULL);
		block = block_add(reader, element == _element_space ? MENU_SCREEN_SPACE : MENU_SCREEN_TITLE_ART, -1);
	}
	else if (parent_element == _element_screen && !strcmp(name, "list"))
	{
		static char const *const names[] = { "id", "focus", NULL };

		element = _element_list;
		attributes_read(reader, name, attributes, names, values);
		if (kind_count(screen, MENU_SCREEN_LIST) || kind_count(screen, MENU_SCREEN_CARDS))
			reader_problem(reader, "a screen has one <list> or <cards>", NULL);
		block = block_add(reader, MENU_SCREEN_LIST, -1);
		if (block >= 0)
		{
			screen->blocks[block].id = values[0];
			screen->blocks[block].focus = values[1];
		}
	}
	else if (parent_element == _element_list && !strcmp(name, "item"))
	{
		static char const *const names[] = { "id", "label", "hint", "do", "when", "picture", NULL };

		element = _element_item;
		attributes_read(reader, name, attributes, names, values);
		block = block_add(reader, MENU_SCREEN_ITEM, parent_block);
		if (block >= 0 && !reader->failed)
		{
			struct menu_screen_block *item = &screen->blocks[block];

			item->id = values[0];
			item->label = values[1];
			item->action = values[3];
			item->when = values[4];
			item->picture = values[5];
			if (values[2])
				item->hints[item->hint_count++] = values[2];
			if (!item->label || !item->label[0])
				reader_problem(reader, "<item> has no label", NULL);
			else if (!item->action)
				reader_problem(reader, "<item> has no do", NULL);
			if (!reader->failed)
				text_check(reader, item->label);
			if (!reader->failed && values[2])
				text_check(reader, values[2]);
			if (!reader->failed)
				action_check(reader, item->action);
			if (!reader->failed && item->when)
				when_check(reader, item->when);
		}
	}
	else if (parent_element == _element_item && !strcmp(name, "hint"))
	{
		static char const *const names[] = { NULL };

		element = _element_hint;
		attributes_read(reader, name, attributes, names, values);
		reader->text_length = 0;
		reader->text_overflow = 0;
		reader->text[0] = 0;
	}
	else if (parent_element == _element_screen && !strcmp(name, "header"))
	{
		static char const *const names[] = { "title", "picture", NULL };

		element = _element_header;
		attributes_read(reader, name, attributes, names, values);
		if (!reader->failed && kind_count(screen, MENU_SCREEN_HEADER))
			reader_problem(reader, "a screen has one <header>", NULL);
		if (!reader->failed && (!values[0] || !values[0][0]))
			reader_problem(reader, "<header> has no title", NULL);
		block = block_add(reader, MENU_SCREEN_HEADER, -1);
		if (block >= 0)
		{
			screen->blocks[block].label = values[0];
			screen->blocks[block].picture = values[1];
		}
	}
	else if (parent_element == _element_screen && !strcmp(name, "cards"))
	{
		static char const *const names[] = { "id", "focus", NULL };

		element = _element_cards;
		attributes_read(reader, name, attributes, names, values);
		if (kind_count(screen, MENU_SCREEN_CARDS) || kind_count(screen, MENU_SCREEN_LIST))
			reader_problem(reader, "a screen has one <list> or <cards>", NULL);
		block = block_add(reader, MENU_SCREEN_CARDS, -1);
		if (block >= 0)
		{
			screen->blocks[block].id = values[0];
			screen->blocks[block].focus = values[1];
		}
	}
	else if (parent_element == _element_cards && !strcmp(name, "card"))
	{
		static char const *const names[] = { "id", "title", "subtitle", "picture", "frame", "do", "when", NULL };

		element = _element_card;
		attributes_read(reader, name, attributes, names, values);
		block = block_add(reader, MENU_SCREEN_CARD, parent_block);
		if (block >= 0 && !reader->failed)
		{
			struct menu_screen_block *card = &screen->blocks[block];
			char *end = NULL;

			card->id = values[0];
			card->label = values[1];
			if (values[2])
				card->hints[card->hint_count++] = values[2];
			card->picture = values[3];
			card->action = values[5];
			card->when = values[6];
			if (values[4])
			{
				card->frame = strtol(values[4], &end, 10);
				if (!end || *end || card->frame < 0 || card->frame > 255)
					reader_problem(reader, "a <card>'s frame is a number, 0 to 255: \"%s\"", values[4]);
			}
			if (!reader->failed && (!card->label || !card->label[0]))
				reader_problem(reader, "<card> has no title", NULL);
			else if (!reader->failed && !card->action)
				reader_problem(reader, "<card> has no do", NULL);
			if (!reader->failed)
				text_check(reader, card->label);
			if (!reader->failed && values[2])
				text_check(reader, values[2]);
			if (!reader->failed)
				action_check(reader, card->action);
			if (!reader->failed && card->when)
				when_check(reader, card->when);
		}
	}
	else if (parent_element == _element_screen && !strcmp(name, "buttons"))
	{
		static char const *const names[] = { NULL };

		element = _element_buttons;
		attributes_read(reader, name, attributes, names, values);
		if (kind_count(screen, MENU_SCREEN_BUTTONS))
			reader_problem(reader, "a screen has one <buttons>", NULL);
		block = block_add(reader, MENU_SCREEN_BUTTONS, -1);
	}
	else if (parent_element == _element_buttons && !strcmp(name, "button"))
	{
		static char const *const names[] = { "key", "label", "side", NULL };

		element = _element_button;
		attributes_read(reader, name, attributes, names, values);
		block = block_add(reader, MENU_SCREEN_BUTTON, parent_block);
		if (block >= 0 && !reader->failed)
		{
			struct menu_screen_block *button = &screen->blocks[block];

			button->key = values[0];
			button->label = values[1];
			button->side = values[2];
			if (!button->key || (strcmp(button->key, "accept") && strcmp(button->key, "back") &&
				strcmp(button->key, "x") && strcmp(button->key, "y")))
			{
				reader_problem(reader, "<button>'s key is accept, back, x or y", NULL);
			}
			else if (!button->label || !button->label[0])
			{
				reader_problem(reader, "<button> has no label", NULL);
			}
			else if (button->side && strcmp(button->side, "left") && strcmp(button->side, "right"))
			{
				reader_problem(reader, "<button>'s side is left or right", NULL);
			}
		}
	}
	else
	{
		char detail[96];

		snprintf(detail, sizeof(detail), "<%s> is not one of what %s may hold", name,
			parent_element == -1 ? "a file" : "its element");
		reader_problem(reader, "%s", detail);
		return;
	}
	reader->elements[reader->depth] = element;
	reader->blocks[reader->depth] = block;
	reader->depth++;
}

static void XMLCALL element_end(void *data, XML_Char const *name)
{
	struct reader *reader = data;
	int element;

	(void)name;
	if (reader->failed || !reader->depth)
		return;
	element = reader->elements[--reader->depth];
	if (element == _element_hint)
	{
		struct menu_screen_block *item = &reader->screen->blocks[reader->blocks[reader->depth - 1]];
		char const *hint;

		if (reader->text_overflow)
		{
			reader_problem(reader, "a <hint> longer than %s bytes", "255");
			return;
		}
		if (item->hint_count >= MENU_SCREEN_MAXIMUM_HINTS)
		{
			reader_problem(reader, "an <item> has no more than %s hints", "4");
			return;
		}
		hint = reader_string(reader, reader->text);
		if (hint)
		{
			text_check(reader, hint);
			item->hints[item->hint_count++] = hint;
		}
	}
	else if (element == _element_list || element == _element_cards)
	{
		struct menu_screen *screen = reader->screen;
		struct menu_screen_block const *list = &screen->blocks[reader->blocks[reader->depth]];
		int index, items = 0, focus_found = !list->focus;

		for (index = 0; index < screen->block_count && !reader->failed; index++)
		{
			struct menu_screen_block const *item = &screen->blocks[index];
			int other;

			if ((item->kind != MENU_SCREEN_ITEM && item->kind != MENU_SCREEN_CARD) ||
				item->parent != reader->blocks[reader->depth])
				continue;
			items++;
			if (list->focus && item->id && !strcmp(item->id, list->focus))
				focus_found = 1;
			for (other = index + 1; item->id && other < screen->block_count; other++)
			{
				if (screen->blocks[other].id && !strcmp(screen->blocks[other].id, item->id))
					reader_problem(reader, "two items' id is \"%s\"", item->id);
			}
		}
		if (!reader->failed && !items)
			reader_problem(reader, "a <list> has no <item>, or <cards> no <card>", NULL);
		if (!reader->failed && !focus_found)
			reader_problem(reader, "the <list>'s focus names no item's id: \"%s\"", list->focus);
	}
}

static void XMLCALL text_data(void *data, XML_Char const *text, int length)
{
	struct reader *reader = data;

	if (reader->failed || !reader->depth || reader->elements[reader->depth - 1] != _element_hint)
		return;
	if (reader->text_length + (unsigned long)length >= sizeof(reader->text))
	{
		reader->text_overflow = 1;
		return;
	}
	memcpy(reader->text + reader->text_length, text, (size_t)length);
	reader->text_length += (unsigned long)length;
	reader->text[reader->text_length] = 0;
}

void menu_screen_free(struct menu_screen *screen)
{
	int index;

	for (index = 0; index < screen->string_count; index++)
		free(screen->strings[index]);
	memset(screen, 0, sizeof(*screen));
}

int menu_screen_read(char const *file, unsigned char const *data, unsigned long size, struct menu_screen *screen,
	char *problem, unsigned long problem_size)
{
	struct reader reader;
	char const *file_copy;

	memset(screen, 0, sizeof(*screen));
	memset(&reader, 0, sizeof(reader));
	reader.screen = screen;
	reader.problem = problem;
	reader.problem_size = problem_size;
	if (problem_size)
		problem[0] = 0;
	file_copy = reader_string(&reader, file ? file : "?");
	screen->file = file_copy;
	reader.parser = XML_ParserCreate("UTF-8");
	if (!reader.parser)
	{
		reader_problem(&reader, "out of memory", NULL);
		menu_screen_free(screen);
		return 0;
	}
	XML_SetUserData(reader.parser, &reader);
	XML_SetElementHandler(reader.parser, element_start, element_end);
	XML_SetCharacterDataHandler(reader.parser, text_data);
	if (XML_Parse(reader.parser, (char const *)data, (int)size, XML_TRUE) == XML_STATUS_ERROR && !reader.failed)
		reader_problem(&reader, "%s", XML_ErrorString(XML_GetErrorCode(reader.parser)));
	if (!reader.failed && !screen->name)
		reader_problem(&reader, "the file has no <screen>", NULL);
	if (!reader.failed && !screen->block_count)
		reader_problem(&reader, "the <screen> has nothing in it", NULL);
	XML_ParserFree(reader.parser);
	if (reader.failed)
	{
		menu_screen_free(screen);
		return 0;
	}
	return 1;
}

int menu_screen_text(char const *text, menu_screen_value value, void *context, char *out, unsigned long size)
{
	unsigned long used = 0;
	int complete = 1;

	if (!size)
		return 0;
	out[0] = 0;
	while (text && *text)
	{
		char const *end;

		if (*text == '{' && (end = strchr(text, '}')) != NULL)
		{
			char name[64], found[MENU_SCREEN_MAXIMUM_TEXT];
			unsigned long length;

			snprintf(name, sizeof(name), "%.*s", (int)(end - text - 1), text + 1);
			found[0] = 0;
			if (!value || !value(context, name, found, sizeof(found)) || !found[0])
				complete = 0;
			length = (unsigned long)strlen(found);
			if (used + length >= size)
				length = size - used - 1;
			memcpy(out + used, found, length);
			used += length;
			text = end + 1;
			continue;
		}
		if (used + 1 < size)
			out[used++] = *text;
		text++;
	}
	out[used] = 0;
	return complete;
}

int menu_screen_when(char const *when)
{
	char const *name = when;

	if (!when)
		return 1;
	while (*name)
	{
		unsigned long length = (unsigned long)strcspn(name, " ");

		if (length && halo_menus_platform(name, length) > 0)
			return 1;
		name += length;
		while (*name == ' ')
			name++;
	}
	return 0;
}

/* ---------- the screens */

static struct
{
	int loaded;
	struct menu_screen screens[MENU_SCREEN_MAXIMUM_SCREENS];
	/* (each one's file, as the menus' files name it or the watched folder's
	file name, and the stamp it was read at) */
	char paths[MENU_SCREEN_MAXIMUM_SCREENS][128];
	unsigned long generations[MENU_SCREEN_MAXIMUM_SCREENS];
	unsigned long generation;
	/* the watched folder's files' stamps, read or not (a file with a
	problem is not read again until it changes) */
	char stamped[MENU_SCREEN_MAXIMUM_SCREENS * 2][128];
	unsigned long long stamps[MENU_SCREEN_MAXIMUM_SCREENS * 2];
	int stamped_count;
	unsigned long polled_at;
	char folder[1024];
	int folder_read;
} screens;

/* a screen read kept: in place of the one of its name, else of its file,
else in a free place; FALSE (it let go) where there is none */
static int screens_keep(char const *path, struct menu_screen *screen)
{
	int index, place = -1;

	for (index = 0; index < MENU_SCREEN_MAXIMUM_SCREENS && place < 0; index++)
	{
		if (screens.screens[index].name && !strcmp(screens.screens[index].name, screen->name))
			place = index;
	}
	for (index = 0; index < MENU_SCREEN_MAXIMUM_SCREENS && place < 0; index++)
	{
		if (screens.screens[index].name && !strcmp(screens.paths[index], path))
			place = index;
	}
	for (index = 0; index < MENU_SCREEN_MAXIMUM_SCREENS && place < 0; index++)
	{
		if (!screens.screens[index].name)
			place = index;
	}
	if (place < 0)
	{
		platform_log("menus: more than %d screens; %s is not read", MENU_SCREEN_MAXIMUM_SCREENS, path);
		menu_screen_free(screen);
		return 0;
	}
	if (screens.screens[place].name)
		menu_screen_free(&screens.screens[place]);
	screens.screens[place] = *screen;
	snprintf(screens.paths[place], sizeof(screens.paths[place]), "%s", path);
	screens.generations[place] = ++screens.generation;
	return 1;
}

/* a file read: kept, or its problem logged (keep: what was there stays) */
static void screens_read(char const *path, unsigned char const *data, unsigned long size, int reload)
{
	struct menu_screen screen;
	char problem[512];

	if (!menu_screen_read(path, data, size, &screen, problem, sizeof(problem)))
	{
		platform_log("menus: %s%s", problem, reload ? "; the screen stays as it was" : "; the screen is not used");
		return;
	}
	if (screens_keep(path, &screen))
		platform_log("menus: screen %s %s from %s", screen.name, reload ? "read again" : "read", path);
}

static void screens_load(void)
{
	char const *path;
	unsigned char const *data;
	unsigned long size;
	long index;

	if (screens.loaded)
		return;
	screens.loaded = 1;
	if (!halo_menus_load())
		return;
	for (index = 0; halo_menus_file_at(index, &path, &data, &size); index++)
	{
		size_t length = strlen(path);

		if (!strncmp(path, SCREENS_FOLDER, strlen(SCREENS_FOLDER)) && strcmp(path, SCREENS_BLOCKS) && length > 4 &&
			!strcmp(path + length - 4, ".xml"))
			screens_read(path, data, size, 0);
	}
}

char const *menu_screens_folder(void)
{
	if (!screens.folder_read)
	{
		char const *files = config_string("debug.menu_files");

		screens.folder_read = 1;
		if (config_string("debug.menu_screens")[0])
			snprintf(screens.folder, sizeof(screens.folder), "%s", config_string("debug.menu_screens"));
		else if (files[0])
			snprintf(screens.folder, sizeof(screens.folder), "%s/screens", files);
	}
	return screens.folder;
}

struct menu_screen const *menu_screens_find(char const *name)
{
	int index;

	screens_load();
	for (index = 0; name && index < MENU_SCREEN_MAXIMUM_SCREENS; index++)
	{
		if (screens.screens[index].name && !strcmp(screens.screens[index].name, name))
			return &screens.screens[index];
	}
	return NULL;
}

unsigned long menu_screens_generation(char const *name)
{
	int index;

	for (index = 0; name && index < MENU_SCREEN_MAXIMUM_SCREENS; index++)
	{
		if (screens.screens[index].name && !strcmp(screens.screens[index].name, name))
			return screens.generations[index];
	}
	return 0;
}

#ifndef HALO_ANDROID
static unsigned char *screens_file_read(char const *path, unsigned long *size)
{
	FILE *file = fopen(path, "rb");
	unsigned char *data = NULL;
	long length;

	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) >= 0 && fseek(file, 0, SEEK_SET) == 0)
	{
		data = malloc((size_t)length + 1);
		if (data && fread(data, 1, (size_t)length, file) == (size_t)length)
		{
			*size = (unsigned long)length;
		}
		else
		{
			free(data);
			data = NULL;
		}
	}
	fclose(file);
	return data;
}

/* the stamp a file was last seen at (its place in stamped, made if new) */
static unsigned long long *screens_stamp(char const *name)
{
	int index;

	for (index = 0; index < screens.stamped_count; index++)
	{
		if (!strcmp(screens.stamped[index], name))
			return &screens.stamps[index];
	}
	if (screens.stamped_count >= (int)(sizeof(screens.stamps) / sizeof(screens.stamps[0])))
		return NULL;
	snprintf(screens.stamped[screens.stamped_count], sizeof(screens.stamped[0]), "%s", name);
	screens.stamps[screens.stamped_count] = 0;
	return &screens.stamps[screens.stamped_count++];
}
#endif

void menu_screens_poll(unsigned long now)
{
#ifdef HALO_ANDROID
	(void)now;
#else
	char const *folder = menu_screens_folder();
	char **names;
	int count = 0, name;

	if (!folder[0] || (screens.polled_at && now - screens.polled_at < SCREENS_POLL_MILLISECONDS))
		return;
	screens.polled_at = now ? now : 1;
	screens_load();
	names = SDL_GlobDirectory(folder, "*.xml", 0, &count);
	for (name = 0; names && name < count; name++)
	{
		char path[1200];
		SDL_PathInfo info;
		unsigned long long stamp, *seen;

		if (!strcmp(names[name], "blocks.xml"))
			continue;
		snprintf(path, sizeof(path), "%s/%s", folder, names[name]);
		if (!SDL_GetPathInfo(path, &info))
			continue;
		stamp = (unsigned long long)info.modify_time * 1099511628211ULL + info.size + 1;
		seen = screens_stamp(names[name]);
		if (!seen || *seen == stamp)
			continue;
		/* (the first look at a file the menus' files read already: as it
		was then, not read again) */
		if (!*seen)
		{
			char embedded[160];
			int index, known = 0;

			snprintf(embedded, sizeof(embedded), "%s%s", SCREENS_FOLDER, names[name]);
			for (index = 0; index < MENU_SCREEN_MAXIMUM_SCREENS; index++)
				known |= screens.screens[index].name && !strcmp(screens.paths[index], embedded) &&
					!config_string("debug.menu_screens")[0];
			if (known)
			{
				*seen = stamp;
				continue;
			}
		}
		*seen = stamp;
		{
			unsigned long size = 0;
			unsigned char *data = screens_file_read(path, &size);
			char named[160];

			if (!data)
			{
				platform_log("menus: %s could not be read", path);
				continue;
			}
			snprintf(named, sizeof(named), "%s%s", SCREENS_FOLDER, names[name]);
			screens_read(named, data, size, 1);
			free(data);
		}
	}
	SDL_free(names);
#endif
}
