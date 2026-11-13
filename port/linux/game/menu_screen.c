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

What the screens name is C's, here: an item's action (do="open multiplayer") is a
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
#include "math/integer_math.h"
#include "text/draw_string.h"
#include "tag_files/tag_groups.h"
#include "text/text_group.h"
#include "interface/player_ui.h"
#include "saved games/player_profile.h"

#include "halo_menus.h"
#include "menu_engine.h"
#include "menu_screens.h"
#include "menu_table.h"
#include "menu_widget.h"
#include "menu_spartan.h"
#include "network_host_controls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void platform_log(char const *format, ...);
char *tag_get_name(long tag_index);
long pc_menu_event_function_index(char const *name);
boolean menu_tags_open(struct widget_instance *widget, struct event_record *event, char const *screen,
	boolean *deleted);
void menu_tags_back(struct widget_instance *widget, struct event_record *event, boolean *deleted);
long menu_tags_background(long tag_index);
long menu_tags_flags(long tag_index);
short menu_tags_legend_growth(long legends_tag);
unsigned long menu_tags_pause_rows(void);
int halo_keyboard_prompts(void);
boolean config_boolean(char const *name);
struct widget_instance *ui_widget_port_add_child(struct widget_instance *parent, long tag_index,
	struct widget_instance *before);
/* (menu_functions.c) */
boolean pc_menu_campaign_saved(char *level, char *difficulty, long size);
void pc_menu_browser_visit(char const *tab);
int pc_menu_browser_visit_kind(void);
void pc_menu_main_menu_shown(struct widget_instance *screen);
int halo_keyboard_prompts(void);
long ui_widget_port_text_box_font(struct widget_instance *widget, short *width);
short ui_widget_port_past_move(void);
boolean ui_widget_port_pointer_focused(void);
void event_manager_post_button(short controller_index, short button_index);
short ui_widget_port_wide_scale(short value, short extra);
short ui_widget_port_past_scale(short value, short past);
short ui_widget_port_wide_extra(void);
short ui_widget_port_past_extra(void);
short ui_widget_port_foot_lift(void);
int ui_widget_port_touch_menus(void);
boolean ui_widget_port_hover_focused(void);
boolean ui_widget_port_take_click(short *x, short *y);
/* (menu_functions.c: the server browser's actions, for the hand-written
browser's) */
boolean pc_menu_browser_tab(char const *word);
boolean pc_menu_browser_sort(char const *word);
boolean pc_menu_browser_favorite(short row);
boolean pc_menu_browser_select(struct widget_instance *widget, struct event_record *event, short row,
	boolean *deleted);

/* ---------- constants */

#define UI_WIDGET_DEFINITION_TAG 'DeLa'

/* the screens open at once (as the engine's: ENGINE_MAXIMUM_SCREENS), and
the most items a screen's list makes */
#define SCREEN_MAXIMUM_OPEN 8
#define SCREEN_MAXIMUM_ITEMS 24
#define SCREEN_ID_LENGTH 32
#define SCREEN_ACTION_LENGTH 48
#define SCREEN_MAXIMUM_BUTTONS 4

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
/* cards (blocks.xml's screens/card: the campaign's mission card, 158 by
240): their gap, and their row's room, below them too */
#define CARD_WIDTH 158
#define CARD_GAP 24
#define CARDS_ROOM 260
/* (the card picture's own height, and the margin above and below the cards
filling their room) */
#define CARD_HEIGHT 240
#define CARD_MARGIN 10
/* a card's title drawn larger than its font (large_ui's) */
#define CARD_TITLE_SCALE 1.4f
/* an inner screen's header's room (its picture where the campaign menu's is),
or the tall layout's band's (port_settings.py's TALL_BAND) */
#define HEADER_ROOM 80
#define HEADER_ROOM_TALL 40
/* the header's words drawn larger than their font (the heading's), as the
tall layout's titles (TALL_TITLE_SCALE) */
#define HEADER_TITLE_SCALE 1.4f
/* the button bar: its room at the foot (port_settings.py's TALL_FOOTER), and
its prompts as the tall layout's footer's (Choose Mission's: port_settings.py's
_missions), from the room's top, the screen's margin (WIDE_MARGIN) at either
side, each as wide as its block (BACK's at the left, the rest's at the
right, this far apart) */
#define BUTTONS_ROOM 37
#define BUTTONS_MARGIN 24
#define PROMPTS_TOP 5
#define PROMPT_TOP 1
#define PROMPT_WIDTH 124
#define PROMPT_LEFT_WIDTH 96
#define PROMPT_GAP 8
/* an item's label drawn larger than its font (large_ui's): at 4:3, and
at 16:9 and wider */
#define ITEM_SCALE_LEAST 1.45f
#define ITEM_SCALE_MOST 1.6f
/* (menu_tags.c's WIDE_LAYOUT_EXTRA: 16:9's width past 640) */
#define WIDE_LAYOUT_EXTRA 212
/* the profile's colors (<colors>): the PC version's picker's, as many as
the game has, each an item of blocks.xml's screens/color, one under another
at the screen's left; their names the picker's list's */
#define SCREEN_COLOR_COUNT 18
#define SCREEN_COLOR_STEP 19
#define SCREEN_COLOR_X 96
#define SCREEN_COLORS_NAMES "pc\\main_menu\\settings_select\\player_setup\\player_profile_edit\\color_edit\\colors_list"
/* the original picture of a color (blocks.xml's screens/spartan: ui.map's
player_color_marine_large, 143 by 294, a frame a color and the last
random), drawn where no Spartan can be */
#define SCREEN_SPARTAN_PICTURE_WIDTH 143
#define SCREEN_SPARTAN_PICTURE_HEIGHT 294
#define SCREEN_SPARTAN_RANDOM_FRAME 18
/* (menu_functions.c's BUTTON_B, BUTTON_X, BUTTON_Y: the buttons a prompt presses) */
/* a <box>'s layout, the Xbox's pause menu's as the port has it (menu_tags.c's
PAUSE_BOX_*): its first row this far below its top, its rows this far apart
and this tall, its legend this far below its last row, its foot this far
below the legend's top; the frame's left and right pieces' width, its first
frame's rows; its rows' width; and the objectives' box beside it: this far
from its right, as wide, its frame's first rows, its title's and its text's
places in it */
#define BOX_ROWS_TOP 9
#define BOX_ROW_PITCH 30
#define BOX_ROW_HEIGHT 27
#define BOX_LEGEND_GAP 14
#define BOX_LEGEND_BOTTOM 25
#define BOX_SIDE 16
#define BOX_FIRST_ROWS 3
#define BOX_FRAMES 5
#define BOX_ROW_WIDTH 202
#define OBJECTIVES_GAP 5
#define OBJECTIVES_MIDDLE 253
#define OBJECTIVES_FIRST_ROWS 5
#define OBJECTIVES_FRAMES 3
#define OBJECTIVES_X 15
#define OBJECTIVES_TITLE_Y 4
#define OBJECTIVES_TEXT_Y 33
/* the parts a box moves as its legend grows (its frame's three, the
objectives' five) */
#define BOX_MAXIMUM_PARTS 8
/* a widget definition's flag: the game paused while it is open */
#define SCREEN_PAUSE_GAME_FLAG 0x2
/* the Xbox's legend row under its pause menu, which a <legend> shows */
#define BOX_LEGENDS "ui\\shell\\main_menu\\button_key_sm"
/* the Xbox's pause menu's rows' frame, framing the focused one */
#define BOX_ROW_PICTURE "ui\\shell\\bitmaps\\menu_bkds"

#define SCREEN_BUTTON_B 1
#define SCREEN_BUTTON_X 2
#define SCREEN_BUTTON_Y 3
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
	/* (the server browser's tab for this visit: "all", "coop"; NULL, the
	player's own) */
	char const *browser_tab;
	char const *before;
	char const *run;
	/* (run once run has succeeded, before the screen opens: the campaign's
	state read, as its menu's opening read it) */
	char const *then;
	char const *open;
	char const *otherwise;
	/* (C's own, run in place of the rest: an action of a hand-written
	screen's blocks, its name given, and the row of a table it is for,
	screen_action_row) */
	boolean (*call)(struct widget_instance *widget, struct event_record *event, char const *name, boolean *deleted);
};

static boolean screen_call_browser(struct widget_instance *widget, struct event_record *event, char const *name,
	boolean *deleted);
static boolean screen_call_search(struct widget_instance *widget, struct event_record *event, char const *name,
	boolean *deleted);
static boolean screen_call_profile(struct widget_instance *widget, struct event_record *event, char const *name,
	boolean *deleted);
static boolean screen_call_lobby(struct widget_instance *widget, struct event_record *event, char const *name,
	boolean *deleted);
static boolean screen_call_close(struct widget_instance *widget, struct event_record *event, char const *name,
	boolean *deleted);
/* the row of the table an action is run for (0 up, on the table), NONE for
none (a key's, a button's) */
static short screen_action_row = NONE;

