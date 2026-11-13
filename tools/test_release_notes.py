"""What's new (port/linux/src/whats_new.c, tools/release_notes.py): a release's highlights picked from its merges
(pull requests' titles, newest first, dev's merges and chores left out, cut to fit), written as the notes a build
embeds, and read back by the game; and when the game shows the card: a build not seen yet, but not a fresh install's
first run nor the build seen already."""
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_notes  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def merge(number, branch, title):
    return f"Merge pull request #{number} from OpenH1/{branch}\n\n{title}\n"


def test_highlights():
    messages = [merge(9, "dev", "Dev"), merge(8, "chat", "Open the text chat on the last channel"),
                merge(7, "tests", "Give the teardown test the host's difficulty"),
                merge(6, "docs", "Say in menu_engine.md how the parity tool opens screens"),
                "Fix a typo\n", merge(5, "chat-again", "Open the text chat on the last channel"),
                merge(4, "long", "Keep a title, a tab or a prompt moved past 16:9 out of a phone's safe area always")]
    lines = release_notes.highlights(messages)
    assert lines[0] == "Open the text chat on the last channel"
    assert len(lines) == 2 and lines[1].endswith("...") and len(lines[1]) <= release_notes.LINE_LENGTH
    assert len(release_notes.highlights([merge(n, f"b{n}", f"Change number {n}") for n in range(9)])) == 5


def test_notes_text(tmp_path):
    page = "https://github.com/OpenH1/goath1/releases/tag/build-2290"
    assert release_notes.whats_new(2290, page, ["One", "Two"]) == f"build 2290\npage {page}\n- One\n- Two\n"
    assert release_notes.whats_new(2290, page, []) == ""
    notes = tmp_path / "whats_new.txt"
    notes.write_text("old")
    assert not release_notes.write(2290, page, [], notes) and not notes.exists()


GAME = r"""
#include "whats_new.h"
#include <stdio.h>
#include <string.h>
/* (the game's, which the parsing and the decision do not use) */
void platform_log(const char *format, ...) { (void)format; }
int platform_open_url(const char *url) { (void)url; return 1; }
int pc_menu_whats_new_ready(void) { return 0; }
int pc_menu_whats_new_open(void) { return 0; }
int halo_menus_file_at(long index, char const **path, unsigned char const **data, unsigned long *size)
{ (void)index; (void)path; (void)data; (void)size; return 0; }
void config_folder(char *path, size_t size) { snprintf(path, size, "./"); }
char *config_file_read(const char *path, size_t *size) { (void)path; (void)size; return 0; }
const char *config_string(const char *name) { (void)name; return ""; }
int config_written_fresh(void) { return 0; }
int report_run_unattended(void) { return 0; }

int main(void)
{
	struct whats_new_notes notes;
	char const *text = "build 2290\r\npage https://example.com/build-2290\n- One\n- Two\n\n- \nnoise\n- Three";
	int failed = 0;

	failed |= !whats_new_parse(text, (unsigned long)strlen(text), &notes);
	failed |= notes.build != 2290 || notes.line_count != 3 || strcmp(notes.lines[2], "Three") ||
		strcmp(notes.page, "https://example.com/build-2290");
	/* (no build, or no highlight: not notes) */
	failed |= whats_new_parse("- One\n", 6, &notes) || whats_new_parse("build 7\n", 8, &notes);
	failed |= whats_new_parse("build x\n- One\n", 14, &notes);
	/* (an older build seen: the card; the same: none; a fresh install: noted, no card; an install from before
	the card, none noted: the card) */
	failed |= whats_new_decide(2290, 2200, 0) != WHATS_NEW_SHOW || whats_new_decide(2290, 2290, 0) != WHATS_NEW_NOTHING;
	failed |= whats_new_decide(2290, 0, 1) != WHATS_NEW_NOTE || whats_new_decide(2290, 0, 0) != WHATS_NEW_SHOW;
	failed |= whats_new_decide(0, 2200, 0) != WHATS_NEW_NOTHING;
	printf("%s\n", failed ? "failed" : "ok");
	return failed;
}
"""


@pytest.mark.skipif(not any(Path(p, "cc").exists() for p in ("/usr/bin", "/bin")), reason="no C compiler")
def test_game_reads_notes(tmp_path):
    (tmp_path / "halo_menus.h").write_text("int halo_menus_file_at(long, char const **, unsigned char const **, "
                                           "unsigned long *);\n")
    (tmp_path / "main.c").write_text(GAME)
    executable = tmp_path / "whats_new"
    src = ROOT / "port/linux/src"
    result = subprocess.run(["cc", "-std=gnu99", "-Wall", "-Werror", "-DHALO_GUEST", "-I", str(tmp_path), "-I", str(src),
                             str(tmp_path / "main.c"), str(src / "whats_new.c"), "-o", str(executable)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(executable)], capture_output=True, text=True)
    assert result.returncode == 0 and result.stdout.strip() == "ok", result.stdout
