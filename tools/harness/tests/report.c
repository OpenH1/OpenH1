/*
REPORT.C (test)

Save Report (port/linux/src/report.c): report.md's frame times (the average,
the 1% and 0.1% lows, the frames of 50 ms or more, the longest frames, a
stall only) and the zip (its CRC-32, its local and central records), which
test_report.py opens with Python's zipfile. test_report.py takes the code.
*/

#include "harness.h"

#include <stdint.h>

#define REPORT_HITCH_MS 50
#define REPORT_LONGEST 8
#define REPORT_ZIP_FILES 8

#include "under_test.inc"

static uint32_t micros[2048], at_ms[2048];

/* how often word is in text */
static int report_count(char const *text, char const *word)
{
	int count = 0;

	while ((text = strstr(text, word)) != NULL)
	{
		count++;
		text += strlen(word);
	}
	return count;
}

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
		static char const fence[] = "%60%60%60", closed[] = "text%0A%60%60%60%0A",
			whole[] = "&body=%60%60%60%0Aall%0A%60%60%60%0A";
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
		/* (a cut inside a code block: the block closed after it) */
		body[0] = 0;
		strcat(body, "## Frames\n\n```\n");
		for (index = 0; index < 20; index++)
			strcat(body, "line of text\n");
		strcat(body, "```\n");
		CHECK(report_issue_url("t", body, url, 200), "a long code block cut to fit");
		printf("%s\n", url);
		CHECK(strlen(url) < 200, "within its room, the fence too");
		CHECK(report_count(url, fence) == 2, "the code block left open by the cut");
		CHECK(strlen(url) > strlen(closed) && !strcmp(url + strlen(url) - strlen(closed), closed),
			"closed after its last line");
		CHECK(report_issue_url("t", "```\nall\n```\n", url, sizeof(url)) && strstr(url, whole) &&
			report_count(url, fence) == 2, "a whole body's block closed twice");
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
			"11.06.26 22:55:22  chat notice: Johnson joined the game\n"
			"chat: a: hi\n"
			"chat: b: x\n"
			"host notice: Keyes (Discord: keyes#1, 12345) banned by the host\n"
			"Discord link: linked as keyes_discord at https://master.example\n"
			"Discord link: opening https://master.example/link in the browser (the page shows the code K7Q2-ZX)\n"
			"Internet play: cannot look up network.public_address home.keyes.example\n"
			"Internet play: network.public_address [2001:db8::7]:2302 is IPv6, which the tunnel does not use\n"
			"v6 2001:db8:85a3:0:0:8a2e:370:7334 and fe80::1c2b:3a4d%en0, mapped ::ffff:198.51.100.9, loopback ::1\n"
			"times 12:30:45 and 12:30:45.250, std::vector, a deadbeef::cafe, build 1.2.3, aa:bb\n"
			"chats are fine\n"
			"11.06.26 22:55:23  chat: the on-screen keyboard opened\n"
			"no newline at the end";
		size_t size;
		char *out = report_scrub(text, sizeof(text) - 1, "/Users/keyes", &size);

		CHECK(out != NULL, "scrubbed");
		CHECK(strlen(out) == size, "the text ended as a string");
		printf("%s\n", out);
		CHECK(strstr(out, "data root: ~/Halo/maps\n"), "the home folder as ~");
		CHECK(!strstr(out, "keyes"), "the account's name left");
		CHECK(strstr(out, "hosting: openh1://(removed) (the rest"), "the link taken out");
		CHECK(strstr(out, "joined x.x.x.x:2302 and x.x.x.x, local 127.0.0.1, any 0.0.0.0\n"), "the addresses");
		CHECK(strstr(out, "Vulkan 1.3.280, driver 24.1.3; version 300.1.2.3.4\n"), "versions left as they are");
		CHECK(strstr(out, "22:55:20  chat: (a chat line, removed from the report)\n"), "a team chat line");
		CHECK(!strstr(out, "warthog") && !strstr(out, "Cortana"), "a chat line left");
		CHECK(strstr(out, "22:55:22  chat notice: (removed)\n") && !strstr(out, "Johnson joined"), "a notice's name");
		CHECK(!strstr(out, "hi\n") && !strstr(out, "x\n") && strstr(out, "\nchat: (a chat line, removed from the "
			"report)\nchat: (a chat line, removed from the report)\n"), "a short chat line");
		CHECK(strstr(out, "host notice: (removed)\n") && !strstr(out, "12345"), "the host's notice, a Discord id");
		CHECK(strstr(out, "Discord link: linked as (removed)\n"), "a Discord name");
		CHECK(strstr(out, "(the page shows the code (removed))\n") && !strstr(out, "K7Q2"), "a link's code");
		CHECK(strstr(out, "look up network.public_address (removed)\n"), "network.public_address's host name");
		CHECK(strstr(out, "network.public_address (removed) is IPv6"), "network.public_address's IPv6 address");
		CHECK(strstr(out, "v6 x::x and x::x%en0, mapped ::ffff:x.x.x.x, loopback ::1\n"), "the IPv6 addresses");
		CHECK(strstr(out, "times 12:30:45 and 12:30:45.250, std::vector, a deadbeef::cafe, build 1.2.3, aa:bb\n"),
			"times, versions and words taken for IPv6 addresses");
		CHECK(strstr(out, "chats are fine\n"), "a word taken for a chat line");
		CHECK(strstr(out, "chat: the on-screen keyboard opened\n"), "the chat's own line, of no player, taken out");
		CHECK(size >= 21 && !memcmp(out + size - 21, "no newline at the end", 21), "the last line");
		CHECK(report_setting_private("server.password") && report_setting_private("server.web_owner_password") &&
			report_setting_private("network.public_address") && report_setting_private("debug.update_key"),
			"a private setting not left out");
		CHECK(report_setting_private("server.admins") && report_setting_private("server.name") &&
			report_setting_private("server.motd") && report_setting_private("debug.friends") &&
			report_setting_private("debug.update_url") && report_setting_private("paths.data") &&
			report_setting_private("server.publisher_key_file") && report_setting_private("debug.friends_folder") &&
			report_setting_private("audio.output_device") && report_setting_private("network.front") &&
			report_setting_private("debug.screenshot_directory") && report_setting_private("debug.play_log"),
			"a name, a path or an address not left out");
		CHECK(!report_setting_private("display.vsync") && !report_setting_private("network.online") &&
			!report_setting_private("server.max_players") && !report_setting_private("debug.network_test"),
			"a setting left out that is not private");
		free(out);
		/* (a log's end cut mid-line: from its next line) */
		CHECK(report_whole_lines("ohnson: hi\nnext\n", 16, 1) == 11, "a cut log's partial first line");
		CHECK(report_whole_lines("whole\nnext\n", 11, 0) == 0, "a whole log's first line");
		CHECK(report_whole_lines("no line end", 11, 1) == 11, "a cut log of no line's end");
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
