/*
MENU_LAYOUT.C

debug.menu_layout: the open screen's geometry, written as JSON to the
setting's path (in the data root, where the game's fopen finds it) for
tools/menu_gallery.py's layout checks, so that they read what the game drew
rather than guess at pixels. The first player's render of the menus
(ui_widget.c's render_ui_widgets) notes each widget drawn, its box and
clip; each string drawn (rasterizer_text.c), the bounds it is laid out in,
its clip and the extent of its characters (and of all of them: the lines
that run past its box's foot are not drawn); each button's glyph (a key's
cap or a controller's button, in words or as a widget's picture); and the
pointer's targets. Every 30th frame of the menus is written, to a file
beside the path then renamed over it, so that a reader never sees half of
one.

{"frame": n, "screen_name": "...", "screen": [x0, y0, x1, y1],
 "prompts": "keyboard" or "controller", "touch_menus": bool,
 "items": [{"kind": "widget", "id", "parent", "name", "type", "box",
	"clip", "focused"}, {"kind": "text", "widget", "text", "box", "clip",
	"extent", "whole", "scale"}, {"kind": "glyph", "widget", "key",
	"name", "box"}, {"kind": "target", "widget", "target", "box"}]}

Rectangles are [x0, y0, x1, y1] in the menus' units (menu_layout.h).
*/

#include "cseries.h"
#include "math/integer_math.h"
#include "menu_layout.h"

#include <stdio.h>
#include <string.h>

int config_boolean(char const *name);
char const *config_string(char const *name);
int halo_keyboard_prompts(void);
int ui_widget_port_touch_menus(void);
void platform_log(char const *format, ...);

#define MENU_LAYOUT_EVERY 30
#define MENU_LAYOUT_MAXIMUM_WIDGETS 4096

static struct
{
	FILE *file;
	char temporary[512];
	long frame;
	long frames;
	long items;
	long widget_count;
	long current;
	void const *widgets[MENU_LAYOUT_MAXIMUM_WIDGETS];
} menu_layout;

static char const *menu_layout_path(
	void)
{
	static int read = 0;
	static char path[480];

	if (!read)
	{
		read = 1;
		snprintf(path, sizeof(path), "%s", config_string("debug.menu_layout"));
	}

	return path;
}

int menu_layout_recording(
	void)
{
	return menu_layout.file != NULL;
}

static void menu_layout_rectangle(
	char const *name,
	rectangle2d const *rectangle)
{
	if (rectangle)
	{
		fprintf(menu_layout.file, ",\"%s\":[%d,%d,%d,%d]", name, rectangle->x0, rectangle->y0, rectangle->x1,
			rectangle->y1);
	}
	else
	{
		fprintf(menu_layout.file, ",\"%s\":null", name);
	}
}

/* a string as JSON's, from UTF-16 (a pair of surrogates one character) */
static void menu_layout_wide_string(
	wchar_t const *text)
{
	long index;

	fputc('"', menu_layout.file);
	for (index = 0; text && text[index] && index < 1024; index++)
	{
		unsigned long character = (unsigned short)text[index];

		if (character >= 0xd800 && character < 0xdc00 && (unsigned short)text[index + 1] >= 0xdc00 &&
			(unsigned short)text[index + 1] < 0xe000)
		{
			character = 0x10000 + ((character - 0xd800) << 10) + ((unsigned short)text[index + 1] - 0xdc00);
			index++;
		}
		if (character == '"' || character == '\\')
			fprintf(menu_layout.file, "\\%c", (char)character);
		else if (character < 0x20 || (character >= 0xd800 && character < 0xe000))
			fprintf(menu_layout.file, "\\u%04lx", character < 0x20 ? character : 0xfffdUL);
		else if (character < 0x80)
			fputc((int)character, menu_layout.file);
		else if (character < 0x800)
		{
			fputc((int)(0xc0 | (character >> 6)), menu_layout.file);
			fputc((int)(0x80 | (character & 0x3f)), menu_layout.file);
		}
		else if (character < 0x10000)
		{
			fputc((int)(0xe0 | (character >> 12)), menu_layout.file);
			fputc((int)(0x80 | ((character >> 6) & 0x3f)), menu_layout.file);
			fputc((int)(0x80 | (character & 0x3f)), menu_layout.file);
		}
		else
		{
			fputc((int)(0xf0 | (character >> 18)), menu_layout.file);
			fputc((int)(0x80 | ((character >> 12) & 0x3f)), menu_layout.file);
			fputc((int)(0x80 | ((character >> 6) & 0x3f)), menu_layout.file);
			fputc((int)(0x80 | (character & 0x3f)), menu_layout.file);
		}
	}
	fputc('"', menu_layout.file);
}

