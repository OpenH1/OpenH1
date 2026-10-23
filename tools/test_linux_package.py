"""The Linux release's layout (tools/linux_package.py): the tree that packages copy to /usr, the zips' modes, and
that the game (install_layout.c, updater.c), tools/ci_build.py, the workflow and the desktop entry agree with them.
Needs no build: the files are stand-ins.

    python -m pytest -q tools/test_linux_package.py
"""

import os
import re
import shutil
import stat
import subprocess
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
    assert (tree / "install.sh").read_bytes() == linux_package.INSTALLER.read_bytes()
    # (nothing but the prefix's folders, and the installer, at its top)
    assert {name.split("/")[0] for name in names} == {"bin", "lib", "share", "install.sh"}


def test_tree_without_sdl(build, tmp_path):
    tree = tmp_path / "tree"
    linux_package.stage_tree(tree, build / "halo", None, linux_package.notices("linux"))
    assert not (tree / "lib").exists()


def test_package_tree_has_no_installer(build, tmp_path):
    """the tree for a package's /usr (linux_package.py tree) has no install.sh, which would be /usr/install.sh"""
    tree = tmp_path / "tree"
    linux_package.stage_tree(tree, build / "halo", None, linux_package.notices("linux"), installer=False)
    assert {name.split("/")[0] for name in files(tree)} == {"bin", "share"}
    script = (ROOT / "tools/linux_package.py").read_text()
    assert script.count("installer=False)") == 2


def test_zip_modes(build, tmp_path):
    executables = {"install.sh", "bin/openh1", "lib/openh1/libSDL3.so.0"}
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
        assert "install.sh" in archive.namelist()


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


# ---------- install.sh (tools/linux_install.sh), run from a staged release's tree

# stand-ins for SteamOS's helpers: kdialog answers as the test's environment says, steamos-add-to-steam and the
# game note that they ran; pgrep finds no game running
STAND_INS = {
    "kdialog": """#!/bin/sh
echo "kdialog $*" >> "$HOME/calls.txt"
case " $* " in
*" --yesnocancel "*) exit "${KDIALOG_ANSWER:-2}" ;;
*" --getexistingdirectory "*) [ -n "${KDIALOG_FOLDER:-}" ] && echo "$KDIALOG_FOLDER" && exit 0; exit 1 ;;
*" --getopenfilename "*) [ -n "${KDIALOG_FILE:-}" ] && echo "$KDIALOG_FILE" && exit 0; exit 1 ;;
*" --yesno "*) exit "${KDIALOG_START:-1}" ;;
esac
exit 1
""",
    "steamos-add-to-steam": """#!/bin/sh
echo "steamos-add-to-steam $*" >> "$HOME/calls.txt"
""",
    "pgrep": "#!/bin/sh\nexit 1\n",
}
GAME = "#!/bin/sh\necho \"started in $(pwd)\" >> \"$HOME/calls.txt\"\n"
# what install.sh runs besides them: the only commands on its PATH, so that a computer's own kdialog, konsole or
# steamos-add-to-steam is never reached
COMMANDS = ("basename", "bash", "cat", "chmod", "cmp", "cp", "dirname", "find", "grep", "ln", "ls", "mkdir", "mv", "nohup",
            "printf", "readlink", "rm", "sed", "sh", "sleep", "sort")


def command_folder(folder):
    """a folder of links to COMMANDS, for a PATH of its own"""
    folder.mkdir(exist_ok=True)
    for name in COMMANDS:
        found = shutil.which(name)
        if found and not (folder / name).exists():
            (folder / name).symlink_to(found)
    return folder


@pytest.fixture
def release(tmp_path):
    """an unzipped release (OpenH1-Linux.zip in ~/Downloads), its bin/openh1 a script that notes it started"""
    build = tmp_path / "build"
    build.mkdir()
    (build / "halo").write_text(GAME)
    for name in ("libSDL3.so.0", "SDL3-LICENSE.txt"):
        (build / name).write_bytes(name.encode())
    folder = tmp_path / "home/Downloads/OpenH1-Linux"
    linux_package.stage_tree(folder, build / "halo", build / "libSDL3.so.0", notices(build))
    archive = tmp_path / "OpenH1-Linux.zip"
    linux_package.write_zip(folder, archive)
    shutil.rmtree(folder)
    # (as the file manager unpacks it: with the zip's modes)
    with zipfile.ZipFile(archive) as opened:
        for info in opened.infolist():
            opened.extract(info, folder)
            (folder / info.filename).chmod((info.external_attr >> 16) & 0o777)
    return folder


def run_install(tree, home, tools=("kdialog", "steamos-add-to-steam"), **answers):
    stand_ins = home.parent / "stand-ins"
    stand_ins.mkdir(exist_ok=True)
    for name in set(tools) | {"pgrep"}:
        (stand_ins / name).write_text(STAND_INS[name])
        (stand_ins / name).chmod(0o755)
    for name in set(STAND_INS) - set(tools) - {"pgrep"}:
        (stand_ins / name).unlink(missing_ok=True)
    commands = command_folder(home.parent / "commands")
    environment = {"HOME": str(home), "PATH": f"{stand_ins}:{commands}", "OPENH1_INSTALL_WINDOW": "1",
                   **{key: str(value) for key, value in answers.items()}}
    result = subprocess.run([str(tree / "install.sh")], env=environment, capture_output=True, text=True,
                            stdin=subprocess.DEVNULL)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout


