/*
REPORT.C (test)

Save Report (port/linux/src/report.c): report.md's frame times (the average,
the 1% and 0.1% lows, the frames of 50 ms or more, the longest frames, a
stall only), the zip (its CRC-32, its local and central records), which
test_report.py opens with Python's zipfile, and the last run's crash: its
heading of report.md (report_crash_summary) and the host's record of it,
the log's ring oldest first (port/android/host/host_common.c).
test_report.py takes the code.
*/

#include "harness.h"

#include <stdint.h>
#include <unistd.h>

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
	if (!strcmp(test, "crash"))
	{
		static char const session[] =
			"build number: 99\nbuild: build-1234 (abcdef0)\nplatform: macOS\nstarted: 2026-11-06 21:10:30\n"
			"reference address: 4a1b2c\n";
		static char record[8192];
		static char summary[16384];
		int index;

		strcpy(record, "signal 11 at address 0x10: pc 0000000000401000 lr 0 sp 0 fp 0\n"
			"  in the guest image: addr2line -e halo_guest.elf 0x401000 0x0\n"
			"  x0 0000000000000000\n");
		for (index = 0; index < 30; index++)
			sprintf(record + strlen(record), "  frame %2d: return %016x\n", index, index);
		strcat(record, REPORT_CRASH_LOG_HEADING);
		for (index = 0; index < 20; index++)
			sprintf(record + strlen(record), "log line %d\n", index);
		strcat(record, "a quote: ```\n```\nlast `line`\n");
		report_crash_summary(session, record, summary, sizeof(summary));
		printf("%s", summary);
		CHECK(strstr(summary, "- Build: build-1234 (abcdef0)\n"), "the build");
		CHECK(strstr(summary, "- Started: 2026-11-06 21:10:30\n"), "when it started");
		CHECK(strstr(summary, "- Crash: signal 11 at address 0x10: pc 0000000000401000 lr 0 sp 0 fp 0\n"),
			"the crash's first line");
		CHECK(strstr(summary, "- Reference address: 4a1b2c "), "the reference address");
		CHECK(strstr(summary, "```\n  in the guest image: addr2line"), "the crash's lines in a block");
		CHECK(strstr(summary, "  frame 21: return") && !strstr(summary, "  frame 22: return"),
			"the crash's lines, at most REPORT_CRASH_LINES");
		CHECK(strstr(summary, "(and more: crash.txt)\n"), "the lines left out said");
		CHECK(!strstr(summary, "log line 12\n") && strstr(summary, "log line 13\nlog line 14\n"),
			"the log's last lines only");
		CHECK(strstr(summary, "a quote: '''\n'''\nlast 'line'\n```\n"), "a quoted line's backticks");
		/* (no record: the session's alone) */
		report_crash_summary(session, NULL, summary, sizeof(summary));
		CHECK(strstr(summary, "- Crash: none written"), "no crash written");
		CHECK(!strstr(summary, "```"), "no blocks without a record");
		/* (a room too small: cut, and ended) */
		report_crash_summary(session, record, summary, 100);
		CHECK(strlen(summary) == 99, "cut to its room");
		return 0;
	}
	if (!strcmp(test, "ring"))
	{
		static char ring[16], out[64];
		static uint64_t written;
		char const *expected = "\n--- the log's last lines\nGHIJKLMNOPQRSTUV";
		int pipes[2];
		ssize_t size;
		unsigned long index;

		/* (22 bytes written to a ring of 16: G to V kept, Q to V at its
		start) */
		for (index = 0; index < 22; index++)
			ring[index % sizeof(ring)] = (char)('A' + index);
		written = 22;
		CHECK(pipe(pipes) == 0, "a pipe");
		crash_record_file = pipes[1];
		crash_record_log = ring;
		crash_record_log_size = sizeof(ring);
		crash_record_log_written = &written;
		host_crash_record_line("signal 11");
		host_crash_record_end();
		CHECK(crash_record_file == -1, "the record closed");
		size = read(pipes[0], out, sizeof(out) - 1);
		CHECK(size > 0, "the record read");
		out[size] = 0;
		printf("%s\n", out);
		CHECK(!strncmp(out, "signal 11\n", 10), "the line");
		CHECK(!strcmp(out + 10, expected), "the log, oldest first");
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
	if (!strcmp(test, "maps"))
	{
		CHECK(report_map_counted("bloodgulch.map"), "a map");
		CHECK(report_map_counted("Hugeass_1A2B3C4D.MAP"), "its extension in capitals");
		CHECK(!report_map_counted("bitmaps.map") && !report_map_counted("Sounds.map") &&
			!report_map_counted("loc.map"), "the resource maps");
		CHECK(report_map_counted("bitmaps2.map") && report_map_counted("loc.map2.map"), "names that only begin as one does");
		CHECK(!report_map_counted("hugeass.map.partial") && !report_map_counted("readme.txt") &&
			!report_map_counted(".map") && !report_map_counted("._a10.map"), "what is not a map");
		return 0;
	}
	fprintf(stderr, "no case %s\n", test);
	return 2;
}
