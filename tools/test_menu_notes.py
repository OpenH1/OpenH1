"""tools/menu_notes.py: the menu studio's notes read, listed, briefed and marked done; menu_gallery.py's
page with them."""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import menu_gallery  # noqa: E402
import menu_notes  # noqa: E402

SCREEN = "main_menu/settings_select/player_setup/player_profile_edit/audio_settings/audio_settings_screen"
WIDGET = "pc\\main_menu\\settings_select\\player_setup\\player_profile_edit\\audio_settings\\output_device_label"


def note(identifier, status="open", widget=WIDGET, text="Say SPEAKERS"):
    return {"id": identifier, "status": status, "time": "2026-11-07T20:00:00Z", "note": text, "screen": SCREEN,
            "screen_tag": "pc\\" + SCREEN.replace("/", "\\"), "widget": widget,
            "widget_path": ["pc\\" + SCREEN.replace("/", "\\"), widget] if widget else [],
            "widget_text": "OUTPUT DEVICE" if widget else "", "box": [-68, 125, 226, 144] if widget else None,
            "input": "keyboard", "shape": "16:9", "screen_width": 852,
            "picture": "/tmp/menu_notes/2026-11-07_20-00-00.png"}


def write(path, notes):
    path.write_text("".join(json.dumps(item) + "\n" for item in notes) + "not json\n")


def test_list_by_screen_and_done(tmp_path):
    path = tmp_path / "menu_notes.jsonl"
    write(path, [note("a1"), note("b2", widget=None, text="Too crowded"), note("c3", status="done")])
    notes = menu_notes.read_notes(path)
    assert [item["id"] for item in notes] == ["a1", "b2", "c3"]
    listed = menu_notes.list_notes(notes, False)
    assert listed.splitlines()[0] == SCREEN and "a1" in listed and "c3" not in listed
    assert "the screen as a whole: Too crowded" in listed
    assert "[done" in menu_notes.list_notes(notes, True)
    assert menu_notes.main(["done", "a", "--file", str(path), "--commit", "abc123"]) == 0
    notes = menu_notes.read_notes(path)
    assert notes[0]["status"] == "done" and notes[0]["commit"] == "abc123" and notes[1]["status"] == "open"


def test_ids_by_their_first_characters(tmp_path):
    notes = [note("a1"), note("a2"), note("b3")]
    assert [item["id"] for item in menu_notes.pick(notes, ["b"])] == ["b3"]
    try:
        menu_notes.pick(notes, ["a"])
    except SystemExit as error:
        assert "more than one" in str(error)
    else:
        raise AssertionError("an ambiguous ID was taken")
    assert [item["id"] for item in menu_notes.pick(notes, [])] == ["a1", "a2", "b3"]


def test_brief_names_the_generator_widget_and_picture():
    text = menu_notes.brief([note("a1"), note("b2", widget=None, text="Too crowded")])
    assert "2 menu notes to address" in text
    assert "[a1] Say SPEAKERS" in text and "tools/port_settings.py" in text
    assert "widget: main_menu/settings_select/player_setup/player_profile_edit/audio_settings/output_device_label" in text
    assert 'its words: "OUTPUT DEVICE"' in text and "picture: /tmp/menu_notes/" in text
    assert "on the screen as a whole" in text and "keyboard input, 16:9 window" in text


def test_note_screens(tmp_path):
    path = tmp_path / "menu_notes.jsonl"
    write(path, [note("a1"), note("b2")])
    assert menu_notes.note_screens(path) == [SCREEN]


def test_the_gallery_page_shows_a_screens_notes_unchanged_or_not(tmp_path):
    screens = [menu_gallery.Screen(SCREEN, "x.xml", "tools/port_settings.py")]
    shown = menu_gallery.capture.Picture(4, 4, [bytes(12)] * 4)
    result = {"picture": shown, "digest": "same", "opened": True, "layout": None}
    key = (SCREEN, "keyboard-16x9")
    page = menu_gallery.write_page(tmp_path, screens, ["keyboard-16x9"], {key: result}, {}, {key: dict(result)}, {},
                                   notes={SCREEN: [note("a1"), note("c3", status="done")]}).read_text()
    assert page.count('<section class="screen"') == 1 and "Say SPEAKERS" in page and "done" in page
