"""Save Report (port/linux/src/report.c): report.md's frame times (the average, the 1% and 0.1% lows, the frames of
50 ms or more, the longest frames, stalls only) and the zip the report is saved as, opened with Python's zipfile;
its key (controls.report, F9), its Settings row and its documentation."""

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
    # the body's characters left as they are: an & or a # ends it early
    "issue-unencoded": ("issue", "character == '~';", "character == '~' || character == '&' || character == '#';"),
}

FUNCTIONS = ("report_crc32", "report_compare_micros", "report_low_fps", "report_frame_summary", "report_put16",
             "report_put32", "report_zip_add", "report_zip_end", "report_percent_encode", "report_issue_url")


def generated(fault=None):
    source = read("port/linux/src/report.c")
    issues = re.search(r"^#define REPORT_ISSUES .*$", source, re.M).group(0)
    code = issues + "\n\n" + structure(source, "report_zip") + "\n\n" + \
        "\n\n".join(function(source, name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *NEGATIVE_CONTROLS[fault][1:])
    return (("under_test.inc", code + "\n"),)


@pytest.mark.parametrize("case", ["crc", "frames", "steady", "issue"])
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
    """F9 by default, an action of the keyboard's that Key Bindings lists; SAVE REPORT on Settings > Profiles, whose
    handler the menus know; the settings table and the controls' table"""
    config = read("port/linux/src/port_config.c")
    assert re.search(r'\{ "controls.report", _config_string, "\\"F9\\"", "HALO_KEY_REPORT"', config)
    assert "HALO_KEYBOARD_REPORT," in read("port/linux/include/halo_keyboard.h")
    assert '"controls.report"' in read("port/linux/src/xinput_sdl.c")
    assert '{ "controls.report", L"SAVE REPORT", 2, FALSE }' in read("port/linux/game/menu_functions.c")
    page = read("port/assets/menus/ce/main_menu.settings_select.player_setup.player_profile_edit."
                "profile_settings.xml")
    assert 'run="port settings report"' in page and "REPORT A PROBLEM" in page
    assert '"port settings report"' in read("port/linux/game/menu_tags.c")
    readme = read("port/linux/README.md")
    assert "| save a report for the developers (refer to \"Reports\") | F9 |" in readme
    assert "`screenshot`, `report`, `push_to_talk`" in readme
    assert "### Reports" in readme
