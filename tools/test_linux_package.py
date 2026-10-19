"""The Linux release's layout (tools/linux_package.py): the tree that packages copy to /usr, the zips' modes, and
that the game (install_layout.c, updater.c), tools/ci_build.py, the workflow and the desktop entry agree with them.
Needs no build: the files are stand-ins.

    python -m pytest -q tools/test_linux_package.py
"""

import re
import stat
import sys
import zipfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ci_build  # noqa: E402
import linux_package  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


@pytest.fixture
def build(tmp_path):
    """stand-ins for the build's executable, SDL and SDL's licence"""
    folder = tmp_path / "build"
    folder.mkdir()
    for name in ("halo", "libSDL3.so.0", "SDL3-LICENSE.txt"):
        (folder / name).write_bytes(name.encode())
    return folder


def notices(build):
    return linux_package.notices("linux") + [(build / "SDL3-LICENSE.txt", "SDL3-LICENSE.txt")]


def files(folder):
    return sorted(path.relative_to(folder).as_posix() for path in folder.rglob("*") if path.is_file())


def test_tree(build, tmp_path):
    tree = tmp_path / "tree"
    linux_package.stage_tree(tree, build / "halo", build / "libSDL3.so.0", notices(build))
    names = files(tree)
    assert (tree / "bin/opence").read_bytes() == b"halo"
    assert "lib/opence/libSDL3.so.0" in names
    assert "share/opence/brokers.txt" in names
    assert "share/applications/opence.desktop" in names
    assert "share/icons/hicolor/48x48/apps/opence.png" in names
    assert "share/licenses/opence/LICENSE.md" in names
    assert "share/licenses/opence/SDL3-LICENSE.txt" in names
    assert "share/doc/opence/README.md" in names
    # (nothing but the prefix's folders at its top)
    assert {name.split("/")[0] for name in names} == {"bin", "lib", "share"}


def test_tree_without_sdl(build, tmp_path):
    tree = tmp_path / "tree"
    linux_package.stage_tree(tree, build / "halo", None, linux_package.notices("linux"))
    assert not (tree / "lib").exists()


def test_zip_modes(build, tmp_path):
    executables = {"bin/opence", "lib/opence/libSDL3.so.0"}
    folder = tmp_path / "tree"
    # (as GitHub's artifacts give them: no file executable)
    linux_package.stage_tree(folder, build / "halo", build / "libSDL3.so.0", notices(build))
    for path in folder.rglob("*"):
        if path.is_file():
            path.chmod(0o644)
    output = tmp_path / "tree.zip"
    linux_package.write_zip(folder, output)
    with zipfile.ZipFile(output) as archive:
        for info in archive.infolist():
            assert info.create_system == 3
            mode = info.external_attr >> 16
            if info.is_dir():
                assert stat.S_ISDIR(mode)
                continue
            assert stat.S_ISREG(mode)
            expected = 0o755 if info.filename in executables else 0o644
            assert stat.S_IMODE(mode) == expected, info.filename
        assert archive.namelist() == sorted(archive.namelist())


def test_tree_fits_the_updater(build, tmp_path):
    """the self-updater unpacks at most MAXIMUM_UPDATE_FILES files (updater.c)"""
    tree = tmp_path / "tree"
    linux_package.stage_tree(tree, build / "halo", build / "libSDL3.so.0", notices(build))
    updater = (ROOT / "port/linux/src/updater.c").read_text()
    maximum = int(re.search(r"#define MAXIMUM_UPDATE_FILES (\d+)", updater).group(1))
    assert len(files(tree)) <= maximum


def test_names_agree():
    """the executable's, the folders' and the desktop entry's name are install_layout.h's"""
    header = (ROOT / "port/linux/src/install_layout.h").read_text()
    assert re.search(r'#define INSTALL_NAME "([^"]+)"', header).group(1) == linux_package.NAME
    entry = dict(line.split("=", 1) for line in linux_package.DESKTOP_ENTRY.read_text().splitlines() if "=" in line)
    assert entry["Exec"] == linux_package.NAME
    assert entry["Icon"] == linux_package.NAME
    assert entry["StartupWMClass"] == linux_package.NAME
    assert sorted(linux_package.ICONS.glob(f"{linux_package.NAME}-48.png"))


def test_asset_names_agree():
    """each Linux build's zip is the one its self-updater asks for (halo-<UPDATE_PLATFORM>-<flavor>.zip): the
    x86-64 build's, its VR build's and the 64-bit ARM build's, as tools/ci_build.py names them and the workflow
    publishes them; there is no 32-bit build, nor a flat release"""
    updater = (ROOT / "port/linux/src/updater.c").read_text()
    platforms = set(re.findall(r'#define UPDATE_PLATFORM "([^"]+)"', updater))
    assert platforms == {"windows", "linux-x86_64", "linux-x86_64-vr", "linux-arm64"}
    assert '#define UPDATE_ASSET "halo-" UPDATE_PLATFORM "-" HALO_BUILD_FLAVOR ".zip"' in updater
    workflow = (ROOT / ".github/workflows/build.yml").read_text()
    for platform, vr in (("linux_x86_64", False), ("linux_x86_64", True), ("linux_arm64", False)):
        for config in ("debug", "release"):
            name = ci_build.dist_name(platform, config, vr)
            assert name == f"halo-{platform.replace('_', '-', 1)}{'-vr' if vr else ''}-{config}"
            assert name[len("halo-"):-len(f"-{config}")] in platforms
            if config == "release" or not vr:
                assert f"name: {name}" in workflow and f"path: dist/{name}" in workflow, name
    assert "i686" not in workflow and "halo-linux-debug" not in workflow and "halo-linux-release" not in workflow
    assert not hasattr(linux_package, "stage_flat")
