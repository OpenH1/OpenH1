"""The first-person body kept out of the near plane's cut without moving it (port/linux/game/first_person_legs.c):
its pose, made for a body of the test's own in views of its own. Its feet stay where the game has them whatever the
view's pitch (#724: set back from the eye and lowered out of the cut, the legs slid across the ground as the view
tilted); crouching in the air, with the eye at the hips, nothing of it is left in the cut but what is folded into the
eye (#692); and the triangles in the cut are found whole, not by their corners (#692's second report)."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, inline, mutated, read, run, structure  # noqa: E402

CASES = ["planted", "crouch_jump", "triangle"]
CONTROLS = [
    # the legs leant back with the upper body, about the hips: the feet swing back as the view looks down
    ("leans[index] = node->parent_node_index != NONE && !first_person_legs_node(node->name);",
     "leans[index] = node->parent_node_index != NONE;", "planted"),
    # nothing folded: the hips and thighs at the eye are cut by the near plane
    ("\t\t\t\t\tfolded[bound[corner]] = TRUE;\n", "", "crouch_jump"),
    # each triangle by its corners alone, as before #692's second report: a thigh's plate across the cut not found
    ("\tcross_product3d(&sides[0], &sides[1], &normal);\n",
     "\tspan[0] = low;\n\tspan[1] = high;\n\treturn low < high;\n\tcross_product3d(&sides[0], &sides[1], &normal);\n",
     "triangle"),
]
FUNCTIONS = ["first_person_legs_name_has", "first_person_legs_node", "first_person_legs_turn",
             "first_person_legs_rotate", "first_person_legs_seen_within", "first_person_legs_lean_split",
             "first_person_legs_splits", "first_person_legs_lean_corner", "first_person_legs_lean",
             "first_person_legs_point_span", "first_person_legs_cut_corners", "first_person_legs_triangle_span",
             "first_person_legs_cut", "first_person_legs_surface", "first_person_legs_fold_cut",
             "first_person_legs_seconds", "first_person_legs_node_matrices"]


def types():
    math = read("source/math/real_math.h")
    mesh = read("port/linux/game/object_mesh.h")
    limits = re.search(r"enum\s*\{\s*OBJECT_MESH_MAXIMUM_NODES[^}]*\};", mesh).group(0)
    return "\n".join([structure(math, "real_plane3d"), "typedef struct real_plane3d real_plane3d;",
                      structure(math, "real_matrix4x3"), "typedef struct real_matrix4x3 real_matrix4x3;", limits,
                      structure(mesh, "object_mesh_vertex"), structure(mesh, "object_mesh")]) + "\n"


def generated(fault=None):
    code = read("port/linux/game/first_person_legs.c")
    math = read("source/math/real_math.h")
    matrices = read("source/math/matrix_math.c")
    defines = "\n".join(re.findall(r"^#define FIRST_PERSON_LEGS_\w+ .*$", code, re.M))
    limits = re.search(r"^enum\s*\{\s*FIRST_PERSON_LEGS_MAXIMUM_NODES[^}]*\};", code, re.M).group(0)
    statics = "\n".join(re.findall(r"^static (?:real_matrix4x3|real_point3d|real|boolean) first_person_legs_\w+\[.*$",
                                   code, re.M))
    text = "\n".join([defines, limits, structure(code, "first_person_legs_split"), statics] +
                     [function(code, name) for name in FUNCTIONS])
    if fault:
        text = mutated(text, *fault)
    sources = [re.search(r"^#define _real_epsilon .*$", math, re.M).group(0),
               re.search(r"^#define NUMBEROF\(.*$", read("source/cseries/cseries.h"), re.M).group(0)]
    worlds = ["static real_point3d const global_origin = { 0.f, 0.f, 0.f };",
              "static real_point3d const *const global_origin3d = &global_origin;",
              "static real_vector3d const global_zero = { 0.f, 0.f, 0.f };",
              "static real_vector3d const *const global_zero_vector3d = &global_zero;",
              "static real_vector3d const global_up = { 0.f, 0.f, 1.f };",
              "static real_vector3d const *const global_up3d = &global_up;"]
    inlines = ["square_root", "scale_vector3d", "dot_product3d", "cross_product3d", "plane3d_distance_to_point",
               "vector_from_points3d", "magnitude_squared3d", "magnitude3d", "normalize3d", "distance_squared3d",
               "distance3d"]
    helpers = "\n".join(sources + worlds + [inline(math, name) for name in inlines] +
                        [function(matrices, name) for name in ["matrix4x3_multiply", "matrix4x3_transform_point",
                                                               "matrix4x3_transform_vector"]])
    return (("types.inc", types()), ("under_test.inc", helpers + "\n" + text))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("first_person_legs_cut", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_negative_control(before, after, case):
    status, output = run(build("first_person_legs_cut", generated((before, after))), case)
    assert status == CHECK_FAILED, output


def test_the_constants():
    # the test's margin, near distance and reach are the code's
    code = read("port/linux/game/first_person_legs.c")
    assert "#define FIRST_PERSON_LEGS_UNCUT 0.01f" in code
    test = read("tools/harness/tests/first_person_legs_cut.c")
    assert "#define MARGIN 0.01f" in test
    assert "#define REACH 0.2f" in test and "#define NEAR_CLIP_REACH 0.2f" in read("port/linux/game/near_clip.c")
    assert re.search(r"rasterizer_global_defaults =\s*\{\s*0\.0625f,", read("source/rasterizer/rasterizer.c"))
    # the frustum's planes in the order the test builds them
    assert re.search(r"_render_frustum_plane_left,\s*_render_frustum_plane_right,\s*_render_frustum_plane_bottom,"
                     r"\s*_render_frustum_plane_top,\s*_render_frustum_plane_near,",
                     read("source/render/render_cameras.c"))


def test_where_the_pose_takes_it():
    code = read("port/linux/game/first_person_legs.c")
    pose = function(code, "first_person_legs_node_matrices")
    # the flat screen's: the body seen from the view drawn (#697), its upper body leant clear of the eye of the view
    # as drawn, then what is left in the cut folded away
    assert "moved = render_interpolation_view_body(object_index, &move);" in pose
    lean = pose.index("first_person_legs_lean(")
    fold = pose.index("first_person_legs_fold_cut(object_index, model, count, "
                      "&global_window_parameters.camera.position)")
    moved = pose.index("moved = render_interpolation_view_body")
    assert moved < lean < fold < pose.rindex("return first_person_legs_matrices;")
    assert "views[0] = views[1] = &global_window_parameters.frustum;" in pose
    # nothing of the body is moved to keep it out of the eye's way (#724): no set-back, no drop
    assert "set_back" not in code and "drop" not in pose
    assert not re.search(r"position\.[xyz] [-+]=", pose)
    # the cut weighed in the view as drawn
    fold_cut = function(code, "first_person_legs_fold_cut")
    assert "first_person_legs_cut(&global_window_parameters.frustum, first_person_legs_points, points," in fold_cut
    # drawing alone: the game's node matrices are read, never written
    assert "object_get_node_matrices" not in code
