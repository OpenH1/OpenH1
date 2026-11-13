/*
The menus' hand-written screens (port/linux/src/menu_screens.c): a screen file
read into its building blocks, checked against the game's actions and
bindings (port/linux/game/menu_screen.c's, as names.inc has them), a file with
a problem refused with its line and the screen read before it kept, texts'
bindings filled, and an item's when. The real main menu's file
(port/assets/menus/screens/main.xml) is main_xml.
*/

#include "harness.h"
#include "expat.h"

#include <stdarg.h>

/* ---------- the fake world */

/* (the platform: a computer, VR's headset when vr_running) */
static int vr_running;

int halo_menus_platform(const char *name, unsigned long length)
{
	static char const *const known[] = { "desktop", "android", "vr", "hdr", "touch", "pointer", "computer" };
	unsigned index;

	for (index = 0; index < sizeof(known) / sizeof(known[0]); index++)
	{
		if (strlen(known[index]) == length && !strncmp(known[index], name, length))
		{
			if (!strncmp(name, "vr", length))
				return vr_running;
			return !strncmp(name, "desktop", length) || !strncmp(name, "computer", length) ||
				!strncmp(name, "pointer", length);
		}
	}
	return -1;
}

struct halo_menus { int unused; };
static struct halo_menus menus;

struct halo_menus const *halo_menus_load(void)
{
	return &menus;
}

/* the menus' files gathered: a screen file of the case's */
static char const *file_paths[4];
static char const *file_texts[4];
static long file_count;

int halo_menus_file_at(long index, char const **path, unsigned char const **data, unsigned long *size)
{
	if (index < 0 || index >= file_count)
		return 0;
	*path = file_paths[index];
	*data = (unsigned char const *)file_texts[index];
	*size = (unsigned long)strlen(file_texts[index]);
	return 1;
}

const char *config_string(const char *name)
{
	(void)name;
	return "";
}

static char logged[8192];

void platform_log(char const *format, ...)
{
	va_list arguments;
	size_t used = strlen(logged);

	va_start(arguments, format);
	vsnprintf(logged + used, sizeof(logged) - used, format, arguments);
	va_end(arguments);
	used = strlen(logged);
	if (used + 1 < sizeof(logged))
	{
		logged[used] = '\n';
		logged[used + 1] = 0;
	}
}

#include "menu_screens.h"
#include "under_test.inc"
#include "names.inc"
#include "main_xml.inc"

/* ---------- the cases */

static struct menu_screen_names const names =
{
	game_actions, sizeof(game_actions) / sizeof(game_actions[0]),
	game_bindings, sizeof(game_bindings) / sizeof(game_bindings[0]),
	game_sources, sizeof(game_sources) / sizeof(game_sources[0]),
};

/* the count of a screen's blocks of a kind (in the block parent; -2: any) */
static int kind_total(struct menu_screen const *screen, int kind, int parent)
{
	int index, count = 0;

	for (index = 0; index < screen->block_count; index++)
		count += screen->blocks[index].kind == kind && (parent == -2 || screen->blocks[index].parent == parent);
	return count;
}

static int kind_first(struct menu_screen const *screen, int kind)
{
	int index;

	for (index = 0; index < screen->block_count; index++)
	{
		if (screen->blocks[index].kind == kind)
			return index;
	}
	return -1;
}

/* a table screen, its table's insides given */
#define TABLE(inside) \
	"<screen name=\"t\"><table source=\"test.rows\">" inside "</table></screen>"
#define COLUMN "<column id=\"server\" label=\"SERVER\" width=\"110\"/>"

static int read_text(char const *text, struct menu_screen *screen, char *problem)
{
	return menu_screen_read("screens/test.xml", (unsigned char const *)text, (unsigned long)strlen(text), screen,
		problem, 512);
}

/* a file that must be refused, its problem naming what */
static void refused(char const *text, char const *what)
{
	struct menu_screen screen;
	char problem[512];

	CHECK(!read_text(text, &screen, problem), "read, but has a problem (%s): %s", what, text);
	CHECK(strstr(problem, what) != NULL, "the problem \"%s\" does not say \"%s\"", problem, what);
	CHECK(strstr(problem, "screens/test.xml:") == problem, "the problem names no file and line: %s", problem);
	CHECK(screen.name == NULL && screen.string_count == 0, "a refused screen keeps something");
}

static struct menu_screen_block const *block_of(struct menu_screen const *screen, char const *id)
{
	int index;