static struct screen_action const screen_actions[] =
{
	/* (the Campaign screen, screens/campaign.xml) */
	{ "open campaign", NULL, NULL, NULL, NULL, "screens/campaign", NULL },
	/* (Choose Mission, the campaign's missions as cards, as the campaign
	menu's CHOOSE MISSION opens it once the menu has read the profile's
	campaign: a profile made first if there is none, as the PC version's
	CAMPAIGN, and then Choose Mission at once: port_settings.py's
	_solo_new_profile) */
	{ "open solo", NULL, NULL, "new game if no plyr profiles", "port campaign init", "main_menu/campaign/mission_screen",
		"main_menu/campaign/solo_creating_profile" },
	/* (the server browser, as Multiplayer's SERVER BROWSER opens it: its mode
	the browser's; every game, or the co-op games, this visit) */
	{ "open multiplayer", "all", "mp type set mode", "new game if no plyr profiles", NULL,
		"main_menu/multiplayer_type_select/join_game/join_game_screen",
		"main_menu/profile_manager/multiplayer_creating_profile" },
	{ "open coop", "coop", "mp type set mode", "new game if no plyr profiles", NULL,
		"main_menu/multiplayer_type_select/join_game/join_game_screen",
		"main_menu/profile_manager/multiplayer_creating_profile" },
	/* (the browser's HOST & MORE: the PC version's Multiplayer screen, the
	hub; its HOST CO-OP: the hub's co-op page, online or split screen) */
	{ "open host", NULL, NULL, NULL, NULL, "main_menu/multiplayer_type_select/multiplayer_type_select_screen", NULL },
	{ "open host coop", NULL, NULL, NULL, NULL, "main_menu/multiplayer_type_select/hub/coop_screen", NULL },
	{ "open settings", NULL, NULL, "port settings begin", NULL,
		"main_menu/settings_select/player_setup/player_profile_edit/video_settings/video_settings_screen", NULL },
	{ "open training", NULL, NULL, NULL, NULL, "main_menu/training/training_screen", NULL },
	/* (the quit question, as QUIT and B on the main menu open it) */
	{ "quit", NULL, NULL, NULL, NULL, "main_menu/quit_select/quit_screen", NULL },
	/* (a screen's searches, <search>: the next one typed into, and every
	one's words cleared) */
	{ "search next", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_search },
	{ "search clear", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_search },
	/* (the server browser's, screens/browser.xml: its back (a search typed
	into cleared, else the browser's own back), its HOST button (the hub,
	or co-op's page), its tabs, its sort (a column's, X the next), a game
	made a favorite, joined (a row, or JOIN GAME), the list asked for again
	(Join by Link's: the link copied joined), its Filters page) */
	{ "browser back", NULL, NULL, "gamespy back handler", NULL, NULL, NULL },
	{ "browser host", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_browser },
	{ "browser tab all", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_browser },
	{ "browser tab dedicated", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_browser },
	{ "browser tab openh1", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_browser },
	{ "browser tab legacy", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_browser },
	{ "browser tab coop", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_browser },
	{ "browser tab favorites", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_browser },
	{ "browser sort players", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_browser },
	{ "browser sort ping", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_browser },
	{ "browser sort server", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_browser },
	{ "browser sort map", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_browser },
	{ "browser sort mode", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_browser },
	{ "browser sort next", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_browser },
	{ "browser favorite", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_browser },
	{ "browser join", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_browser },
	{ "browser refresh", NULL, NULL, "port browser refresh", NULL, NULL, NULL },
	{ "browser filters", NULL, NULL, "port browser filters", NULL,
		"main_menu/settings_select/player_setup/player_profile_edit/network_setup/browser_filters/browser_filters_screen",
		NULL },
	/* (Select Profile's, screens/profile_select.xml, as the generated
	screen's: a row's profile made player 1's and back to the screen
	before, or on the row that makes one its question; CREATE NEW's (Y) a
	new profile's name asked; DELETE's (X) its question, for the chosen
	row's profile) */
	{ "profile select", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_profile },
	{ "profile new", NULL, NULL, "new campaign chosen", NULL, "main_menu/profile_manager/pm_empty_creating_profile",
		NULL },
	{ "profile delete", NULL, NULL, "request del player profile", NULL,
		"error/confirm_delete_profile_modal_fullscreen", NULL },
	/* (the lobby's, screens/lobby.xml, as the generated lobby's
	(menu_functions.c's lobby_menu.inc): a row chosen (a click its lane's
	team, else the host's start), the start, a team (left and right on the
	players, CHANGE TEAM), the invite copied, Server Setup, Host Controls,
	another player of this machine, and the bots' choices stepped) */
	{ "lobby row", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_lobby },
	{ "lobby start", NULL, NULL, "port lobby start", NULL, NULL, NULL },
	{ "lobby team red", NULL, NULL, "port lobby team red", NULL, NULL, NULL },
	{ "lobby team blue", NULL, NULL, "port lobby team blue", NULL, NULL, NULL },
	{ "lobby team toggle", NULL, NULL, "port lobby team toggle", NULL, NULL, NULL },
	{ "lobby invite", NULL, NULL, "port lobby invite", NULL, NULL, NULL },
	{ "lobby settings", NULL, NULL, "port lobby settings", NULL, NULL, NULL },
	{ "lobby host", NULL, NULL, "port host open", NULL, "main_menu/host_controls/host_controls_screen", NULL },
	{ "lobby add player", NULL, NULL, "port lobby add player", NULL, NULL, NULL },
	{ "lobby bots left", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_lobby },
	{ "lobby bots right", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_lobby },
	{ "lobby skill left", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_lobby },
	{ "lobby skill right", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_lobby },
	{ "lobby teams left", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_lobby },
	{ "lobby teams right", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_lobby },
	/* (the lobby's chat: a line begun, the on-screen keyboard's on a
	controller or a touchscreen) */
	{ "lobby chat", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_lobby },
	/* (the pause menu's, screens/pause.xml and pause_campaign.xml, as its
	rows did on the map's: the screen closed (RESUME GAME), or every screen
	(B; the campaign's RESUME GAME); the player out of a multiplayer game
	(LEAVE GAME, the screen closed); the host's Host Controls and END GAME;
	network co-op's FRIENDLY FIRE; the campaign's REVERT TO SAVED and
	RESTART LEVEL, each its question first, and SAVE AND QUIT) */
	{ "close", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_close },
	{ "close all", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_close },
	{ "pause leave", NULL, NULL, NULL, NULL, NULL, NULL, screen_call_close },
	{ "open host controls", NULL, NULL, "port host open", NULL, "main_menu/host_controls/host_controls_screen", NULL },
	{ "pause end game", NULL, NULL, "port pause end game", NULL, NULL, NULL },
	{ "pause friendly fire", NULL, NULL, "port coop friendly fire", NULL, NULL, NULL },
	{ "pause revert", NULL, NULL, NULL, NULL, "ui\\shell\\error\\confirm_restartatsave_modal_fullscreen", NULL },
	{ "pause restart", NULL, NULL, NULL, NULL, "ui\\shell\\error\\confirm_restartlevel_modal_fullscreen", NULL },
	{ "pause quit", NULL, NULL, "pause game return to main menu", NULL, NULL, NULL },
};

static char const *const screen_action_names[] =
{
	"open campaign", "open solo", "open multiplayer", "open coop", "open host", "open host coop", "open settings",
	"open training", "quit", "search next", "search clear", "browser back", "browser host", "browser tab all",
	"browser tab dedicated", "browser tab openh1", "browser tab legacy", "browser tab coop", "browser tab favorites",
	"browser sort players", "browser sort ping", "browser sort server", "browser sort map", "browser sort mode",
	"browser sort next", "browser favorite", "browser join", "browser refresh", "browser filters", "profile select",
	"profile new", "profile delete", "lobby row", "lobby start", "lobby team red", "lobby team blue",
	"lobby team toggle", "lobby invite", "lobby settings", "lobby host", "lobby add player", "lobby bots left",
	"lobby bots right", "lobby skill left", "lobby skill right", "lobby teams left", "lobby teams right",
	"lobby chat", "close", "close all", "pause leave", "open host controls", "pause end game",
	"pause friendly fire", "pause revert", "pause restart", "pause quit",
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
	"campaign.level", "campaign.difficulty", "browser.title", "browser.counts", "browser.empty", "browser.searched",
	"browser.host", "browser.tab", "browser.sort", "browser.sort.title", "browser.search.server", "browser.search.map",
	"browser.search.any", "browser.panel.title", "browser.panel.where", "browser.panel.kind",
	"browser.panel.kind.tone", "browser.panel.tick", "browser.panel.opence", "browser.panel.players",
	"browser.panel.ping", "browser.panel.status", "browser.joinable", "browser.server", "browser.link",
	"browser.filtered", "profile.title", "profile.name", "profile.level", "profile.difficulty", "profile.controls",
	"profile.color", "profile.chosen", "profile.current", "lobby.title", "lobby.subtitle", "lobby.red",
	"lobby.red.count", "lobby.red.tone", "lobby.blue", "lobby.blue.count", "lobby.blue.tone", "lobby.gametype",
	"lobby.map", "lobby.limits", "lobby.kind", "lobby.kind.tone", "lobby.tick", "lobby.opence", "lobby.after",
	"lobby.bots", "lobby.bots.shown", "lobby.bots.locked", "lobby.bots.count.locked", "lobby.skill", "lobby.teams",
	"lobby.status", "lobby.start", "lobby.host", "lobby.settings", "lobby.controls", "lobby.waiting",
	"lobby.team.change", "lobby.invite", "lobby.add", "lobby.chat", "pause.friendly_fire", "pause.host_controls",
	"pause.end_game", "pause.chapters",
};

/* the row sources a table may name (menu_table.h), and their names */
static struct menu_table_source const *const screen_sources[] =
{
	&pc_menu_browser_source,
	&pc_menu_profile_source,
	&pc_menu_lobby_source,
};

static char const *const screen_source_names[] =
{
	"browser.games",
	"profiles",
	"lobby.players",
};

/* the chats a <chat> may name (menu_table.h), and their names */
static struct menu_chat_source const *const screen_chats[] =
{
	&pc_menu_lobby_chat,
};