def calls(home):
    path = home / "calls.txt"
    lines = path.read_text().splitlines() if path.exists() else []
    path.unlink(missing_ok=True)
    return lines


def tree_files(folder):
    return {name for name in files(folder) if name.split("/")[0] in ("install.sh", "bin", "lib", "share")}


def desktop_entry(home):
    text = (home / ".local/share/applications/openh1.desktop").read_text()
    return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)


def test_install_sh_installs_the_tree(release, tmp_path):
    """install.sh, opened in the unzipped release, copies the tree to ~/Applications/OpenH1 (its modes as the zip's,
    the self-updater's tree: bin/openh1 beside share/openh1), copies the maps folder the player names, writes the
    menu entry (the tree's, with the installed program, folder and icon) and adds it to Steam"""
    home = tmp_path / "home"
    disc = tmp_path / "disc"
    (disc / "maps").mkdir(parents=True)
    for name in ("a10.map", "ui.map", "loading.tga"):
        (disc / "maps" / name).write_bytes(name.encode())
    (disc / "maps/notes.txt").write_text("not a map")
    output = run_install(release, home, KDIALOG_ANSWER=1, KDIALOG_FOLDER=disc)
    installed = home / "Applications/OpenH1"
    assert tree_files(installed) == tree_files(release)
    for name in tree_files(release):
        assert (installed / name).read_bytes() == (release / name).read_bytes(), name
        expected = 0o755 if name in ("install.sh", "bin/openh1", "lib/openh1/libSDL3.so.0") else 0o644
        assert stat.S_IMODE((installed / name).stat().st_mode) == expected, name
    assert (installed / "share/openh1").is_dir()
    assert not list(installed.rglob("*.new"))
    assert sorted(path.name for path in (installed / "maps").iterdir()) == ["a10.map", "loading.tga", "ui.map"]
    assert not (installed / "maps.partial").exists()
    entry = desktop_entry(home)
    assert entry["Name"] == "OpenH1"
    assert entry["Exec"] == f'"{installed}/bin/openh1"'
    assert entry["Path"] == str(installed)
    assert entry["Icon"] == f"{installed}/share/icons/hicolor/256x256/apps/openh1.png"
    assert Path(entry["Icon"]).is_file()
    tree_entry = dict(line.split("=", 1) for line in linux_package.DESKTOP_ENTRY.read_text().splitlines() if "=" in line)
    assert {key: value for key, value in entry.items() if key not in ("Exec", "Icon", "Path")} == \
        {key: value for key, value in tree_entry.items() if key not in ("Exec", "Icon")}
    assert calls(home) == [
        "kdialog --title OpenH1: your game's maps --yes-label A disc image --no-label A maps folder --yesnocancel "
        + "OpenH1 plays the maps of your own Halo: Combat Evolved for the Xbox.",
        "",
        "Where are they: in a disc image of the game (.iso or .xiso), or in a maps folder copied from its disc?",
        f"kdialog --title OpenH1: your game's maps --getexistingdirectory {home}",
        f"steamos-add-to-steam {home}/.local/share/applications/openh1.desktop",
    ]
    assert "Added to Steam as OpenH1" in output
    assert f"{release} can be deleted" in output


def test_install_sh_again_and_update(release, tmp_path):
    """run again, install.sh changes nothing, asks nothing, and adds no second shortcut once Steam has one; a newer
    release's replaces the files that differ and never the player's (config.toml, maps, debug.txt, the
    self-updater's leftovers), nor touches the saved games"""
    home = tmp_path / "home"
    run_install(release, home, KDIALOG_ANSWER=2)
    installed = home / "Applications/OpenH1"
    calls(home)
    shortcuts = home / ".local/share/Steam/userdata/12345/config/shortcuts.vdf"
    shortcuts.parent.mkdir(parents=True)
    shortcuts.write_bytes(b"\x00shortcuts\x00\x00exe\x00\"" + str(installed / "bin/openh1").encode() + b"\"\x00")
    player = {"config.toml": "[display]\nvsync = false\n", "maps/a10.map": "mine", "debug.txt": "log",
              "bin/openh1.old": "an update's", "update-old.txt": "bin/openh1.old\n"}
    for name, text in player.items():
        (installed / name).parent.mkdir(parents=True, exist_ok=True)
        (installed / name).write_text(text)
    saves = home / ".local/share/halo-linux/z/saved.bin"
    saves.parent.mkdir(parents=True)
    saves.write_text("save")

    output = run_install(release, home)
    assert "Nothing changed" in output
    assert calls(home) == []
    assert "Steam has OpenH1 already" in output

    (release / "bin/openh1").write_text(GAME + "# a newer build\n")
    (release / "share/openh1/brokers.txt").write_text("newer brokers\n")
    output = run_install(release, home)
    assert "  bin/openh1: updated" in output and "  share/openh1/brokers.txt: updated" in output
    assert (installed / "bin/openh1").read_bytes() == (release / "bin/openh1").read_bytes()
    assert stat.S_IMODE((installed / "bin/openh1").stat().st_mode) == 0o755
    assert (installed / "share/openh1/brokers.txt").read_text() == "newer brokers\n"
    for name, text in player.items():
        assert (installed / name).read_text() == text, name
    assert saves.read_text() == "save"
    assert not list(installed.rglob("*.new"))
    assert calls(home) == []


