/*
REPORT.C (test)

Save Report (port/linux/src/report.c): report.md's frame times (the average,
the 1% and 0.1% lows, the frames of 50 ms or more, the longest frames, a
stall only), its Where section (report_where_text: the game, the player, the
network game, the console's lines to look from there) and the zip (its
CRC-32, its local and central records), which test_report.py opens with
Python's zipfile. test_report.py takes the code.
*/

#include "harness.h"

#include <stdint.h>

#define REPORT_HITCH_MS 50
#define REPORT_LONGEST 8
#define REPORT_ZIP_FILES 8

#include "under_test.inc"

static uint32_t micros[2048], at_ms[2048];

int main(int argc, char **argv)
{
	char const *test = argc > 1 ? argv[1] : "";
	static char text[4096];
	/* (CHECK's condition is printed as a format: no percent signs in it) */
	static char const low_10[] = "1% low: 10.0 fps\n", lowest_10[] = "0.1% low: 10.0 fps\n";
	static char const low_60[] = "1% low: 60.0 fps";

	if (!strcmp(test, "crc"))
	{
		CHECK(report_crc32(0, (const unsigned char *)"123456789", 9) == 0xCBF43926u, "the check value");
		CHECK(report_crc32(report_crc32(0, (const unsigned char *)"1234", 4), (const unsigned char *)"56789", 5) ==
			0xCBF43926u, "in parts");
		return 0;
	}
	if (!strcmp(test, "frames"))
	{
		unsigned long index;

		/* (1000 frames of 10 ms, every hundredth of 100 ms: 10 of them) */
		for (index = 0; index < 1000; index++)
		{
			micros[index] = index % 100 == 99 ? 100000 : 10000;
			at_ms[index] = (uint32_t)(index * 10);
		}
		report_frame_summary(micros, at_ms, 1000, 12000, text, sizeof(text));
		printf("%s", text);
		CHECK(strstr(text, "Frames: 1000 in the last 10.9 seconds\n"), "the frames and their time");
		CHECK(strstr(text, "Average: 91.7 fps (10.90 ms a frame)\n"), "the average");
		CHECK(strstr(text, "Median frame: 10.00 ms\n"), "the median");
		CHECK(strstr(text, low_10), "the 1 percent low (the slowest 10 frames)");
		CHECK(strstr(text, lowest_10), "the 0.1 percent low (the slowest frame)");
		CHECK(strstr(text, "Frames of 50 ms or more: 10\n"), "the frames of 50 ms or more");
		/* (the latest of the longest first, and how long before the report) */
		CHECK(strstr(text, "Longest frames:\n  100.0 ms, 2.0 s ago\n  100.0 ms, 3.0 s ago\n"), "the longest");
		CHECK(!strstr(text, "  10.0 ms,"), "a frame of the median's time listed as one of the longest");
		return 0;
	}
	if (!strcmp(test, "steady"))
	{
		unsigned long index;

		for (index = 0; index < 600; index++)
		{
			micros[index] = 16667;
			at_ms[index] = (uint32_t)(index * 17);
		}
		report_frame_summary(micros, at_ms, 600, 10200, text, sizeof(text));
		printf("%s", text);
		CHECK(strstr(text, "Average: 60.0 fps"), "the average");
		CHECK(strstr(text, low_60), "the 1 percent low");
		CHECK(strstr(text, "Longest frames: none twice the median or longer\n"), "no stalls");
		/* (a frame of 50 ms exactly is one) */
		micros[0] = micros[1] = 50000;
		report_frame_summary(micros, at_ms, 600, 10200, text, sizeof(text));
		CHECK(strstr(text, "Frames of 50 ms or more: 2\n"), "frames of 50 ms exactly");
		report_frame_summary(micros, at_ms, 0, 0, text, sizeof(text));
		CHECK(!strcmp(text, "No frames yet.\n"), "no frames");
		return 0;
	}
	if (!strcmp(test, "issue"))
	{
		static char url[300], body[400];
		unsigned long index;

		CHECK(report_issue_url("Problem report (macOS, build-1)", "# Hi\n- a&b=c\n", url, sizeof(url)), "it fits");
		printf("%s\n", url);
		CHECK(!strcmp(url, "https://github.com/OpenH1/OpenH1/issues/new?title=Problem%20report%20%28macOS%2C%20build-1"
			"%29&body=%23%20Hi%0A-%20a%26b%3Dc%0A"), "the address");
		/* (a long body cut at a line's end) */
		for (index = 0; index < 20; index++)
			strcat(body, "line of text\n");
		CHECK(report_issue_url("t", body, url, 150), "a long one cut to fit");
		printf("%s\n", url);
		CHECK(strlen(url) < 150, "within its room");
		CHECK(strlen(url) > 3 && !strcmp(url + strlen(url) - 3, "%0A"), "cut at a line's end");
		CHECK(!report_issue_url("t", "no line ends here at all, and too long for the room", url, 70), "no line to cut at");
		return 0;
	}
	if (!strcmp(test, "scrub"))
	{
		static char const text[] =
			"data root: /Users/keyes/Halo/maps\n"
			"hosting: openh1://join/fba388a4deadbeef (the rest is left out)\n"
			"joined 203.0.113.7:2302 and 10.0.0.5, local 127.0.0.1, any 0.0.0.0\n"
			"vulkan: Vulkan 1.3.280, driver 24.1.3; version 300.1.2.3.4\n"
			"11.06.26 22:55:20  chat (team): Johnson: meet at the warthog\n"
			"11.06.26 22:55:21  chat: Cortana: hello\n"
			"11.06.26 22:55:22  chat notice: Text chat is off in this game\n"
			"no newline at the end";
		size_t size;
		char *out = report_scrub(text, sizeof(text) - 1, "/Users/keyes", &size);

		CHECK(out != NULL, "scrubbed");
		out[size] = 0;
		printf("%s\n", out);
		CHECK(strstr(out, "data root: ~/Halo/maps\n"), "the home folder as ~");
		CHECK(!strstr(out, "keyes"), "the account's name left");
		CHECK(strstr(out, "hosting: openh1://(removed) (the rest"), "the link taken out");
		CHECK(strstr(out, "joined x.x.x.x:2302 and x.x.x.x, local 127.0.0.1, any 0.0.0.0\n"), "the addresses");
		CHECK(strstr(out, "Vulkan 1.3.280, driver 24.1.3; version 300.1.2.3.4\n"), "versions left as they are");
		CHECK(strstr(out, "22:55:20  chat: (a chat line, removed from the report)\n"), "a team chat line");
		CHECK(!strstr(out, "warthog") && !strstr(out, "Cortana"), "a chat line left");
		CHECK(strstr(out, "chat notice: Text chat is off"), "a notice taken out");
		CHECK(size >= 21 && !memcmp(out + size - 21, "no newline at the end", 21), "the last line");
		CHECK(report_setting_private("server.password") && report_setting_private("server.web_owner_password") &&
			report_setting_private("network.public_address") && report_setting_private("debug.update_key"),
			"a private setting not left out");
		CHECK(!report_setting_private("display.vsync") && !report_setting_private("network.online"),
			"a setting left out that is not private");
		free(out);
		return 0;
	}
	if (!strcmp(test, "where"))
	{
		static struct report_where where;

		/* (the menus: nothing) */
		memset(&where, 0, sizeof(where));
		strcpy(where.map, "levels\\a10\\a10");
		where.camera = 1;
		report_where_text(&where, text, sizeof(text));
		CHECK(text[0] == 0, "a section for the menus");
		/* (a campaign on Heroic, in a vehicle, its camera wider than the script's takes) */
		where.playing = 1;
		strcpy(where.game, "levels\\a10\\a10 (campaign)");
		where.campaign = 1;
		where.difficulty = 2;
		where.bsp = 1;
		where.ticks = 4500;
		where.ticks_per_second = 30;
		where.player = where.alive = 1;
		where.position[0] = 1.5f;
		where.position[1] = -2.25f;
		where.position[2] = 3.0f;
		where.yaw = 90.0f;
		where.pitch = -10.0f;
		strcpy(where.vehicle, "vehicles\\warthog\\mp_warthog");
		where.camera_position[0] = 1.0f;
		where.camera_position[1] = 2.0f;
		where.camera_position[2] = 3.5f;
		where.camera_yaw = 45.0f;
		where.camera_pitch = -5.0f;
		where.camera_roll = 0.0f;
		where.camera_field_of_view = 110.0f;
		report_where_text(&where, text, sizeof(text));
		printf("%s", text);
		CHECK(strstr(text, "\n## Where\n\n- Game: levels\\a10\\a10 (campaign), Heroic\n"), "the game and its difficulty");
		CHECK(strstr(text, "- Time: 4500 ticks (150.0 s), BSP 1\n"), "the time and the BSP");
		CHECK(strstr(text, "- Player: at 1.500 -2.250 3.000, facing 90.0 yaw -10.0 pitch, in vehicles\\warthog\\mp_warthog\n"),
			"the player in a vehicle");
		CHECK(!strstr(text, "- Network:"), "a network game where there is none");
		CHECK(strstr(text, "```\ngame_difficulty_set hard\nmap_name levels\\a10\\a10\nswitch_bsp 1\n"
			"debug_camera_set \"1.000 2.000 3.500 45.0 -5.0 0.0 89.0\"\n```\n"), "the console's lines");
		/* (dead, a multiplayer game hosted: the slowest client's ping, the builds) */
		where.alive = 0;
		where.campaign = 0;
		strcpy(where.game, "levels\\test\\bloodgulch\\bloodgulch (Slayer)");
		where.camera_field_of_view = 70.0f;
		where.network = 1;
		where.rate = 60;
		where.ping = 48;
		where.players = 4;
		where.build_kinds = 3;
		where.builds[0] = 812;
		where.build_counts[0] = 2;
		where.builds[1] = 0;
		where.build_counts[1] = 1;
		where.builds[2] = REPORT_WHERE_OPENCE;
		where.build_counts[2] = 1;
		where.builds_unknown = 1;
		report_where_text(&where, text, sizeof(text));
		printf("%s", text);
		CHECK(strstr(text, "- Game: levels\\test\\bloodgulch\\bloodgulch (Slayer)\n"), "a multiplayer game");
		CHECK(strstr(text, "- Player: dead\n") && !strstr(text, "- Player: at"), "a dead player's place");
		CHECK(strstr(text, "- Network: hosting, OpenH1 60 Tick, 4 players, ping 48 ms (the slowest client's)\n"),
			"the network game");
		CHECK(strstr(text, "- Other machines' builds: build-812 x2, a local build, OpenCE, 1 not told\n"), "the builds");
		CHECK(!strstr(text, "game_difficulty_set"), "a difficulty for a multiplayer game");
		CHECK(strstr(text, "debug_camera_set \"1.000 2.000 3.500 45.0 -5.0 0.0 70.0\"\n"), "the field of view kept");
		/* (joined a Legacy game, its host silent, the ping not known) */
		where.network = 2;
		where.rate = 0;
		where.ping = -1;
		where.players = 1;
		where.connection_problem = 1;
		where.build_kinds = where.builds_unknown = 0;
		report_where_text(&where, text, sizeof(text));
		CHECK(strstr(text, "- Network: joined, Legacy, 1 player, the host silent\n"), "a client's network game");
		CHECK(!strstr(text, "builds:"), "builds where none were told");
		where.ping = 30;
		report_where_text(&where, text, sizeof(text));
		CHECK(strstr(text, "- Network: joined, Legacy, 1 player, ping 30 ms, the host silent\n"), "a client's ping");
		/* (a small room: cut, ended) */
		report_where_text(&where, text, 40);
		CHECK(strlen(text) == 39, "cut to its room");
		return 0;
	}
	if (!strcmp(test, "zip") && getenv("REPORT_ZIP"))
	{
		struct report_zip zip;
		static unsigned char picture[100000];
		unsigned long index;

		for (index = 0; index < sizeof(picture); index++)
			picture[index] = (unsigned char)(index * 7 + index / 13);
		memset(&zip, 0, sizeof(zip));
		zip.time = (unsigned short)(21 << 11 | 10 << 5 | 30 / 2);
		zip.date = (unsigned short)((2026 - 1980) << 9 | 11 << 5 | 6);
		zip.file = fopen(getenv("REPORT_ZIP"), "wb");
		CHECK(zip.file != NULL, "the zip opened");
		report_zip_add(&zip, "report.md", "OpenH1 report\n", 14);
		report_zip_add(&zip, "empty.txt", "", 0);
		report_zip_add(&zip, "screenshot.png", picture, sizeof(picture));
		CHECK(report_zip_end(&zip), "the zip written");
		CHECK(fclose(zip.file) == 0, "the zip closed");
		return 0;
	}
	fprintf(stderr, "no case %s\n", test);
	return 2;
}
