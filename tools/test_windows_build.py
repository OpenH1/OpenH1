"""The Windows build's generators: the host's import thunks
(tools/android_imports.py --host-abi sysv) and the guest's build steps that
stand in for shell commands (tools/guest_build_steps.py)."""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"


def run(*arguments):
    subprocess.run([sys.executable, *map(str, arguments)], check=True, cwd=ROOT)


def test_sysv_thunks_take_each_class(tmp_path):
    prototypes = tmp_path / "guest.h"
    prototypes.write_text(
        "long long host_syscall(long long number, long long a, long long b, long long c,\n"
        "\tlong long d, long long e, long long f);\n"
        "void host_log(int priority, const char *text);\n"
        "int host_sdl_set_window_fullscreen_mode(unsigned int window, unsigned int display, int width, int height,\n"
        "\tfloat refresh_rate);\n"
        "void host_exit(int code) __attribute__((noreturn));\n",
        encoding="utf-8")
    posix = tmp_path / "posix.h"
    posix.write_text("void *posix_directory_open(const char *path);\n", encoding="utf-8")
    names = tmp_path / "imports.list"
    names.write_text("host_syscall\nhost_log\nhost_sdl_set_window_fullscreen_mode\nhost_exit\n"
                     "hostposix_directory_open\n", encoding="utf-8")
    table = tmp_path / "table.c"
    run(TOOLS / "android_imports.py", "--arch", "x32", "--host-abi", "sysv", "--prototypes", prototypes,
        "--prototypes", posix, "--host-table", table, tmp_path / "imports.s", names)
    text = table.read_text(encoding="utf-8")
    assert "#define GUEST_ABI __attribute__((sysv_abi))" in text
    assert ("static GUEST_ABI long long import_0(long long a0, long long a1, long long a2, long long a3, "
            "long long a4, long long a5, long long a6)") in text
    assert "static GUEST_ABI void import_1(int a0, void * a1)" in text
    assert "static GUEST_ABI int import_2(int a0, int a1, int a2, int a3, float a4)" in text
    assert "\thost_exit(a0);" in text
    # (a posix.h import calls posix_<name>)
    assert "void * posix_directory_open(void *);" in text
    assert '{ "hostposix_directory_open", (void *)import_4 },' in text
    assert "void *host_resolve_import(const char *name)" in text
    # (the guest's stubs are the same as without --host-abi)
    plain = tmp_path / "plain.s"
    run(TOOLS / "android_imports.py", "--arch", "x32", plain, names)
    assert plain.read_text(encoding="utf-8") == (tmp_path / "imports.s").read_text(encoding="utf-8")


def test_sysv_thunks_need_every_prototype(tmp_path):
    names = tmp_path / "imports.list"
    names.write_text("host_missing\n", encoding="utf-8")
    prototypes = tmp_path / "guest.h"
    prototypes.write_text("void host_log(int priority, const char *text);\n", encoding="utf-8")
    result = subprocess.run([sys.executable, str(TOOLS / "android_imports.py"), "--arch", "x32", "--host-abi", "sysv",
                             "--prototypes", str(prototypes), "--host-table", str(tmp_path / "table.c"),
                             str(tmp_path / "imports.s"), str(names)], cwd=ROOT, capture_output=True, text=True)
    assert result.returncode != 0
    assert "host_missing" in result.stderr


def test_build_steps(tmp_path):
    arch = tmp_path / "arch.in"
    arch.write_text("#define _Addr int\nTYPEDEF int wchar_t;\n", encoding="utf-8")
    common = tmp_path / "common.in"
    common.write_text("TYPEDEF unsigned _Addr size_t;\nSTRUCT iovec { void *iov_base; size_t iov_len; };\n"
                      "UNION _G_fpos64_t { char __opaque[16]; double __align; };\n", encoding="utf-8")
    alltypes = tmp_path / "alltypes.h"
    run(TOOLS / "guest_build_steps.py", "alltypes", alltypes, arch, common)
    assert alltypes.read_text(encoding="utf-8") == (
        "#define _Addr int\n"
        "#if defined(__NEED_wchar_t) && !defined(__DEFINED_wchar_t)\ntypedef int wchar_t;\n"
        "#define __DEFINED_wchar_t\n#endif\n\n"
        "#if defined(__NEED_size_t) && !defined(__DEFINED_size_t)\ntypedef unsigned _Addr size_t;\n"
        "#define __DEFINED_size_t\n#endif\n\n"
        "#if defined(__NEED_struct_iovec) && !defined(__DEFINED_struct_iovec)\n"
        "struct iovec { void *iov_base; size_t iov_len; };\n#define __DEFINED_struct_iovec\n#endif\n\n"
        "#if defined(__NEED_union__G_fpos64_t) && !defined(__DEFINED_union__G_fpos64_t)\n"
        "union _G_fpos64_t { char __opaque[16]; double __align; };\n#define __DEFINED_union__G_fpos64_t\n#endif\n\n")
    numbers = tmp_path / "syscall.in"
    numbers.write_text("#define __NR_read 63\n#define __NR_write 64\n", encoding="utf-8")
    syscall_h = tmp_path / "syscall.h"
    run(TOOLS / "guest_build_steps.py", "syscall-h", syscall_h, numbers)
    assert syscall_h.read_text(encoding="utf-8") == (
        "#define __NR_read 63\n#define __NR_write 64\n#define SYS_read 63\n#define SYS_write 64\n")
    version = tmp_path / "version.h"
    run(TOOLS / "guest_build_steps.py", "version-h", version, "1.2.5")
    assert version.read_text(encoding="utf-8") == '#define VERSION "1.2.5"\n'


def test_xdk_enums_step(tmp_path):
    pdb = tmp_path / "xdk_pdb.h"
    pdb.write_text("/* intro */\n/* ---------- enumerations */\nenum a { b };\n\n"
                   "/* ---------- structures and unions */\nstruct c;\n", encoding="utf-8")
    d3d8 = tmp_path / "xdk_d3d8.h"
    d3d8.write_text("#define D3DFORMAT_X 1\n#define OTHER 2\n#define D3DSIZE_Y 3\n#define D3DTEXTURE_Z 4\n",
                    encoding="utf-8")
    output = tmp_path / "xdk_enums.h"
    run(TOOLS / "guest_build_steps.py", "xdk-enums", output, pdb, d3d8)
    assert output.read_text(encoding="utf-8") == (
        "/* ---------- enumerations */\nenum a { b };\n\n#define D3DFORMAT_X 1\n#define D3DSIZE_Y 3\n"
        "#define D3DTEXTURE_Z 4\n")
