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
};

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
	"<item id=\"a\" label=\"" label "\" do=\"open versus\"/><item id=\"b\" label=\"QUIT\" do=\"quit\"/></list>" \
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
		versus = block_of(&screen, "versus");
		CHECK(campaign && versus && block_of(&screen, "settings") && block_of(&screen, "quit"),
			"the four doors are not all there");
		CHECK(!strcmp(campaign->action, "open campaign") && !strcmp(versus->action, "open versus"),
			"CAMPAIGN or VERSUS does not open its screen");
		CHECK(!strcmp(block_of(&screen, "quit")->action, "quit"), "QUIT does not quit");
		CHECK(campaign->picture && !strcmp(campaign->picture, "main_menu/menu_load_campaign") && versus->picture &&
			!strcmp(versus->picture, "main_menu/menu_versus"), "CAMPAIGN or VERSUS is not its lettered picture");
		list = &screen.blocks[campaign->parent];
		CHECK(list->kind == MENU_SCREEN_LIST && list->focus && !strcmp(list->focus, "campaign"),
			"the list's focus is not CAMPAIGN");
		CHECK(items == 5 && buttons == 0, "%d items and %d buttons", items, buttons);
		/* (no PRO 2V2 and no MULTIPLAYER) */
		for (index = 0; index < screen.block_count; index++)
		{
			char const *label = screen.blocks[index].label;

			CHECK(!label || (!strstr(label, "2V2") && strcmp(label, "MULTIPLAYER")), "the main menu has %s", label);
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
		refused("<screen name=\"main\"><table/></screen>", "<table> is not one of");
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
	else
	{
		fprintf(stderr, "no case %s\n", name);
		return 2;
	}
	return 0;
}
