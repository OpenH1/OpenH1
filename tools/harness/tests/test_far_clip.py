"""display.draw_distance (port/linux/game/far_clip.c's render_far_clip_distance): the far clip plane the local views
are drawn to, the game's own by default; and where the game takes it (the projection, the structure's culling) and
what keeps the game's own (the teammates' marks' fade)."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["default", "farther", "out_of_range", "script_nearer", "script_farther"]
CONTROLS = [
    # the setting unread
    ("\treturn distance;\n}", "\treturn game_distance;\n}", "farther"),
    # a script's nearer far clip overridden by the setting
    ("if (!(game_distance >= rasterizer_global_defaults.far_clip_distance) || !(distance > game_distance))",
     "if (!(distance > game_distance))", "script_nearer"),
    # the setting in place of a script's farther one
    ("if (!(game_distance >= rasterizer_global_defaults.far_clip_distance) || !(distance > game_distance))",
     "if (!(game_distance >= rasterizer_global_defaults.far_clip_distance) || !distance)", "script_farther"),
    # read once, never again
    ("if (!initialized || read_at != changes)", "if (!initialized || !read_at)", "farther"),
    # no upper bound
    ("setting <= DRAW_DISTANCE_MOST", "TRUE", "out_of_range"),
]


def _model_slider(key: str) -> str:
    """the model's slider row of the key (port/assets/menus/model, which menu_functions.c's sliders are made from):
    its element's line"""
    model = read("port/assets/menus/model/settings.xml")
    return next(line for line in model.splitlines() if f'<row key="{key}" kind="slider"' in line)

def generated(fault=None):
    code = read("port/linux/game/far_clip.c")
    text = "\n".join(re.findall(r"^#define DRAW_DISTANCE_\w+ .*$", code, re.M)) + "\n" + \
        function(code, "render_far_clip_distance")
    if fault:
        text = mutated(text, *fault)
    return (("under_test.inc", text),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("far_clip", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_negative_control(before, after, case):
    status, output = run(build("far_clip", generated((before, after))), case)
    assert status == CHECK_FAILED, output


def test_where_the_game_takes_it():
    # the local views' projection (and from it the render camera's far distance, which the structure's visibility
    # walks portals to, and the frustum's far plane the objects are culled against), and the structure culling's
    # own far plane
    main = read("source/main/main.c")
    assert "\twindow->rasterizer_camera.z_far = render_far_clip_distance();\n" in main
    assert main.count("rasterizer_globals.far_clip_distance") == 1  # (the rasterizer's test loop, no player's view)
    culling = read("port/linux/game/structure_culling.c")
    assert "render.camera.position.z +\n\t\trender_far_clip_distance();" in culling
    assert "rasterizer_globals.far_clip_distance" not in culling
    visibility = read("source/structures/structure_visibility.c")
    assert "render.camera.z_far)))" in visibility


def test_the_marks_keep_their_fade():
    # the teammates' marks fade over a share of the game's own far distance, not the drawn one
    hud = read("source/interface/hud.c")
    assert "(MIN(render.frustum.z_far, rasterizer_globals.far_clip_distance) - render.frustum.z_near);" in hud


def test_the_setting():
    config = read("port/linux/src/port_config.c")
    assert '{ "display.draw_distance", _config_real, "1024.0", "HALO_DRAW_DISTANCE",' in config
    row = _model_slider("draw_distance")
    assert 'setting="display.draw_distance"' in row
    assert 'least="1024" most="10000" step="1" typed_least="1024" typed_most="10000" whole="true"' in row
    code = read("port/linux/game/far_clip.c")
    assert "#define DRAW_DISTANCE_LEAST 1024.0" in code and "#define DRAW_DISTANCE_MOST 10000.0" in code