def test_install_sh_without_steamos(release, tmp_path):
    """on a desktop without kdialog or steamos-add-to-steam, install.sh installs, writes the menu entry and says
    what is left: the maps (which the game asks for at its first start) and Steam's shortcut, if Steam is there"""
    home = tmp_path / "home"
    (home / ".local/share/Steam").mkdir(parents=True)
    output = run_install(release, home, tools=())
    assert (home / "Applications/OpenH1/bin/openh1").is_file()
    assert desktop_entry(home)["Name"] == "OpenH1"
    assert "No maps yet: at its first start" in output
    assert "Add a Non-Steam Game to My Library" in output and "Tick OpenH1 in the list" in output
    assert calls(home) == []


def test_install_sh_disc_image(release, tmp_path):
    """a disc image is the game's own to copy from (its first start: sdl_platform.c's platform_offer_game_data,
    which takes an image in the game's folder without a file picker): install.sh links it there, and, if the
    player says so, starts the game in its folder"""
    home = tmp_path / "home"
    image = tmp_path / "Halo.iso"
    image.write_bytes(b"MICROSOFT*XBOX*MEDIA")
    output = run_install(release, home, KDIALOG_ANSWER=0, KDIALOG_FILE=image, KDIALOG_START=0)
    installed = home / "Applications/OpenH1"
    link = installed / "Halo.iso"
    assert link.is_symlink() and os.readlink(link) == str(image)
    assert not (installed / "maps").exists()
    assert f"copies the maps from {image}" in output
    for _ in range(100):
        if (home / "calls.txt").exists() and "started" in (home / "calls.txt").read_text():
            break
        subprocess.run(["sleep", "0.05"])
    assert f"started in {installed}" in calls(home)
    # (once linked, not asked again)
    run_install(release, home, KDIALOG_START=1)
    assert not [line for line in calls(home) if "--yesnocancel" in line]


def test_install_sh_in_place(release, tmp_path):
    """a tree already played (config.toml or maps beside bin/: a release unpacked before install.sh came with it,
    which the self-updater then gave it) stays where it is: install.sh copies nothing, and writes its menu entry"""
    home = tmp_path / "home"
    (release / "maps").mkdir()
    (release / "maps/a10.map").write_text("mine")
    output = run_install(release, home)
    assert not (home / "Applications").exists()
    assert desktop_entry(home)["Exec"] == f'"{release}/bin/openh1"'
    assert "it stays there" in output
    assert calls(home) == [f"steamos-add-to-steam {home}/.local/share/applications/openh1.desktop"]


def test_install_sh_refuses_another_folder(release, tmp_path):
    """~/Applications/OpenH1 holding something else is left alone"""
    home = tmp_path / "home"
    (home / "Applications/OpenH1").mkdir(parents=True)
    (home / "Applications/OpenH1/something.txt").write_text("not the game's")
    commands = command_folder(tmp_path / "commands")
    result = subprocess.run([str(release / "install.sh")], capture_output=True, text=True,
                            env={"HOME": str(home), "PATH": str(commands), "OPENH1_INSTALL_WINDOW": "1"})
    assert result.returncode == 1 and "holds no OpenH1" in result.stderr
    assert sorted(path.name for path in (home / "Applications/OpenH1").iterdir()) == ["something.txt"]


def test_install_sh_refuses_while_running(release, tmp_path):
    """while the game runs (pgrep -x openh1), install.sh copies nothing"""
    home = tmp_path / "home"
    stand_ins = tmp_path / "running"
    stand_ins.mkdir()
    (stand_ins / "pgrep").write_text("#!/bin/sh\nprintf '41\\n42\\n'\n")
    (stand_ins / "pgrep").chmod(0o755)
    commands = command_folder(tmp_path / "commands")
    result = subprocess.run([str(release / "install.sh")], capture_output=True, text=True,
                            env={"HOME": str(home), "PATH": f"{stand_ins}:{commands}", "OPENH1_INSTALL_WINDOW": "1"})
    assert result.returncode == 3 and "OpenH1 is running (process 41 42)" in result.stderr
    assert not (home / "Applications/OpenH1/bin").exists()


def test_install_sh_shell():
    """bash -n, and shellcheck where there is one"""
    subprocess.run(["bash", "-n", str(linux_package.INSTALLER)], check=True)
    if shutil.which("shellcheck"):
        subprocess.run(["shellcheck", str(linux_package.INSTALLER)], check=True)
