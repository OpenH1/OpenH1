"""The server browsers' favorites (port/linux/src/browser_favorites.c): their
keys, the file read as untrusted input, written and read back, starred and
unstarred. The file is compiled as it is, with config.toml's folder a test's
own."""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "port/linux/src/browser_favorites.c"

DRIVER = r"""
#include "browser_favorites.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* port_config.c's, with the folder the test gives */
void config_folder(char *path, size_t size)
{
	snprintf(path, size, "%s/", getenv("FAVORITES_FOLDER"));
}

char *config_file_read(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	char *text;
	long length;

	if (!file)
		return NULL;
	fseek(file, 0, SEEK_END);
	length = ftell(file);
	fseek(file, 0, SEEK_SET);
	text = malloc((size_t)length + 1);
	*size = fread(text, 1, (size_t)length, file);
	text[*size] = 0;
	fclose(file);
	return text;
}

static void print(const struct browser_favorite *favorite)
{
	printf("%s|%lld|%d|%d|%s|%s|%s\n", favorite->key, favorite->last_seen, favorite->engine_type,
		favorite->kind, favorite->name, favorite->map, favorite->gametype);
}

int main(int argc, char **argv)
{
	const char *command = argv[1];

	if (!strcmp(command, "keys"))
	{
		static const unsigned char identifier[6] = { 0x02, 0xab, 0xcd, 0x01, 0x23, 0xff };
		unsigned char address[4] = { 192, 168, 1, 20 };
		unsigned long value = 0;
		char key[BROWSER_FAVORITE_KEY_SIZE];

		browser_favorite_key_internet(identifier, key);
		printf("%s %d\n", key, browser_favorite_key_valid(key));
		memcpy(&value, address, 4);
		browser_favorite_key_address(value, key);
		printf("%s %d\n", key, browser_favorite_key_valid(key));
		return 0;
	}
	if (!strcmp(command, "valid"))
	{
		printf("%d\n", browser_favorite_key_valid(argv[2]));
		return 0;
	}
	if (!strcmp(command, "parse"))
	{
		struct browser_favorite favorites[BROWSER_FAVORITES_MAXIMUM];
		size_t size = 0;
		char path[1024];
		char *text;
		int count, index;

		snprintf(path, sizeof(path), "%s/favorites.txt", getenv("FAVORITES_FOLDER"));
		text = config_file_read(path, &size);
		count = browser_favorites_parse(text ? text : "", size, favorites, BROWSER_FAVORITES_MAXIMUM);
		for (index = 0; index < count; index++)
			print(&favorites[index]);
		return 0;
	}
	if (!strcmp(command, "list"))
	{
		int index;

		for (index = 0; index < browser_favorites_count(); index++)
			print(browser_favorites_get(index));
		return 0;
	}
	if (!strcmp(command, "toggle"))
	{
		/* toggle <key> <name> <map> <gametype> <engine> <kind> */
		struct browser_favorite game;

		memset(&game, 0, sizeof(game));
		snprintf(game.key, sizeof(game.key), "%s", argv[2]);
		snprintf(game.name, sizeof(game.name), "%s", argv[3]);
		snprintf(game.map, sizeof(game.map), "%s", argv[4]);
		snprintf(game.gametype, sizeof(game.gametype), "%s", argv[5]);
		game.engine_type = atoi(argv[6]);
		game.kind = atoi(argv[7]);
		printf("%d\n", browser_favorites_toggle(&game));
		return 0;
	}
	if (!strcmp(command, "rekey"))
	{
		browser_favorites_rekey(browser_favorites_find(argv[2]), argv[3]);
		printf("%d %d\n", browser_favorites_find(argv[2]), browser_favorites_find(argv[3]));
		return 0;
	}
	if (!strcmp(command, "when"))
	{
		static const long long ages[] = { 0, 30, 61, 3599, 3600, 86399, 86400, 3 * 86400 };
		char text[32];
		unsigned index;

		browser_favorite_last_seen_text(0, 1000000, text, sizeof(text));
		printf("%s\n", text);
		for (index = 0; index < sizeof(ages) / sizeof(ages[0]); index++)
		{
			browser_favorite_last_seen_text(1000000 - ages[index], 1000000, text, sizeof(text));
			printf("%s\n", text);
		}
		return 0;
	}
	return 1;
}
"""


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    compiler = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
    if not compiler:
        pytest.skip("no C compiler")
    folder = tmp_path_factory.mktemp("favorites_driver")
    (folder / "driver.c").write_text(DRIVER)
    executable = folder / "driver"
    # (HALO_GUEST: the C library's files, not SDL's)
    subprocess.run([compiler, "-std=gnu99", "-Wall", "-Werror", "-DHALO_GUEST", f"-I{SOURCE.parent}",
                    str(SOURCE), str(folder / "driver.c"), "-o", str(executable)], check=True)
    return executable


