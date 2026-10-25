"""The texture cache in the game (port/linux/include/halo_port_capacity.h): a mod plays Blood Gulch's level, scenery
and sky bitmaps redrawn at 1024x1024 (4 MB each), and a frame draws more texture than the Xbox's 22 MB cache holds,
as a large Custom Edition map's does (the AArch64 builds of the cache's Xbox size stab it: "YOU GOT STABBED" in
debug.txt, and the default texture drawn). The map takes the cache's larger size, and every texture is drawn.
Negative controls: the same bitmaps at 4096x4096 (64 MB each) draw more than the largest cache holds, and are
stabbed; Blood Gulch without the mod keeps an Xbox map's size.

Needs this computer's build of the game (build/macos/halo on a Mac; build/linux_arm64/halo or build/linux_x86_64/halo
on Linux, with xvfb-run and a Vulkan driver that draws in it, as tools/test_mods.py) and assets/maps/bloodgulch.map.
The bitmaps alike are links to one file, of zeros (a large one's sparse): the mods take some 12 MB of disk."""

import os
import platform
import re
import shutil
import signal
import struct
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from test_mods import TAG_BASE, MapTags, header  # noqa: E402

MAPS = ROOT / "assets" / "maps"
LEVEL = "levels\\test\\bloodgulch\\bloodgulch"
MAC = platform.system() == "Darwin"
ARM64 = platform.machine().lower() in ("aarch64", "arm64")
# (HALO_GAME: another build's executable, as the scorecard's)
GAME = Path(os.environ.get("HALO_GAME") or
            ROOT / "build" / ("macos" if MAC else "linux_arm64" if ARM64 else "linux_x86_64") / "halo")
# the files the game needs beside it
GAME_FILES = ("halo", "libSDL3.0.dylib", "shaders.metallib", "libSDL3.so.0")
# the bitmaps the mod redraws: the map's own folders (the ground, the cliffs,
# the bases), its scenery and its sky, of 2D bitmaps without sprites
FOLDERS = ("levels\\", "scenery\\", "sky\\")
STABBED = "YOU GOT STABBED"
# an Xbox map's cache on the AArch64 guests, which a Custom Edition map's
# or a modded map's is larger than
XBOX_CACHE_MB = 22
# the memory from which those take the whole 256 MB (a 32nd of it), whose
# quarter a 4096x4096 bitmap's 64 MB is: a mod's bitmap may take no more
WHOLE_CACHE_MEMORY = 8 << 30


def memory_bytes() -> int:
    if MAC:
        return int(subprocess.run(["sysctl", "-n", "hw.memsize"], capture_output=True, text=True, check=True).stdout)
    return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")


def redrawn_bitmaps(tags: MapTags) -> dict:
    """the map's bitmap tags the mod redraws, by name: each one's sequences (first bitmap, count) and bitmap count"""
    found = {}
    for group, name, definition in tags.instances.values():
        if group != b"bitm" or not definition or not name.startswith(FOLDERS):
            continue
        root = tags.tags[definition - TAG_BASE:][:0x6C]
        sequences = tags.block(definition, 0x54, 0x40)
        bitmaps = tags.block(definition, 0x60, 0x30)
        if struct.unpack_from("<h", root, 0)[0] != 0 or not bitmaps:  # (2D textures)
            continue
        if any(struct.unpack_from("<h", bitmap, 0xA)[0] != 0 for _, bitmap in bitmaps):  # (all 2D)
            continue
        if any(struct.unpack_from("<i", sequence, 0x34)[0] for _, sequence in sequences):  # (no sprites)
            continue
        found[name] = ([struct.unpack_from("<hh", sequence, 0x20) for _, sequence in sequences], len(bitmaps))
    return found


def write_bitmap(path: Path, sequences: list, count: int, size: int) -> None:
    """a bitmap tag file (bitmap_group.h, as tools/test_mods.py's bitmap_file) of the map's sequences and `count` 2D
    A8R8G8B8 bitmaps of size x size, their pixels zero"""
    pixels_size = size * size * 4
    root = bytearray(0x6C)
    struct.pack_into(">I", root, 0x30, pixels_size * count)  # processed pixel data
    struct.pack_into(">I", root, 0x54, len(sequences))
    struct.pack_into(">I", root, 0x60, count)
    tail = b""
    for index, (first, bitmap_count) in enumerate(sequences):
        sequence = bytearray(0x40)
        sequence[0:8] = (b"seq%d" % index).ljust(8, b"\0")
        struct.pack_into(">hh", sequence, 0x20, first, bitmap_count)
        tail += bytes(sequence)
    for index in range(count):
        bitmap = bytearray(0x30)
        bitmap[0:4] = b"bitm"
        struct.pack_into(">hhhhhH", bitmap, 4, size, size, 1, 0, 11, 1)  # 2D, A8R8G8B8, power of two
        struct.pack_into(">i", bitmap, 0x18, index * pixels_size)
        tail += bytes(bitmap)
    with path.open("wb") as file:
        file.write(header(b"bitm", 7) + bytes(root))
        file.seek(pixels_size * count, os.SEEK_CUR)
        file.write(tail)