static char const *const screen_chat_names[] =
{
	"lobby.chat",
};

static int screen_value(void *context, char const *name, char *value, unsigned long size)
{
	struct screen_values *values = context;

	if (!values->read && !strncmp(name, "campaign.", 9))
	{
		values->read = TRUE;
		values->saved = pc_menu_campaign_saved(values->level, values->difficulty, (long)sizeof(values->level));
	}
	/* (the server browser's: menu_functions.c's) */
	if (!strncmp(name, "browser.", 8))
		return pc_menu_browser_value(name, value, size);
	/* (Select Profile's: menu_functions.c's) */
	if (!strncmp(name, "profile.", 8))
		return pc_menu_profile_value(name, value, size);
	/* (the lobby's: menu_functions.c's lobby_menu.inc) */
	if (!strncmp(name, "lobby.", 6))
		return pc_menu_lobby_value(name, value, size);
	/* (the pause menu's: the rows this machine's has, as menu_tags.c adds
	them to the map's; FRIENDLY FIRE's words, this machine's choice) */
	if (!strncmp(name, "pause.", 6))
	{
		unsigned long rows = menu_tags_pause_rows();

		if (!strcmp(name, "pause.friendly_fire") && TEST_FLAG(rows, _host_controls_pause_friendly_fire))
			snprintf(value, size, config_boolean("network.coop_own_friendly_fire") ? "FRIENDLY FIRE ON" :
				"FRIENDLY FIRE OFF");
		else if ((!strcmp(name, "pause.host_controls") && TEST_FLAG(rows, _host_controls_pause_host_controls)) ||
			(!strcmp(name, "pause.end_game") && TEST_FLAG(rows, _host_controls_pause_end_game)) ||
			(!strcmp(name, "pause.chapters") && TEST_FLAG(rows, _host_controls_pause_chapters)))
		{
			snprintf(value, size, "yes");
		}
		else
			return 0;
		return 1;
	}
	if (!strcmp(name, "campaign.level") && values->saved)
		snprintf(value, size, "%s", values->level);
	else if (!strcmp(name, "campaign.difficulty") && values->saved)
		snprintf(value, size, "%s", values->difficulty);
	else
		return 0;
	return value[0] != 0;
}

/* a binding's value set (a search's words typed: <search value=>) */
static void screen_value_set(char const *name, char const *text)
{
	/* (the server browser's searches: menu_functions.c's) */
	if (name && !strncmp(name, "browser.search.", 15))
		pc_menu_browser_search_set(name, text);
}

static struct menu_screen_names const screen_names =
{
	screen_action_names, NUMBEROF(screen_action_names),
	screen_binding_names, NUMBEROF(screen_binding_names),
	screen_source_names, NUMBEROF(screen_source_names),
	screen_chat_names, NUMBEROF(screen_chat_names),
};

/* the menus' screens a hand-written one takes the place of, by its name, and
what it does each frame it is drawn (the main menu: the update and crash
questions may open over it) */
struct screen_place
{
	char const *screen;
	char const *widget;
	void (*shown)(struct widget_instance *screen);
	/* (whether it takes the screen's place this time: NULL, always) */
	int (*takes)(void);
};

static struct screen_place const screen_places[] =
{
	{ "main", "main_menu\\main_menu", pc_menu_main_menu_shown, NULL },
	{ "campaign", "screens\\campaign", NULL, NULL },
	/* (Join Game's, in each of its modes: the server browser, the local
	network's list, Join by Link) */
	{ "browser", "main_menu\\multiplayer_type_select\\join_game\\join_game_screen", NULL, NULL },
	/* (Settings' COLOR: the profile's color, its Spartan beside it) */
	{ "profile_color", "main_menu\\settings_select\\player_setup\\player_profile_edit\\color_edit\\color_select_screen",
		NULL, NULL },
	/* (Settings' MANAGE PROFILES: the profiles, the chosen one's Spartan
	beside them) */
	{ "profile_select", "main_menu\\profile_manager\\player_profile_manager", NULL, NULL },
	/* (the lobby, a network game's and a game of this machine alone's: its
	generated screen's own handlers kept (B leaves, START joins another
	player, X adds one, Y the map vote or the invite), and what its list did
	each frame done: pc_menu_lobby_frame) */
	{ "lobby", "main_menu\\multiplayer_type_select\\lobby\\lobby_screen", pc_menu_lobby_frame, NULL },
	/* (the pause menu, in place of the map's: menu_screen_pause_name opens
	these, a campaign's or a multiplayer game's, pausing the game or not) */
	{ "pause", "screens\\pause", NULL, NULL },
	{ "pause", "screens\\pause_live", NULL, NULL },
	{ "pause_campaign", "screens\\pause_campaign", NULL, NULL },
	{ "pause_campaign", "screens\\pause_campaign_live", NULL, NULL },
};

/* the server browser, whose screen gets a button of the hand-written
screens' (screens/button) over it: HOST & MORE, or HOST CO-OP when the
browser was opened on co-op; its key the controller's Back (F1) */
#define SCREEN_BROWSER "main_menu\\multiplayer_type_select\\join_game\\join_game_screen"
#define SCREEN_BROWSER_BUTTON_X 276
#define SCREEN_BROWSER_BUTTON_Y 8
static struct screen_place const screen_browser_place = { "browser host", SCREEN_BROWSER, NULL, NULL };

/* ---------- the screens open */

