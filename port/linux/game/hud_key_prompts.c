/*
HUD_KEY_PROMPTS.C

The HUD's button prompts for the keyboard and mouse (input.button_prompts,
"auto" by default): while the first player plays with the keyboard and
mouse, a prompt's controller button ("Hold [X] to pick up", the campaign's
"Press [flashlight]") is drawn as the key or mouse button bound to the
action instead, its name in the prompt's font and colour in a drawn key
cap, as Halo PC named the bound key; while they play with a controller (or
the touch controls) it is the controller's button, as before. The keys are
config.toml's [controls] (Settings > Controls Setup), read as they change,
so a prompt follows a new binding at once (port/linux/src/xinput_sdl.c:
halo_keyboard_prompts, halo_keyboard_action_name).

The game names a prompt's button by a game control (the action, the
flashlight) or by a controller button; a button is the control the
player's profile puts on it (source/input/input_abstraction.c:
input_abstraction_port_keyboard_action). The keyboard's player is the first
local player's; what the keyboard has no action for (the D-pad, the menus'
accept and back) keeps the controller's button. Movement is the four
movement keys ("WASD"), looking the mouse.

In VR (HALO_VR) a prompt names what presses its controller button on
the controllers in the hands instead, as the OpenXR runtime names the
input bound to it, or the gesture that does (port/linux/src/vr.c:
halo_vr_button_name), unless input.button_prompts is "controller".

Only what this machine draws changes: nothing is sent, and nothing the
machines of a network game agree on.
*/

#include "cseries.h"
#include "bitmaps/bitmaps.h"
#include "cutscene/cinematics.h"
#include "game/players.h"
#include "input/input_abstraction.h"
#include "rasterizer/rasterizer.h"
#include "text/draw_string.h"
#include "halo_keyboard.h"

#ifdef HALO_VR
/* port/linux/src/vr.c (vr.h) */
int halo_vr_running(void);
int halo_vr_button_name(int button, char *name, unsigned long size);
char const *config_string(char const *name);
#endif

/* ---------- constants */

/* the HUD's icons of the sticks (hud_messaging.c's _icon_left_stick and
_icon_right_stick, and the controller buttons' indices before them) */
enum
{
	HUD_KEY_PROMPT_LEFT_STICK = 16,
	HUD_KEY_PROMPT_RIGHT_STICK = 17
};

/* ---------- prototypes */

boolean hud_key_prompt_text(short local_player_index, short game_control, short gamepad_button,
	wchar_t *text, long length);
boolean hud_key_prompt_draw(short local_player_index, short game_control, short gamepad_button,
	rectangle2d *bounds, rectangle2d *cursor, pixel32 color);
boolean hud_key_prompt_draw_key(char const *name, rectangle2d *bounds, rectangle2d *cursor, pixel32 color);

/* ---------- private code */

static void latin1_to_wide(char const *name, wchar_t *text, long length)
{
	long index;

	for (index = 0; index < length - 1 && name[index]; index++)
		text[index] = (wchar_t)(unsigned char)name[index];
	text[index] = 0;
}

/* the movement keys as one name: "WASD" for single letters, else each
separated by a slash */
static void movement_name(char *name, long size)
{
	static short const actions[] =
	{
		HALO_KEYBOARD_MOVE_FORWARD,
		HALO_KEYBOARD_STRAFE_LEFT,
		HALO_KEYBOARD_MOVE_BACKWARD,
		HALO_KEYBOARD_STRAFE_RIGHT
	};
	char keys[NUMBEROF(actions)][32];
	boolean letters = TRUE;
	long index, used = 0;

	name[0] = 0;
	for (index = 0; index < NUMBEROF(actions); index++)
	{
		if (!halo_keyboard_action_name(actions[index], keys[index], sizeof(keys[index])))
		{
			name[0] = 0;
			return;
		}
		letters = letters && keys[index][1] == 0;
	}
	for (index = 0; index < NUMBEROF(actions) && used < size - 1; index++)
	{
		char const *key = keys[index];

		if (!letters && index > 0 && used < size - 1)
			name[used++] = '/';
		while (*key && used < size - 1)
			name[used++] = *key++;
	}
	name[used] = 0;
	return;
}

