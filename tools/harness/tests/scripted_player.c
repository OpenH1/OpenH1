#include "harness.h"

#include <math.h>

/* The automated tests' scripted player (port/linux/src/xinput_sdl.c's
test_input_gamepad, debug.test_input "bot", "look", "walk" and the network
test's held action button) plays in a game, never in the menus: while the
menus' map is loaded (the main menu, the lobby, Server Setup) it leaves the
controller alone, as its stick and its A every few seconds would work the
lobby's buttons (GAME SETTINGS, then Server Setup's START GAME, which a
network test's game takes as the host's own choices). The menus' own
scripts ("menu:") still play there */

typedef unsigned short WORD;
typedef short SHORT;
typedef unsigned char BYTE;

enum { XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_BLACK,
	XINPUT_GAMEPAD_WHITE, XINPUT_GAMEPAD_LEFT_TRIGGER, XINPUT_GAMEPAD_RIGHT_TRIGGER, NUMBER_OF_ANALOG_BUTTONS };
#define XINPUT_GAMEPAD_DPAD_UP 0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN 0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT 0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT 0x0008
#define XINPUT_GAMEPAD_START 0x0010

typedef struct
{
	WORD wButtons;
	BYTE bAnalogButtons[NUMBER_OF_ANALOG_BUTTONS];
	SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XINPUT_GAMEPAD;

#include "config.inc"

/* the fake world: debug.test_input, the clock, the menus' map loaded */
static char const *test_input_setting = "";
static double clock_seconds;
static boolean menus_loaded;
static int menu_script_plays;

static char const *config_string(char const *name)
{
	return !strcmp(name, "debug.test_input") ? test_input_setting : "";
}

static unsigned char network_chat_keyboard_open(void) { return 0; }
static unsigned char main_menu_is_loaded(void) { return (unsigned char)menus_loaded; }
static unsigned long long SDL_GetTicks(void) { return (unsigned long long)(clock_seconds * 1000.0); }
static double test_input_seconds(void) { return clock_seconds; }

/* (the other ways of debug.test_input, each its own function in the code) */
static unsigned long test_input_pressed;
static int test_input_holding_action;
static double test_input_holding_action_since;
static int test_script_on;
static char test_input_menu[512];
static unsigned long long test_input_menu_since;

static void test_script_parse(char const *script) { (void)script; test_script_on = 1; }
static void test_script_gamepad(XINPUT_GAMEPAD *pad) { (void)pad; }
static void test_pad_gamepad(XINPUT_GAMEPAD *pad) { (void)pad; }
static void test_latency_gamepad(XINPUT_GAMEPAD *pad, int seed) { (void)pad; (void)seed; }

static void test_input_menu_gamepad(XINPUT_GAMEPAD *pad)
{
	menu_script_plays++;
	pad->bAnalogButtons[XINPUT_GAMEPAD_A] = 255;
}

#include "under_test.inc"

static boolean pad_touched(XINPUT_GAMEPAD const *pad)
{
	long index;

	for (index = 0; index < NUMBER_OF_ANALOG_BUTTONS; index++)
	{
		if (pad->bAnalogButtons[index])
			return TRUE;
	}
	return pad->wButtons || pad->sThumbLX || pad->sThumbLY || pad->sThumbRX || pad->sThumbRY;
}

/* the controller read every 50 ms for 20 seconds, the menus' map loaded or
not: how many reads it was touched in, and pressed A in */
static void play(boolean loaded, int *touched, int *pressed_a)
{
	int step;

	menus_loaded = loaded;
	*touched = *pressed_a = 0;
	for (step = 0; step < 400; step++)
	{
		XINPUT_GAMEPAD pad;

		memset(&pad, 0, sizeof(pad));
		clock_seconds = step * 0.05;
		test_input_gamepad(&pad);
		*touched += pad_touched(&pad);
		*pressed_a += pad.bAnalogButtons[XINPUT_GAMEPAD_A] != 0;
	}
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	int touched, pressed_a;

	CASE("bot_menus")
	{
		test_input_setting = "bot:1";
		play(TRUE, &touched, &pressed_a);
		CHECK(!touched, "the scripted player worked the menus in %d reads (A in %d)", touched, pressed_a);
		/* (and plays once a game is loaded) */
		play(FALSE, &touched, &pressed_a);
		CHECK(touched > 300 && pressed_a > 0, "the scripted player does not play in a game: %d reads, A in %d",
			touched, pressed_a);
		return 0;
	}
	CASE("bot_game")
	{
		test_input_setting = "bot:2";
		play(FALSE, &touched, &pressed_a);
		CHECK(touched > 300 && pressed_a > 0, "the scripted player does not play in a game: %d reads, A in %d",
			touched, pressed_a);
		return 0;
	}
	CASE("look_menus")
	{
		test_input_setting = "look:1";
		play(TRUE, &touched, &pressed_a);
		CHECK(!touched, "the looking player turned in the menus in %d reads", touched);
		play(FALSE, &touched, &pressed_a);
		CHECK(touched > 300, "the looking player does not look in a game");
		return 0;
	}
	CASE("walk_menus")
	{
		test_input_setting = "walk:1";
		play(TRUE, &touched, &pressed_a);
		CHECK(!touched, "the walking player worked the menus in %d reads (A in %d)", touched, pressed_a);
		play(FALSE, &touched, &pressed_a);
		CHECK(touched > 300, "the walking player does not walk in a game");
		return 0;
	}
	CASE("hold_menus")
	{
		/* (the network test's player picking up: X held) */
		test_input_setting = "bot:1";
		test_input_holding_action = 1;
		play(TRUE, &touched, &pressed_a);
		CHECK(!touched, "the held action button pressed in the menus in %d reads", touched);
		play(FALSE, &touched, &pressed_a);
		CHECK(touched > 300, "the action button is not held in a game");
		return 0;
	}
	CASE("menu_script")
	{
		/* (debug.test_input "menu:", whose buttons are the menus') */
		test_input_setting = "menu:a";
		play(TRUE, &touched, &pressed_a);
		CHECK(menu_script_plays == 400 && pressed_a == 400, "the menus' script did not play in the menus: %d",
			menu_script_plays);
		return 0;
	}
	CHECK(FALSE, "no such case: %s", case_name);
	return 1;
}
