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
    "scrub-chat": ("scrub", "chat = text + index;", "chat = NULL;"),
    # a password written into settings.txt
    "private-password": ("scrub", 'if (strstr(name, "password") ||', 'if (0 ||'),
    # the body's characters left as they are: an & or a # ends it early
    "issue-unencoded": ("issue", "character == '~';", "character == '~' || character == '&' || character == '#';"),
    # a code block the cut leaves open left open: the rest of the issue in it
    "fence-open": ("issue", "fences % 2 == 0 ||", "fences >= 0 ||"),
    # a short chat line ("chat: a: hi") kept
    "scrub-chat-short": ("scrub", "index + 6 <= end && !chat", "index + 12 <= end && !chat"),
    # what the markers name (a Discord name, a host's notice) kept
    "scrub-marker": ("scrub", "if (marker < sizeof", "if (0 && marker < sizeof"),
    # IPv6 addresses kept
    "scrub-ipv6": ("scrub", "(length = report_address6_length(text + index, end - index)) != 0",
                   "(length = 0 * report_address6_length(text + index, end - index)) != 0"),
    # a time (12:30:45) taken for an IPv6 address
    "ipv6-times": ("scrub", "groups != 8 || colons != 7", "groups < 3 + 0 * colons"),
    # a cut log's partial first line kept
    "whole-lines-kept": ("scrub", "return at < size ? at + 1 : size;", "return 0;"),
    # the chat's own lines, which name nobody, taken for players' chat lines
    "scrub-chat-own": ("scrub", "if (colons == 2)", "if (colons >= 1)"),
    # an audio device's name (its owner's) in settings.txt
    "private-device": ("scrub", '"audio.output_device", "audio.input_device",', ""),
    # the scrubbed text not ended as a string: the issue's body runs on into the heap
    "scrub-unended": ("scrub", "out[used] = 0;\n\t*scrubbed_size", "out[used] = 'x';\n\t*scrubbed_size"),
    # a folder's path in settings.txt (the account's name)
    "private-paths": ("scrub", '!strncmp(name, "paths.", 6) ||', "0 ||"),
}

FUNCTIONS = ("report_crc32", "report_compare_micros", "report_low_fps", "report_frame_summary", "report_put16",
             "report_put32", "report_zip_add", "report_zip_end", "report_percent_encode", "report_issue_url", "report_setting_private",
             "report_address_length", "report_hex_digit", "report_word_character", "report_address6_length",
             "report_whole_lines", "report_scrub")


def markers(source):
    """report_markers, the table of what log lines go on to name"""
    end = source.index("} report_markers[] = {")
    start = source.rindex("static const struct", 0, end)
    return source[start:source.index("\n};", end) + 3]


def generated(fault=None):
    source = read("port/linux/src/report.c")
    issues = re.search(r"^#define REPORT_ISSUES .*$", source, re.M).group(0)
    code = issues + "\n\n" + structure(source, "report_zip") + "\n\n" + \
        "\n\n".join(function(source, name) for name in FUNCTIONS[:-1]) + "\n\n" + markers(source) + "\n\n" + \
        function(source, FUNCTIONS[-1])
    if fault:
        code = mutated(code, *NEGATIVE_CONTROLS[fault][1:])
    return (("under_test.inc", code + "\n"),)


@pytest.mark.parametrize("case", ["crc", "frames", "steady", "issue", "scrub"])
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


def test_note_and_issue_wait():
    """the note asked for only with the keyboard's key (a menu's row has no keyboard to answer it); the issue held
    for the main menu, not a loading screen; the note's line closed out of a game, its report mode with it"""
    assert "report_request(1);" in read("port/linux/src/xinput_sdl.c")
    assert "report_request(FALSE);" in read("port/linux/game/menu_functions.c")
    report = read("port/linux/src/report.c")
    assert "if (report_asked_by_key && report_game_playing() && report_game_note_begin())" in report
    assert "if (report_pending.count && report_game_menus())" in report
    assert "if (report_job.url && !report_game_menus())" in report
    assert "return report_game_map() == 2 && ui_widgets_active();" in read("port/linux/game/report_game.c")
    chat = read("port/linux/game/network_chat.c")
    end = function(chat, "chat_input_end")
    assert "chat_input.report = FALSE;" in end
    assert "if (!report_game_playing() || chat_hidden())" in function(chat, "chat_report_update")