static void menu_layout_string(
	char const *text)
{
	long index;

	fputc('"', menu_layout.file);
	for (index = 0; text && text[index]; index++)
	{
		unsigned char character = (unsigned char)text[index];

		if (character == '"' || character == '\\')
			fprintf(menu_layout.file, "\\%c", character);
		else if (character < 0x20)
			fprintf(menu_layout.file, "\\u%04x", character);
		else
			fputc(character, menu_layout.file);
	}
	fputc('"', menu_layout.file);
}

/* an item's opening, after the one before */
static void menu_layout_item(
	char const *kind)
{
	fprintf(menu_layout.file, "%s\n{\"kind\":\"%s\"", menu_layout.items ? "," : "", kind);
	menu_layout.items++;
}

static long menu_layout_widget_id(
	void const *widget)
{
	long index;

	for (index = menu_layout.widget_count - 1; widget && index >= 0; index--)
	{
		if (menu_layout.widgets[index] == widget)
			return index;
	}

	return -1;
}

void menu_layout_frame_begin(
	long screen_width)
{
	char const *path = menu_layout_path();

	if (menu_layout.file || !path[0])
		return;
	if (menu_layout.frames++ % MENU_LAYOUT_EVERY != MENU_LAYOUT_EVERY - 1)
		return;
	snprintf(menu_layout.temporary, sizeof(menu_layout.temporary), "%s.part", path);
	menu_layout.file = fopen(menu_layout.temporary, "w");
	if (!menu_layout.file)
	{
		static int reported = 0;

		if (!reported)
			platform_log("menus: debug.menu_layout: cannot write %s", menu_layout.temporary);
		reported = 1;
		return;
	}
	menu_layout.frame++;
	menu_layout.items = 0;
	menu_layout.widget_count = 0;
	menu_layout.current = -1;
	fprintf(menu_layout.file, "{\"frame\":%ld,\"prompts\":\"%s\",\"touch_menus\":%s,\"screen\":[%ld,0,%ld,480],"
		"\"items\":[", menu_layout.frame, halo_keyboard_prompts() ? "keyboard" : "controller",
		ui_widget_port_touch_menus() ? "true" : "false", -(screen_width - 640) / 2,
		640 + (screen_width - 640) / 2);
}

void menu_layout_frame_end(
	char const *screen_name)
{
	char const *path = menu_layout_path();
	int written;

	if (!menu_layout.file)
		return;
	fputs("],\"screen_name\":", menu_layout.file);
	menu_layout_string(screen_name ? screen_name : "");
	fputs("}\n", menu_layout.file);
	written = fclose(menu_layout.file) == 0;
	menu_layout.file = NULL;
	if (!written || rename(menu_layout.temporary, path) != 0)
		remove(menu_layout.temporary);
}

void menu_layout_widget(
	void const *widget,
	void const *parent,
	char const *name,
	short type,
	rectangle2d const *box,
	rectangle2d const *clip,
	int focused)
{
	if (!menu_layout.file || menu_layout.widget_count >= MENU_LAYOUT_MAXIMUM_WIDGETS)
		return;
	menu_layout.current = menu_layout.widget_count;
	menu_layout.widgets[menu_layout.widget_count++] = widget;
	menu_layout_item("widget");
	fprintf(menu_layout.file, ",\"id\":%ld,\"parent\":%ld,\"type\":%d,\"focused\":%s,\"name\":", menu_layout.current,
		menu_layout_widget_id(parent), type, focused ? "true" : "false");
	menu_layout_string(name);
	menu_layout_rectangle("box", box);
	menu_layout_rectangle("clip", clip);
	fputc('}', menu_layout.file);
}

