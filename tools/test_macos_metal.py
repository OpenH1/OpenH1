"""Runs the Metal backend's tests (port/macos/tests/xgpu_metal_test.m) on a
Mac with Apple silicon: build them first with ``ninja macos_metal_test``.
They draw with Metal and read the pixels back; no window opens. The second
run with the same pipeline archive must make every pipeline from it."""
import platform
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
TEST = ROOT / "build" / "macos" / "xgpu_metal_test"

pytestmark = pytest.mark.skipif(
    sys.platform != "darwin" or platform.machine().lower() != "arm64" or not TEST.is_file(),
    reason="needs a Mac with Apple silicon and build/macos/xgpu_metal_test (ninja macos_metal_test)")


def run(*arguments):
    result = subprocess.run([str(TEST), *arguments], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout


def test_backend():
    assert "0 failed" in run()


def test_pipeline_archive(tmp_path):
    archive = tmp_path / "pipelines.bin"
    run("--archive", str(archive))
    assert archive.is_file()
    assert "0 failed" in run("--archive", str(archive), "--expect-archive")