def write_mod(data: Path, size: int) -> int:
    """the mod of the redrawn bitmaps at size x size: how many (the files of bitmaps alike are one file's links)"""
    mod = data / "mods" / "big_textures"
    (mod / "tags").mkdir(parents=True)
    (mod / "mod.toml").write_text('name = "Big textures"\nversion = "1"\n', encoding="utf-8")
    bitmaps = redrawn_bitmaps(MapTags(MAPS / "bloodgulch.map"))
    written = {}
    for name, (sequences, count) in bitmaps.items():
        path = mod / "tags" / (name.replace("\\", "/") + ".bitmap")
        path.parent.mkdir(parents=True, exist_ok=True)
        key = (tuple(sequences), count)
        if key in written:
            os.link(written[key], path)
        else:
            write_bitmap(path, sequences, count, size)
            written[key] = path
    return len(bitmaps)


@pytest.fixture(scope="module")
def game(tmp_path_factory) -> Path:
    """the build, copied with what it needs to a folder of its own: its config.toml, which it writes beside itself,
    is the test's"""
    if not GAME.is_file() or not (MAPS / "bloodgulch.map").is_file() or (not MAC and not shutil.which("xvfb-run")):
        pytest.skip(f"needs {GAME},assets/maps/bloodgulch.map{'' if MAC else ' and xvfb-run'}")
    folder = tmp_path_factory.mktemp("game")
    for name in GAME_FILES:
        if (GAME.parent / name).is_file():
            shutil.copy2(GAME.parent / name, folder / name)
    return folder / "halo"


def run_game(game: Path, folder: Path, size: int = 0) -> tuple:
    """Blood Gulch for some seconds, with the mod at size x size (0: none): debug.txt, and the bitmaps redrawn"""
    data, saves, home = folder / "data", folder / "saves", folder / "home"
    for path in (data, saves, home):
        path.mkdir()
    (data / "maps").symlink_to(MAPS.resolve())
    (data / "init.txt").write_text(f"map_name {LEVEL}\n", encoding="ascii")
    count = write_mod(data, size) if size else 0
    # (a window, hidden: debug.exit_after ends a game that has one, and the
    # textures are drawn; on Linux on a display of its own)
    environment = {**os.environ, "HOME": str(home), "HALO_DATA_ROOT": str(data), "HALO_SAVE_ROOT": str(saves),
                   "HALO_EXIT_AFTER": "25", "HALO_HIDDEN_WINDOW": "1", "HALO_DISPLAY_MODE": "windowed",
                   "HALO_WINDOW_SIZE": "640x480", "HALO_RESOLUTION_SCALING": "original", "HALO_VOLUME": "0",
                   "HALO_NET_ONLINE": "0", "HALO_NO_CLIPBOARD": "1"}
    command = [str(game)]
    if not MAC:
        environment["SDL_VIDEO_DRIVER"] = "x11"
        environment.pop("WAYLAND_DISPLAY", None)
        command = ["xvfb-run", "-a", "-s", "-screen 0 640x480x24", *command]
    process = subprocess.Popen(command, cwd=game.parent, env=environment, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, start_new_session=True)
    try:
        output, _ = process.communicate(timeout=180)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        pytest.fail("the game did not exit")
    assert process.returncode == 0, output[-4000:]
    return (data / "debug.txt").read_text(errors="replace"), count


def cache_sizes(log: str) -> list:
    """the sizes the texture cache took for the map (xbox_texture_cache.c, texture_cache_size_for_map)"""
    return [int(size) for size in re.findall(r"texture cache: (\d+) MB for this map", log)]


def test_textures_larger_than_the_xbox_cache_are_drawn(game, tmp_path):
    log, count = run_game(game, tmp_path, 1024)
    assert count * 4 > 2 * XBOX_CACHE_MB, count
    assert f"mods: {count} bitmaps from their files ({count} new), 0 given back, 0 left out" in log, log[-4000:]
    assert STABBED not in log, log[-4000:]
    if MAC or ARM64:
        # (the AArch64 guests', by the memory their host says the computer
        # has: the desktop builds' is as large for every map)
        assert cache_sizes(log)[-1:] == [min(max(memory_bytes() // 32 >> 20, 64), 256)], cache_sizes(log)


def test_more_than_the_cache_holds_is_stabbed(game, tmp_path):
    # (the negative control: the test sees a texture that is not drawn)
    if (MAC or ARM64) and memory_bytes() < WHOLE_CACHE_MEMORY:
        pytest.skip("a 4096x4096 bitmap needs the whole cache, from 8 GB")
    log, count = run_game(game, tmp_path, 4096)
    assert f"mods: {count} bitmaps from their files ({count} new), 0 given back, 0 left out" in log, log[-4000:]
    assert STABBED in log, log[-4000:]


def test_an_xbox_map_keeps_its_size(game, tmp_path):
    # (an Xbox map takes no more of the cache than it did: the AArch64 guests
    # keep the Xbox's 22 MB for it, the desktop builds their 256)
    log, _ = run_game(game, tmp_path)
    assert cache_sizes(log) == [], cache_sizes(log)
    assert STABBED not in log, log[-4000:]
