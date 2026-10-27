"""Android's back key (the edge swipe of gesture navigation, or the back button) pauses a game as Escape does
(xinput_sdl.c's keyboard_bound_actions and keyboard_controls), as Start; the desktop's builds have no such key."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, function, mutated, read, run  # noqa: E402

# the back key's lines, which the negative control takes out (the code before it paused nothing)
BACK = re.compile(r"#ifdef HALO_ANDROID\n\t/\* the system back key.*?#endif\n", re.S)


def generated(android=True, fault=False):
    inputs = read("port/linux/src/xinput_sdl.c")
    code = "\n".join(function(inputs, name) for name in ("input_held", "keyboard_bound_actions", "keyboard_controls"))
    if fault:
        back = BACK.findall(code)
        assert len(back) == 1, "the back key's lines are no longer found"
        code = mutated(code, back[0], "")
    config = "#define HALO_ANDROID 1\n" if android else ""
    config += "\n".join(re.findall(r"^#define XINPUT_GAMEPAD_\w+ [^\n]+", read("port/include/xdk/xdk_xbox.h"), re.M))
    config += "\n" + enum_with(read("port/linux/include/halo_keyboard.h"), "HALO_KEYBOARD_PAUSE") + "\n"
    config += f"#define MAXIMUM_BINDINGS {constant(inputs, 'MAXIMUM_BINDINGS')}\n"
    names = set(re.findall(r"SDL_SCANCODE_\w+", code)) - {"SDL_SCANCODE_COUNT"}
    names |= {"SDL_SCANCODE_AC_BACK", "SDL_SCANCODE_ESCAPE", "SDL_SCANCODE_TAB"}
    config += "enum {" + ", ".join(sorted(names)) + ", TEST_SCANCODE_COUNT};\n"
    return (("config.inc", config), ("under_test.inc", code + "\n"))


@pytest.mark.parametrize("android,case", [(True, "back-pauses"), (False, "desktop")])
def test_back_pause(android, case):
    status, output = run(build("back_pause", generated(android)), case)
    assert status == 0, output


def test_back_not_pausing_detected():
    """without the back key's lines (the code before the change), Back pauses nothing"""
    status, output = run(build("back_pause", generated(fault=True)), "back-pauses")
    assert status == CHECK_FAILED, output