void menu_layout_widget_none(
	void)
{
	menu_layout.current = -1;
}

/* a rectangle laid out before a scale about its origin, as drawn */
static void menu_layout_scaled(
	rectangle2d const *rectangle,
	real scale,
	real origin_x,
	real origin_y,
	rectangle2d *scaled)
{
	scaled->x0 = (short)(origin_x + (rectangle->x0 - origin_x) * scale);
	scaled->x1 = (short)(origin_x + (rectangle->x1 - origin_x) * scale);
	scaled->y0 = (short)(origin_y + (rectangle->y0 - origin_y) * scale);
	scaled->y1 = (short)(origin_y + (rectangle->y1 - origin_y) * scale);
}

void menu_layout_text(
	rectangle2d const *box,
	rectangle2d const *clip,
	rectangle2d const *extent,
	rectangle2d const *whole,
	wchar_t const *text,
	real scale,
	real origin_x,
	real origin_y)
{
	rectangle2d scaled_box, scaled_clip, scaled_extent;

	if (!menu_layout.file)
		return;
	if (scale <= 0.0f)
		scale = 1.0f;
	menu_layout_scaled(box, scale, origin_x, origin_y, &scaled_box);
	menu_layout_scaled(clip, scale, origin_x, origin_y, &scaled_clip);
	menu_layout_item("text");
	fprintf(menu_layout.file, ",\"widget\":%ld,\"scale\":%.3f,\"text\":", menu_layout.current, (double)scale);
	menu_layout_wide_string(text);
	menu_layout_rectangle("box", &scaled_box);
	menu_layout_rectangle("clip", &scaled_clip);
	/* (none for a string of nothing but spaces) */
	if (extent->x1 >= extent->x0)
	{
		menu_layout_scaled(extent, scale, origin_x, origin_y, &scaled_extent);
		menu_layout_rectangle("extent", &scaled_extent);
	}
	else
	{
		menu_layout_rectangle("extent", NULL);
	}
	if (whole->x1 >= whole->x0)
	{
		menu_layout_scaled(whole, scale, origin_x, origin_y, &scaled_extent);
		menu_layout_rectangle("whole", &scaled_extent);
	}
	else
	{
		menu_layout_rectangle("whole", NULL);
	}
	fputc('}', menu_layout.file);
}

void menu_layout_glyph(
	int key,
	char const *name,
	short x0,
	short y0,
	short x1,
	short y1)
{
	rectangle2d box;

	if (!menu_layout.file)
		return;
	box.x0 = x0;
	box.y0 = y0;
	box.x1 = x1;
	box.y1 = y1;
	menu_layout_item("glyph");
	fprintf(menu_layout.file, ",\"widget\":%ld,\"key\":%s,\"name\":", menu_layout.current, key ? "true" : "false");
	menu_layout_string(name);
	menu_layout_rectangle("box", &box);
	fputc('}', menu_layout.file);
}

void menu_layout_glyph_wide(
	int key,
	wchar_t const *name,
	short x0,
	short y0,
	short x1,
	short y1)
{
	char narrow[64];
	long index;

	for (index = 0; name && name[index] && index < (long)sizeof(narrow) - 1; index++)
		narrow[index] = name[index] < 0x80 ? (char)name[index] : '?';
	narrow[index] = 0;
	menu_layout_glyph(key, narrow, x0, y0, x1, y1);
}

void menu_layout_target(
	void const *widget,
	short kind,
	rectangle2d const *box)
{
	if (!menu_layout.file)
		return;
	menu_layout_item("target");
	fprintf(menu_layout.file, ",\"widget\":%ld,\"target\":%d", menu_layout_widget_id(widget), kind);
	menu_layout_rectangle("box", box);
	fputc('}', menu_layout.file);
}
