/*
WIDE_LAYOUT.C (test)

The wide layout of the port's own screens (port/linux/game/menu_tags.c):
the extra width a view lays them out in (none at the menus' 640 or less, the
view's past 640 up to 16:9's, 16:9's on a wider one) and a wide
attribute's part of it (rounded half away from 0, none at 4:3, its whole at
16:9, as much each way). test_wide_layout.py takes the code (under_test.inc)
and checks tools/port_settings.py's copy of it against the table it prints.
*/

#include "harness.h"

#include "config.inc"
#include "under_test.inc"

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	CASE("extra")
	{
		CHECK(ui_widget_port_wide_extra_of(320) == 0, "a narrow view: %d", ui_widget_port_wide_extra_of(320));
		CHECK(ui_widget_port_wide_extra_of(640) == 0, "4:3: %d", ui_widget_port_wide_extra_of(640));
		CHECK(ui_widget_port_wide_extra_of(768) == 128, "16:10: %d", ui_widget_port_wide_extra_of(768));
		CHECK(ui_widget_port_wide_extra_of(852) == WIDE_LAYOUT_EXTRA, "16:9: %d", ui_widget_port_wide_extra_of(852));
		CHECK(ui_widget_port_wide_extra_of(1138) == WIDE_LAYOUT_EXTRA, "21:9: %d",
			ui_widget_port_wide_extra_of(1138));
		CHECK(ui_widget_port_wide_extra_of(1920) == WIDE_LAYOUT_EXTRA, "32:9: %d",
			ui_widget_port_wide_extra_of(1920));
		return 0;
	}
	CASE("scale")
	{
		short value;

		for (value = -WIDE_LAYOUT_EXTRA; value <= WIDE_LAYOUT_EXTRA; value++)
		{
			CHECK(ui_widget_port_wide_scale(value, 0) == 0, "%d at 4:3: %d", value,
				ui_widget_port_wide_scale(value, 0));
			CHECK(ui_widget_port_wide_scale(value, WIDE_LAYOUT_EXTRA) == value, "%d at 16:9: %d", value,
				ui_widget_port_wide_scale(value, WIDE_LAYOUT_EXTRA));
			CHECK(ui_widget_port_wide_scale(value, 128) == -ui_widget_port_wide_scale((short)-value, 128),
				"%d at 16:10 is not -%d's negated", value, value);
		}
		/* (a half a unit away from 0, less toward it) */
		CHECK(ui_widget_port_wide_scale(1, WIDE_LAYOUT_EXTRA / 2) == 1 &&
			ui_widget_port_wide_scale(-1, WIDE_LAYOUT_EXTRA / 2) == -1, "a half rounded to %d and %d",
			ui_widget_port_wide_scale(1, WIDE_LAYOUT_EXTRA / 2), ui_widget_port_wide_scale(-1, WIDE_LAYOUT_EXTRA / 2));
		CHECK(ui_widget_port_wide_scale(7, 100) == 3, "7 at 100 is %d", ui_widget_port_wide_scale(7, 100));
		/* (half the extra width each side: the edges' moves meet the screen's) */
		CHECK(ui_widget_port_wide_scale(-WIDE_LAYOUT_EXTRA / 2, 128) == -64, "the left edge at 16:10: %d",
			ui_widget_port_wide_scale(-WIDE_LAYOUT_EXTRA / 2, 128));
		return 0;
	}
	/* (each value's part at each extra width, for the generator's copy) */
	CASE("table")
	{
		short value, extra;

		for (extra = 0; extra <= WIDE_LAYOUT_EXTRA; extra += 2)
		{
			for (value = -WIDE_LAYOUT_EXTRA; value <= WIDE_LAYOUT_EXTRA; value += 7)
				printf("%d %d %d\n", value, extra, ui_widget_port_wide_scale(value, extra));
		}
		return 0;
	}
	fprintf(stderr, "unknown case: %s\n", case_name);
	return 2;
}
