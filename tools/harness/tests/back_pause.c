/*
BACK_PAUSE.C (test)

Android's system back key (the edge swipe of gesture navigation, or the back
button) pauses a game as Escape does (port/linux/src/xinput_sdl.c's
keyboard_bound_actions and keyboard_controls); the desktop's builds have no
such key. test_back_pause.py takes the enums (config.inc, with HALO_ANDROID
for Android's build) and the code (under_test.inc).
*/

#include "harness.h"

typedef int BOOL;

#include "config.inc"

#define SDL_SCANCODE_COUNT TEST_SCANCODE_COUNT
/* the codes past the keys: the mouse's buttons, then the wheel's */
enum { INPUT_MOUSE = TEST_SCANCODE_COUNT, INPUT_WHEEL = INPUT_MOUSE + 8, INPUT_WHEEL_UP, INPUT_WHEEL_DOWN };

struct platform_input_state { unsigned char keys[TEST_SCANCODE_COUNT], mouse_buttons[8]; BOOL mouse_released; };
typedef struct { unsigned short wButtons; } XINPUT_GAMEPAD;

/* the bindings: Escape pauses, Tab shows the scores, nothing else */
static int bindings[NUMBER_OF_HALO_KEYBOARD_ACTIONS][MAXIMUM_BINDINGS];
static unsigned long keyboard_actions_held;
static unsigned long wheel_press_until_ms;
static int wheel_direction;
#define SDL_GetTicks() 0UL

static void bindings_read(void)
{
	int action, slot;

	for (action = 0; action < NUMBER_OF_HALO_KEYBOARD_ACTIONS; action++)
	{
		for (slot = 0; slot < MAXIMUM_BINDINGS; slot++)
			bindings[action][slot] = NONE;
	}
	bindings[HALO_KEYBOARD_PAUSE][0] = SDL_SCANCODE_ESCAPE;
	bindings[HALO_KEYBOARD_SCOREBOARD][0] = SDL_SCANCODE_TAB;
}

#include "under_test.inc"

/* the game's controller from the keys held (as xinput_sdl.c's game does) */
static unsigned short buttons(int key)
{
	struct platform_input_state input = { { 0 } };
	XINPUT_GAMEPAD pad = { 0 };

	if (key >= 0)
		input.keys[key] = 1;
	keyboard_controls(keyboard_bound_actions(&input), &pad);
	return pad.wButtons;
}

int main(
	int argc,
	char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	CASE("back-pauses")
	{
		CHECK(buttons(-1) == 0, "nothing held, nothing pressed: %x", buttons(-1));
		CHECK(buttons(SDL_SCANCODE_ESCAPE) == XINPUT_GAMEPAD_START, "Escape pauses");
		CHECK(buttons(SDL_SCANCODE_AC_BACK) == XINPUT_GAMEPAD_START, "Back pauses, as Start: %x",
			buttons(SDL_SCANCODE_AC_BACK));
		CHECK(keyboard_actions_held == 1UL << HALO_KEYBOARD_PAUSE, "Back is the pause action alone");
		return 0;
	}
	CASE("desktop")
	{
		CHECK(buttons(SDL_SCANCODE_ESCAPE) == XINPUT_GAMEPAD_START, "Escape pauses");
		CHECK(buttons(SDL_SCANCODE_AC_BACK) == 0, "the desktop's back key is no binding");
		return 0;
	}
	fprintf(stderr, "unknown case: %s\n", case_name);
	return 2;
}