struct screen_open
{
	struct widget_instance *widget;
	struct screen_place const *place;
	unsigned long generation;
	struct widget_instance *list;
	struct widget_instance *items[SCREEN_MAXIMUM_ITEMS];
	struct widget_instance *hints[SCREEN_MAXIMUM_ITEMS];
	/* (each item's picture's bitmap tag, NONE for its label; a card's, its
	picture part's) */
	long pictures[SCREEN_MAXIMUM_ITEMS];
	long card_pictures[SCREEN_MAXIMUM_ITEMS];
	char ids[SCREEN_MAXIMUM_ITEMS][SCREEN_ID_LENGTH];
	char actions[SCREEN_MAXIMUM_ITEMS][SCREEN_ACTION_LENGTH];
	long item_count;
	char back[SCREEN_ACTION_LENGTH];
	char start[SCREEN_ACTION_LENGTH];
	/* (a button over a generated screen, its own widgets kept: the
	browser's; and the child its focus goes back to from the button) */
	boolean extra;
	struct widget_instance *kept_focus;
	/* (its header, and the picture its file names for it, NONE for the
	block's own) */
	struct widget_instance *header;
	long header_picture;
	/* (its <colors>: its items the colors, in order; its <spartan>, and the
	original picture drawn where no Spartan is) */
	boolean colors;
	boolean has_spartan;
	struct menu_screen_block spartan;
	struct widget_instance *spartan_picture;
	/* (its cards' scale, to fill their room: 0 for their own size) */
	real card_scale;
	/* (its button bar's prompts, each its key, and its place in the 640
	and its side, which past 16:9 moves it to the screen's edge each frame) */
	struct widget_instance *prompts[SCREEN_MAXIMUM_BUTTONS];
	char prompt_keys[SCREEN_MAXIMUM_BUTTONS][SCREEN_ID_LENGTH];
	short prompt_x[SCREEN_MAXIMUM_BUTTONS];
	boolean prompt_right[SCREEN_MAXIMUM_BUTTONS];
	long prompt_count;
	/* (its <box>'s: its items' blocks, whose labels' bindings are read
	again each frame (FRIENDLY FIRE's ON and OFF); the parts that move as
	its legend grows while the keyboard's keys are drawn in it (its frame's
	left and middle left by half of it, the middle as much wider, its right
	and the objectives' box right by half), each its place and way, the
	middle's width, and the growth they have now) */
	boolean box;
	struct menu_screen_block const *item_blocks[SCREEN_MAXIMUM_ITEMS];
	struct widget_instance *box_parts[BOX_MAXIMUM_PARTS];
	short box_part_x[BOX_MAXIMUM_PARTS];
	short box_part_way[BOX_MAXIMUM_PARTS];
	long box_part_count;
	struct widget_instance *box_middle;
	struct widget_instance *box_legend;
	short box_growth;
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
	long prompts_tag;
	long color_tag, spartan_tag;
	long title_art_tag, list_tag, item_tag, hint_tag, prompt_left_tag, button_tag, cards_tag, card_tag, card_title_tag, card_art_tag, card_subtitle_tag, header_tag, prompt_tag, header_text_tag;
	/* (a <box>'s, its <legend>'s and its <objectives>') */
	long box_left_tag, box_center_tag, box_right_tag, box_list_tag, box_item_tag, legend_tag;
	long objectives_left_tag, objectives_center_tag, objectives_right_tag, objectives_title_tag, objectives_text_tag;
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
		if (!strcmp(name + 3, screen_places[index].widget) &&
			(!screen_places[index].takes || screen_places[index].takes()))
		{
			return &screen_places[index];
		}
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
	screens.color_tag = screen_block_tag("pc\\screens\\color");
	screens.spartan_tag = screen_block_tag("pc\\screens\\spartan");
	screens.title_art_tag = screen_block_tag("pc\\screens\\title_art");
	screens.list_tag = screen_block_tag("pc\\screens\\list");
	screens.item_tag = screen_block_tag("pc\\screens\\item");
	screens.hint_tag = screen_block_tag("pc\\screens\\hint");
	screens.prompt_left_tag = screen_block_tag("pc\\screens\\prompt_left");
	screens.button_tag = screen_block_tag("pc\\screens\\button");
	screens.cards_tag = screen_block_tag("pc\\screens\\cards");
	screens.card_tag = screen_block_tag("pc\\screens\\card");
	screens.card_title_tag = screen_block_tag("pc\\screens\\card_title");
	screens.card_art_tag = screen_block_tag("pc\\screens\\card_art");
	screens.header_tag = screen_block_tag("pc\\screens\\header");
	screens.card_subtitle_tag = screen_block_tag("pc\\screens\\card_subtitle");
	screens.prompt_tag = screen_block_tag("pc\\screens\\prompt");
	screens.prompts_tag = screen_block_tag("pc\\screens\\prompts");
	screens.header_text_tag = screen_block_tag("pc\\screens\\header_text");
	screens.box_left_tag = screen_block_tag("pc\\screens\\box_left");
	screens.box_center_tag = screen_block_tag("pc\\screens\\box_center");
	screens.box_right_tag = screen_block_tag("pc\\screens\\box_right");
	screens.box_list_tag = screen_block_tag("pc\\screens\\box_list");
	screens.box_item_tag = screen_block_tag("pc\\screens\\box_item");
	screens.legend_tag = screen_block_tag("pc\\screens\\legend");
	screens.objectives_left_tag = screen_block_tag("pc\\screens\\objectives_left");
	screens.objectives_center_tag = screen_block_tag("pc\\screens\\objectives_center");
	screens.objectives_right_tag = screen_block_tag("pc\\screens\\objectives_right");
	screens.objectives_title_tag = screen_block_tag("pc\\screens\\objectives_title");
	screens.objectives_text_tag = screen_block_tag("pc\\screens\\objectives_text");
	if (screens.title_art_tag == NONE || screens.list_tag == NONE || screens.item_tag == NONE ||
		screens.hint_tag == NONE || screens.prompt_left_tag == NONE || screens.button_tag == NONE ||
		screens.cards_tag == NONE || screens.card_tag == NONE || screens.card_title_tag == NONE ||
		screens.card_art_tag == NONE || screens.header_tag == NONE ||
		screens.card_subtitle_tag == NONE || screens.prompt_tag == NONE || screens.prompts_tag == NONE ||
		screens.header_text_tag == NONE || screens.color_tag == NONE || screens.spartan_tag == NONE ||
		screens.box_left_tag == NONE || screens.box_center_tag == NONE || screens.box_right_tag == NONE ||
		screens.box_list_tag == NONE || screens.box_item_tag == NONE || screens.legend_tag == NONE ||
		screens.objectives_left_tag == NONE || screens.objectives_center_tag == NONE ||
		screens.objectives_right_tag == NONE || screens.objectives_title_tag == NONE ||
		screens.objectives_text_tag == NONE)
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

/* a text box's words, from the game's own (a string list's) */
static void screen_text_set_wide(struct widget_instance *widget, wchar_t const *words)
{
	long length = 0;
	wchar_t *buffer;

	while (words[length] && length < MENU_SCREEN_MAXIMUM_TEXT - 1)
		length++;
	buffer = ui_widget_realloc(widget->parameters.text_box.text, (word)((length + 1) * (long)sizeof(wchar_t)),
		__FILE__, __LINE__);
	if (!buffer)
		return;
	csmemcpy(buffer, words, length * (long)sizeof(wchar_t));
	buffer[length] = 0;
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
	char shown[MENU_SCREEN_MAXIMUM_TEXT];

	return menu_screen_when(item->when) && menu_screen_text(item->label, screen_value, values, label, size) &&
		(!item->condition || screen_value(values, item->condition, shown, sizeof(shown)));
}

/* a button's side of the bar: back's the left, the rest's the right (or as
its side says) */
static boolean screen_button_right(struct menu_screen_block const *button)
{
	return button->side ? !strcmp(button->side, "right") : strcmp(button->key, "back") != 0;
}

static boolean tall_build(struct screen_open *open, struct menu_screen const *screen);
static boolean box_build(struct screen_open *open, struct menu_screen const *screen);

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
	short fixed = 0, spaces = 0, spaces_left, extra, top, shrink = 0;
	long index, place_index = open->place - screen_places;
	struct widget_instance *focus = NULL;
	long focus_index = 0;