#ifdef HALO_VR
/* in the headset: what presses the prompt's controller button on the
controllers in the hands (a game control's, the player's profile puts it
on), as the runtime names it, or the gesture that does (port/linux/src/
vr.c: halo_vr_button_name); FALSE for the sticks' moving and looking, and
with input.button_prompts "controller", which keep the button */
static boolean vr_prompt_text(
	short local_player_index,
	short game_control,
	short gamepad_button,
	wchar_t *text,
	long length)
{
	struct game_input_preferences preferences;
	char name[64];

	if (!strcmp(config_string("input.button_prompts"), "controller"))
		return FALSE;
	if (game_control != NONE)
	{
		if (game_control < 0 || game_control >= NUMBEROF(preferences.game_control_to_xbox_buttons))
			return FALSE;
		input_abstraction_get_local_player_preferences(local_player_index, &preferences);
		gamepad_button = preferences.game_control_to_xbox_buttons[game_control];
	}
	if (gamepad_button == NONE || gamepad_button >= HUD_KEY_PROMPT_LEFT_STICK ||
		!halo_vr_button_name(gamepad_button, name, sizeof(name)))
	{
		return FALSE;
	}
	latin1_to_wide(name, text, length);
	return TRUE;
}
#endif

/* ---------- public code */

/* the key's name at the cursor in a key cap, as the HUD draws a button
there, the cursor moved past it */
/* a part of a key cap, laid out as the text is: drawn where the text is,
scaled as it is (rasterizer_text_set_scale: the scoreboard's prompts) */
static void key_cap_quad(rectangle2d const *part, pixel32 color)
{
	real scale, origin_x, origin_y;
	rectangle2d drawn = *part;

	rasterizer_text_get_scale(&scale, &origin_x, &origin_y);
	if (scale != 1.0f)
	{
		drawn.x0 = (short)(origin_x + (part->x0 - origin_x) * scale);
		drawn.x1 = (short)(origin_x + (part->x1 - origin_x) * scale);
		drawn.y0 = (short)(origin_y + (part->y0 - origin_y) * scale);
		drawn.y1 = (short)(origin_y + (part->y1 - origin_y) * scale);
		/* (an edge a pixel wide at the least) */
		drawn.x1 = (short)MAX(drawn.x1, drawn.x0 + 1);
		drawn.y1 = (short)MAX(drawn.y1, drawn.y0 + 1);
	}
	draw_quad(&drawn, color);
}

