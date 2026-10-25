/*
SCOREBOARD_PAD.C (test)

The scoreboard's player menu by controller and keyboard: the game's side
(source/game/game_engine.c's scoreboard_pad_update and its helpers: the
light over the players, moved up and down a list and across the columns,
through the pages; the scoreboard pinned open as it moves; the menu opened,
its items moved through, one chosen, the menu closed and the scoreboard let
go; the prompts) and the controller's (port/linux/src/xinput_sdl.c's
scoreboard_pad_read and halo_scoreboard_pad: the presses kept for the game
only while its scoreboard asks, what the scoreboard takes from the game while
it offers its menu and while it holds the controls, and what it took kept
from the game till let go of). test_scoreboard_pad.py takes the code.
*/

#include "harness.h"

#include <wchar.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define PIN(n, floor, ceiling) ((n) < (floor) ? (floor) : MIN((n), (ceiling)))
#define MULTIPLAYER_MAXIMUM_PLAYERS 16
#define PORT_COUNT 4

typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef short SHORT;
typedef struct
{
	WORD wButtons;
	BYTE bAnalogButtons[8];
	SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XINPUT_GAMEPAD;
typedef int pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER 0
#define pthread_mutex_lock(lock) ((void)(lock))
#define pthread_mutex_unlock(lock) ((void)(lock))

#include "config.inc"
#include "under_test.inc"

#define BIT(name) (1UL << HALO_SCOREBOARD_PAD_##name)

/* ---------- the game's side */

static struct scoreboard_slot slots[MULTIPLAYER_MAXIMUM_PLAYERS];
static short slot_count;

static void slots_clear(void)
{
	slot_count = 0;
}

/* a player (by an index of his own) in a list at a place */
static void slot_add(long player_index, short list, short position, boolean pickable)
{
	struct scoreboard_slot *slot = &slots[slot_count++];

	slot->player_index = player_index;
	slot->list = list;
	slot->position = position;
	slot->pickable = pickable;
}

static void view_reset(struct scoreboard_view *view)
{
	memset(view, 0, sizeof(*view));
	view->player_index = NONE;
	view->item = NONE;
	view->picked = NONE;
}

/* a frame of presses on one list of rows by columns */
static short one_list(struct scoreboard_view *view, unsigned long pressed, long rows, long columns, short items,
	short *chosen)
{
	return scoreboard_pad_update(view, pressed, slots, slot_count, FALSE, rows, columns, rows * columns, slot_count,
		items, chosen);
}

/* ... on two team columns of rows (total: the longer's) */
static short team_lists(struct scoreboard_view *view, unsigned long pressed, long rows, long total, short items,
	short *chosen)
{
	return scoreboard_pad_update(view, pressed, slots, slot_count, TRUE, rows, 2, rows, total, items, chosen);
}

/* ---------- the controller's side */

static XINPUT_GAMEPAD pad_with(WORD buttons, int analog, int analog2)
{
	XINPUT_GAMEPAD pad;

	memset(&pad, 0, sizeof(pad));
	pad.wButtons = buttons;
	if (analog >= 0)
		pad.bAnalogButtons[analog] = 255;
	if (analog2 >= 0)
		pad.bAnalogButtons[analog2] = 255;
	return pad;
}

/* a frame: the controller read (the game's say, as the reading finds it),
then the game's ask of it, saying how it holds it from now */
static unsigned long frame(int port, XINPUT_GAMEPAD *pad, BOOL menus, int ask)
{
	int hold = scoreboard_pad_port_hold(port, menus);

	scoreboard_pad_read(port, pad, 0, hold);
	return ask >= 0 ? halo_scoreboard_pad((short)port, ask) : 0;
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	struct scoreboard_view view;
	short chosen;
	short result;

	CASE("light")
	{
		/* one list: the viewer (not pickable) first, then three others, a
		page of 2 rows by 1 column showing places 2 and 3 */
		slots_clear();
		slot_add(100, 0, 0, FALSE);
		slot_add(101, 0, 1, TRUE);
		slot_add(102, 0, 2, TRUE);
		slot_add(103, 0, 3, FALSE);
		slot_add(104, 0, 4, TRUE);
		view_reset(&view);
		view.scroll = 2;
		one_list(&view, 0, 2, 1, 0, &chosen);
		CHECK(view.picked == 102, "the first pickable shown is lit: %ld", view.picked);
		CHECK(!view.pinned, "lighting pins nothing");
		/* down past the one that is not pickable */
		one_list(&view, BIT(DOWN), 2, 1, 0, &chosen);
		CHECK(view.picked == 104, "down skips another of this machine's players: %ld", view.picked);
		CHECK(view.scroll == 3, "the page follows the light: %ld", view.scroll);
		one_list(&view, BIT(DOWN), 2, 1, 0, &chosen);
		CHECK(view.picked == 104, "no wrap past the last: %ld", view.picked);
		one_list(&view, BIT(UP), 2, 1, 0, &chosen);
		one_list(&view, BIT(UP), 2, 1, 0, &chosen);
		CHECK(view.picked == 101, "up to the first pickable: %ld", view.picked);
		CHECK(view.scroll == 1, "the page follows it up: %ld", view.scroll);
		one_list(&view, BIT(UP), 2, 1, 0, &chosen);
		CHECK(view.picked == 101, "nothing above it to light: %ld", view.picked);
		one_list(&view, BIT(LEFT) | BIT(RIGHT), 2, 1, 0, &chosen);
		CHECK(view.picked == 101, "one column: left and right stay: %ld", view.picked);

		/* the light follows its player as the list reorders */
		slots[1].position = 4;
		slots[4].position = 1;
		one_list(&view, 0, 2, 1, 0, &chosen);
		CHECK(view.picked == 101, "the lit player stays lit as he moves: %ld", view.picked);
		/* ... and a player gone lights the first shown again */
		slots[1].pickable = FALSE;
		one_list(&view, 0, 2, 1, 0, &chosen);
		CHECK(view.picked == 104, "his leaving lights the first shown: %ld", view.picked);

		/* one list in two columns of 3 rows: right goes rows on */
		slots_clear();
		slot_add(200, 0, 0, TRUE);
		slot_add(201, 0, 1, TRUE);
		slot_add(202, 0, 2, TRUE);
		slot_add(203, 0, 3, FALSE);
		slot_add(204, 0, 4, TRUE);
		view_reset(&view);
		one_list(&view, BIT(DOWN), 3, 2, 0, &chosen);
		CHECK(view.picked == 201, "down: %ld", view.picked);
		one_list(&view, BIT(RIGHT), 3, 2, 0, &chosen);
		CHECK(view.picked == 204, "right: the other column's nearest the row: %ld", view.picked);
		one_list(&view, BIT(LEFT), 3, 2, 0, &chosen);
		CHECK(view.picked == 201, "left back: %ld", view.picked);

		/* team columns: red's and blue's, the first lit the top row's
		left */
		slots_clear();
		slot_add(300, 1, 0, TRUE);
		slot_add(301, 0, 0, FALSE);
		slot_add(302, 0, 1, TRUE);
		slot_add(303, 1, 1, TRUE);
		slot_add(304, 0, 2, TRUE);
		slot_add(305, 1, 3, TRUE);
		view_reset(&view);
		team_lists(&view, 0, 4, 4, 0, &chosen);
		CHECK(view.picked == 300, "the top row's first pickable: %ld", view.picked);
		team_lists(&view, BIT(LEFT), 4, 4, 0, &chosen);
		CHECK(view.picked == 302, "left: red's nearest the row: %ld", view.picked);
		team_lists(&view, BIT(LEFT), 4, 4, 0, &chosen);
		CHECK(view.picked == 302, "no further left: %ld", view.picked);
		team_lists(&view, BIT(DOWN), 4, 4, 0, &chosen);
		CHECK(view.picked == 304, "down red's column: %ld", view.picked);
		team_lists(&view, BIT(RIGHT), 4, 4, 0, &chosen);
		CHECK(view.picked == 303, "right: blue's nearest, the higher of two as near: %ld", view.picked);
		team_lists(&view, BIT(DOWN), 4, 4, 0, &chosen);
		CHECK(view.picked == 305, "down blue's column: %ld", view.picked);

		/* none to pick */
		slots_clear();
		slot_add(400, 0, 0, FALSE);
		view_reset(&view);
		result = one_list(&view, BIT(DOWN) | BIT(OPEN), 4, 1, 0, &chosen);
		CHECK(view.picked == NONE && view.player_index == NONE && result == _scoreboard_pad_nothing,
			"none lit, no menu, with no player to pick");
		return 0;
	}

	CASE("pin")
	{
		slots_clear();
		slot_add(10, 0, 0, FALSE);
		slot_add(11, 0, 1, TRUE);
		slot_add(12, 0, 2, TRUE);
		view_reset(&view);
		/* held open: the stick moves the player, not the light */
		one_list(&view, BIT(STICK_DOWN), 4, 1, 0, &chosen);
		CHECK(view.picked == 11 && !view.pinned, "the stick moves no light until pinned: %ld", view.picked);
		CHECK(scoreboard_pad_hold(&view) == HALO_SCOREBOARD_PAD_OFFERED, "offered: the D-pad and X taken");
		/* the D-pad pins it, moving */
		one_list(&view, BIT(DOWN), 4, 1, 0, &chosen);
		CHECK(view.pinned && view.picked == 12, "the D-pad pins and moves: %ld", view.picked);
		CHECK(scoreboard_pad_hold(&view) == HALO_SCOREBOARD_PAD_HOLDS, "pinned: the controls held");
		one_list(&view, BIT(STICK_UP), 4, 1, 0, &chosen);
		CHECK(view.picked == 11, "pinned, the stick moves the light: %ld", view.picked);
		/* A while not pinned is the game's (a jump) */
		one_list(&view, BIT(CANCEL), 4, 1, 0, &chosen);
		CHECK(!view.pinned, "B lets it go");
		one_list(&view, BIT(ACCEPT), 4, 1, 0, &chosen);
		CHECK(view.player_index == NONE && !view.pinned, "A opens nothing while not pinned");
		one_list(&view, BIT(DOWN), 4, 1, 0, &chosen);
		one_list(&view, BIT(CLOSE), 4, 1, 0, &chosen);
		CHECK(!view.pinned, "Back lets it go");
		/* a menu up, the game over, the scoreboard shown anew: let go */
		one_list(&view, BIT(DOWN), 4, 1, 0, &chosen);
		one_list(&view, BIT(LAPSED) | BIT(OPEN), 4, 1, 0, &chosen);
		CHECK(!view.pinned && view.picked == NONE && view.player_index == NONE, "a lapse lets it go, opening nothing");
		return 0;
	}

	CASE("menu")
	{
		slots_clear();
		slot_add(20, 0, 0, TRUE);
		slot_add(21, 0, 1, TRUE);
		view_reset(&view);
		/* X while held open: the lit player's menu, pinned */
		result = one_list(&view, BIT(OPEN), 4, 1, 5, &chosen);
		CHECK(result == _scoreboard_pad_opened && view.player_index == 20 && view.pinned && view.item == 0 &&
			!view.pointer_opened, "X opens the lit player's menu, pinned, its first item lit");
		/* up and down within its items */
		one_list(&view, BIT(UP), 4, 1, 5, &chosen);
		CHECK(view.item == 0, "no item above the first: %d", view.item);
		one_list(&view, BIT(DOWN), 4, 1, 5, &chosen);
		one_list(&view, BIT(STICK_DOWN), 4, 1, 5, &chosen);
		CHECK(view.item == 2, "down by the D-pad and stick: %d", view.item);
		one_list(&view, BIT(DOWN), 4, 1, 5, &chosen);
		one_list(&view, BIT(DOWN), 4, 1, 5, &chosen);
		one_list(&view, BIT(DOWN), 4, 1, 5, &chosen);
		CHECK(view.item == 4, "no item below the last: %d", view.item);
		CHECK(view.picked == 20, "the light stays on the menu's player: %ld", view.picked);
		/* A chooses it */
		result = one_list(&view, BIT(ACCEPT), 4, 1, 5, &chosen);
		CHECK(result == _scoreboard_pad_chose && chosen == 4, "A chooses the lit item: %d", chosen);
		/* B closes the menu, the scoreboard still pinned */
		one_list(&view, BIT(CANCEL), 4, 1, 5, &chosen);
		CHECK(view.player_index == NONE && view.pinned, "B closes the menu, the scoreboard pinned");
		/* A (pinned) opens it again, Back closes both */
		result = one_list(&view, BIT(DOWN) | BIT(ACCEPT), 4, 1, 5, &chosen);
		CHECK(result == _scoreboard_pad_opened && view.player_index == 21, "A opens the next lit: %ld",
			view.player_index);
		one_list(&view, BIT(CLOSE), 4, 1, 5, &chosen);
		CHECK(view.player_index == NONE && !view.pinned, "Back closes the menu and lets the scoreboard go");
		/* the mouse's menu: its first A lights, the next chooses; a lapse
		keeps it */
		view_reset(&view);
		view.player_index = 21;
		view.pointer_opened = TRUE;
		result = one_list(&view, BIT(ACCEPT), 4, 1, 3, &chosen);
		CHECK(result == _scoreboard_pad_nothing && view.item == 0, "the mouse's menu: A lights its first item");
		result = one_list(&view, BIT(ACCEPT), 4, 1, 3, &chosen);
		CHECK(result == _scoreboard_pad_chose && chosen == 0, "... and chooses it next");
		one_list(&view, BIT(LAPSED), 4, 1, 3, &chosen);
		CHECK(view.player_index == 21, "a lapse leaves the mouse's menu to the mouse");
		return 0;
	}

	CASE("prompts")
	{
		struct scoreboard_prompt prompts[2];
		short count;

		view_reset(&view);
		count = scoreboard_pad_prompts(&view, FALSE, prompts);
		CHECK(count == 1 && prompts[0].icon == _gamepad_analog_button_x && !wcscmp(prompts[0].label, L"Player options"),
			"held open: [X] Player options");
		count = scoreboard_pad_prompts(&view, TRUE, prompts);
		CHECK(count == 1 && !strcmp(prompts[0].key, "Enter"), "the keyboard's: Enter");
		view.pinned = TRUE;
		count = scoreboard_pad_prompts(&view, FALSE, prompts);
		CHECK(count == 2 && prompts[0].icon == _gamepad_analog_button_a && prompts[1].icon == _gamepad_analog_button_b &&
			!wcscmp(prompts[1].label, L"Close"), "pinned: [A] Player options, [B] Close");
		CHECK(!strcmp(prompts[1].key, "Esc"), "the keyboard's B: Esc");
		view.player_index = 5;
		count = scoreboard_pad_prompts(&view, FALSE, prompts);
		CHECK(count == 2 && !wcscmp(prompts[0].label, L"Choose") && !wcscmp(prompts[1].label, L"Back"),
			"the menu: [A] Choose, [B] Back");
		return 0;
	}

	CASE("read")
	{
		XINPUT_GAMEPAD pad;
		unsigned long pressed;
		int index;

		/* the scoreboard not shown: nothing taken, no press kept */
		pad = pad_with(XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_X, -1);
		frame(0, &pad, FALSE, -1);
		CHECK(pad.wButtons == XINPUT_GAMEPAD_DPAD_UP && pad.bAnalogButtons[XINPUT_GAMEPAD_X] == 255,
			"hidden: the game gets the D-pad and X");
		pad = pad_with(0, -1, -1);
		frame(0, &pad, FALSE, -1);
		/* its first ask: lapsed (what was pressed before is not its) */
		pad = pad_with(0, XINPUT_GAMEPAD_X, -1);
		pressed = frame(0, &pad, FALSE, HALO_SCOREBOARD_PAD_OFFERED);
		CHECK(pressed == BIT(LAPSED), "the first ask is told of the lapse only: %lx", pressed);
		/* offered: the D-pad and X taken, A and the stick the game's */
		pad = pad_with(XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_X);
		pad.sThumbLY = 30000;
		pressed = frame(0, &pad, FALSE, HALO_SCOREBOARD_PAD_OFFERED);
		CHECK(pad.wButtons == XINPUT_GAMEPAD_BACK && pad.bAnalogButtons[XINPUT_GAMEPAD_A] == 255 &&
			pad.bAnalogButtons[XINPUT_GAMEPAD_X] == 0 && pad.sThumbLY == 30000,
			"offered: the D-pad and X taken, Back, A and the stick left: %x", pad.wButtons);
		CHECK(pressed == (BIT(DOWN) | BIT(CLOSE) | BIT(ACCEPT) | BIT(STICK_UP)), "pressed: %lx", pressed);
		/* held, not pressed again */
		pad = pad_with(XINPUT_GAMEPAD_DPAD_DOWN, -1, -1);
		pressed = frame(0, &pad, FALSE, HALO_SCOREBOARD_PAD_HOLDS);
		CHECK(pressed == 0, "a button held is pressed once: %lx", pressed);
		/* holding the controls: everything taken */
		pad = pad_with(XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_RIGHT_TRIGGER);
		pad.sThumbLX = -30000;
		pad.sThumbRY = 20000;
		pressed = frame(0, &pad, FALSE, HALO_SCOREBOARD_PAD_OFFERED);
		CHECK(pad.wButtons == 0 && pad.sThumbLX == 0 && pad.sThumbRY == 0, "holding: the buttons and sticks taken");
		for (index = 0; index < 8; index++)
			CHECK(pad.bAnalogButtons[index] == 0, "holding: analog button %d taken", index);
		CHECK(pressed == (BIT(CANCEL) | BIT(STICK_LEFT)), "pressed while holding: %lx", pressed);
		/* let go of (offered again): the B held stays the scoreboard's... */
		pad = pad_with(0, XINPUT_GAMEPAD_B, -1);
		frame(0, &pad, FALSE, -1);
		CHECK(pad.bAnalogButtons[XINPUT_GAMEPAD_B] == 0, "the B that closed it is no melee while held");
		/* ... until let go of; then it is the game's */
		pad = pad_with(0, -1, -1);
		frame(0, &pad, FALSE, HALO_SCOREBOARD_PAD_OFFERED);
		pad = pad_with(0, XINPUT_GAMEPAD_B, -1);
		frame(0, &pad, FALSE, HALO_SCOREBOARD_PAD_OFFERED);
		CHECK(pad.bAnalogButtons[XINPUT_GAMEPAD_B] == 255, "a new B is the game's");

		/* the game stops asking (its game over): the controller let go
		within a few readings, the presses dropped, the next ask lapsed */
		for (index = 0; index <= SCOREBOARD_PAD_READINGS; index++)
		{
			pad = pad_with(XINPUT_GAMEPAD_DPAD_UP, -1, -1);
			frame(0, &pad, FALSE, -1);
		}
		pad = pad_with(XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_X, -1);
		frame(0, &pad, FALSE, -1);
		CHECK(pad.wButtons == XINPUT_GAMEPAD_DPAD_DOWN && pad.bAnalogButtons[XINPUT_GAMEPAD_X] == 255,
			"no longer asked: the game's again: %x", pad.wButtons);
		pressed = halo_scoreboard_pad(0, HALO_SCOREBOARD_PAD_OFFERED);
		CHECK(pressed == BIT(LAPSED), "the next ask is lapsed, no stale press: %lx", pressed);
		/* a slow frame lets nothing go: asked each frame however long */
		pad = pad_with(0, -1, -1);
		frame(0, &pad, FALSE, HALO_SCOREBOARD_PAD_HOLDS);
		pad = pad_with(XINPUT_GAMEPAD_DPAD_UP, -1, -1);
		pressed = frame(0, &pad, FALSE, HALO_SCOREBOARD_PAD_HOLDS);
		CHECK(pressed == BIT(UP) && pad.wButtons == 0, "asked every frame: kept: %lx", pressed);
		/* a menu up: the game's, and lapsed */
		pad = pad_with(XINPUT_GAMEPAD_DPAD_DOWN, -1, -1);
		pressed = frame(0, &pad, TRUE, HALO_SCOREBOARD_PAD_HOLDS);
		CHECK(pad.wButtons == XINPUT_GAMEPAD_DPAD_DOWN && pressed == BIT(LAPSED),
			"a menu up: the controller the menu's: %x %lx", pad.wButtons, pressed);
		/* another port's is its own */
		pad = pad_with(XINPUT_GAMEPAD_DPAD_DOWN, -1, -1);
		frame(1, &pad, FALSE, -1);
		CHECK(pad.wButtons == XINPUT_GAMEPAD_DPAD_DOWN, "port 1 is not port 0's scoreboard's");
		return 0;
	}

	CASE("keys")
	{
		unsigned char keys[TEST_SCANCODE_COUNT];

		memset(keys, 0, sizeof(keys));
		keys[SDL_SCANCODE_UP] = keys[SDL_SCANCODE_RETURN] = 1;
		CHECK(scoreboard_pad_keys(keys, 0) == (BIT(UP) | BIT(OPEN) | BIT(ACCEPT)), "up and Enter");
		memset(keys, 0, sizeof(keys));
		keys[SDL_SCANCODE_ESCAPE] = keys[SDL_SCANCODE_DOWN] = 1;
		CHECK(scoreboard_pad_keys(keys, 1UL << HALO_KEYBOARD_SCOREBOARD) == (BIT(DOWN) | BIT(CANCEL) | BIT(CLOSE)),
			"down, Escape and the scoreboard's key");
		memset(keys, 0, sizeof(keys));
		keys[SDL_SCANCODE_SPACE] = keys[SDL_SCANCODE_W] = 1;
		CHECK(scoreboard_pad_keys(keys, 1UL << HALO_KEYBOARD_JUMP) == 0, "the game's keys are none of its");
		return 0;
	}

	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
