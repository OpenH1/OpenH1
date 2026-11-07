"""Save Report (port/linux/src/report.c): report.md's frame times (the average, the 1% and 0.1% lows, the frames of
50 ms or more, the longest frames, stalls only) and the zip the report is saved as, opened with Python's zipfile;
its key (controls.report, F8), its Settings row and its documentation."""

import re
import sys
import zipfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure  # noqa: E402

NEGATIVE_CONTROLS = {
    # the CRC's polynomial wrong: no zip reader takes the files
    "crc-polynomial": ("crc", "0xEDB88320u ^ (value >> 1)", "0x04C11DB7u ^ (value >> 1)"),
    # the lows from the fastest frames, not the slowest
    "low-fastest": ("frames", "for (index = count - slowest; index < count; index++)",
                    "for (index = 0; index < slowest; index++)"),
    # frames of 50 ms counted as no hitch
    "hitch-strict": ("steady", "if (micros[index] >= REPORT_HITCH_MS * 1000)",
                     "if (micros[index] > REPORT_HITCH_MS * 1000)"),
    # every frame listed as one of the longest, a stall or not
    "longest-all": ("steady", "if (micros[best] < 2 * (uint64_t)sorted[count / 2])", "if (0)"),
    # the home folder kept: the account's name in a public issue
    "scrub-home": ("scrub", "if (home_length > 1 && end - index >= home_length", "if (0 && home_length > 1"),
    # this machine's own loopback taken for an address
    "scrub-loopback": ("scrub", "return loopback || zero ? 0 : at;", "return (loopback && 0) || zero ? 0 : at;"),
    # other players' chat lines kept
    "scrub-chat": ("scrub", "if (chat && end - at >= 6)", "if (0)"),
    # a password written into settings.txt
    "private-password": ("scrub", 'if (strstr(name, "password"))', 'if (0)'),
    # the body's characters left as they are: an & or a # ends it early
    "issue-unencoded": ("issue", "character == '~';", "character == '~' || character == '&' || character == '#';"),
    # the crash's first line (the signal and the address) not the one shown
    "crash-first-line": ("crash", "first_end = memchr(record, '\\n', (size_t)(crash_end - record));",
                         "first_end = memchr(record + 1, '\\n', (size_t)(crash_end - record - 1)) ? NULL : NULL;"),
    # a backtick of a log line kept: a line of three ends report.md's block early
    "crash-backticks": ("crash", "line[index] == '`' ? '\\'' : line[index];", "line[index];"),
    # the session's value taken from a line that only begins with its name
    "crash-session-name": ("crash", "&& line[length] == ':'", ""),
    # the log's ring written in the order it is kept, not oldest first
    "ring-order": ("ring", "if (first + kept > crash_record_log_size)", "if (0)"),
}

FUNCTIONS = ("report_crc32", "report_compare_micros", "report_low_fps", "report_frame_summary", "report_put16",
             "report_put32", "report_zip_add", "report_zip_end", "report_percent_encode", "report_issue_url", "report_setting_private",
             "report_address_length", "report_scrub", "report_session_value", "report_crash_line",
             "report_crash_summary")
# the host's record of a crash, written from its signal handler (port/android/host/host_common.c)
HOST_FUNCTIONS = ("crash_record_write", "host_crash_record_line", "host_crash_record_end")


def generated(fault=None):
    source = read("port/linux/src/report.c")
    issues = re.search(r"^#define REPORT_ISSUES .*$", source, re.M).group(0)
    defines = "\n".join(re.search(r"^#define " + name + r" .*$", source, re.M).group(0) for name in
                        ("REPORT_CRASH_LINES", "REPORT_CRASH_LOG_LINES", "REPORT_CRASH_LINE_MOST",
                         "REPORT_CRASH_LOG_HEADING"))
    host = read("port/android/host/host_common.c")
    host_state = "\n".join(re.search(r"^static .*\bcrash_record_" + name + r"\b.*;$", host, re.M).group(0) for name in
                           ("log", "log_size", "log_written", "file"))
    code = issues + "\n" + defines + "\n\n" + structure(source, "report_zip") + "\n\n" + \
        "\n\n".join(function(source, name) for name in FUNCTIONS) + "\n\n" + host_state + "\n\n" + \
        "\n\n".join(function(host, name) for name in HOST_FUNCTIONS)
    if fault:
        code = mutated(code, *NEGATIVE_CONTROLS[fault][1:])
    return (("under_test.inc", code + "\n"),)


@pytest.mark.parametrize("case", ["crc", "frames", "steady", "issue", "scrub", "crash", "ring"])
def test_case(case):
    status, output = run(build("report", generated()), case)
    assert status == 0, output


def test_zip(tmp_path, monkeypatch):
    path = tmp_path / "report.zip"
    monkeypatch.setenv("REPORT_ZIP", str(path))
    status, output = run(build("report", generated()), "zip")
    assert status == 0, output
    with zipfile.ZipFile(path) as archive:
        assert archive.testzip() is None
        assert [entry.filename for entry in archive.infolist()] == ["report.md", "empty.txt", "screenshot.png"]
        assert archive.read("report.md") == b"OpenH1 report\n"
        assert archive.read("empty.txt") == b""
        assert archive.read("screenshot.png") == bytes((index * 7 + index // 13) & 0xFF for index in range(100000))
        assert archive.getinfo("report.md").date_time == (2026, 11, 6, 21, 10, 30)


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    status, output = run(build("report", generated(control)), NEGATIVE_CONTROLS[control][0])
    assert status == CHECK_FAILED, f"{control} was not caught: {output}"


def test_key_row_and_documentation():
    """F8 by default (F9 is the game's own debug key, Profile Reset: debug_keys.c), an action of the keyboard's that Key Bindings lists; SAVE REPORT on Settings > Profiles, whose
    handler the menus know; the settings table and the controls' table"""
    config = read("port/linux/src/port_config.c")
    assert re.search(r'\{ "controls.report", _config_string, "\\"F8\\"", "HALO_KEY_REPORT"', config)
    assert "HALO_KEYBOARD_REPORT," in read("port/linux/include/halo_keyboard.h")
    assert '"controls.report"' in read("port/linux/src/xinput_sdl.c")
    assert '{ "controls.report", L"SAVE REPORT", 2, FALSE }' in read("port/linux/game/menu_functions.c")
    page = read("port/assets/menus/ce/main_menu.settings_select.player_setup.player_profile_edit."
                "profile_settings.xml")
    assert 'run="port settings report"' in page and "REPORT A PROBLEM" in page
    assert '"port settings report"' in read("port/linux/game/menu_tags.c")
    readme = read("port/linux/README.md")
    assert "| report a problem to the developers (refer to \"Reports\") | F8 |" in readme
    assert "`screenshot`, `report`, `push_to_talk`" in readme
    assert "### Reports" in readme and '"What happened?"' in readme
    # the note's line: the chat's, in any game, its Enter and Escape giving it to the report
    chat = read("port/linux/game/network_chat.c")
    assert 'L"What happened?"' in chat and "report_note_done(chat_input.text);" in chat
    assert "report_note_done(NULL);" in chat
