"""The DXT textures as ASTC (port/linux/astc): the transcoding's exact blocks
and the encoder's bounds, the workers and the disk cache, the setting's
choice (port/linux/tests/astc_test.c), built on this machine with
astc-encoder (port/third_party/astc-encoder) as the Apple and Android hosts
build it (tools/build_common.py's ASTC_* flags; scalar, not NEON, on a
processor without it). No GPU or game data."""
import platform
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from tools.build_common import (ASTC_CXX_FLAGS, ASTC_DIR, ASTC_HOST_CXX_SOURCES, ASTC_HOST_SOURCES,  # noqa: E402
                                astc_encoder_sources)

TEST = ROOT / "port" / "linux" / "tests" / "astc_test.c"


def compilers():
    cc = shutil.which("clang") or shutil.which("cc")
    cxx = shutil.which("clang++") or shutil.which("c++")
    return cc, cxx


def cxx_flags():
    """the hosts' flags, their folders from the repository's root"""
    flags = [flag if not flag.startswith("-I") else f"-I{ROOT / flag[2:]}" for flag in ASTC_CXX_FLAGS]
    if platform.machine().lower() not in ("arm64", "aarch64"):
        flags = [flag.replace("-DASTCENC_NEON=1", "-DASTCENC_NEON=0") for flag in flags]
    return flags


@pytest.fixture(scope="module")
def astc_test(tmp_path_factory):
    cc, cxx = compilers()
    if not cc or not cxx:
        pytest.skip("needs a C and a C++ compiler")
    folder = tmp_path_factory.mktemp("astc")
    objects = []
    flags = cxx_flags()
    jobs = []
    for source in [*astc_encoder_sources(), *ASTC_HOST_CXX_SOURCES]:
        obj = folder / (source.name + ".o")
        jobs.append(subprocess.Popen([cxx, *flags, "-w", f"-I{ROOT / ASTC_DIR}", "-c", str(ROOT / source), "-o",
                                      str(obj)]))
        objects.append(obj)
    for job in jobs:
        assert job.wait() == 0
    for source in [*ASTC_HOST_SOURCES, TEST.relative_to(ROOT)]:
        obj = folder / (source.name + ".o")
        subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-D_GNU_SOURCE", f"-I{ROOT / ASTC_DIR}",
                        "-c", str(ROOT / source), "-o", str(obj)], check=True)
        objects.append(obj)
    binary = folder / "astc_test"
    subprocess.run([cxx, *map(str, objects), "-pthread", "-o", str(binary)], check=True)
    return binary


def run(binary, *arguments):
    result = subprocess.run([str(binary), *map(str, arguments)], capture_output=True, text=True, timeout=600)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout


def test_transcode(astc_test):
    """every DXT1 block but transparent black among two or more colors, and
    DXT3 and DXT5 blocks of one alpha, transcoded within a third of a step;
    the others flagged, and encoded from gradients within bounds (36 dB, DXT1 50)"""
    print(run(astc_test, "transcode"))


def test_cache(astc_test, tmp_path):
    """the disk cache: keyed by the blocks and their format, a damaged file
    encoded and written again, a cancelled job never returned, noise left
    decoded (and kept so), the size bound keeping the most recent files"""
    print(run(astc_test, "cache", tmp_path / "cache"))


def test_choice(astc_test):
    """display.astc_textures: auto where the GPU has ASTC and not BC, always
    where it has ASTC, off never"""
    run(astc_test, "choice")


def test_setting_documented():
    """the setting is the game's (port_config.c, with its values), the
    host's (host_common.c) and the README's"""
    config = (ROOT / "port/linux/src/port_config.c").read_text()
    assert '"display.astc_textures"' in config and '"HALO_ASTC_TEXTURES"' in config
    assert '{ "display.astc_textures", { "auto", "always", "off", NULL }, NULL }' in config
    assert '"display.astc_textures"' in (ROOT / "port/android/host/host_common.c").read_text()
    assert "`display.astc_textures`" in (ROOT / "port/linux/README.md").read_text()
