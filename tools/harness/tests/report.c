/*
REPORT.C (test)

Save Report (port/linux/src/report.c): report.md's frame times (the average,
the 1% and 0.1% lows, the frames of 50 ms or more, the longest frames, a
stall only), the GPU's time and what held the frames back (GPU-bound,
CPU-bound, vsync or the frame cap) and the zip (its CRC-32, its local and
central records), which test_report.py opens with Python's zipfile.
test_report.py takes the code.
*/

#include "harness.h"

#include <stdint.h>

#define REPORT_HITCH_MS 50
#define REPORT_LONGEST 8
#define REPORT_ZIP_FILES 8
#define REPORT_GPU_UNKNOWN 0xFFFFFFFFu
#define REPORT_PACED_PERCENT 20
#define REPORT_BUSY_PERCENT 95
#define REPORT_GPU_FEWEST 30

#include "under_test.inc"

static uint32_t micros[2048], at_ms[2048];
static struct report_frame_work work[2048];

/* 600 frames of 10 ms, each with the GPU's time, the renderer's thread's,
the game's waits and of them the pacing's */
static void gpu_frames(uint32_t gpu, uint32_t render, uint32_t waited, uint32_t paced)
{
	unsigned long index;

	for (index = 0; index < 600; index++)
	{
		micros[index] = 10000;
		work[index].gpu = gpu;
		work[index].render = render;
		work[index].waited = waited;
		work[index].paced = paced;
	}
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
	if (!strcmp(test, "gpu"))
	{
		unsigned long index;

		/* (the GPU busy nearly all the frame, the game waiting for it) */
		gpu_frames(9600, 3000, 6000, 0);
		report_gpu_summary(micros, work, 600, text, sizeof(text));
		printf("%s", text);
		CHECK(strstr(text, "GPU time: 9.60 ms a frame, 9.60 ms at the 99th percentile\n"), "the GPU's time");
		CHECK(strstr(text, "Busy: the GPU 96%, the game 40%, the renderer's thread 30% of the time\n"), "the shares");
		CHECK(strstr(text, "Waiting for vsync or the frame cap: 0% of the time\n"), "the pacing's share");
		CHECK(strstr(text, "Verdict: GPU-bound\n"), "GPU-bound");
		/* (the game busy, the GPU and the renderer not) */
		gpu_frames(3000, 2000, 600, 0);
		report_gpu_summary(micros, work, 600, text, sizeof(text));
		printf("%s", text);
		CHECK(strstr(text, "the game 94%"), "the game's share");
		CHECK(strstr(text, "Verdict: CPU-bound (the game)\n"), "CPU-bound, the game");
		/* (the renderer's thread busy, the game waiting for it) */
		gpu_frames(5000, 9000, 8000, 0);
		report_gpu_summary(micros, work, 600, text, sizeof(text));
		printf("%s", text);
		CHECK(strstr(text, "Verdict: CPU-bound (the renderer's thread)\n"), "CPU-bound, the renderer's thread");
		/* (neither: the game waits most of the frame for the display) */
		gpu_frames(3000, 2000, 8500, 7000);
		report_gpu_summary(micros, work, 600, text, sizeof(text));
		printf("%s", text);
		CHECK(strstr(text, "Waiting for vsync or the frame cap: 70% of the time\n"), "the pacing's share, paced");
		CHECK(strstr(text, "Verdict: limited by vsync or the frame cap\n"), "vsync or the cap");
		/* (at the frame cap, the GPU busy for most but not all of the time) */
		gpu_frames(8600, 4400, 8800, 4800);
		report_gpu_summary(micros, work, 600, text, sizeof(text));
		CHECK(strstr(text, "Verdict: limited by vsync or the frame cap\n"), "the cap, the GPU busy");
		/* (waits for the cap, but the GPU all but always busy: the GPU) */
		gpu_frames(9700, 3000, 6000, 3000);
		report_gpu_summary(micros, work, 600, text, sizeof(text));
		CHECK(strstr(text, "Verdict: GPU-bound\n"), "the cap, the GPU saturated");
		/* (few waits for the cap, none busy most of the time: the busiest) */
		gpu_frames(6500, 5000, 8000, 1000);
		report_gpu_summary(micros, work, 600, text, sizeof(text));
		CHECK(strstr(text, "Verdict: GPU-bound\n"), "the busiest, the GPU");
		gpu_frames(5000, 6500, 8000, 1000);
		report_gpu_summary(micros, work, 600, text, sizeof(text));
		CHECK(strstr(text, "Verdict: CPU-bound (the renderer's thread)\n"), "the busiest, the renderer's thread");
		/* (the 99th percentile: 10 of the 600 frames of 20 ms) */
		gpu_frames(3000, 2000, 8500, 7000);
		for (index = 0; index < 600; index += 60)
			work[index].gpu = 20000;
		report_gpu_summary(micros, work, 600, text, sizeof(text));
		CHECK(strstr(text, "ms a frame, 20.00 ms at the 99th percentile\n"), "the 99th percentile");
		/* (frames not measured left out: half of them) */
		gpu_frames(9600, 3000, 6000, 0);
		for (index = 0; index < 600; index += 2)
			work[index].gpu = REPORT_GPU_UNKNOWN;
		report_gpu_summary(micros, work, 600, text, sizeof(text));
		CHECK(strstr(text, "GPU time: 9.60 ms a frame, 9.60 ms at the 99th percentile\n"), "unmeasured frames left out");
		CHECK(strstr(text, "Verdict: GPU-bound\n"), "GPU-bound with unmeasured frames");
		/* (none measured: no verdict) */
		for (index = 0; index < 600; index++)
			work[index].gpu = REPORT_GPU_UNKNOWN;
		report_gpu_summary(micros, work, 600, text, sizeof(text));
		CHECK(!strcmp(text, "GPU time: not measured\n"), "no GPU time");
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
