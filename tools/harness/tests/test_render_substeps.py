"""The frames between the ticks at 60 and 120 Hz (port/linux/game/render_interpolation.c, on sim_rate.c's
sim_substep_blend): an object of the micro lane, and the first-person view of a player's biped, are drawn between
the poses its sub-steps left (a frame in sub-step j of the tick drawn: between the end of j - 1, or of the tick
before, and the end of j), what it carries with it; at 30 the frames are the line between the ticks' snapshots, as
they were. A projectile's head start and hold, and the VR mode's grenade let go of in a sub-step, are drawn as
before, from a tick's motion back or from the hand, with no dart and no pop."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure  # noqa: E402

CASES = ["classic", "substeps", "substeps_60", "carried", "camera_slow", "projectile", "projectile_late", "vr_release"]

# a fault in the code, and the case that must catch it
NEGATIVE_CONTROLS = {
    # the micro lane drawn on the line between the ticks, as before: a tick's sub-steps' path not followed
    "no-substep-path": (("\t\tif (!jumped && render_interpolation_substep_offset(object_index, snap, &offset))",
                         "\t\tif (0)"), "substeps"),
    # the sub-step drawn the frame's fraction of the tick, not of its sub-step
    "tick-fraction": (("point_lerp(before, &substeps->positions[buffer][interpolation_substep_index], "
                       "interpolation_substep_fraction,",
                       "point_lerp(before, &substeps->positions[buffer][interpolation_substep_index], "
                       "interpolation_fraction,"), "substeps"),
    # what is carried left on the line
    "carried-left": (("\troot_index = object_get_ultimate_parent(object_index);",
                      "\troot_index = object_index;"), "carried"),
    # the view left on the line between the ticks
    "camera-on-the-line": (("\tif (substep_unit_index != NONE && render_interpolation_substep_offset(substep_unit_index, "
                            "FALSE, &offset))",
                            "\tif (0)"), "substeps"),
    # the view's snapshot taken with the next tick's first sub-steps in it (a frame slower than a sub-step)
    "camera-unshifted": (("\t\t\trender_interpolation_camera_at_tick(substep_unit_index, &camera->latest);",
                          "\t\t\t;"), "camera_slow"),
    # a projectile's head start and hold drawn as they run: it darts out and waits
    "head-start-drawn": (("\t\t!substeps->continuing[buffer] ||", ""), "projectile"),
    # ... and the hold's end in the tick after
    "hold-drawn": (("\t\t!substeps->continuing[buffer] ||", ""), "projectile_late"),
    # the grenade let go of in a sub-step drawn from the tick before's snapshot (the body's hand) until the tick ends
    "release-from-the-body": (("\tif ((pending = render_interpolation_pending_from(object_index)) != NULL)",
                               "\tif ((pending = NULL) != NULL)"), "vr_release"),
}


def generated(fault=None):
    header = read("port/linux/game/sim_rate.h")
    clock = read("port/linux/game/sim_rate.c")
    code = read("port/linux/game/render_interpolation.c")

    decls = enum_with(header, "SIM_RATE_CLASSIC") + "\n" + enum_with(header, "NUMBER_OF_SIM_SUBSTEP_SLOTS") + "\n"
    start = header.index("typedef void (*sim_substep_procedure)")
    decls += header[start:header.index(";", start) + 1] + "\n"

    # the constants and structures
    text = code[code.index("/* ---------- constants */"):code.index("/* ---------- structures */")] + "\n"
    for name in ("interpolation_quaternion", "interpolation_rotation", "interpolated_object", "interpolated_substeps",
                 "interpolated_camera"):
        text += structure(code, name) + "\n"
    # the globals, but for the first-person weapon's, the particles' and the ghosts' (the test's own)
    globals_text = code[code.index("static struct interpolated_object *interpolated_objects;"):
                        code.index("/* ---------- blending */")]
    text += "\n".join(line for line in globals_text.splitlines()
                      if not re.search(r"interpolated_first_person|later_particle|ghosts\[", line)) + "\n"
    # sim_substep_blend, on the test's clock
    text += function(clock, "sim_substep_blend").replace("sim_rate_globals.substep_count", "fake_substep_count") + "\n"
    text += "static void render_interpolation_substep(short substep_index, short substep_count, real seconds);\n"
    text += "static void render_interpolation_camera_anchors(void);\n"
    for name in ("lerp", "point_lerp", "vector_length", "vector_nlerp", "basis_is_rotation", "quaternion_from_basis",
                 "basis_from_quaternion", "rotation_from_matrix", "matrix_blend_rotations", "correction_significant",
                 "correction_advance", "correction_drawn", "correction_add", "distance_squared", "vector_in_frame",
                 "vector_from_frame", "point_in_frame", "point_from_frame", "matrix_in_frame", "matrix_from_frame",
                 "frame_blend", "render_interpolation_previous_values", "render_interpolation_register",
                 "render_interpolation_tick",
                 "render_interpolation_object_from", "render_interpolation_substep",
                 "render_interpolation_substep_offset", "render_interpolation_substep_frame",
                 "render_interpolation_frame_begin", "render_interpolation_frame_end",
                 "render_interpolation_pending_from", "render_interpolation_blend_in_frame",
                 "render_interpolation_object_node_matrices", "observer_blend", "observer_cut",
                 "render_interpolation_camera_substep_unit", "render_interpolation_camera_at_tick",
                 "render_interpolation_camera_anchors", "render_interpolation_camera_substep_vehicle",
                 "render_interpolation_camera_at_tick_in_vehicle", "render_interpolation_camera_vehicle_offset",
                 "render_interpolation_blended_camera"):
        text += function(code, name) + "\n"
    if fault:
        text = mutated(text, *fault)
    return (("decls.inc", decls), ("under_test.inc", text))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("render_substeps", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("render_substeps", generated(fault)), case)
    assert status == CHECK_FAILED, f"the code with a fault ({control}) passed '{case}': the test cannot see it"


def test_registered_last():
    # the poses are kept after every slot that moves anything, and the frames read only render-side tables
    header = read("port/linux/game/sim_rate.h")
    for slot in ("_sim_substep_bipeds,", "_sim_substep_items,", "_sim_substep_projectiles,", "_sim_substep_vr,"):
        assert header.index(slot) < header.index("_sim_substep_render_interpolation,")
    code = read("port/linux/game/render_interpolation.c")
    assert "sim_substep_register(_sim_substep_render_interpolation, render_interpolation_substep);" in code
