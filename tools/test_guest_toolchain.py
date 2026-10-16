"""Tests of the guest builds' pinned LLVM (tools/guest_toolchain.py): which
archive each computer takes, what is kept of it and how, and when the
pinned LLVM is not used."""
import hashlib
import io
import sys
import tarfile
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from tools import guest_toolchain  # noqa: E402
from tools.guest_toolchain import LLVM_MAJOR, LLVM_VERSION  # noqa: E402

TOP = f"LLVM-{LLVM_VERSION}-Linux-X64"


def release_archive(path: Path, extra=()):
    """a small stand-in for the release's archive: its top folder, the kept
    tools (clang and ld.lld as links), and what is not kept"""
    with tarfile.open(path, "w:xz") as tar:
        def add(name, data=b"", mode=0o755, link=None):
            info = tarfile.TarInfo(f"{TOP}/{name}")
            if link:
                info.type, info.linkname = tarfile.SYMTYPE, link
            else:
                info.size, info.mode = len(data), mode
            tar.addfile(info, io.BytesIO(data) if not link else None)
        add(f"bin/clang-{LLVM_MAJOR}", b"clang")
        add("bin/clang", link=f"clang-{LLVM_MAJOR}")
        add("bin/lld", b"lld")
        add("bin/ld.lld", link="lld")
        for name in ("llvm-ar", "llvm-objdump", "llvm-profdata", "llvm-symbolizer", "llvm-nm", "llvm-objcopy",
                     "llvm-bolt", "mlir-opt"):
            add(f"bin/{name}", name.encode())
        add(f"lib/clang/{LLVM_MAJOR}/include/stddef.h", b"/* stddef */", 0o644)
        add(f"lib/clang/{LLVM_MAJOR}/lib/linux/libclang_rt.builtins-x86_64.a", b"!<arch>\n", 0o644)
        add("lib/libLLVM.a", b"!<arch>\n", 0o644)
        for item in extra:
            add(*item[:1], **item[1])


def test_each_computer_has_its_archive():
    assert guest_toolchain.llvm_archive("linux", "x86_64")[0] == f"LLVM-{LLVM_VERSION}-Linux-X64.tar.xz"
    assert guest_toolchain.llvm_archive("linux", "AMD64")[0] == f"LLVM-{LLVM_VERSION}-Linux-X64.tar.xz"
    assert guest_toolchain.llvm_archive("linux", "aarch64")[0] == f"LLVM-{LLVM_VERSION}-Linux-ARM64.tar.xz"
    assert guest_toolchain.llvm_archive("linux", "arm64")[0] == f"LLVM-{LLVM_VERSION}-Linux-ARM64.tar.xz"
    assert guest_toolchain.llvm_archive("darwin", "arm64")[0] == f"LLVM-{LLVM_VERSION}-macOS-ARM64.tar.xz"
    assert guest_toolchain.llvm_archive("darwin", "x86_64") is None
    assert guest_toolchain.llvm_archive("linux", "riscv64") is None
    for name, digest in guest_toolchain.LLVM_ARCHIVES.values():
        assert len(digest) == 64 and int(digest, 16) >= 0


def test_unpack_keeps_the_tools_and_their_links(tmp_path):
    archive = tmp_path / "llvm.tar.xz"
    release_archive(archive)
    folder = tmp_path / "llvm"
    folder.mkdir()
    guest_toolchain.unpack(archive, folder)
    kept = sorted(path.relative_to(folder).as_posix() for path in folder.rglob("*") if not path.is_dir())
    assert kept == sorted([*guest_toolchain.KEPT_FILES, f"lib/clang/{LLVM_MAJOR}/include/stddef.h"])
    assert (folder / "bin" / "clang").is_symlink() and (folder / "bin" / "clang").read_bytes() == b"clang"
    assert (folder / "bin" / "ld.lld").is_symlink()
    assert (folder / "bin" / "llvm-ar").stat().st_mode & 0o111
    toolchain = guest_toolchain.toolchain_in(folder)
    assert toolchain.cc == str(folder / "bin" / "clang") and toolchain.ld == str(folder / "bin" / "ld.lld")


