"""The VR mode's effects (port/linux/vulkan/README.md, "VR"): the thickness
bake's distances to the edge of known shapes and each eye's transforms
(port/linux/vulkan/tests/effects_test.c, the real xgpu_vulkan_effects.c), and
the effects' and glows' SPIR-V (the emitter's, tools/spirv_corpus) valid for
Vulkan 1.1. No GPU, display or game data is needed.
"""

import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parent.parent


def compiler():
    return os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")


def test_thickness_and_views(tmp_path):
    cc = compiler()
    if not cc:
        pytest.skip("needs a C compiler")
    binary = tmp_path / "effects_test"
    subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror",
                    str(ROOT / "port/linux/vulkan/tests/effects_test.c"), "-o", str(binary), "-lm"], check=True)
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    assert run.returncode == 0 and run.stdout.strip() == "ok", run.stdout + run.stderr


def test_effect_shaders(tmp_path):
    cc = compiler()
    if not cc:
        pytest.skip("needs a C compiler")
    build = tmp_path / "build"
    subprocess.run(["sh", str(ROOT / "tools/spirv_corpus/build.sh"), str(ROOT), str(build)], check=True,
                   env={**os.environ, "CC": cc})
    captures = tmp_path / "captures"
    captures.mkdir()
    corpus = tmp_path / "corpus"
    subprocess.run([str(build / "spirv_corpus"), str(captures), str(corpus)], check=True, capture_output=True)
    rows = [line.split("\t") for line in (corpus / "index.txt").read_text().splitlines()
            if line.startswith(("effect-", "glow-"))]
    # soft and thick, one map and two, each fade, flat and foveated; the
    # glows' two shaders flat and foveated
    assert len([row for row in rows if row[0].startswith("effect-")]) == 2 * 2 * 6 * 2
    assert len([row for row in rows if row[0].startswith("glow-")]) == 4
    files = sorted({row[1] for row in rows})
    assert all((corpus / f"{name}.spv").stat().st_size > 0 for name in files)
    validator = shutil.which("spirv-val")
    if not validator:
        pytest.skip("needs spirv-val (SPIRV-Tools)")
    for name in files:
        subprocess.run([validator, "--target-env", "vulkan1.1", str(corpus / f"{name}.spv")], check=True)
