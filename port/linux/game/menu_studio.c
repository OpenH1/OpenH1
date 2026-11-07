/*
MENU_STUDIO.C

debug.menu_studio: the game as the menus' builder, so that what is seen
working on them is what the game draws (no other renderer to drift from
it). The game starts on the setting's screen of the menus (as
debug.menu_open, pc_menus_root_name), and then:

- the screen is reloaded within a second of the menu files changing (their
  folder, debug.menu_files' or the menus folder beside config.toml, looked
  at twice a second: halo_menus_files_stamp), or on F7: the screens are
  closed, the menus' tags let go and built again from the files read anew
  (menu_tags.c, menu_files.c), and the screen opened again with the focus
  where it was. Each reload is logged with its time, and, with
  debug.screenshot_directory, its first frame saved (studio-<n>.bmp);
- F3 steps the input the menus are drawn for: the keyboard and mouse's
  prompts, the controller's, the touchscreen's layout (debug.touch_menus,
  its screens: a reload) with the controller's prompts;
- F4 steps the window's shape through the gallery's (tools/menu_gallery.py):
  16:9, 16:10, a phone's 20:9, 4:3, 21:9;
- F5 makes every label of the menus longer by half (each word's first
  letters again), to see where a longer one would not fit;
- F6 outlines each widget's box and the problems the layout's checks find
  (menu_layout.c's menu_layout_overlay: tools/menu_gallery.py's checks).

A line at the top says which, and the problems' count. The settings these
change are this run's alone (config_set_for_run). In a build without
HALO_RELEASE the keys work in any game's menus too, without the setting.
*/

#include "cseries.h"
#include "math/integer_math.h"
#include "menu_layout.h"

#include <stdio.h>
#include <string.h>

int config_set_for_run(const char *name, const char *value);
int config_boolean(char const *name);
char const *config_string(char const *name);
void platform_log(char const *format, ...);
int platform_menu_studio_keys(void);
void platform_display_apply(void);
void halo_screenshot_named(const char *name);
unsigned long system_milliseconds(void);
void halo_menus_reload(void);
unsigned long long halo_menus_files_stamp(void);
void menu_tags_unloaded(void);
void menu_tags_loaded(char const *map_name);
void menu_tags_files_changed(void);
boolean ui_widgets_active(void);
/* (ui_widget.c) */
char const *ui_widget_port_screen_name(void);
long ui_widget_port_focus_path(short *path, long maximum);
void ui_widget_port_focus_restore(short const *path, long count);
boolean ui_widget_port_open_screen(char const *tag_name);

/* the frames between two looks at the files (twice a second at 60) */
#define MENU_STUDIO_WATCH_FRAMES 30
#define MENU_STUDIO_FOCUS_DEPTH 16

enum
{
	_menu_studio_keyboard,
	_menu_studio_controller,
	_menu_studio_touch,
	NUMBER_OF_MENU_STUDIO_INPUTS
};

static char const *const menu_studio_input_names[NUMBER_OF_MENU_STUDIO_INPUTS] =
{
	"keyboard", "controller", "touch"
};

/* the gallery's shapes, as windows (points) that fit a laptop's screen */
static struct
{
	char const *name;
	char const *size;
} const menu_studio_shapes[] =
{
	{ "16:9", "1280x720" },
	{ "16:10", "1280x800" },
	{ "phone 20:9", "1600x720" },
	{ "4:3", "1024x768" },
	{ "21:9", "1680x720" },
};

static struct
{
	boolean started;
	boolean on;
	long frame;
	unsigned long long stamp;
	short input;
	short shape;
	boolean long_text;
	boolean overlay;
	boolean drawing;
	long reloads;
	long shot_in;
	unsigned long reloaded_milliseconds;
} menu_studio;

boolean menu_studio_on(
	void)
{
	return config_string("debug.menu_studio")[0] != 0;
}

/* whether the keys work: the studio, or a build with its assertions in the
menus */
static boolean menu_studio_keys_work(
	void)
{
#ifdef HALO_RELEASE
	return menu_studio_on();
#else
	return menu_studio_on() || ui_widgets_active();
#endif
}

/* the open screen built again from its files, the focus where it was */
static void menu_studio_reload(
	char const *why)
{
	unsigned long started = system_milliseconds();
	char name[256];
	short path[MENU_STUDIO_FOCUS_DEPTH];
	long depth;

	snprintf(name, sizeof(name), "%s", ui_widget_port_screen_name());
	depth = ui_widget_port_focus_path(path, MENU_STUDIO_FOCUS_DEPTH);
	ui_widget_port_open_screen(NULL);
	menu_tags_unloaded();
	halo_menus_reload();
	menu_tags_files_changed();
	menu_tags_loaded("ui");
	if (!name[0] || !ui_widget_port_open_screen(name))
	{
		platform_log("menus: studio: %s could not be opened again; the main menu", name[0] ? name : "the screen");
		ui_widget_port_open_screen("pc\\main_menu\\main_menu");
	}
	else
	{
		ui_widget_port_focus_restore(path, depth);
	}
	menu_studio.stamp = halo_menus_files_stamp();
	menu_studio.reloads++;
	menu_studio.reloaded_milliseconds = system_milliseconds() - started;
	menu_studio.shot_in = 2;
	platform_log("menus: studio: %s reloaded (%s) in %lu ms", name, why, menu_studio.reloaded_milliseconds);
}

