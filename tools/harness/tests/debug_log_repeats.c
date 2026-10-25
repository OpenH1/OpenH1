/*
DEBUG_LOG_REPEATS.C (test)

debug.txt's lines written again as they were, a line or a few in turn
(source/cseries/errors.c, write_to_debug_file): counted once they come
twice in a row, and written down as "last N lines repeated M times" when a
different line comes, or the log ends. test_debug_log_repeats.py takes the
constants (config.inc) and the code (under_test.inc); the log is a file of
the test's own.
*/

#include "harness.h"

#include <unistd.h>

#include "config.inc"

#define csstrcmp strcmp
#define csstrlen strlen
#define csstrcpy strcpy

/* (the game's d:\debug.txt and its roll, as the test's files) */
static char log_path[512], old_path[512];
static FILE *test_fopen(const char *path, const char *mode)
{
	return fopen(strstr(path, "old") ? old_path : log_path, mode);
}
#define fopen(path, mode) test_fopen(path, mode)
#define remove(path) remove(old_path)
#define rename(from, to) rename(log_path, old_path)

#include "under_test.inc"

#undef fopen
#undef remove
#undef rename

static void line(const char *text)
{
	char string[1100];

	snprintf(string, sizeof(string), "%s\r\n", text);
	write_to_debug_file(string, FALSE);
}

/* the log's lines (without their "\r\n" and the date of a count) joined
by '|' */
static char const *logged(void)
{
	static char text[1 << 16];
	char row[1200];
	FILE *file;
	size_t length = 0;

	text[0] = 0;
	file = fopen(log_path, "rb");
	if (!file)
		return text;
	while (length < sizeof(text) - 1 && fgets(row, sizeof(row), file))
	{
		char *end = row + strcspn(row, "\r\n");
		char *start = strstr(row, "last ");

		*end = 0;
		if (!start || strncmp(row, "last ", 5) == 0)
			start = row;
		length += (size_t)snprintf(text + length, sizeof(text) - length, "%s%s", length ? "|" : "", start);
		if (length > sizeof(text) - 1)
			length = sizeof(text) - 1;
	}
	fclose(file);
	return text;
}

#define EXPECT(want) CHECK(!strcmp(logged(), want), "logged \"%s\", not \"%s\"", logged(), want)

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "";
	int index;

	snprintf(log_path, sizeof(log_path), "%s/debug-%s-%d.txt", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", name,
		(int)getpid());
	snprintf(old_path, sizeof(old_path), "%s.old", log_path);
	remove(log_path);

	if (!strcmp(name, "one-line"))
	{
		/* the same warning each tick */
		for (index = 0; index < 5; index++)
			line("A");
		line("B");
		EXPECT("A|A|last line repeated 3 times|B");
	}
	else if (!strcmp(name, "two-lines"))
	{
		/* a failure and its caller's, each frame (lastprof.txt's) */
		for (index = 0; index < 1000; index++)
		{
			line("file_open('z:\\lastprof.txt') error 0x00000002");
			line("failed to open 'z:\\lastprof.txt'");
		}
		line("C");
		EXPECT("file_open('z:\\lastprof.txt') error 0x00000002|failed to open 'z:\\lastprof.txt'|"
			"file_open('z:\\lastprof.txt') error 0x00000002|failed to open 'z:\\lastprof.txt'|"
			"last 2 lines repeated 998 times|C");
	}
	else if (!strcmp(name, "four-lines"))
	{
		for (index = 0; index < 10; index++)
		{
			line("1");
			line("2");
			line("3");
			line("4");
		}
		line("5");
		EXPECT("1|2|3|4|1|2|3|4|last 4 lines repeated 8 times|5");
	}
	else if (!strcmp(name, "unfinished-pass"))
	{
		/* a pass not finished: its lines written as they came, nothing lost */
		line("A");
		line("B");
		line("A");
		line("B");
		line("A");
		line("B");
		line("A");
		line("C");
		EXPECT("A|B|A|B|last 2 lines repeated 1 times|A|C");
	}
	else if (!strcmp(name, "distinct"))
	{
		/* lines that do not repeat, and a line seen again but not in a run */
		line("A");
		line("B");
		line("C");
		line("A");
		line("D");
		line("A");
		line("A");
		line("E");
		EXPECT("A|B|C|A|D|A|A|E");
	}
	else if (!strcmp(name, "long-lines"))
	{
		/* lines alike to their last characters are not the same line */
		char one[1001], two[1001];

		memset(one, 'x', 1000);
		one[1000] = 0;
		strcpy(two, one);
		two[990] = 'y';
		for (index = 0; index < 3; index++)
		{
			line(one);
			line(two);
		}
		line("E");
		CHECK(strstr(logged(), "last 2 lines repeated 1 times|E") != NULL, "%s", logged());
		CHECK(strstr(logged(), "last line") == NULL, "two lines that differ late are one");
	}
	else if (!strcmp(name, "end"))
	{
		/* the log's end writes a count down */
		for (index = 0; index < 10; index++)
			line("A");
		write_to_debug_file(NULL, FALSE);
		EXPECT("A|A|last line repeated 8 times");
	}
	else
	{
		fprintf(stderr, "no case %s\n", name);
		return 2;
	}
	remove(log_path);
	return 0;
}
