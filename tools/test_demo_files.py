"""The demos' files (port/linux/src/demo_files.c, demo_unpack.c;
docs/demos.md) without the game or its maps: port/linux/tests/demo_files_test.c
writes a demo with the writer's thread and reads it back, its blocks packed
with deflate (the system's here, the game's in the builds) and unpacked with
the port's zlib, a damaged block
refused, and keeps the demos as demo.keep_count and demo.keep_megabytes say
(never a pinned one); then tools/demo_info.py reads the demo it wrote."""
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "port" / "linux" / "src"
PORT_ZLIB = ROOT / "port" / "third_party" / "zlib"


@pytest.fixture(scope="module")
def demo_files_test(tmp_path_factory):
    compiler = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not compiler:
        pytest.skip("no C compiler")
    folder = tmp_path_factory.mktemp("demo_files_build")
    for name in ("demo_files.c", "demo_files.h", "demo_unpack.c", "posix.h"):
        shutil.copy(SOURCE / name, folder / name)
    (folder / "platform.h").write_text("void platform_log(const char *format, ...);\n"
                                       "const char *platform_data_root(void);\n")
    objects = []
    # (the writer's deflate the system's zlib here, the game's own being
    # tied to the game's allocator; the reader's inflate the port's zlib,
    # prefixed, as the builds have it)
    (folder / "memory" / "zlib").mkdir(parents=True)
    (folder / "memory" / "zlib" / "zlib.h").write_text("#include <zlib.h>\n")
    for name in ("adler32.c", "crc32.c", "inffast.c", "inflate.c", "inftrees.c", "uncompr.c", "zutil.c"):
        output = folder / f"port_{name}.o"
        subprocess.run([compiler, "-c", "-w", "-O1", "-DZ_PREFIX", "-Dz_errmsg=z_port_errmsg",
                        str(PORT_ZLIB / name), "-o", str(output)], check=True)
        objects.append(output)
    binary = folder / "demo_files_test"
    command = [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-O1", "-g",
               "-D_GNU_SOURCE", f"-I{folder}", "-pthread",
               str(folder / "demo_files.c"), str(SOURCE / "demo_unpack.c"), str(SOURCE / "posix_files.c"),
               str(ROOT / "port" / "linux" / "tests" / "demo_files_test.c"), *map(str, objects), "-lz", "-o", str(binary)]
    subprocess.run(command, check=True)
    return binary


def test_demo_written_read_and_kept(demo_files_test, tmp_path):
    result = subprocess.run([str(demo_files_test), str(tmp_path)], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    # (the retention's deletions logged)
    assert "c-3.demo deleted" in result.stderr


def test_demo_info_reads_a_demo(demo_files_test, tmp_path):
    result = subprocess.run([str(demo_files_test), str(tmp_path), "write"], capture_output=True, text=True,
                            timeout=120)
    assert result.returncode == 0, result.stderr
    path = Path(result.stdout.strip().splitlines()[0])
    assert path.parent == tmp_path / "demos"
    sys.path.insert(0, str(ROOT / "tools"))
    import demo_info

    demo = demo_info.Demo(path.read_bytes())
    assert demo.map_name == "levels\\test\\demo\\demo"
    assert [block[0] for block in demo.blocks] == ["R", "R", "K", "k"]
    assert [block[1] for block in demo.blocks] == [0, 30, 60, 90]
    assert demo.trailing == 0
    ticks = [record for _, kind, record in demo.records("R") if kind == "T"]
    assert len(ticks) == 2 * (3000 // 13)
    output = subprocess.run([sys.executable, str(ROOT / "tools" / "demo_info.py"), str(path)], capture_output=True,
                            text=True, check=True).stdout
    assert "map levels\\test\\demo\\demo" in output