	if (!screen_blocks_found())
		return FALSE;
	/* (a screen with a table: the tall layout's, menu_screen_tall.inc) */
	for (index = 0; index < screen->block_count; index++)
	{
		if (screen->blocks[index].kind == MENU_SCREEN_TABLE)
			return tall_build(open, screen);
		/* (a screen with a <box>: the pause menu's layout) */
		if (screen->blocks[index].kind == MENU_SCREEN_BOX)
			return box_build(open, screen);
	}
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
		case MENU_SCREEN_CARDS:
			heights[index] = CARDS_ROOM;
			break;
		case MENU_SCREEN_HEADER:
			heights[index] = block->picture ? HEADER_ROOM : HEADER_ROOM_TALL;
			break;
		case MENU_SCREEN_CARD:
		case MENU_SCREEN_ITEM:
			shown[index] = screen_item_shown(block, &values, label, sizeof(label));
			if (block->kind == MENU_SCREEN_CARD)
				break;
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
		case MENU_SCREEN_COLORS:
			heights[index] = SCREEN_COLOR_COUNT * SCREEN_COLOR_STEP;
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
	open->colors = FALSE;
	open->has_spartan = FALSE;
	open->spartan_picture = NULL;
	focus_id = screens.focused[place_index][0] ? screens.focused[place_index] : NULL;
	top = 0;
	spaces_left = spaces;
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
			/* (the last the rest of it, so that a bar after it is at the
			screen's foot) */
			spaces_left--;
			height = spaces ? (short)(spaces_left ? extra / spaces : extra - (spaces - 1) * (extra / spaces)) : 0;
			break;
		case MENU_SCREEN_HEADER:
		{
			/* (its picture where the campaign menu has its own; or its
			title's words in the tall layout's band, as Settings' and the
			browsers' titles) */
			struct widget_instance *header = screen_widget_add(widget,
				block->picture ? screens.header_tag : screens.header_text_tag, 0, top);

			open->header_picture = block->picture ? screen_picture_tag(block->picture) : NONE;
			open->header = header;
			if (header && !block->picture)
			{
				menu_screen_text(block->label, screen_value, &values, label, sizeof(label));
				screen_text_set(header, label);
			}
			break;
		}
		case MENU_SCREEN_CARDS:
		{
			long card_index, count = 0;
			short x, card_width, card_gap;

			for (card_index = 0; card_index < screen->block_count; card_index++)
				count += screen->blocks[card_index].kind == MENU_SCREEN_CARD &&
					screen->blocks[card_index].parent == index && shown[card_index];
			open->list = screen_widget_add(widget, screens.cards_tag, 0, 0);
			if (!open->list)
				break;
			if (!focus_id)
				focus_id = block->focus;
			/* (at their own size: a scale, 0 for none, would draw them larger) */
			open->card_scale = 0.0f;
			card_width = CARD_WIDTH;
			card_gap = CARD_GAP;
			x = (short)((SCREEN_WIDTH - count * card_width - (count - 1) * card_gap) / 2);
			for (card_index = 0; card_index < screen->block_count; card_index++)
			{
				struct menu_screen_block const *card = &screen->blocks[card_index];
				struct widget_instance *made, *part;

				if (card->kind != MENU_SCREEN_CARD || card->parent != index || !shown[card_index] ||
					open->item_count >= SCREEN_MAXIMUM_ITEMS)
				{
					continue;
				}
				made = ui_widget_port_add_child(open->list, screens.card_tag, NULL);
				if (!made)
					break;
				open->card_pictures[open->item_count] = NONE;
				/* (its place: its parts' are in it) */
				made->horizontal_offset = (short)(open->list->horizontal_offset + x);
				made->vertical_offset = (short)(open->list->vertical_offset + top - shrink + CARD_MARGIN);
				/* (its parts: the picture, the title, the subtitle) */
				part = made->child;
				if (part)
				{
					long picture = card->picture ? screen_picture_tag(card->picture) : NONE;

					part->animation.current_frame_index = (short)(card->frame >= 0 ? card->frame : 0);
					if (picture != NONE)
						open->card_pictures[open->item_count] = picture;
					part = part->next;
				}
				menu_screen_text(card->label, screen_value, &values, label, sizeof(label));
				if (part)
				{
					screen_text_set(part, label);
					part = part->next;
				}
				if (part)
				{
					text[0] = 0;
					if (card->hint_count)
						menu_screen_text(card->hints[0], screen_value, &values, text, sizeof(text));
					screen_text_set(part, text);
				}
				open->items[open->item_count] = made;
				open->hints[open->item_count] = NULL;
				open->pictures[open->item_count] = NONE;
				snprintf(open->ids[open->item_count], SCREEN_ID_LENGTH, "%s", card->id ? card->id : "");
				snprintf(open->actions[open->item_count], SCREEN_ACTION_LENGTH, "%s", card->action);
				if (!focus || (focus_id && card->id && !strcmp(card->id, focus_id)))
				{
					focus = made;
					focus_index = open->item_count;
				}
				open->item_count++;
				x = (short)(x + card_width + card_gap);
			}
			break;
		}
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
		case MENU_SCREEN_COLORS:
		{
			struct player_profile *profile = player_ui_get_edit_player_profile();
			long names = tag_loaded('ustr', SCREEN_COLORS_NAMES);
			long color;

			open->list = screen_widget_add(widget, screens.list_tag, 0, 0);
			if (!open->list)
				break;
			open->colors = TRUE;
			for (color = 0; color < SCREEN_COLOR_COUNT && open->item_count < SCREEN_MAXIMUM_ITEMS; color++)
			{
				struct widget_instance *made = screen_widget_add(open->list, screens.color_tag, SCREEN_COLOR_X,
					(short)(top + color * SCREEN_COLOR_STEP));
				wchar_t const *name = names != NONE ? unicode_string_list_get_string(names, (short)color) : NULL;

				if (!made)
					break;
				if (name)
					screen_text_set_wide(made, name);
				open->items[open->item_count] = made;
				open->hints[open->item_count] = NULL;
				open->pictures[open->item_count] = NONE;
				snprintf(open->ids[open->item_count], SCREEN_ID_LENGTH, "color%ld", color);
				snprintf(open->actions[open->item_count], SCREEN_ACTION_LENGTH, "color %ld", color);
				/* (first, the profile's own color) */
				if (!focus || (profile && profile->primary_color_index == color))
				{
					focus = made;
					focus_index = open->item_count;
				}
				open->item_count++;
			}
			break;
		}
		case MENU_SCREEN_SPARTAN:
			/* (over the screen's other blocks, where its box is: the original
			picture made at the box's foot, shown where no Spartan is) */
			open->has_spartan = TRUE;
			open->spartan = *block;
			open->spartan_picture = screen_widget_add(widget, screens.spartan_tag,
				(short)(block->box_x + (block->box_width - SCREEN_SPARTAN_PICTURE_WIDTH) / 2),
				(short)(block->box_y + block->box_height - SCREEN_SPARTAN_PICTURE_HEIGHT));
			break;
		case MENU_SCREEN_BUTTONS:
		{
			long button;
			short left_x = BUTTONS_MARGIN, right_x = (short)(SCREEN_WIDTH - BUTTONS_MARGIN - PROMPT_WIDTH);
			struct widget_instance *bar =
				screen_widget_add(widget, screens.prompts_tag, 0, (short)(top + PROMPTS_TOP));

			/* (each a prompt, its key's picture, "%a-button": the menus' key
			for it while the keyboard plays, and its word; back at the left,
			the rest from the right, on the footer's line) */
			open->prompt_count = 0;
			for (button = 0; button < screen->block_count && open->prompt_count < SCREEN_MAXIMUM_BUTTONS; button++)
			{
				struct menu_screen_block const *block_button = &screen->blocks[button];
				boolean right;
				struct widget_instance *prompt;
				long made = open->prompt_count;

				if (!bar || block_button->kind != MENU_SCREEN_BUTTON || block_button->parent != index)
					continue;
				right = screen_button_right(block_button);
				/* (in the bar, where it is) */
				prompt = ui_widget_port_add_child(bar, right ? screens.prompt_tag : screens.prompt_left_tag, NULL);
				if (!prompt)
					continue;
				prompt->horizontal_offset = right ? right_x : left_x;
				prompt->vertical_offset = PROMPT_TOP;
				snprintf(text, sizeof(text), "%s %s", !strcmp(block_button->key, "accept") ? "%a-button" :
					!strcmp(block_button->key, "back") ? "%b-button" : !strcmp(block_button->key, "x") ? "%x-button" :
					"%y-button", block_button->label);
				screen_text_set(prompt, text);
				open->prompts[made] = prompt;
				open->prompt_x[made] = right ? right_x : left_x;
				open->prompt_right[made] = right;
				snprintf(open->prompt_keys[made], SCREEN_ID_LENGTH, "%s", block_button->key);
				open->prompt_count++;
				if (right)
					right_x = (short)(right_x - PROMPT_GAP - PROMPT_WIDTH);
				else
					left_x = (short)(left_x + PROMPT_LEFT_WIDTH + PROMPT_GAP);
			}
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
	snprintf(open->start, sizeof(open->start), "%s", screen->start ? screen->start : "");
	open->generation = menu_screens_generation(screen->name);
	return TRUE;
}

static boolean screen_action_run(struct widget_instance *widget, struct event_record *event, char const *name,
	boolean *deleted);
static void screen_spartan_frame(struct screen_open *open, long focused);

#include "menu_screen_tall.inc"

/* ---------- a <box> (the pause menu's): the Xbox's box of rows as the port
has it, its legend under them, and the objectives' box beside it */

/* a part of a box made where it is, moved the way the legend's growth
moves it (-1: left by half the growth, the box's middle (way 0): left by
half and as much wider, 1: right by half) */
static struct widget_instance *box_part_add(struct screen_open *open, long tag_index, short x, short y, short way,
	short frame)
{
	struct widget_instance *part = screen_widget_add(open->widget, tag_index, x, y);

	if (!part)
		return NULL;
	part->animation.current_frame_index = frame;
	if (open->box_part_count < BOX_MAXIMUM_PARTS)
	{
		open->box_parts[open->box_part_count] = part;
		open->box_part_x[open->box_part_count] = x;
		open->box_part_way[open->box_part_count] = way;
		open->box_part_count++;
	}
	return part;
}

/* the box's widgets made: its rows those shown (each its if's and its
label's bindings with values), as tall as they need, its frame the one for
that many rows, at its x and from its y (or about it: align="middle"); its
legend under them; the objectives' box beside it, as tall. FALSE if the
screen has no <box> with a <list> */
static boolean box_build(struct screen_open *open, struct menu_screen const *screen)
{
	struct widget_instance *widget = open->widget;
	struct screen_values values;
	char label[MENU_SCREEN_MAXIMUM_TEXT];
	struct menu_screen_block const *box = NULL, *list = NULL, *legend = NULL, *objectives = NULL;
	long index, rows = 0, list_index = NONE;
	short height, top, x, frame;
	struct widget_instance *focus = NULL;
	char const *focus_id;
	long focus_index = 0;

	for (index = 0; index < screen->block_count; index++)
	{
		struct menu_screen_block const *block = &screen->blocks[index];

		if (block->kind == MENU_SCREEN_BOX)
			box = block;
		else if (block->kind == MENU_SCREEN_LIST)
		{
			list = block;
			list_index = index;
		}
		else if (block->kind == MENU_SCREEN_LEGEND)
			legend = block;
		else if (block->kind == MENU_SCREEN_OBJECTIVES)
			objectives = block;
	}
	if (!box || !list)
		return FALSE;
	memset(&values, 0, sizeof(values));
	for (index = 0; index < screen->block_count; index++)
	{
		if (screen->blocks[index].kind == MENU_SCREEN_ITEM && screen->blocks[index].parent == list_index &&
			screen_item_shown(&screen->blocks[index], &values, label, sizeof(label)))
		{
			rows++;
		}
	}
	rows = MAX(rows, 1);
	height = (short)(BOX_ROWS_TOP + (rows - 1) * BOX_ROW_PITCH + BOX_ROW_HEIGHT + BOX_LEGEND_GAP + BOX_LEGEND_BOTTOM);
	top = (short)(box->side && !strcmp(box->side, "middle") ? box->box_y - height / 2 : box->box_y);
	x = (short)box->box_x;
	frame = (short)PIN(rows - BOX_FIRST_ROWS, 0, BOX_FRAMES - 1);
	focus_id = screens.focused[open->place - screen_places][0] ? screens.focused[open->place - screen_places] :
		list->focus;
	/* the screen's own widgets let go of, and its own made */
	widget->focused_child = NULL;
	while (widget->child)
		ui_widget_delete(widget->child);
	open->list = NULL;
	open->item_count = 0;
	open->colors = FALSE;
	open->has_spartan = FALSE;
	open->spartan_picture = NULL;
	open->header = NULL;
	open->prompt_count = 0;
	open->box = TRUE;
	open->box_part_count = 0;
	open->box_growth = 0;
	/* (its frame: the middle as wide as the box less its sides) */
	box_part_add(open, screens.box_left_tag, x, top, -1, frame);
	open->box_middle = box_part_add(open, screens.box_center_tag, (short)(x + BOX_SIDE), top, 0, frame);
	box_part_add(open, screens.box_right_tag, (short)(x + box->box_width - BOX_SIDE), top, 1, frame);
	/* (its rows, in the middle of it) */
	open->list = screen_widget_add(widget, screens.box_list_tag, 0, 0);
	if (!open->list)
		return FALSE;
	for (index = 0; index < screen->block_count; index++)
	{
		struct menu_screen_block const *item = &screen->blocks[index];
		struct widget_instance *made;

		if (item->kind != MENU_SCREEN_ITEM || item->parent != list_index ||
			!screen_item_shown(item, &values, label, sizeof(label)) || open->item_count >= SCREEN_MAXIMUM_ITEMS)
		{
			continue;
		}
		made = screen_widget_add(open->list, screens.box_item_tag, (short)(x + (box->box_width - BOX_ROW_WIDTH) / 2),
			(short)(top + BOX_ROWS_TOP + open->item_count * BOX_ROW_PITCH));
		if (!made)
			break;
		screen_text_set(made, label);
		open->items[open->item_count] = made;
		open->item_blocks[open->item_count] = item;
		open->hints[open->item_count] = NULL;
		open->pictures[open->item_count] = NONE;
		snprintf(open->ids[open->item_count], SCREEN_ID_LENGTH, "%s", item->id ? item->id : "");
		snprintf(open->actions[open->item_count], SCREEN_ACTION_LENGTH, "%s", item->action);
		/* (the item focused last, else the list's focus, else its first) */
		if (!focus || (focus_id && item->id && !strcmp(item->id, focus_id)))
		{
			focus = made;
			focus_index = open->item_count;
		}
		open->item_count++;
	}
	/* (its legend, under its last row) */
	open->box_legend = NULL;
	if (legend)
	{
		long legends = tag_loaded(UI_WIDGET_DEFINITION_TAG, BOX_LEGENDS);

		open->box_legend = screen_widget_add(widget, screens.legend_tag, (short)(x + legend->box_x),
			(short)(top + BOX_ROWS_TOP + (rows - 1) * BOX_ROW_PITCH + BOX_ROW_HEIGHT + BOX_LEGEND_GAP));
		if (open->box_legend && legends != NONE)
		{
			struct widget_instance *row = ui_widget_port_add_child(open->box_legend, legends, NULL);

			/* (where its parent is: children are drawn from their parent's
			place) */
			if (row)
			{
				row->horizontal_offset = 0;
				row->vertical_offset = 0;
			}
		}
	}
	/* (the objectives' box beside it, as tall: its frame for that many
	rows, its title and the game's objectives in it) */
	if (objectives)
	{
		short beside = (short)(x + box->box_width + OBJECTIVES_GAP);
		short frame_beside = (short)PIN(rows - OBJECTIVES_FIRST_ROWS, 0, OBJECTIVES_FRAMES - 1);
		struct widget_instance *title;

		box_part_add(open, screens.objectives_left_tag, beside, top, 1, frame_beside);
		box_part_add(open, screens.objectives_center_tag, (short)(beside + BOX_SIDE), top, 1, frame_beside);
		box_part_add(open, screens.objectives_right_tag, (short)(beside + BOX_SIDE + OBJECTIVES_MIDDLE), top, 1,
			frame_beside);
		title = box_part_add(open, screens.objectives_title_tag, (short)(beside + OBJECTIVES_X),
			(short)(top + OBJECTIVES_TITLE_Y), 1, 0);
		if (title && objectives->label && menu_screen_text(objectives->label, screen_value, &values, label,
			sizeof(label)))
		{
			screen_text_set(title, label);
		}
		box_part_add(open, screens.objectives_text_tag, (short)(beside + OBJECTIVES_X),
			(short)(top + OBJECTIVES_TEXT_Y), 1, 0);
	}
	if (focus)
	{
		widget->focused_child = open->list;
		open->list->focused_child = focus;
		open->list->parameters.list.selected_index = (short)focus_index;
	}
	snprintf(open->back, sizeof(open->back), "%s", screen->back ? screen->back : "");
	open->generation = menu_screens_generation(screen->name);
	return TRUE;
}

/* each frame: its rows' labels with bindings read again (FRIENDLY FIRE's
ON or OFF), and its parts moved as wide as its legend needs while the
keyboard's keys are drawn in it (menu_tags.c's pause_boxes_fit), the
Xbox's width while a controller's buttons are */
static void box_frame(struct screen_open *open)
{
	struct screen_values values;
	char label[MENU_SCREEN_MAXIMUM_TEXT];
	short growth = 0;
	long index;

	memset(&values, 0, sizeof(values));
	for (index = 0; index < open->item_count; index++)
	{
		struct menu_screen_block const *item = open->item_blocks[index];

		if (item && item->label && strchr(item->label, '{') &&
			menu_screen_text(item->label, screen_value, &values, label, sizeof(label)))
		{
			screen_text_set(open->items[index], label);
		}
	}
	if (open->box_legend && halo_keyboard_prompts())
		growth = menu_tags_legend_growth(tag_loaded(UI_WIDGET_DEFINITION_TAG, BOX_LEGENDS));
	open->box_growth = growth;
	for (index = 0; index < open->box_part_count; index++)
	{
		open->box_parts[index]->horizontal_offset = (short)(open->box_part_x[index] +
			(open->box_part_way[index] > 0 ? growth / 2 : -(growth / 2)));
	}
}

/* a box's middle drawn as much wider as its legend's growth, and its rows
with the game map's frame of them (BOX_ROW_PICTURE: the menus have it on
the maps the pause menu is played on, not ui.map, which their files are
made on too): TRUE for them */
static boolean box_picture_of(struct widget_instance const *widget, long *bitmap_tag, rectangle2d *bounds)
{
	long open_index, index;

	for (open_index = 0; widget && open_index < SCREEN_MAXIMUM_OPEN; open_index++)
	{
		struct screen_open const *open = &screens.open[open_index];

		if (!open->widget || !open->box)
			continue;
		if (open->box_middle == widget)
		{
			*bitmap_tag = NONE;
			bounds->x1 = (short)(bounds->x1 + open->box_growth);
			return TRUE;
		}
		for (index = 0; index < open->item_count; index++)
		{
			if (open->items[index] == widget)
			{
				*bitmap_tag = tag_loaded(BITMAP_GROUP_TAG, BOX_ROW_PICTURE);
				return TRUE;
			}
		}
	}
	return FALSE;
}

/* "close", "close all" and "pause leave": the screen closed (the pause
menu's RESUME GAME), every screen (B; the campaign's RESUME GAME), or the
player taken out of the game (the Xbox's "mp game player quit") and the
screen closed */
static boolean screen_call_close(struct widget_instance *widget, struct event_record *event, char const *name,
	boolean *deleted)
{
	struct widget_instance *screen = widget_instance_get_topmost_parent(widget);

	*deleted = FALSE;
	if (!strcmp(name, "pause leave"))
	{
		long index = pc_menu_event_function_index("mp game player quit");

		if (index == NONE || !ui_widget_event_handler_function_invoke(widget, event, (word)index, deleted))
			return FALSE;
		if (*deleted)
			return TRUE;
	}
	if (!strcmp(name, "close all"))
		ui_widgets_close_all();
	else
		ui_widget_delete(screen);
	*deleted = TRUE;
	return TRUE;
}

/* the screen the pause menu opens as, for the game's (name: the map's
pause screen): under the menu engine, the hand-written one in its place, a
campaign's or a multiplayer game's, pausing the game as the map's does or
not (screens/blocks.xml's stand-ins), its focus on its first row; else the
game's own (the classic engine; a file or its blocks missing or refused) */
char const *menu_screen_pause_name(char const *name)
{
	long tag_index = name ? tag_loaded(UI_WIDGET_DEFINITION_TAG, name) : NONE;
	boolean campaign = name && strstr(name, "\\solo_game\\") != NULL;
	char const *stand_in;
	long index;

	if (tag_index == NONE || !menu_engine_looks())
		return name;
	if (campaign)
		stand_in = menu_tags_flags(tag_index) & SCREEN_PAUSE_GAME_FLAG ? "pc\\screens\\pause_campaign" :
			"pc\\screens\\pause_campaign_live";
	else
		stand_in = menu_tags_flags(tag_index) & SCREEN_PAUSE_GAME_FLAG ? "pc\\screens\\pause" : "pc\\screens\\pause_live";
	screens_names_give();
	menu_screens_poll(ui_widget_port_milliseconds());
	if (tag_loaded(UI_WIDGET_DEFINITION_TAG, stand_in) == NONE ||
		!menu_screens_find(campaign ? "pause_campaign" : "pause") || !screen_blocks_found())
	{
		return name;
	}
	for (index = 0; index < NUMBEROF(screen_places); index++)
	{
		if (!strncmp(screen_places[index].screen, "pause", 5))
			screens.focused[index][0] = 0;
	}
	return stand_in;
}

/* ---------- the engine's calls */

/* the server browser's HOST button made over it (TRUE), where it is the
server browser (not the local network's list, nor Join by Link) */
static boolean screen_browser_opened(struct widget_instance *widget)
{
	char const *name = tag_get_name(widget->definition_tag_index);
	long index;

	if (!name || strncmp(name, "pc\\", 3) || strcmp(name + 3, SCREEN_BROWSER) || !pc_menu_browser_visit_kind() ||
		!screen_blocks_found())
	{
		return FALSE;
	}
	for (index = 0; index < SCREEN_MAXIMUM_OPEN; index++)
	{
		struct screen_open *open = &screens.open[index];
		struct widget_instance *button;
		boolean coop = pc_menu_browser_visit_kind() == 2;

		if (open->widget)
			continue;
		memset(open, 0, sizeof(*open));
		button = screen_widget_add(widget, screens.button_tag, SCREEN_BROWSER_BUTTON_X, SCREEN_BROWSER_BUTTON_Y);
		if (!button)
			return FALSE;
		open->widget = widget;
		open->place = &screen_browser_place;
		open->extra = TRUE;
		open->items[0] = button;
		open->pictures[0] = NONE;
		open->item_count = 1;
		snprintf(open->ids[0], SCREEN_ID_LENGTH, "host");
		snprintf(open->actions[0], SCREEN_ACTION_LENGTH, "%s", coop ? "open host coop" : "open host");
		screen_text_set(button, coop ? "%back-button HOST CO-OP" : "%back-button HOST & MORE");
		return TRUE;
	}
	return FALSE;
}

boolean menu_screen_opened(struct widget_instance *widget)
{
	struct screen_place const *place = screen_place_of(widget->definition_tag_index);
	struct menu_screen const *screen;
	long index;

	if (!place)
		return screen_browser_opened(widget);
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
		/* (its table's rows told it opened: Select Profile's on player 1's
		profile) */
		if (tall_of(open) && tall_of(open)->source && tall_of(open)->source->opened)
			tall_of(open)->source->opened();
		return TRUE;
	}
	return FALSE;
}

