/*
MENU_TOUR.C

debug.menu_tour: one run of the game walks the menu screens a list names
(a file in the data root, a screen a line, as port/assets/menus names it:
main_menu/settings_select/...), for tools/menu_gallery.py. Once the menus
are up, each screen in turn is opened alone (the others closed, a player
profile being edited, as debug.menu_open opens one), shown for
debug.menu_tour_frames frames, and then its layout written
(menu_layout.c: tour/<n>.json in the data root, n its line, from 0) and,
where frames are drawn, its picture saved (debug.screenshot_directory's
tour-<n>.bmp); after the last the game quits. A screen that cannot be
opened is logged and has neither. Its run is its own: the screens are
opened as no player could open them, one over the state another left.
*/

#include "cseries.h"
#include "interface/ui_widget.h"
#include "interface/virtual_keyboard.h"
#include "menu_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int config_integer(char const *name);
char const *config_string(char const *name);
void platform_log(char const *format, ...);
void halo_screenshot_named(const char *name);
extern boolean pc_menu_profile_edit_begin(void);

#define MENU_TOUR_MAXIMUM_SCREENS 512
#define MENU_TOUR_NAME_LENGTH 256

static struct
{
	int read;
	long count;
	long index;
	long frame;
	char (*names)[MENU_TOUR_NAME_LENGTH];
} menu_tour;

/* the list's screens, read once; FALSE for no tour */
static boolean menu_tour_read(
	void)
{
	char const *path = config_string("debug.menu_tour");
	char line[MENU_TOUR_NAME_LENGTH];
	FILE *file;

	if (menu_tour.read)
		return menu_tour.count > 0;
	menu_tour.read = 1;
	if (!path[0])
		return FALSE;
	file = fopen(path, "r");
	if (!file)
	{
		platform_log("menus: debug.menu_tour: cannot read %s", path);
		return FALSE;
	}
	menu_tour.names = malloc(MENU_TOUR_MAXIMUM_SCREENS * sizeof(*menu_tour.names));
	while (menu_tour.names && menu_tour.count < MENU_TOUR_MAXIMUM_SCREENS && fgets(line, sizeof(line), file))
	{
		size_t length = strcspn(line, "\r\n");

		line[length] = 0;
		if (length)
			snprintf(menu_tour.names[menu_tour.count++], MENU_TOUR_NAME_LENGTH, "%s", line);
	}
	fclose(file);
	platform_log("menus: debug.menu_tour: %ld screens", menu_tour.count);
	return menu_tour.count > 0;
}

/* the screen of the menu files' name opened alone; FALSE where it is not
one of theirs */
static boolean menu_tour_open(
	char const *name)
{
	char tag_name[MENU_TOUR_NAME_LENGTH + 8];
	long index;

	snprintf(tag_name, sizeof(tag_name), "pc\\%s", name);
	for (index = 0; tag_name[index]; index++)
	{
		if (tag_name[index] == '/')
			tag_name[index] = '\\';
	}
	if (virtual_keyboard_active())
		virtual_keyboard_close();
	ui_widgets_close_all();
	pc_menu_profile_edit_begin();
	return ui_widget_load_by_name_or_tag(tag_name, NONE, NULL, NONE, NONE, NONE, NONE) != NULL;
}

/* (each frame of the menus, as they update: ui_widget.c) */
void menu_tour_update(
	void)
{
	long frames;
	char name[64];

	if (!menu_tour_read())
		return;
	frames = config_integer("debug.menu_tour_frames");
	if (frames < 1)
		frames = 1;
	if (menu_tour.index >= menu_tour.count)
	{
		/* (the last picture saved as the frame before was presented) */
		platform_log("menus: debug.menu_tour: done");
		exit(EXIT_SUCCESS);
	}
	if (menu_tour.frame == 0 && !menu_tour_open(menu_tour.names[menu_tour.index]))
	{
		platform_log("menus: debug.menu_tour: cannot open %s", menu_tour.names[menu_tour.index]);
		menu_tour.index++;
		return;
	}
	menu_tour.frame++;
	if (menu_tour.frame == frames)
	{
		/* (this frame's drawing: its layout and its picture) */
		snprintf(name, sizeof(name), "tour/%ld.json", menu_tour.index);
		menu_layout_request(name);
		snprintf(name, sizeof(name), "tour-%ld", menu_tour.index);
		halo_screenshot_named(name);
	}
	else if (menu_tour.frame > frames)
	{
		/* (a screen that drew no menus, under the keyboard: none) */
		menu_layout_request(NULL);
		menu_tour.frame = 0;
		menu_tour.index++;
	}
}
