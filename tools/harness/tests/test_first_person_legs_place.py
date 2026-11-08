"""The first-person body on the flat screen (port/linux/game/first_person_legs.c): its pose, made for a body of the
test's own placed about the eye as the cyborg's is standing, crouching and in a crouch jump. It is the same however
the view turns and tilts, set back along the body's own heading (#724); the same drawn again whatever came before,
nothing stepped from frame to frame (#772's shaking and flickering); the game's pose moved whole, nothing stretched
(#772's stretching), its chest and all above folded into the chest's joint and nothing drawn within the near plane's
distance of the eye (#772's camera in the legs), lowered only where the eye is at the hips in a crouch jump (#692);
and its shadow placed as its legs are."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, inline, mutated, read, run, structure  # noqa: E402

CASES = ["planted", "stateless", "clear", "crouch_jump", "shadow"]
CONTROLS = [
    # set back along the view's heading rather than the body's: the feet swing about as the view turns (#724)
    ("\tforward = object->object.forward;\n\tif (moved)",
     "\tforward = global_window_parameters.camera.forward;\n\tif (moved)", "planted"),
    # the chest drawn: its joint and the neck at the eye, in the near plane's reach
    ("\t\tif (first_person_legs_below(model, index, chest))\n", "\t\tif (index == 4)\n", "clear"),
    # never lowered: a crouch jump's hips and thighs at the eye
    ("\tif (drop > 0.f)\n", "\tif (drop > 1000.f)\n", "crouch_jump"),
    # a lean kept from frame to frame (#772): drawn again, the pose is not what it was
    ("\tdrop = placed[head].position.z",
     "\t{ static real kept = 0.f; for (index = 0; index < count; index++) placed[index].position.z -= kept; "
     "kept += 0.001f; }\n\tdrop = placed[head].position.z", "stateless"),
]
FUNCTIONS = ["first_person_legs_name_has", "first_person_legs_below", "first_person_legs_model",
             "first_person_legs_place", "first_person_legs_node_matrices",
             "first_person_legs_shadow_node_matrices"]


def types():
    math = read("source/math/real_math.h")
    return "\n".join([structure(math, "real_matrix4x3"), "typedef struct real_matrix4x3 real_matrix4x3;"]) + "\n"


def generated(fault=None):
    code = read("port/linux/game/first_person_legs.c")
    math = read("source/math/real_math.h")
    matrices = read("source/math/matrix_math.c")
    defines = "\n".join(line for line in re.findall(r"^#define FIRST_PERSON_LEGS_\w+ .*$", code, re.M))
    limits = re.search(r"^enum\s*\{\s*FIRST_PERSON_LEGS_MAXIMUM_NODES[^}]*\};", code, re.M).group(0)
    statics = "static real_matrix4x3 first_person_legs_matrices[FIRST_PERSON_LEGS_MAXIMUM_NODES];"
    assert statics in code
    text = "\n".join([defines, limits, statics] + [function(code, name) for name in FUNCTIONS])
    if fault:
        text = mutated(text, *fault)
    inlines = ["square_root", "scale_vector3d", "dot_product3d", "magnitude_squared3d", "magnitude3d", "normalize3d"]
    helpers = "\n".join([re.search(r"^#define _real_epsilon .*$", math, re.M).group(0)] +
                        [inline(math, name) for name in inlines] +
                        [function(matrices, name) for name in ["matrix4x3_multiply", "matrix4x3_transform_vector"]])
    return (("types.inc", types()), ("under_test.inc", helpers + "\n" + text))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("first_person_legs_place", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_negative_control(before, after, case):
    status, output = run(build("first_person_legs_place", generated((before, after))), case)
    assert status == CHECK_FAILED, output


def test_the_near_distance():
    # the test's near distance is the game's
    assert re.search(r"rasterizer_global_defaults =\s*\{\s*0\.0625f,", read("source/rasterizer/rasterizer.c"))
    assert "#define GAME_NEAR 0.0625f" in read("tools/harness/tests/first_person_legs_place.c")


def test_where_the_pose_takes_it():
    code = read("port/linux/game/first_person_legs.c")
    place = function(code, "first_person_legs_place")
    # the flat screen's: the body seen from the view drawn (#697), then set back and lowered; nothing weighed against
    # the view but the eye's height, nothing kept from one frame to the next
    assert "moved = render_interpolation_view_body(object_index, &move);" in place
    assert "global_window_parameters.frustum" not in code
    assert "global_window_parameters.camera.forward" not in code
    assert not re.search(r"^\s+static ", place, re.M)
    # drawing alone: the game's node matrices are read, never written
    assert "object_get_node_matrices" not in code