	for (index = 0; index < screen->block_count; index++)
	{
		if (screen->blocks[index].id && !strcmp(screen->blocks[index].id, id))
			return &screen->blocks[index];
	}
	return NULL;
}

/* the values of the bindings: the saved game's, when there is one */
static int saved_game;

static int value(void *context, char const *name, char *out, unsigned long size)
{
	(void)context;
	if (!saved_game)
		return 0;
	if (!strcmp(name, "campaign.level"))
		snprintf(out, size, "%s", "The Silent Cartographer");
	else if (!strcmp(name, "campaign.difficulty"))
		snprintf(out, size, "%s", "Legendary");
	else
		return 0;
	return 1;
}

#define HINTED \
	"<screen name=\"main\"><list><item id=\"campaign\" label=\"CAMPAIGN\" do=\"open campaign\">" \
	"<hint>Continue \xC2\xB7 {campaign.level} \xC2\xB7 {campaign.difficulty}</hint>" \
	"<hint>The story \xC2\xB7 solo, or with friends online</hint></item></list></screen>"

#define GOOD(label) \
	"<screen name=\"main\" back=\"quit\"><title-art/><list focus=\"a\">" \
	"<item id=\"a\" label=\"" label "\" do=\"open multiplayer\"/><item id=\"b\" label=\"QUIT\" do=\"quit\"/></list>" \
	"<space/><buttons><button key=\"accept\" label=\"SELECT\"/></buttons></screen>"

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";
	struct menu_screen screen;
	char problem[512];

	menu_screens_names(&names);
	if (!strcmp(name, "main"))
	{
		struct menu_screen_block const *campaign, *versus, *list;
		int index, items = 0, buttons = 0, kinds[6] = { 0 };

		CHECK(read_text(main_xml, &screen, problem), "the main menu's file is refused: %s", problem);
		CHECK(!strcmp(screen.name, "main"), "its name is %s", screen.name);
		CHECK(screen.back && !strcmp(screen.back, "quit"), "its back is not quit");
		for (index = 0; index < screen.block_count; index++)
		{
			kinds[screen.blocks[index].kind]++;
			items += screen.blocks[index].kind == MENU_SCREEN_ITEM;
			buttons += screen.blocks[index].kind == MENU_SCREEN_BUTTON;
		}
		CHECK(kinds[MENU_SCREEN_TITLE_ART] == 1 && kinds[MENU_SCREEN_LIST] == 1, "not one title art and list");
		campaign = block_of(&screen, "campaign");
		versus = block_of(&screen, "multiplayer");
		CHECK(campaign && versus && block_of(&screen, "settings") && block_of(&screen, "quit"),
			"the four doors are not all there");
		CHECK(!strcmp(campaign->action, "open campaign") && !strcmp(versus->action, "open multiplayer"),
			"CAMPAIGN or MULTIPLAYER does not open its screen");
		CHECK(!strcmp(block_of(&screen, "quit")->action, "quit"), "QUIT does not quit");
		CHECK(campaign->picture && !strcmp(campaign->picture, "main_menu/menu_load_campaign") && versus->picture &&
			!strcmp(versus->picture, "main_menu/menu_multiplayer"), "CAMPAIGN or MULTIPLAYER is not its lettered picture");
		list = &screen.blocks[campaign->parent];
		CHECK(list->kind == MENU_SCREEN_LIST && list->focus && !strcmp(list->focus, "campaign"),
			"the list's focus is not CAMPAIGN");
		CHECK(items == 5 && buttons == 0, "%d items and %d buttons", items, buttons);
		/* (no PRO 2V2) */
		for (index = 0; index < screen.block_count; index++)
		{
			char const *label = screen.blocks[index].label;

			CHECK(!label || !strstr(label, "2V2"), "the main menu has %s", label);
		}
		menu_screen_free(&screen);
	}
	else if (!strcmp(name, "hints"))
	{
		char text[256];
		struct menu_screen_block const *campaign;

		CHECK(read_text(HINTED, &screen, problem), "the hinted screen is refused: %s", problem);
		campaign = block_of(&screen, "campaign");
		/* (no saved game: the first hint's binding has no value, so its line
		is not shown, and the second is) */
		saved_game = 0;
		CHECK(!menu_screen_text(campaign->hints[0], value, NULL, text, sizeof(text)),
			"a hint with a binding of no value is complete");
		CHECK(menu_screen_text(campaign->hints[1], value, NULL, text, sizeof(text)), "the story's hint is not");
		CHECK(!strcmp(text, "The story \xC2\xB7 solo, or with friends online"), "the story's hint is %s", text);
		saved_game = 1;
		CHECK(menu_screen_text(campaign->hints[0], value, NULL, text, sizeof(text)), "the saved game's is not");
		CHECK(!strcmp(text, "Continue \xC2\xB7 The Silent Cartographer \xC2\xB7 Legendary"),
			"the saved game's hint is %s", text);
		/* (too small a room: cut, not overrun) */
		{
			char small[8];

			menu_screen_text(campaign->hints[0], value, NULL, small, sizeof(small));
			CHECK(strlen(small) == 7, "the text overran its room: %s", small);
		}
		menu_screen_free(&screen);
	}
	else if (!strcmp(name, "good"))
	{
		CHECK(read_text(GOOD("VERSUS"), &screen, problem), "refused: %s", problem);
		menu_screen_free(&screen);
	}
	else if (!strcmp(name, "syntax"))
	{
		refused("<screen name=\"main\"><list><item label=\"A\" do=\"quit\"></list></screen>", "mismatched tag");
		refused("", "no element found");
	}
	else if (!strcmp(name, "elements"))
	{
		refused("<screen name=\"main\"><grid/></screen>", "<grid> is not one of");
		refused("<screen name=\"main\"><list><column id=\"a\" width=\"1\"/></list></screen>", "<column> is not one of");
		refused("<screen name=\"main\"><panel><tab id=\"a\" label=\"A\" do=\"quit\"/></panel></screen>",
			"<tab> is not one of");
		refused("<screen name=\"main\"><item label=\"A\" do=\"quit\"/></screen>", "<item> is not one of");
		refused("<screen name=\"main\"><list><item label=\"A\" do=\"quit\" colour=\"red\"/></list></screen>",
			"has no attribute colour");
		refused("<screen><title-art/></screen>", "has no name");
		refused("<screen name=\"main\"/>", "nothing in it");
		refused("<screen name=\"main\"><list/></screen>", "has no <item>");
		refused("<screen name=\"main\"><title-art/><title-art/></screen>", "one <title-art>");
	}
	else if (!strcmp(name, "items"))
	{
		refused("<screen name=\"main\"><list><item do=\"quit\"/></list></screen>", "has no label");
		refused("<screen name=\"main\"><list><item label=\"A\"/></list></screen>", "has no do");
		refused("<screen name=\"main\"><list focus=\"x\"><item id=\"a\" label=\"A\" do=\"quit\"/></list></screen>",
			"focus names no item");
		refused("<screen name=\"main\"><list><item id=\"a\" label=\"A\" do=\"quit\"/>"
			"<item id=\"a\" label=\"B\" do=\"quit\"/></list></screen>", "two items' id");
		refused("<screen name=\"main\"><list><item label=\"A\" when=\"phone\" do=\"quit\"/></list></screen>",
			"when=\"phone\"");
		refused("<screen name=\"main\"><buttons><button key=\"z\" label=\"A\"/></buttons></screen>", "key is");
	}
	else if (!strcmp(name, "actions"))
	{
		refused("<screen name=\"main\"><list><item label=\"A\" do=\"open nowhere\"/></list></screen>",
			"no action \"open nowhere\"");
		refused("<screen name=\"main\" back=\"explode\"><title-art/></screen>", "no action \"explode\"");
		/* (every action the game has is one a file may name) */
		{
			unsigned index;

			for (index = 0; index < sizeof(game_actions) / sizeof(game_actions[0]); index++)
			{
				char text[256];

				snprintf(text, sizeof(text), "<screen name=\"main\"><list><item label=\"A\" do=\"%s\"/></list>"
					"</screen>", game_actions[index]);
				CHECK(read_text(text, &screen, problem), "the game's action %s is refused: %s", game_actions[index],
					problem);
				menu_screen_free(&screen);
			}
		}
	}
	else if (!strcmp(name, "bindings"))
	{
		refused("<screen name=\"main\"><list><item label=\"A {nothing.here}\" do=\"quit\"/></list></screen>",
			"no binding {nothing.here}");
		refused("<screen name=\"main\"><list><item label=\"A\" hint=\"{campaign.level\" do=\"quit\"/></list></screen>",
			"a { with no }");
		refused("<screen name=\"main\"><list><item label=\"A\" do=\"quit\"><hint>{}</hint></item></list></screen>",
			"a binding with no name");
		refused("<screen name=\"main\"><list><item label=\"A}\" do=\"quit\"/></list></screen>", "a } with no {");
	}
	else if (!strcmp(name, "when"))
	{
		vr_running = 0;
		CHECK(menu_screen_when(NULL), "no when does not hold");
		CHECK(!menu_screen_when("vr"), "vr holds out of the headset");
		CHECK(menu_screen_when("vr desktop"), "vr or desktop does not hold on the desktop");
		vr_running = 1;
		CHECK(menu_screen_when("vr"), "vr does not hold in the headset");
	}
	else if (!strcmp(name, "kept"))
	{
		struct menu_screen const *found;
		unsigned long generation;

		/* (the embedded file read, then a broken one in its place: the first
		kept, the problem logged; then a good one: read again) */
		file_paths[0] = "screens/main.xml";
		file_texts[0] = GOOD("VERSUS");
		file_paths[1] = "screens/blocks.xml";
		file_texts[1] = "<menus/>";
		file_count = 2;
		found = menu_screens_find("main");
		CHECK(found && !strcmp(found->blocks[2].label, "VERSUS"), "the embedded screen is not read");
		generation = menu_screens_generation("main");
		CHECK(generation, "no generation");
		CHECK(!strstr(logged, "blocks.xml"), "the blocks' file is read as a screen");
		screens_read("screens/main.xml", (unsigned char const *)"<screen name=\"main\"><list>", 26, 1);
		found = menu_screens_find("main");
		CHECK(found && !strcmp(found->blocks[2].label, "VERSUS"), "the broken file took the screen's place");
		CHECK(menu_screens_generation("main") == generation, "the broken file changed the generation");
		CHECK(strstr(logged, "screens/main.xml:1:") && strstr(logged, "the screen stays as it was"),
			"the problem is not logged: %s", logged);
		screens_read("screens/main.xml", (unsigned char const *)GOOD("FIGHT"), (unsigned long)strlen(GOOD("FIGHT")),
			1);
		found = menu_screens_find("main");
		CHECK(found && !strcmp(found->blocks[2].label, "FIGHT"), "the file read again is not the screen");
		CHECK(menu_screens_generation("main") > generation, "the generation did not change");
		CHECK(!menu_screens_find("versus"), "a screen no file has is found");
	}
	else if (!strcmp(name, "browser"))
	{
		struct menu_screen_block const *table, *server, *ping;
		int table_index, index, searches = 0, touch_searches = 0;

		/* (the real server browser's file: its tabs, its table's columns and
		their searches, its panel and its bar) */
		CHECK(read_text(browser_xml, &screen, problem), "the browser's file is refused: %s", problem);
		CHECK(!strcmp(screen.name, "browser") && screen.back && !strcmp(screen.back, "browser back"),
			"its name or back is not the browser's");
		CHECK(kind_total(&screen, MENU_SCREEN_TAB, -2) == 6, "%d tabs", kind_total(&screen, MENU_SCREEN_TAB, -2));
		table_index = kind_first(&screen, MENU_SCREEN_TABLE);
		CHECK(table_index >= 0, "no table");
		table = &screen.blocks[table_index];
		CHECK(!strcmp(table->source, "browser.games") && !strcmp(table->value, "browser.sort") &&
			!strcmp(table->action, "browser join") && table->empty, "the table's source, sort, do or empty");
		CHECK(kind_total(&screen, MENU_SCREEN_COLUMN, table_index) == 6, "%d columns",
			kind_total(&screen, MENU_SCREEN_COLUMN, table_index));
		server = block_of(&screen, "server");
		ping = block_of(&screen, "ping");
		CHECK(server && server->cell == MENU_SCREEN_CELL_STRONG && server->width == 110 && server->grow == 64 &&
			server->touch_width == 220 && server->title_width == 76, "the server's column");
		CHECK(ping && ping->cell == MENU_SCREEN_CELL_PING && ping->right, "the ping's column");
		CHECK(block_of(&screen, "players")->gap == 2 && block_of(&screen, "map")->gap == 4, "the columns' gaps");
		CHECK(block_of(&screen, "favorite")->cell == MENU_SCREEN_CELL_STAR &&
			!strcmp(block_of(&screen, "favorite")->action, "browser favorite"), "the star's column");
		for (index = 0; index < screen.block_count; index++)
		{
			if (screen.blocks[index].kind != MENU_SCREEN_SEARCH)
				continue;
			if (screen.blocks[index].parent < 0)
				touch_searches++;
			else
				searches++;
		}
		CHECK(searches == 2 && touch_searches == 1, "%d columns' searches and %d of the screen's", searches,
			touch_searches);
		CHECK(kind_total(&screen, MENU_SCREEN_PANEL, -2) == 1 && kind_total(&screen, MENU_SCREEN_PILL, -2) == 3 &&
			kind_total(&screen, MENU_SCREEN_PAIR, -2) == 2, "the panel");
		CHECK(kind_total(&screen, MENU_SCREEN_BUTTON, kind_first(&screen, MENU_SCREEN_BUTTONS)) == 8, "the bar's buttons");
		menu_screen_free(&screen);
	}
	else if (!strcmp(name, "tabs"))
	{
		CHECK(read_text("<screen name=\"t\"><tabs current=\"test.value\"><tab id=\"a\" label=\"A\" do=\"quit\"/>"
			"</tabs></screen>", &screen, problem), "good tabs refused: %s", problem);
		menu_screen_free(&screen);
		refused("<screen name=\"t\"><tabs/></screen>", "has no <tab>");
		refused("<screen name=\"t\"><tabs><tab label=\"A\" do=\"quit\"/></tabs></screen>", "<tab> has no id");
		refused("<screen name=\"t\"><tabs><tab id=\"a\" do=\"quit\"/></tabs></screen>", "<tab> has no label");
		refused("<screen name=\"t\"><tabs><tab id=\"a\" label=\"A\"/></tabs></screen>", "<tab> has no do");
		refused("<screen name=\"t\"><tabs><tab id=\"a\" label=\"A\" do=\"quit\"/><tab id=\"a\" label=\"B\" "
			"do=\"quit\"/></tabs></screen>", "id is \"a\"");
		refused("<screen name=\"t\"><tabs current=\"nothing.here\"><tab id=\"a\" label=\"A\" do=\"quit\"/></tabs>"
			"</screen>", "no binding {nothing.here}");
		refused("<screen name=\"t\"><tabs if=\"nothing.here\"><tab id=\"a\" label=\"A\" do=\"quit\"/></tabs>"
			"</screen>", "no binding {nothing.here}");
		refused("<screen name=\"t\"><tabs><tab id=\"a\" label=\"A\" do=\"quit\"/></tabs><tabs><tab id=\"b\" "
			"label=\"B\" do=\"quit\"/></tabs></screen>", "one <tabs>");
	}
	else if (!strcmp(name, "table"))
	{
		struct menu_screen_block const *column;

		CHECK(read_text(TABLE(COLUMN "<column id=\"ping\" kind=\"ping\" width=\"54\" align=\"right\" "
			"touch=\"272 30 80 144 0\"><search value=\"test.value\"/></column>"), &screen, problem),
			"a good table refused: %s", problem);
		column = block_of(&screen, "ping");
		CHECK(column && column->right && column->touch_x == 272 && column->touch_y == 30 && column->touch_width == 80 &&
			column->touch_move == 144 && column->touch_grow == 0 && column->gap == 4, "the ping's column as read");
		menu_screen_free(&screen);
		refused("<screen name=\"t\"><table>" COLUMN "</table></screen>", "has no source");
		refused("<screen name=\"t\"><table source=\"nowhere\">" COLUMN "</table></screen>",
			"no row source \"nowhere\"");
		refused(TABLE(""), "has no <column>");
		refused(TABLE("<column label=\"A\" width=\"10\"/>"), "<column> has no id");
		refused(TABLE("<column id=\"a\"/>"), "<column> has no width");
		refused(TABLE("<column id=\"a\" width=\"wide\"/>"), "width is a number");
		refused(TABLE("<column id=\"a\" width=\"10\" kind=\"bars\"/>"), "kind is text, strong, ping or star");
		refused(TABLE("<column id=\"a\" width=\"10\" align=\"center\"/>"), "align is left or right");
		refused(TABLE("<column id=\"a\" width=\"10\" touch=\"1 2\"/>"), "touch is");
		refused(TABLE("<column id=\"a\" width=\"10\" sort=\"sort nowhere\"/>"), "no action \"sort nowhere\"");
		refused(TABLE(COLUMN COLUMN), "id is \"server\"");
		refused(TABLE("<column id=\"a\" width=\"10\"><search/></column>"), "<search> has no value");
		refused(TABLE("<column id=\"a\" width=\"10\"><search value=\"nothing.here\"/></column>"),
			"no binding {nothing.here}");
		refused("<screen name=\"t\"><table source=\"test.rows\" sorted=\"nothing.here\">" COLUMN "</table>"
			"</screen>", "no binding {nothing.here}");
		refused("<screen name=\"t\"><table source=\"test.rows\" empty=\"{nothing.here}\">" COLUMN "</table>"
			"</screen>", "no binding {nothing.here}");
		refused("<screen name=\"t\">" "<table source=\"test.rows\">" COLUMN "</table>"
			"<table source=\"test.rows\">" COLUMN "</table></screen>", "one <table>");
	}
	else if (!strcmp(name, "panel"))
	{
		CHECK(read_text("<screen name=\"t\"><panel><heading text=\"{test.value}\" lines=\"2\"/>"
			"<text text=\"A\" lines=\"3\" touch-lines=\"4\"/><pills><pill text=\"B\" tone=\"test.value\" "
			"width=\"64\"/><pill text=\"C\" tone=\"2\" width=\"64\"/></pills><pair label=\"P\" value=\"1\"/>"
			"<button key=\"accept\" label=\"JOIN\" do=\"quit\"/></panel></screen>", &screen, problem),
			"a good panel refused: %s", problem);
		CHECK(screen.blocks[kind_first(&screen, MENU_SCREEN_PANEL_TEXT)].lines == 3 &&
			screen.blocks[kind_first(&screen, MENU_SCREEN_PANEL_TEXT)].touch_lines == 4, "a text's lines");
		menu_screen_free(&screen);
		refused("<screen name=\"t\"><panel/></screen>", "has nothing in it");
		refused("<screen name=\"t\"><panel><text/></panel></screen>", "<text> has no text");
		refused("<screen name=\"t\"><panel><text text=\"A\" lines=\"9\"/></panel></screen>", "lines is a number");
		refused("<screen name=\"t\"><panel><pills/></panel></screen>", "has no <pill>");
		refused("<screen name=\"t\"><panel><pills><pill text=\"A\"/></pills></panel></screen>", "has no width");
		refused("<screen name=\"t\"><panel><pills><pill text=\"A\" width=\"64\" tone=\"nothing.here\"/></pills>"
			"</panel></screen>", "no binding {nothing.here}");
		refused("<screen name=\"t\"><panel><pills><pill text=\"A\" width=\"64\" tone=\"99\"/></pills></panel>"
			"</screen>", "tone is a number");
		refused("<screen name=\"t\"><panel><pair label=\"A\"/></panel></screen>", "<pair> has no value");
		refused("<screen name=\"t\"><panel><pair value=\"A\"/></panel></screen>", "<pair> has no label");
	}
	else if (!strcmp(name, "buttons"))
	{
		static char const *const keys[] = { "accept", "back", "x", "y", "lb", "rb", "lt", "rt", "r3", "view" };
		unsigned index;

		for (index = 0; index < sizeof(keys) / sizeof(keys[0]); index++)
		{
			char text[256];

			snprintf(text, sizeof(text), "<screen name=\"t\"><buttons><button key=\"%s\" label=\"A\" width=\"90\" "
				"if=\"test.flag\" do=\"quit\"/></buttons></screen>", keys[index]);
			CHECK(read_text(text, &screen, problem), "the key %s refused: %s", keys[index], problem);
			menu_screen_free(&screen);
		}
		refused("<screen name=\"t\"><buttons><button key=\"l3\" label=\"A\"/></buttons></screen>", "key is");
		refused("<screen name=\"t\"><buttons><button key=\"x\" label=\"A\" do=\"nowhere\"/></buttons></screen>",
			"no action \"nowhere\"");
		refused("<screen name=\"t\"><buttons><button key=\"x\" label=\"A\" if=\"nothing.here\"/></buttons></screen>",
			"no binding {nothing.here}");
		refused("<screen name=\"t\"><buttons><button key=\"x\" label=\"A\" width=\"0\"/></buttons></screen>",
			"width is a number");
		refused("<screen name=\"t\"><header title=\"T\"><button key=\"view\" label=\"{nothing.here}\"/></header>"
			"</screen>", "no binding {nothing.here}");
	}
	else
	{
		fprintf(stderr, "no case %s\n", name);
		return 2;
	}
	return 0;
}