def test_unpack_refuses_a_link_out_of_its_folder(tmp_path):
    archive = tmp_path / "llvm.tar.xz"
    release_archive(archive, extra=[("bin/llvm-symbolizer", {"link": "/usr/bin/true"})])
    folder = tmp_path / "llvm"
    folder.mkdir()
    with pytest.raises(RuntimeError, match="links outside"):
        guest_toolchain.unpack(archive, folder)


def test_fetch_downloads_once_and_checks_the_digest(tmp_path, monkeypatch):
    source = tmp_path / "release.tar.xz"
    release_archive(source)
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    monkeypatch.setattr(guest_toolchain, "llvm_archive", lambda: (f"{TOP}.tar.xz", digest))
    downloads = []

    def download(url, expected, destination):
        downloads.append(url)
        assert expected == digest
        destination.write_bytes(source.read_bytes())
    monkeypatch.setattr(guest_toolchain, "_download", download)
    cache = tmp_path / "cache"
    folder = guest_toolchain.fetch_llvm(cache)
    assert folder == cache / TOP and (folder / guest_toolchain.COMPLETE).is_file()
    assert downloads == [f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{LLVM_VERSION}/{TOP}.tar.xz"]
    # (the archive is not kept, nor the folder it was unpacked in)
    assert sorted(path.name for path in cache.iterdir()) == [TOP]
    assert guest_toolchain.fetch_llvm(cache) == folder and len(downloads) == 1


def test_an_interrupted_unpacking_is_not_taken_as_complete(tmp_path, monkeypatch):
    monkeypatch.setattr(guest_toolchain, "llvm_archive", lambda: (f"{TOP}.tar.xz", "0" * 64))
    (tmp_path / TOP / "bin").mkdir(parents=True)

    def download(url, expected, destination):
        destination.write_bytes(b"not an archive")
    monkeypatch.setattr(guest_toolchain, "_download", download)
    with pytest.raises(RuntimeError, match="cannot unpack"):
        guest_toolchain.fetch_llvm(tmp_path)
    assert not (tmp_path / f"{TOP}.tar.xz").exists() and not (tmp_path / f"{TOP}.unpacking").exists()


def test_a_compiler_of_the_build_or_the_system_is_used_as_it_is(tmp_path, monkeypatch):
    monkeypatch.setattr(guest_toolchain, "fetch_llvm", lambda cache: pytest.fail("downloaded"))
    assert guest_toolchain.guest_toolchain(SimpleNamespace(), "/opt/clang") is None
    assert guest_toolchain.guest_toolchain(SimpleNamespace(guest_llvm="system")) is None
    with pytest.raises(RuntimeError, match="no clang"):
        guest_toolchain.guest_toolchain(SimpleNamespace(guest_llvm=str(tmp_path)))


def test_pinned_is_the_default_in_the_cache_asked_for(tmp_path, monkeypatch):
    archive = tmp_path / "llvm.tar.xz"
    release_archive(archive)
    asked = []

    def fetch(cache):
        asked.append(cache)
        folder = tmp_path / "unpacked"
        folder.mkdir(exist_ok=True)
        guest_toolchain.unpack(archive, folder)
        return folder
    monkeypatch.setattr(guest_toolchain, "fetch_llvm", fetch)
    toolchain = guest_toolchain.guest_toolchain(SimpleNamespace(guest_llvm_cache=str(tmp_path / "c")))
    assert toolchain.objdump == str(tmp_path / "unpacked" / "bin" / "llvm-objdump")
    assert asked == [tmp_path / "c"]
    guest_toolchain.guest_toolchain(SimpleNamespace(guest_llvm="pinned"))
    assert asked[-1] == guest_toolchain.default_cache()