void menu_screen_closed(struct widget_instance *widget)
{
	struct screen_open *open = screen_open_of(widget);

	if (!open)
		return;
	if (tall_of(open))
	{
		struct screen_tall *tall = tall_of(open);

		if (tall_search_editing(tall))
			pc_menu_text_field_end(FALSE);
		if (tall_search_screen == tall)
			tall_search_screen = NULL;
		tall_screens[open - screens.open] = NULL;
	}
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

/* a <spartan> of a binding's color whose binding has no value: none, nor its
picture (Select Profile's row that makes a profile) */
#define SCREEN_SPARTAN_NONE (-2)
/* a <spartan> of a profile's color where there is no profile: no Spartan,
the original picture of random */
#define SCREEN_SPARTAN_NO_PROFILE (-3)

/* the color a <spartan>'s player shows: the focused color's, the profile's
(the one being edited, else the player's own), or its binding's (a color's
number); MENU_SPARTAN_RANDOM for random; SCREEN_SPARTAN_NO_PROFILE for no
profile; SCREEN_SPARTAN_NONE for a binding's none */
static short screen_spartan_color(struct screen_open const *open, long focused, short player)
{
	if (open->spartan.color == MENU_SCREEN_SPARTAN_FOCUS && open->colors && focused != NONE)
		return (short)focused;
	if (open->spartan.color == MENU_SCREEN_SPARTAN_BINDING)
	{
		struct screen_values values;
		char value[16];

		memset(&values, 0, sizeof(values));
		if (!open->spartan.value || !screen_value(&values, open->spartan.value, value, sizeof(value)))
			return SCREEN_SPARTAN_NONE;
		return value[0] >= '0' && value[0] <= '9' ? (short)atoi(value) : MENU_SPARTAN_RANDOM;
	}
	if (player == 0 && player_ui_get_edit_player_profile())
	{
		short color = player_ui_get_edit_player_profile()->primary_color_index;

		return color < 0 || color >= SCREEN_COLOR_COUNT ? MENU_SPARTAN_RANDOM : color;
	}
	if (player_ui_get_active_player_profile_index(player) != NONE)
	{
		struct player_profile profile;

		player_ui_get_active_player_profile(player, &profile);
		return profile.primary_color_index < 0 || profile.primary_color_index >= SCREEN_COLOR_COUNT ?
			MENU_SPARTAN_RANDOM : profile.primary_color_index;
	}
	return SCREEN_SPARTAN_NO_PROFILE;
}

/* a <spartan>'s Spartans asked for this frame, each player's side by side in
its box (a random color's cycling through the colors: menu_spartan_color),
and the original picture shown where there is none (no Spartan's tags:
menu_spartan_available; or no profile) */
static void screen_spartan_frame(struct screen_open *open, long focused)
{
	struct menu_screen_block const *block = &open->spartan;
	boolean drawn = FALSE;
	short player, first = screen_spartan_color(open, focused, 0);
	short players = (short)block->players;
	short test_colors[MAXIMUM_MENU_SPARTANS];
	short tested = menu_spartan_test_colors(test_colors);

	/* (debug: HALO_MENU_SPARTAN_TEST's colors in place of its players') */
	if (tested)
	{
		players = tested;
		first = test_colors[0];
	}
	for (player = 0; player < players; player++)
	{
		short color = tested ? test_colors[player] : player ? screen_spartan_color(open, focused, player) : first;

		if ((color != MENU_SPARTAN_RANDOM && (color < 0 || color >= SCREEN_COLOR_COUNT)) || !menu_spartan_available())
			continue;
		menu_spartan_place(player, color,
			(real)block->box_x + (real)block->box_width * ((real)player + 0.5f) / (real)players,
			(real)(block->box_y + block->box_height), (real)block->box_height / (real)(players > 2 ? 2 : 1));
		drawn |= player == 0;
	}
	if (open->spartan_picture)
	{
		open->spartan_picture->visible = !drawn && first != SCREEN_SPARTAN_NONE;
		open->spartan_picture->animation.current_frame_index =
			(short)(first >= 0 && first < SCREEN_COLOR_COUNT ? first : SCREEN_SPARTAN_RANDOM_FRAME);
	}
}

void menu_screen_widget_drawing(struct widget_instance const *widget, point2d offset)
{
	long index;

	for (index = 0; widget && index < SCREEN_MAXIMUM_OPEN; index++)
	{
		struct screen_open const *open = &screens.open[index];

		/* (its <spartan>'s Spartans in their view, where the picture is in
		the widgets' order: over what is drawn before, under what after) */
		if (open->widget && open->has_spartan && open->spartan_picture == widget)
			menu_spartan_draw(offset.x, offset.y);
		/* (its chat's on-screen keyboard, over everything) */
		if (open->widget && tall_screens[index] && tall_screens[index]->chat_keyboard == widget)
			tall_chat_keyboard_draw(tall_screens[index], offset);
	}
}

/* a color item's color chosen: the profile's (as the PC version's picker
chose it: player_profile_color_picker_select_color), and back to the
screen before */
static boolean screen_color_choose(struct widget_instance *widget, char const *which)
{
	struct player_profile *profile = player_ui_get_edit_player_profile();
	long color = strtol(which, NULL, 10);

	(void)widget;
	if (!profile || color < 0 || color >= SCREEN_COLOR_COUNT)
	{
		platform_log("menus: a color is chosen with no profile being edited");
		return FALSE;
	}
	profile->primary_color_index = (short)color;
	platform_log("menus: the profile's color %ld chosen", color);
	event_manager_post_button(0, SCREEN_BUTTON_B);
	return TRUE;
}

void menu_screen_frame(struct widget_instance *widget)
{
	struct screen_open *open = screen_open_of(widget);
	long focused, index;

	if (!open)
		return;
	/* (the browser's button: the focus back to the browser's own widgets
	once the pointer is not what gave the button it, the d-pad its) */
	if (open->extra)
	{
		if (widget->focused_child != open->items[0])
			open->kept_focus = widget->focused_child;
		else if (open->kept_focus && !ui_widget_port_pointer_focused())
			widget->focused_child = open->kept_focus;
		return;
	}
	/* (its file read again: made again, the focus on the same item) */
	menu_screens_poll(ui_widget_port_milliseconds());
	if (menu_screens_generation(open->place->screen) != open->generation)
	{
		struct menu_screen const *screen = menu_screens_find(open->place->screen);

		focused = screen_focused(open);
		if (focused != NONE)
			snprintf(screens.focused[open->place - screen_places], SCREEN_ID_LENGTH, "%s", open->ids[focused]);
		if (tall_of(open))
			tall_screens[open - screens.open] = NULL;
		if (screen && screen_build(open, screen))
			platform_log("menus: screen %s made again", screen->name);
		else
			open->generation = menu_screens_generation(open->place->screen);
	}
	/* (a box's: its rows' labels, its legend's room) */
	if (open->box)
		box_frame(open);
	/* (the tall layout's: its blocks each frame) */
	if (tall_of(open))
	{
		tall_frame(open);
		if (open->place->shown)
			open->place->shown(widget);
		return;
	}
	/* (past 16:9, the band's title and the bar's prompts to the screen's
	edges, as the tall layout's titles and footer's prompts) */
	if (open->header && open->header_picture == NONE)
		open->header->horizontal_offset = (short)(widget->horizontal_offset - ui_widget_port_past_move() / 2);
	for (index = 0; index < open->prompt_count; index++)
	{
		open->prompts[index]->horizontal_offset = (short)(open->prompt_x[index] +
			(open->prompt_right[index] ? 1 : -1) * (ui_widget_port_past_move() / 2));
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
	if (open->has_spartan)
		screen_spartan_frame(open, focused);
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
	/* (a <colors> item's: its color chosen) */
	if (!strncmp(name, "color ", 6))
		return screen_color_choose(widget, name + 6);
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
	if (action->call)
		return action->call(widget, event, name, deleted);
	if (action->browser_tab || action->before)
		pc_menu_browser_visit(action->browser_tab);
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
	if (succeeded && action->then)
	{
		index = pc_menu_event_function_index(action->then);
		if (index != NONE)
			ui_widget_event_handler_function_invoke(widget, event, (word)index, deleted);
		if (*deleted)
			return TRUE;
	}
	open = succeeded ? action->open : action->otherwise;
	if (open && !menu_tags_open(widget, event, open, deleted))
		platform_log("menus: action \"%s\": there is no screen %s", name, open);
	return TRUE;
}

/* the server browser's actions (menu_functions.c's), for screens/browser.xml */
static boolean screen_call_browser(struct widget_instance *widget, struct event_record *event, char const *name,
	boolean *deleted)
{
	*deleted = FALSE;
	if (!strcmp(name, "browser host"))
	{
		return screen_action_run(widget, event, pc_menu_browser_visit_kind() == 2 ? "open host coop" : "open host",
			deleted);
	}
	if (!strncmp(name, "browser tab ", 12))
		return pc_menu_browser_tab(name + 12);
	if (!strcmp(name, "browser sort next"))
		return pc_menu_browser_sort(NULL);
	if (!strncmp(name, "browser sort ", 13))
		return pc_menu_browser_sort(name + 13);
	if (!strcmp(name, "browser favorite"))
		return pc_menu_browser_favorite(screen_action_row);
	if (!strcmp(name, "browser join"))
		return pc_menu_browser_select(widget, event, screen_action_row, deleted);
	return FALSE;
}

/* Select Profile's "profile select" (menu_functions.c's model): the row's
profile (the chosen one's, for OK) made player 1's and back to the screen
before, as the generated list's "profile manager select" with its back; on
the row that makes one, the generated list's question (pm_empty_selected) */
#define SCREEN_PROFILE_EMPTY "main_menu/profile_manager/pm_empty_selected"

static boolean screen_call_profile(struct widget_instance *widget, struct event_record *event, char const *name,
	boolean *deleted)
{
	*deleted = FALSE;
	if (strcmp(name, "profile select"))
		return FALSE;
	if (pc_menu_profile_choose(widget, event, screen_action_row))
	{
		menu_tags_back(widget, event, deleted);
		return TRUE;
	}
	if (!menu_tags_open(widget, event, SCREEN_PROFILE_EMPTY, deleted))
		platform_log("menus: action \"%s\": there is no screen %s", name, SCREEN_PROFILE_EMPTY);
	return TRUE;
}

/* the lobby's actions of C's own (menu_functions.c's lobby_menu.inc), the
row given */
static boolean screen_call_lobby(struct widget_instance *widget, struct event_record *event, char const *name,
	boolean *deleted)
{
	int lobby_deleted = FALSE;
	boolean result = pc_menu_lobby_action(widget, event, name, screen_action_row, &lobby_deleted) != 0;

	*deleted = lobby_deleted != 0;
	return result;
}

boolean menu_screen_step(struct widget_instance *widget, struct event_record *event, boolean *deleted, short step)
{
	struct screen_open *open = screen_open_of(widget_instance_get_topmost_parent(widget));

	*deleted = FALSE;
	return open && tall_of(open) && tall_step(open, widget, event, deleted, step);
}

boolean menu_screen_do(struct widget_instance *item, struct event_record *event, boolean *deleted)
{
	struct screen_open *open = screen_open_of(widget_instance_get_topmost_parent(item));
	long index;

	*deleted = FALSE;
	if (open && tall_of(open))
		return tall_do(open, item, event, deleted);
	for (index = 0; open && index < open->item_count; index++)
	{
		if (open->items[index] == item)
		{
			if (!open->extra)
				snprintf(screens.focused[open->place - screen_places], SCREEN_ID_LENGTH, "%s", open->ids[index]);
			return screen_action_run(item, event, open->actions[index], deleted);
		}
	}
	return FALSE;
}

boolean menu_screen_key(struct widget_instance *prompt, struct event_record *event, boolean *deleted, short controller)
{
	struct screen_open *open = screen_open_of(widget_instance_get_topmost_parent(prompt));
	long index;

	*deleted = FALSE;
	if (open && tall_of(open))
		return tall_prompt(open, prompt, event, deleted, controller);
	for (index = 0; open && index < open->prompt_count; index++)
	{
		char const *key = open->prompt_keys[index];

		if (open->prompts[index] != prompt)
			continue;
		/* (accept: the item the list has chosen, as A on it; the others their
		button pressed, which the screen takes as it takes the controller's) */
		if (!strcmp(key, "accept"))
			return open->list && open->list->focused_child && menu_screen_do(open->list->focused_child, event, deleted);
		event_manager_post_button(controller, !strcmp(key, "back") ? SCREEN_BUTTON_B :
			!strcmp(key, "x") ? SCREEN_BUTTON_X : SCREEN_BUTTON_Y);
		return TRUE;
	}
	return FALSE;
}

boolean menu_screen_back(struct widget_instance *widget, struct event_record *event, boolean *deleted)
{
	struct screen_open *open = screen_open_of(widget);

	*deleted = FALSE;
	/* (Start anywhere on a screen of its own start: that, where it does
	something) */
	if (open && open->start[0] && event->type == SCREEN_EVENT_BUTTON && event->data.button.value == 1 &&
		event->data.button.index == _gamepad_binary_button_start &&
		screen_action_run(widget, event, open->start, deleted))
	{
		return TRUE;
	}
	/* (a tall screen's keys: its buttons' and its tabs') */
	if (open && tall_of(open) && tall_key(open, widget, event, deleted))
		return TRUE;
	if (open && open->extra)
	{
		if (event->type != SCREEN_EVENT_BUTTON || event->data.button.value != 1 ||
			event->data.button.index != _gamepad_binary_button_back)
		{
			return FALSE;
		}
		return screen_action_run(widget, event, open->actions[0], deleted);
	}
	if (!open || !open->back[0] || event->type != SCREEN_EVENT_BUTTON || event->data.button.value != 1 ||
		(event->data.button.index != _gamepad_analog_button_b &&
			event->data.button.index != _gamepad_binary_button_back))
	{
		return FALSE;
	}
	/* (a tall screen whose band has a button of Back's: that, not back) */
	if (tall_of(open) && event->data.button.index == _gamepad_binary_button_back && tall_of(open)->header_button.widget)
		return FALSE;
	if (!screen_action_run(widget, event, open->back, deleted))
		return FALSE;
	return TRUE;
}

/* a block's text scale (menu_tags.c's pc_menu_text_scale): an item's label
the larger, from 4:3 to 16:9 */
boolean menu_screen_text_scale(long definition_index, short wide_extra, real *scale)
{
	if (screens.blocks_found && definition_index == screens.header_text_tag)
	{
		*scale = HEADER_TITLE_SCALE;
		return TRUE;
	}
	if (screens.blocks_found && (definition_index == screens.card_title_tag ||
		definition_index == screens.card_subtitle_tag))
	{
		real card_scale = 1.0f;
		long open_index;

		for (open_index = 0; open_index < SCREEN_MAXIMUM_OPEN; open_index++)
		{
			if (screens.open[open_index].widget && screens.open[open_index].card_scale > 0.0f)
				card_scale = screens.open[open_index].card_scale;
		}
		*scale = (definition_index == screens.card_title_tag ? CARD_TITLE_SCALE : 1.0f) * card_scale;
		return TRUE;
	}
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
	/* (a color's name in its color, white while focused; black's grey, to be
	read) */
	if (screens.blocks_found && widget->definition_tag_index == screens.color_tag && widget->parent)
	{
		long open_index, index;

		if (widget->parent->focused_child == widget)
		{
			color->red = color->green = color->blue = 1.0f;
			return TRUE;
		}
		for (open_index = 0; open_index < SCREEN_MAXIMUM_OPEN; open_index++)
		{
			struct screen_open const *open = &screens.open[open_index];

			for (index = 0; open->widget && open->colors && index < open->item_count; index++)
			{
				if (open->items[index] == widget)
				{
					real_rgb_color rgb = player_profile_get_rgb_color(index);

					color->red = MAX(rgb.red, 0.3f);
					color->green = MAX(rgb.green, 0.3f);
					color->blue = MAX(rgb.blue, 0.3f);
					return TRUE;
				}
			}
		}
		return FALSE;
	}
	if (screens.blocks_found && tall_color_of(widget, color))
		return TRUE;
	if (!screens.blocks_found || widget->definition_tag_index != screens.item_tag || !widget->parent ||
		widget->parent->focused_child != widget)
	{
		return FALSE;
	}
	color->red = color->green = color->blue = 1.0f;
	return TRUE;
}

boolean menu_screen_picture_stretched(struct widget_instance const *widget)
{
	long open_index;

	for (open_index = 0; widget && screens.blocks_found && open_index < SCREEN_MAXIMUM_OPEN; open_index++)
	{
		if (screens.open[open_index].widget && tall_screens[open_index] &&
			tall_screens[open_index]->spartan_picture == widget)
		{
			return TRUE;
		}
	}
	return FALSE;
}

boolean menu_screen_item_picture(struct widget_instance const *widget, long *bitmap_tag, rectangle2d *bounds)
{
	long open_index, index;

	/* (a tall screen's blocks: their places' widths and pictures) */
	if (screens.blocks_found && tall_picture_of(widget, bitmap_tag, bounds))
		return TRUE;
	/* (a box's middle: as wide as its legend needs) */
	if (screens.blocks_found && box_picture_of(widget, bitmap_tag, bounds))
		return TRUE;
	/* (a header: the picture its file names) */
	if (screens.blocks_found && widget->definition_tag_index == screens.header_tag)
	{
		for (open_index = 0; open_index < SCREEN_MAXIMUM_OPEN; open_index++)
		{
			struct screen_open const *open = &screens.open[open_index];

			if (open->widget && open->header == widget && open->header_picture != NONE)
			{
				*bitmap_tag = open->header_picture;
				return TRUE;
			}
		}
		return FALSE;
	}
	/* (a card and its parts: scaled to the cards' room, from the card's
	corner; its picture part, its card's picture where its file names one) */
	if (screens.blocks_found && widget->parent && (widget->definition_tag_index == screens.card_tag ||
		widget->definition_tag_index == screens.card_art_tag || widget->definition_tag_index == screens.card_title_tag ||
		widget->definition_tag_index == screens.card_subtitle_tag))
	{
		struct widget_instance const *card = widget->definition_tag_index == screens.card_tag ? widget : widget->parent;

		for (open_index = 0; open_index < SCREEN_MAXIMUM_OPEN; open_index++)
		{
			struct screen_open const *open = &screens.open[open_index];

			for (index = 0; open->widget && index < open->item_count; index++)
			{
				real scale = open->card_scale;

				if (open->items[index] != card)
					continue;
				*bitmap_tag = widget->definition_tag_index == screens.card_art_tag ? open->card_pictures[index] : NONE;
				if (scale > 0.0f)
				{
					bounds->x0 = (short)(bounds->x0 * scale);
					bounds->y0 = (short)(bounds->y0 * scale);
					bounds->x1 = (short)(bounds->x1 * scale + 0.5f);
					bounds->y1 = (short)(bounds->y1 * scale + 0.5f);
				}
				return scale > 0.0f || *bitmap_tag != NONE;
			}
		}
		return FALSE;
	}
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
