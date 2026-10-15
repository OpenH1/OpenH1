"""Runs the C tests of the guest's memory with the base register
(port/android/host/host_memory_base.c) with the build machine's C compiler,
on a 64-bit Linux or macOS host (the unit reserves 4 GB of address space).
On macOS, whose pages are 16 KB, they test the guest's 4 KB pages on larger
host pages."""
import platform
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent


def compiler():
    for name in ("clang", "cc", "gcc"):
        path = shutil.which(name)
        if path:
            return path
    return None


def test_host_memory_base(tmp_path):
    cc = compiler()
    if not cc:
        pytest.skip("no C compiler")
    if sys.platform not in ("linux", "darwin") or platform.architecture()[0] != "64bit":
        pytest.skip("needs a 64-bit Linux or macOS host")
    binary = tmp_path / "host_memory_base_test"
    subprocess.run(
        [cc, "-std=gnu11", "-Wall", "-Werror", "-O1", "-D_GNU_SOURCE", "-D_DARWIN_C_SOURCE", "-DHALO_GUEST_BASE=1",
         f"-I{ROOT / 'port' / 'android' / 'host'}", f"-I{ROOT / 'port' / 'android' / 'include'}",
         str(ROOT / "port" / "android" / "tests" / "host_memory_base_test.c"),
         str(ROOT / "port" / "android" / "host" / "host_memory_base.c"),
         "-o", str(binary), "-lpthread"],
        check=True,
    )
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
