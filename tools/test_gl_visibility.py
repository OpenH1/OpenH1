"""Runs port/android/tests/gl_visibility_test.c: the OpenGL ES backend's
visibility tests counted by atomic counters (port/linux/src/xgpu_gl.c), as
the Android host builds it, over a fake OpenGL ES whose GPU runs behind the
CPU and whose undefined results are garbage. Built with the build machine's
clang or cc on a Linux host with the OpenGL ES 3 headers, as
tools/host_gl_build.py builds the host's backend: its files copied beside
the host's stand-ins for the guest's headers (port/linux/host/gl)."""
import platform
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SHARED = ("xgpu.h", "xgpu_backend.h", "xgpu_shader.h", "xgpu_gl.h", "gl.h", "xgpu_gl.c")
XDK = ROOT / "port" / "include" / "xdk"


def xdk_enums() -> str:
    """the XDK's enumerations and texture format words, as
    tools/host_gl_build.py's xdk_enums rule writes them"""
    pdb = (XDK / "xdk_pdb.h").read_text(encoding="utf-8").splitlines(keepends=True)
    start = next(index for index, line in enumerate(pdb) if line.startswith("/* ---------- enumerations"))
    end = next(index for index, line in enumerate(pdb) if line.startswith("/* ---------- structures and unions"))
    defines = [line for line in (XDK / "xdk_d3d8.h").read_text(encoding="utf-8").splitlines(keepends=True)
               if line.startswith(("#define D3DFORMAT_", "#define D3DSIZE_", "#define D3DTEXTURE_",
                                   "#define D3DTSS_MAXSTAGES "))]
    return "".join(pdb[start:end] + defines)


def test_gl_visibility(tmp_path):
    cc = shutil.which("clang") or shutil.which("cc")
    if not cc or not sys.platform.startswith("linux") or platform.architecture()[0] != "64bit":
        pytest.skip("needs a C compiler on 64-bit Linux")
    if not Path("/usr/include/GLES3/gl32.h").exists():
        pytest.skip("no OpenGL ES 3 headers")
    for name in SHARED:
        shutil.copy(ROOT / "port" / "linux" / "src" / name, tmp_path / name)
    for header in (ROOT / "port" / "linux" / "host" / "gl").glob("*.h"):
        shutil.copy(header, tmp_path / header.name)
    (tmp_path / "xdk_enums.h").write_text(xdk_enums(), encoding="utf-8")
    shutil.copy(ROOT / "port" / "android" / "tests" / "gl_visibility_test.c", tmp_path)
    binary = tmp_path / "gl_visibility_test"
    # (the rest of the backend, which the test does not reach, is left
    # unlinked)
    subprocess.run(
        [cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wno-unused-function", "-D_GNU_SOURCE", "-DHALO_GLES=1",
         "-DXGPU_GL_HOST=1", f"-I{tmp_path}", f"-I{ROOT / 'port' / 'android' / 'include'}",
         f"-I{ROOT / 'port' / 'android' / 'host'}", f"-I{ROOT / 'port' / 'linux' / 'src'}",
         f"-I{ROOT / 'port' / 'third_party' / 'tomlc17'}", str(tmp_path / "gl_visibility_test.c"),
         "-fno-pic", "-no-pie", "-Wl,--unresolved-symbols=ignore-all", "-o", str(binary)],
        check=True)
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
