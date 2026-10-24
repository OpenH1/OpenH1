#include "harness.h"

/* Server Setup's bots rows (port/linux/game/menu_functions.c's
server_settings_bots, tools/port_settings.py's BOTS_ROWS): in a game of this
machine alone (PLAY WITH BOTS) they take the network's rows' places, each
written to config.toml as it changes; a gametype the bots do not play locks
them, dimmed and passed over, and the line below them says why; in a network
game they are hidden */

#define MAXIMUM_STRINGS 32
#define NUMBEROF(array) ((long)(sizeof(array) / sizeof((array)[0])))

struct widget_instance
{
	char const *name;
	long definition_tag_index;
	boolean disabled;
	boolean visible;
	real alpha_modifier;
	struct widget_instance *parent, *child, *next, *focused_child;
	union
	{
		struct
		{
			short selected_index;
			struct widget_instance *extended_description;
		} list;
		struct
		{
			short string_list_index;
		} text_box;
	} parameters;
};

#include "config.inc"

/* the fake world: the three settings, config.toml's values, the writes */
static struct pc_menu_setting settings[3];
static char config_values[3][16];
static int writes;

static struct pc_menu_setting *pc_menu_setting_get(long definition_index)
{
	return definition_index >= 1 && definition_index <= 3 ? &settings[definition_index - 1] : NULL;
}

static struct widget_instance *named(struct widget_instance *widget, char const *name, long nth)
{
	struct widget_instance *child;

	(void)nth;
	if (!widget)
		return NULL;
	if (!strcmp(widget->name, name))
		return widget;
	for (child = widget->child; child; child = child->next)
	{
		struct widget_instance *found = named(child, name, 0);

		if (found)
			return found;
	}
	return NULL;
}

static void visible_set(struct widget_instance *widget, boolean visible)
{
	if (widget)
		widget->visible = visible;
}

static boolean setting_write(char const *name, char const *value)
{
	long index;

	for (index = 0; index < 3; index++)
	{
		if (!strcmp(name, settings[index].setting))
		{
			snprintf(config_values[index], sizeof(config_values[index]), "%s", value);
			writes++;
			return TRUE;
		}
	}
	return FALSE;
}

#define platform_log(...) ((void)0)

#include "under_test.inc"

/* Server Setup: the screen (its list, the help, the bots' line); the list's
rows: GAME NAME, GAME (the network's), the three bots rows (a label and a
spinner each), MAP SHARING, the buttons */
static struct widget_instance screen, list, help, hint, name_row, game_mode_row, invite_row, map_share_row, button_bar;
static struct widget_instance rows[3], labels[3], spinners[3];

static void setup(char const *count, char const *skill, char const *teams)
{
	static char const *const names[3][3] =
	{
		{ "op_bots_count", "bots_count_label", "bots_count_spinner" },
		{ "op_bots_skill", "bots_skill_label", "bots_skill_spinner" },
		{ "op_bots_teams", "bots_teams_label", "bots_teams_spinner" },
	};
	static char const *const setting_names[3] = { "bots.count", "bots.skill", "bots.teams" };
	static char const *const values[3][4] =
	{
		{ "0", "1", "2", "3" },
		{ "easy", "normal", "heroic", "legendary" },
		{ "even", "against" },
	};
	static long const value_counts[3] = { 4, 4, 2 };
	char const *const loaded[3] = { count, skill, teams };
	long index;

	writes = 0;
	screen = (struct widget_instance){ .name = "server_settings_screen", .visible = TRUE, .alpha_modifier = 1.0f,
		.child = &list };
	list = (struct widget_instance){ .name = "options_menu", .visible = TRUE, .alpha_modifier = 1.0f,
		.parent = &screen, .child = &name_row, .next = &hint, .focused_child = &name_row };
	list.parameters.list.extended_description = &help;
	help = (struct widget_instance){ .name = "help", .visible = TRUE, .alpha_modifier = 1.0f };
	hint = (struct widget_instance){ .name = "bots_hint", .visible = TRUE, .alpha_modifier = 1.0f, .parent = &screen };
	name_row = (struct widget_instance){ .name = "op_server_name", .visible = TRUE, .alpha_modifier = 1.0f,
		.parent = &list, .next = &game_mode_row };
	game_mode_row = (struct widget_instance){ .name = "op_game_mode", .visible = TRUE, .alpha_modifier = 1.0f,
		.parent = &list, .next = &invite_row };
	invite_row = (struct widget_instance){ .name = "op_invite", .visible = TRUE, .alpha_modifier = 1.0f,
		.parent = &list, .next = &rows[0] };
	for (index = 0; index < 3; index++)
	{
		long value;

		rows[index] = (struct widget_instance){ .name = names[index][0], .visible = TRUE, .alpha_modifier = 1.0f,
			.parent = &list, .child = &labels[index], .next = index < 2 ? &rows[index + 1] : &map_share_row };
		labels[index] = (struct widget_instance){ .name = names[index][1], .visible = TRUE, .alpha_modifier = 1.0f,
			.parent = &rows[index], .next = &spinners[index] };
		spinners[index] = (struct widget_instance){ .name = names[index][2], .definition_tag_index = index + 1,
			.visible = TRUE, .alpha_modifier = 1.0f, .parent = &rows[index] };
		memset(&settings[index], 0, sizeof(settings[index]));
		settings[index].definition_index = index + 1;
		settings[index].setting = setting_names[index];
		settings[index].value_count = value_counts[index];
		for (value = 0; value < value_counts[index]; value++)
			settings[index].values[value] = values[index][value];
		snprintf(config_values[index], sizeof(config_values[index]), "%s", loaded[index]);
		/* ("port setting load", on the spinner's created event) */
		settings[index].loaded_index = 0;
		for (value = 0; value < value_counts[index]; value++)
		{
			if (!strcmp(values[index][value], loaded[index]))
				settings[index].loaded_index = (short)value;
		}
		spinners[index].parameters.list.selected_index = settings[index].loaded_index;
	}
	map_share_row = (struct widget_instance){ .name = "op_map_share_options", .visible = TRUE, .alpha_modifier = 1.0f,
		.parent = &list, .next = &button_bar };
	button_bar = (struct widget_instance){ .name = "button_bar", .visible = TRUE, .alpha_modifier = 1.0f,
		.parent = &list };
}

