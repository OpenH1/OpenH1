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
import release_names  # noqa: E402

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
    assert (tree / "bin/openh1").read_bytes() == b"halo"
    assert "lib/openh1/libSDL3.so.0" in names
    assert "share/openh1/brokers.txt" in names
    assert "share/applications/openh1.desktop" in names
    assert "share/icons/hicolor/48x48/apps/openh1.png" in names
    assert "share/licenses/openh1/LICENSE" in names
    assert "share/licenses/openh1/SDL3-LICENSE.txt" in names
    assert "share/doc/openh1/README.md" in names
    # (nothing but the prefix's folders at its top)
    assert {name.split("/")[0] for name in names} == {"bin", "lib", "share"}


def test_tree_without_sdl(build, tmp_path):
    tree = tmp_path / "tree"
    linux_package.stage_tree(tree, build / "halo", None, linux_package.notices("linux"))
    assert not (tree / "lib").exists()


def test_zip_modes(build, tmp_path):
    executables = {"bin/openh1", "lib/openh1/libSDL3.so.0"}
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
    """each updating build's zip is the one its self-updater asks for (UPDATE_NAME[-debug].zip): Windows', the
    x86-64 build's, its VR build's and the 64-bit ARM build's, as tools/ci_build.py names their artifacts,
    tools/release_names.py the release's files, and the workflow publishes them; there is no 32-bit build, nor
    a flat release"""
    updater = (ROOT / "port/linux/src/updater.c").read_text()
    platforms = set(re.findall(r'#define UPDATE_PLATFORM "([^"]+)"', updater))
    assert platforms == {"windows", "linux-x86_64", "linux-x86_64-vr", "linux-arm64"}
    names = re.findall(r'#define UPDATE_PLATFORM "([^"]+)"\n#define UPDATE_NAME "([^"]+)"', updater)
    assert dict(names) == {platform: release_names.NAMES[platform] for platform in platforms}
    assert '#define UPDATE_ASSET_RELEASE UPDATE_NAME ".zip"' in updater
    assert '#define UPDATE_ASSET_DEBUG UPDATE_NAME "-debug.zip"' in updater
    for platform in platforms:
        for config in ("release", "debug"):
            artifact = f"halo-{platform}-{config}"
            assert release_names.published(artifact) == release_names.NAMES[platform] + (
                "-debug" if config == "debug" else "")
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


def test_frame_release_names_agree():
    """the 64-bit ARM VR build is the Steam Frame's package (tools/frame_package.py), which the release has as
    halo-steam-frame-vr-release.zip, as it is: no UPDATE_PLATFORM names it (frame_install.sh updates it, never
    the self-updater), tools/ci_build.py names it, and the workflow builds it on master, uploads it and publishes
    it with the zip's own modes"""
    updater = (ROOT / "port/linux/src/updater.c").read_text()
    platforms = set(re.findall(r'#define UPDATE_PLATFORM "([^"]+)"', updater))
    name = ci_build.dist_name("linux_arm64", "release", True)
    assert name == "halo-steam-frame-vr-release"
    assert name[len("halo-"):-len("-release")] not in platforms
    assert "linux-arm64-vr" not in platforms
    workflow = (ROOT / ".github/workflows/build.yml").read_text()
    assert "run: python3 tools/ci_build.py linux_arm64 release --vr" in workflow
    assert f"name: {name}" in workflow and f"path: dist/{name}" in workflow
    assert 'halo-steam-frame-*) cp "$build/$name.zip" "release/$published.zip"' in workflow
    assert release_names.published(name) == "OpenH1-Steam-Frame"
    assert "/download/OpenH1-Steam-Frame.zip" in workflow


def test_frame_zip(tmp_path):
    """the Steam Frame's zip (frame_package.write_zip): its folder under the package's name, the programs 0755,
    the rest 0644, in name order"""
    import frame_package

    package = tmp_path / frame_package.NAME
    package.mkdir()
    names = ["halo", "libSDL3.so.0", "halo-vr.sh", "brokers.txt", "README.md", "VERSION", "SHA256SUMS"]
    for name in names:
        (package / name).write_bytes(name.encode())
        (package / name).chmod(0o644)
    output = tmp_path / "halo-steam-frame-vr-release.zip"
    frame_package.write_zip(package, output)
    with zipfile.ZipFile(output) as archive:
        assert archive.namelist() == [f"{frame_package.NAME}/"] + sorted(f"{frame_package.NAME}/{name}" for name in names)
        for info in archive.infolist():
            assert info.create_system == 3
            mode = info.external_attr >> 16
            if info.is_dir():
                assert stat.S_ISDIR(mode)
                continue
            assert stat.S_ISREG(mode)
            base = info.filename.split("/", 1)[1]
            expected = 0o755 if base in ("halo", "libSDL3.so.0", "halo-vr.sh") else 0o644
            assert stat.S_IMODE(mode) == expected, info.filename
            assert archive.read(info) == base.encode()


def test_release_names():
    """the names players see: OpenH1-<machine>[-debug][-symbols]; Android's (whose app asks for its old name,
    Updater.java) and anything unknown keep theirs"""
    assert release_names.published("halo-windows-release") == "OpenH1-Windows"
    assert release_names.published("halo-windows-debug-symbols") == "OpenH1-Windows-debug-symbols"
    assert release_names.published("halo-linux-x86_64-release") == "OpenH1-Linux"
    assert release_names.published("halo-macos-release") == "OpenH1"
    assert release_names.published("halo-macos-debug") == "OpenH1-debug"
    assert release_names.published("halo-android-release") == "halo-android-release"
    workflow = (ROOT / ".github/workflows/build.yml").read_text()
    assert 'published="$(python3 tools/release_names.py "$name")"' in workflow
    # (the release job's checkout has only the files it names)
    release_job = workflow[workflow.index("\n  release:"):]
    assert "            tools/release_names.py\n" in release_job[:release_job.index("download-artifact")]
