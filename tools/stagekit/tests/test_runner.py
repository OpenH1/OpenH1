"""The runner's own (runner.py): runs side by side each find their libraries in their own folder."""

import concurrent.futures
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
import runner

# a library, and a program that says where it loaded it from (beside itself: its rpath, as the game's for its SDL)
LIBRARY = "int library_value(void) { return 42; }\n"
PROGRAM = """#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
int library_value(void);
int main(void) {
    Dl_info info;
    if (!dladdr((void *)library_value, &info) || library_value() != 42)
        return 1;
    printf("%s\\n", info.dli_fname);
    return 0;
}
"""
FOLDERS = 24


def built(folder):
    compiler = shutil.which("cc") or shutil.which("clang")
    if not compiler:
        pytest.skip("needs a C compiler")
    (folder / "library.c").write_text(LIBRARY)
    (folder / "program.c").write_text(PROGRAM)
    if sys.platform == "darwin":
        library = folder / "libplace.dylib"
        subprocess.run([compiler, "-dynamiclib", "-install_name", "@rpath/libplace.dylib", "-o", library,
                        folder / "library.c"], check=True)
        rpath = "@executable_path"
    else:
        library = folder / "libplace.so"
        subprocess.run([compiler, "-shared", "-fPIC", "-o", library, folder / "library.c"], check=True)
        rpath = "$ORIGIN"
    program = folder / "program"
    subprocess.run([compiler, "-o", program, folder / "program.c", f"-L{folder}", "-lplace", f"-Wl,-rpath,{rpath}",
                    *([] if sys.platform == "darwin" else ["-ldl"])], check=True)
    return program, library


def test_runs_side_by_side_load_their_own_libraries(tmp_path):
    """each run's program and library placed in its own folder, as launch does the game's, then all of them run at
    once, twice: each loads its library from its own folder (with hard links, macOS's dyld finds @executable_path
    in whichever folder the file was last looked up by)"""
    program, library = built(tmp_path)
    folders = [tmp_path / f"run{number}" for number in range(FOLDERS)]
    for folder in folders:
        folder.mkdir()
        runner.place(program, folder / program.name)
        runner.place(library, folder / library.name)

    def loaded(folder):
        return subprocess.run([str(folder / program.name)], capture_output=True, text=True).stdout.strip()

    with concurrent.futures.ThreadPoolExecutor(FOLDERS) as pool:
        for _ in range(2):
            found = [Path(path).resolve() if path else None for path in pool.map(loaded, folders)]
            assert found == [(folder / library.name).resolve() for folder in folders]


def test_place_replaces_whole(tmp_path):
    """placed over an older file, a file is the new one, and nothing is left beside it"""
    (tmp_path / "new").write_text("new")
    (tmp_path / "target").write_text("old")
    runner.place(tmp_path / "new", tmp_path / "target")
    assert (tmp_path / "target").read_text() == "new"
    assert sorted(path.name for path in tmp_path.iterdir()) == ["new", "target"]
