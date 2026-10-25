"""The guest's conditional branches relaxed once the converted code is too long for them
(tools/guest_branch_relax.py): a tbnz, cbz and b.cond out of reach become the opposite branch over a b, those in
reach are left, the measure is taken again when relaxing moves a branch out of reach, and a function near the reach
of what is not relaxed is an error naming it. Assembled for AArch64 where clang can (the assembler refuses the
code before the relaxation: the negative control)."""

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import guest_branch_relax  # noqa: E402
from guest_branch_relax import FUNCTION_LIMIT, MARGIN, REACH, RelaxError, relax  # noqa: E402


def function(body, name="big_function"):
    return ["\t.text", "\t.globl " + name, "\t.p2align 2", f"{name}:", *body, "\tret"]


def nops(count):
    return ["\tnop"] * count


def assemble(lines, folder: Path):
    """the assembler's (status, errors), None if there is no AArch64 assembler"""
    compiler = shutil.which("clang")
    if not compiler:
        return None
    source = folder / "code.s"
    source.write_text("\n".join(lines) + "\n")
    result = subprocess.run([compiler, "--target=aarch64-linux-gnu", "-c", str(source), "-o", str(folder / "code.o")],
                            capture_output=True, text=True)
    if result.returncode and "out of range" not in result.stderr:
        return None
    return result.returncode, result.stderr


FAR_TBNZ = function(["\ttbnz w8, #0, .LBB0_far", "\tcbz w1, .LBB0_near", *nops(9000), ".LBB0_near:", "\tnop",
                     ".LBB0_far:", "\tmov w0, #1"])


def test_far_tbnz_relaxed_near_left(tmp_path):
    out, count = relax(FAR_TBNZ)
    assert count == 1
    at = out.index("\tb .LBB0_far")
    assert out[at - 1].startswith("\ttbz w8, #0, .Lrelax_") and out[at + 1] == out[at - 1].split(", ")[-1] + ":"
    assert "\tcbz w1, .LBB0_near" in out
    assembled = assemble(out, tmp_path)
    if assembled is None:
        pytest.skip("needs clang with an AArch64 assembler")
    assert assembled[0] == 0, assembled[1]


def test_negative_control_unrelaxed_fails(tmp_path):
    assembled = assemble(FAR_TBNZ, tmp_path)
    if assembled is None:
        pytest.skip("needs clang with an AArch64 assembler")
    assert assembled[0] != 0 and "out of range" in assembled[1]


def test_conditions_and_backward_branches(tmp_path, monkeypatch):
    # (a function past 1 MB, which the limit would refuse first)
    monkeypatch.setattr(guest_branch_relax, "FUNCTION_LIMIT", 4 << 20)
    lines = function([".LBB1_top:", *nops(270000), "\tb.hi .LBB1_top", "\tcbnz x3, .LBB1_end", *nops(270000),
                      ".LBB1_end:"])
    out, count = relax(lines)
    assert count == 2
    assert "\tb.ls " in "\n".join(out) and "\tb .LBB1_top" in out
    assert any(line.startswith("\tcbz x3, .Lrelax_") for line in out) and "\tb .LBB1_end" in out
    assembled = assemble(out, tmp_path)
    if assembled is not None:
        assert assembled[0] == 0, assembled[1]


def test_relaxing_moves_another_out_of_reach():
    # (the second branch's target just in reach, until the far one between
    # them takes its extra instruction)
    within = (REACH["tbz"] - MARGIN) // 4
    lines = function(["\ttbz w2, #3, .LBB2_edge", "\ttbz w4, #1, .LBB2_far", *nops(within - 2), ".LBB2_edge:",
                      *nops(9000), ".LBB2_far:"])
    out, count = relax(lines)
    assert count == 2, out[:12]


def test_other_sections_and_symbols_left():
    lines = function(["\tcbz w0, external_function", "\tb.eq .LBB3_data", *nops(10)]) + \
        ["\t.section .rodata,\"a\",@progbits", ".LBB3_data:", "\t.long 0"]
    out, count = relax(lines)
    assert count == 0 and out == lines


def test_alignment_and_data_measured_from_above():
    # (an alignment's padding and data counted at their most: the branch is
    # relaxed though its nops alone are in reach)
    lines = function(["\ttbnz w0, #2, .LBB4_after", *nops(7000), "\t.p2align 12", "\t.zero 4096", ".LBB4_after:"])
    assert relax(lines)[1] == 1


def test_function_near_the_limit_is_named():
    lines = function(nops(FUNCTION_LIMIT // 4 + 1), name="pc_menu_giant")
    with pytest.raises(RelaxError, match="pc_menu_giant"):
        relax(lines)
    relax(function(nops(FUNCTION_LIMIT // 4 - 64)))