static boolean key_cap_draw(wchar_t const *text, rectangle2d *bounds, rectangle2d *cursor, pixel32 color)
{
	rectangle2d text_bounds, box, edge;
	real scale;
	short margin, inset, line_top, middle, half;
	short start;
	pixel32 fill;

	/* (in the HUD's units, as hud_hires_button_draw's: the key cap's margin
	about its name, the text's leading inset, and half its height) */
	scale = local_player_count() > 1 ? 0.75f : 1.0f;
	margin = (short)(3.0f * scale);
	inset = (short)(4.0f * scale);
	half = (short)(10.0f * scale);
	start = cursor->x0;
	line_top = cursor->y0;
	cursor->x0 += margin;
	draw_string_set_indents(cursor->x0 - bounds->x0, 0);
	draw_unicode_string_compute_bounds(bounds, text, &text_bounds, cursor);
	cursor->x0 -= 3;
	box.x0 = (short)(start + inset);
	box.x1 = (short)(cursor->x0 + 4 + margin);
	if (cursor->y0 == line_top)
	{
		/* a key cap: the prompt's colour faintly filled, edged in it */
		middle = draw_unicode_string_capital_middle(cursor, L"");
		box.y0 = (short)(middle - half + 1);
		box.y1 = (short)(middle + half);
		fill = ((pixel32)((color >> 24) * 3 / 10) << 24) | (color & 0x00FFFFFFUL);
		key_cap_quad(&box, fill);
		edge = box;
		edge.y1 = (short)(box.y0 + 1);
		key_cap_quad(&edge, color);
		edge.y0 = (short)(box.y1 - 1);
		edge.y1 = box.y1;
		key_cap_quad(&edge, color);
		edge.y0 = (short)(box.y0 + 1);
		edge.y1 = (short)(box.y1 - 1);
		edge.x1 = (short)(box.x0 + 1);
		key_cap_quad(&edge, color);
		edge.x0 = (short)(box.x1 - 1);
		edge.x1 = box.x1;
		key_cap_quad(&edge, color);
	}
	text_bounds.x0 = bounds->x0;
	rasterizer_draw_unicode_string(&text_bounds, NULL, NULL, 0, text);
	bounds->y0 = cursor->y0;
	/* (the text that follows starts past the cap) */
	cursor->x0 = (short)(box.x1 - inset + 1);
	return TRUE;
}

/* the key a prompt names for the keyboard and mouse, for a game control or
(game_control NONE) a controller button; FALSE when the keyboard has none */
boolean hud_key_prompt_text(
	short local_player_index,
	short game_control,
	short gamepad_button,
	wchar_t *text,
	long length)
{
	char name[64];
	short action;

	name[0] = 0;
	if (game_control == NONE && gamepad_button == NONE)
		return FALSE;
	if (game_control == NONE && gamepad_button == HUD_KEY_PROMPT_LEFT_STICK)
	{
		movement_name(name, sizeof(name));
	}
	else if (game_control == NONE && gamepad_button == HUD_KEY_PROMPT_RIGHT_STICK)
	{
		csstrcpy(name, "Mouse");
	}
	else
	{
		action = input_abstraction_port_keyboard_action(local_player_index, game_control, gamepad_button);
		if (action != NONE)
			halo_keyboard_action_name(action, name, sizeof(name));
	}
	if (!name[0])
		return FALSE;
	latin1_to_wide(name, text, length);
	return TRUE;
}

/* draws the prompt's key at the cursor, as the HUD draws a button there,
and moves the cursor past it; FALSE (nothing drawn) when the prompt keeps
the controller's button */
boolean hud_key_prompt_draw(
	short local_player_index,
	short game_control,
	short gamepad_button,
	rectangle2d *bounds,
	rectangle2d *cursor,
	pixel32 color)
{
	wchar_t text[64];

	if (local_player_index != 0)
		return FALSE;
#ifdef HALO_VR
	if (halo_vr_running())
	{
		if (!vr_prompt_text(local_player_index, game_control, gamepad_button, text, NUMBEROF(text)))
			return FALSE;
	}
	else
#endif
	if (!halo_keyboard_prompts() ||
		!hud_key_prompt_text(local_player_index, game_control, gamepad_button, text, NUMBEROF(text)))
	{
		return FALSE;
	}
	return key_cap_draw(text, bounds, cursor, color);
}

/* the menus' key of their own (Settings' tabs' Q and E for LB and RB:
port/linux/src/xinput_sdl.c), drawn as the prompts' keys are while the
keyboard and mouse play; FALSE (nothing drawn) while a controller does */
boolean hud_key_prompt_draw_key(
	char const *name,
	rectangle2d *bounds,
	rectangle2d *cursor,
	pixel32 color)
{
	wchar_t text[16];

#ifdef HALO_VR
	if (halo_vr_running())
		return FALSE;
#endif
	if (!halo_keyboard_prompts())
		return FALSE;
	latin1_to_wide(name, text, NUMBEROF(text));
	return key_cap_draw(text, bounds, cursor, color);
}