static void frames(boolean local, boolean playable, int count)
{
	while (count--)
		server_settings_bots(&list, local, playable);
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	long index;

	/* a network game (system link, internet): no bots rows, the network's
	rows, nothing written */
	if (!strcmp(case_name, "network"))
	{
		setup("2", "normal", "even");
		frames(FALSE, TRUE, 3);
		for (index = 0; index < 3; index++)
			CHECK(!rows[index].visible, "bots row %ld shown in a network game", index);
		CHECK(game_mode_row.visible && invite_row.visible && map_share_row.visible, "a network row hidden");
		CHECK(!hint.visible, "the bots' line shown in a network game");
		spinners[0].parameters.list.selected_index = 3;
		frames(FALSE, TRUE, 1);
		CHECK(writes == 0, "a network game wrote %d settings", writes);
		return 0;
	}

	/* PLAY WITH BOTS, a Slayer gametype: the rows in the network's rows'
	place, each written once as it changes, each its help */
	if (!strcmp(case_name, "local"))
	{
		setup("0", "normal", "even");
		frames(TRUE, TRUE, 3);
		for (index = 0; index < 3; index++)
		{
			CHECK(rows[index].visible && !rows[index].disabled && !spinners[index].disabled &&
				rows[index].alpha_modifier == 1.0f, "bots row %ld hidden, locked or dimmed", index);
		}
		CHECK(!game_mode_row.visible && !invite_row.visible && !map_share_row.visible, "a network row shown");
		CHECK(name_row.visible, "GAME NAME hidden");
		CHECK(!hint.visible, "the bots' line shown for Slayer");
		CHECK(writes == 0, "nothing changed, %d written", writes);
		spinners[0].parameters.list.selected_index = 3;
		spinners[1].parameters.list.selected_index = 3;
		spinners[2].parameters.list.selected_index = 1;
		frames(TRUE, TRUE, 3);
		CHECK(writes == 3, "%d writes, not one a change", writes);
		CHECK(!strcmp(config_values[0], "3") && !strcmp(config_values[1], "legendary") &&
			!strcmp(config_values[2], "against"), "wrote %s %s %s", config_values[0], config_values[1],
			config_values[2]);
		for (index = 0; index < 3; index++)
		{
			list.focused_child = &rows[index];
			frames(TRUE, TRUE, 1);
			CHECK(help.parameters.text_box.string_list_index == BOTS_HELP + index, "row %ld's help is %d", index,
				help.parameters.text_box.string_list_index);
		}
		return 0;
	}

	/* PLAY WITH BOTS, a gametype the bots do not play (CTF, Oddball, King,
	Race): the rows dimmed, passed over, never written, the line shown; the
	focus on one goes to the buttons */
	if (!strcmp(case_name, "locked"))
	{
		setup("2", "heroic", "against");
		list.focused_child = &rows[1];
		frames(TRUE, FALSE, 3);
		for (index = 0; index < 3; index++)
		{
			CHECK(rows[index].visible, "locked bots row %ld hidden", index);
			CHECK(rows[index].disabled && spinners[index].disabled, "locked bots row %ld takes focus", index);
			CHECK(rows[index].alpha_modifier > 0.0f && rows[index].alpha_modifier < 1.0f, "row %ld not dimmed", index);
		}
		CHECK(hint.visible, "no line saying why the bots are locked");
		CHECK(!game_mode_row.visible && !invite_row.visible, "a network row shown");
		CHECK(list.focused_child == &button_bar && list.parameters.list.selected_index == 7,
			"focus left on a locked row");
		spinners[0].parameters.list.selected_index = 0;
		frames(TRUE, FALSE, 2);
		CHECK(writes == 0 && !strcmp(config_values[0], "2"), "a locked row wrote %s", config_values[0]);
		/* (back to Slayer: unlocked, the line gone) */
		spinners[0].parameters.list.selected_index = settings[0].loaded_index;
		frames(TRUE, TRUE, 1);
		CHECK(!rows[0].disabled && rows[0].alpha_modifier == 1.0f && !hint.visible, "still locked");
		return 0;
	}

	fprintf(stderr, "unknown case: %s\n", case_name);
	return 2;
}
