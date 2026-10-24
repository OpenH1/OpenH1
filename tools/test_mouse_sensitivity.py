"""The mouse's turn in Counter-Strike 2's units (port/linux/include/mouse_sensitivity.h): its arithmetic in numbers
(port/linux/tests/mouse_sensitivity_test.c), and that the game uses it for the mouse alone: the platform layer's mouse
turns by it, each event's counts at once, and the zoom scales the mouse by input.zoom_sensitivity while the sticks,
the touch controls and VR keep the game's scaling."""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent


def test_mouse_sensitivity_numbers(tmp_path):
    """N counts at sensitivity S turn N * 0.022 * S degrees; zoomed, CS2's and the game's scaling; a stick and the
    mouse in one tick each by its own"""
    cc = next((path for path in map(shutil.which, ("clang", "cc", "gcc")) if path), None)
    if not cc:
        pytest.skip("no C compiler")
    binary = tmp_path / "mouse_sensitivity_test"
    # (the header alone: port/linux/include stands in for the C library's headers)
    shutil.copy(ROOT / "port" / "linux" / "include" / "mouse_sensitivity.h", tmp_path)
    build = subprocess.run([cc, "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-O1", f"-I{tmp_path}",
                            str(ROOT / "port" / "linux" / "tests" / "mouse_sensitivity_test.c"), "-lm", "-o",
                            str(binary)], capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0 and "PASS" in result.stdout, result.stdout + result.stderr


def test_mouse_alone_uses_it():
    """the mouse's look (xinput_sdl.c) turns its counts by mouse_radians_per_count, with nothing of the frame's time
    or the field of view; player_control.c's sticks and touch keep the game's one over the magnification, and only
    the mouse's turn takes mouse_zoom_look_scale"""
    xinput = (ROOT / "port/linux/src/xinput_sdl.c").read_text()
    look = xinput[xinput.index("int halo_linux_mouse_look("):]
    look = look[:look.index("\n}\n")]
    assert "radians = mouse_radians_per_count(mouse_sensitivity());" in look
    assert "*yaw = -x * radians;" in look and "* radians * vertical_sensitivity;" in look
    assert not re.search(r"time|fov|field_of_view|scale", look.split("radians = ")[1])
    control = (ROOT / "source/game/player_control.c").read_text()
    scale_look = control[control.index("static void player_control_scale_look("):]
    scale_look = scale_look[:scale_look.index("\n}\n")]
    assert "1.f / unit_get_zoom_magnification(" in scale_look and "mouse" not in scale_look
    mouse = control[control.index("if (halo_linux_mouse_look(gamepad_index, &mouse_yaw, &mouse_pitch))"):]
    mouse = mouse[:mouse.index("input->facing_delta.pitch += mouse_pitch;")]
    assert "mouse_zoom_look_scale(" in mouse and "unit_get_zoom_magnification" not in mouse
    # (the mouse's turn is added after the sticks' and touch's is made, on its own)
    assert control.index("player_control_scale_look(player->unit_index, control->zoom_level, &look_delta);") < \
        control.index("if (halo_linux_mouse_look(gamepad_index, &mouse_yaw, &mouse_pitch))")
    fov = (ROOT / "port/linux/game/view_fov.c").read_text()
    assert 'strcmp(config_string("input.zoom_sensitivity"), "halo")' in fov