static void menu_studio_input_apply(
	void)
{
	boolean touch = menu_studio.input == _menu_studio_touch;
	boolean was_touch = config_boolean("debug.touch_menus") != 0;

	config_set_for_run("input.button_prompts",
		menu_studio.input == _menu_studio_keyboard ? "keyboard" : "controller");
	config_set_for_run("debug.touch_menus", touch ? "true" : "false");
	if (touch != was_touch)
		menu_studio_reload("input");
}

/* (each frame of the menus, as they update: ui_widget.c) */
void menu_studio_update(
	void)
{
	int keys;

	if (!menu_studio.started)
	{
		menu_studio.started = TRUE;
		menu_studio.on = menu_studio_on();
		if (menu_studio.on)
		{
			menu_studio.stamp = halo_menus_files_stamp();
			platform_log("menus: studio: F3 input, F4 shape, F5 long labels, F6 boxes, F7 reload");
		}
	}
	keys = platform_menu_studio_keys();
	if (keys && menu_studio_keys_work())
	{
		if (keys & (1 << 0))
		{
			menu_studio.input = (short)((menu_studio.input + 1) % NUMBER_OF_MENU_STUDIO_INPUTS);
			menu_studio_input_apply();
		}
		if (keys & (1 << 1))
		{
			menu_studio.shape = (short)((menu_studio.shape + 1) % NUMBEROF(menu_studio_shapes));
			config_set_for_run("display.window_size", menu_studio_shapes[menu_studio.shape].size);
			platform_display_apply();
		}
		if (keys & (1 << 2))
			menu_studio.long_text = !menu_studio.long_text;
		if (keys & (1 << 3))
		{
			menu_studio.overlay = !menu_studio.overlay;
			menu_layout_keep(menu_studio.overlay);
		}
		if (keys & (1 << 4))
			menu_studio_reload("F7");
	}
	if (menu_studio.on && ++menu_studio.frame % MENU_STUDIO_WATCH_FRAMES == 0 &&
		halo_menus_files_stamp() != menu_studio.stamp)
	{
		menu_studio_reload("its files changed");
	}
	/* (a reload's first frame drawn, saved: its time from the files'
	change to the screen, measured) */
	if (menu_studio.shot_in && !--menu_studio.shot_in && config_string("debug.screenshot_directory")[0])
	{
		char name[32];

		snprintf(name, sizeof(name), "studio-%ld", menu_studio.reloads);
		halo_screenshot_named(name);
	}
}

/* (render_ui_widgets: the menus being drawn, not the studio's own line) */
void menu_studio_drawing(
	boolean drawing)
{
	menu_studio.drawing = drawing;
}

boolean menu_studio_overlay_on(
	void)
{
	return menu_studio.overlay || menu_studio.on;
}

/* a label the menus draw, made longer by half where F5 asks: each word's
first letters again after it (rasterizer_text.c) */
wchar_t const *menu_studio_text(
	wchar_t const *text)
{
	static wchar_t longer[1024];
	long in, out = 0, word = 0;

	if (!menu_studio.long_text || !menu_studio.drawing || !text)
		return text;
	for (in = 0; text[in] && out < (long)NUMBEROF(longer) - 1; in++)
	{
		boolean letter = text[in] != ' ' && text[in] != '\r' && text[in] != '\n' && text[in] != '%' &&
			text[in] != '\t';

		if (!letter && word)
		{
			long extra, start = in - word;

			for (extra = 0; extra < (word + 1) / 2 && out < (long)NUMBEROF(longer) - 1; extra++)
				longer[out++] = text[start + extra];
			word = 0;
		}
		/* (an icon's %name stays as it is) */
		if (text[in] == '%')
		{
			while (text[in] && text[in] != ' ' && out < (long)NUMBEROF(longer) - 1)
				longer[out++] = text[in++];
			in--;
			continue;
		}
		word = letter ? word + 1 : 0;
		longer[out++] = text[in];
	}
	if (word)
	{
		long extra, start = in - word;

		for (extra = 0; extra < (word + 1) / 2 && out < (long)NUMBEROF(longer) - 1; extra++)
			longer[out++] = text[start + extra];
	}
	longer[out] = 0;

	return longer;
}

/* the studio's line: what it draws the menus for, and the problems */
void menu_studio_status(
	wchar_t *line,
	long size,
	long problems)
{
	char text[256];
	long index;

	snprintf(text, sizeof(text), "STUDIO  %s  %s%s  |  F3 input  F4 shape  F5 long  F6 boxes  F7 reload%s",
		menu_studio_input_names[menu_studio.input], menu_studio_shapes[menu_studio.shape].name,
		menu_studio.long_text ? "  long labels" : "",
		menu_studio.overlay ? (problems ? "  |  problems: " : "  |  no problems") : "");
	for (index = 0; text[index] && index < size - 16; index++)
		line[index] = (unsigned char)text[index];
	if (menu_studio.overlay && problems)
	{
		char count[16];
		long digit;

		snprintf(count, sizeof(count), "%ld", problems);
		for (digit = 0; count[digit] && index < size - 1; digit++)
			line[index++] = count[digit];
	}
	line[index] = 0;
}