def run(driver, folder, *arguments):
    environment = dict(os.environ, FAVORITES_FOLDER=str(folder))
    return subprocess.run([str(driver), *arguments], env=environment, check=True, capture_output=True,
                          text=True).stdout.splitlines()


def test_keys(driver, tmp_path):
    assert run(driver, tmp_path, "keys") == ["net:02abcd0123ff 1", "lan:192.168.1.20 1"]
    for key in ["net:02abcd0123f", "net:02abcd0123fff", "net:02ABCD0123FF", "lan:1.2.3", "lan:1.2.3.256",
                "lan:1.2.3.4:2302", "lan:1..3.4", "wan:1.2.3.4", "", "net:"]:
        assert run(driver, tmp_path, "valid", key) == ["0"], key


def test_parse_untrusted(driver, tmp_path):
    lines = [
        "# a comment\tnet:02abcd0123ff\t1\t1\t1\tx\tx\tx",
        "net:02abcd0123ff\t1729700000\t2\t1\tBob's Slayer\tbloodgulch\tSlayer",
        # (the same key again: the first kept)
        "net:02abcd0123ff\t5\t2\t1\tAgain\tmap\ttype",
        # (keys and numbers not valid)
        "net:zzabcd0123ff\t1\t1\t1\ta\tb\tc",
        "lan:10.0.0.1\t-5\t1\t1\ta\tb\tc",
        "lan:10.0.0.2\t5\t256\t1\ta\tb\tc",
        "lan:10.0.0.3\t99999999999999999999\t1\t1\ta\tb\tc",
        # (too few fields)
        "lan:10.0.0.4\t5\t1\t1\ta\tb",
        # (control characters and long text: cleaned, cut)
        "lan:10.0.0.5\t7\t0\t3\tna\x01me\x7f\t" + "m" * 100 + "\tCTF\r",
        "\x00\xff garbage",
        "lan:10.0.0.6\t8\t1\t0\tlast\tmap\ttype\textra",
    ]
    (tmp_path / "favorites.txt").write_bytes("\n".join(lines).encode("latin-1"))
    assert run(driver, tmp_path, "parse") == [
        "net:02abcd0123ff|1729700000|2|1|Bob's Slayer|bloodgulch|Slayer",
        "lan:10.0.0.5|7|0|3|name|" + "m" * 32 + "|CTF",
        # (a field past the seventh left out)
        "lan:10.0.0.6|8|1|0|last|map|type",
    ]


def test_parse_after_nul(driver, tmp_path):
    # (a NUL ends a line, and the file reads on after it)
    (tmp_path / "favorites.txt").write_bytes(b"lan:1.2.3.4\t1\t1\t0\ta\tb\tc\x00lan:1.2.3.5\t1\t1\t0\td\te\tf\n")
    assert run(driver, tmp_path, "parse") == ["lan:1.2.3.4|1|1|0|a|b|c", "lan:1.2.3.5|1|1|0|d|e|f"]


def test_toggle_written_and_read_back(driver, tmp_path):
    assert run(driver, tmp_path, "list") == []
    assert run(driver, tmp_path, "toggle", "net:02abcd0123ff", "Tab\there", "bloodgulch", "Slayer", "2", "1") == ["1"]
    assert run(driver, tmp_path, "toggle", "lan:127.0.0.2", "LAN game", "wizard", "", "1", "0") == ["1"]
    assert run(driver, tmp_path, "toggle", "bad", "x", "y", "z", "0", "0") == ["-1"]
    listed = run(driver, tmp_path, "list")
    assert [line.split("|")[0] for line in listed] == ["net:02abcd0123ff", "lan:127.0.0.2"]
    assert listed[0].split("|")[4] == "Tabhere"
    assert int(listed[0].split("|")[1]) > 1700000000
    assert not (tmp_path / "favorites.txt.new").exists()
    # (a new host identifier for the first: found again by name, rekeyed)
    assert run(driver, tmp_path, "rekey", "net:02abcd0123ff", "net:060000000001") == ["-1 0"]
    # (unstarred)
    assert run(driver, tmp_path, "toggle", "net:060000000001", "", "", "", "0", "0") == ["0"]
    assert [line.split("|")[0] for line in run(driver, tmp_path, "list")] == ["lan:127.0.0.2"]


def test_last_seen_text(driver, tmp_path):
    assert run(driver, tmp_path, "when") == ["never", "just now", "just now", "1 min ago", "59 min ago", "1 h ago",
                                             "23 h ago", "yesterday", "3 days ago"]
