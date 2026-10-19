"""The guest builds' compiler (configure.py --guest-llvm): LLVM's own release
of one pinned version, the same on every computer that builds a guest.

The guest images (tools/android_build.py, generate_guest_image: Android,
Linux arm64 and x86-64, macOS) are compiled by clang for its arm64_32 or x32
target, linked by ld.lld and checked with llvm-objdump. The system's clang
was too old on some computers for the profiles in pgo/ (Xcode's), and
another's lld did not match its LLVM (Homebrew's), so by default the guest
builds download LLVM's release archive for this computer
(github.com/llvm/llvm-project/releases), check its SHA-256 and keep the
tools they use, once, in a folder outside the build folder: the user's
cache (~/.cache/openh1, or ~/Library/Caches/openh1 on macOS) or
--guest-llvm-cache's. The hosts keep the system's compiler.

The release's Linux tools need only the C and C++ runtime libraries and
zlib, which every Linux computer that builds the hosts has, and one more:
lld is linked to libxml2.so.2, for lld-link's (COFF) manifests, which the
guests' links never use and which some Linux distributions no longer have
(Arch's is libxml2.so.16). Its lib/ folder gets a stand-in for it
(tools/library_stand_in.py), which the tools load before the system's: so
the same tools run on every Linux computer (with glibc 2.34 or later, the
release's own need).

--guest-llvm=system uses clang and LLVM's tools on the PATH instead (and the
NDK's for Android, Homebrew's for macOS), and --guest-llvm=FOLDER the
LLVM installation in FOLDER (its bin/). A build's own compiler
(--linux-arm64-cc, --linux-x86-64-cc, --android-guest-cc, --macos-cc,
--macos-guest-cc) is used as it is, as with --guest-llvm=system.

Run as a script, it fetches the pinned LLVM and prints its bin/ folder, for
the guests' tests (tools/test_guest_image_check.py and others take clang,
ld.lld and LLVM's tools from the PATH):

    PATH="$(python3 tools/guest_toolchain.py)":$PATH python3 -m pytest ...
"""

import argparse
import hashlib
import http.client
import os
import platform
import shutil
import sys
import tarfile
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Dict, List, Optional, Tuple

if __package__:
    from . import library_stand_in
else:  # (run as a script)
    import library_stand_in

LLVM_VERSION = "22.1.8"
LLVM_MAJOR = LLVM_VERSION.split(".")[0]
LLVM_URL = "https://github.com/llvm/llvm-project/releases/download/llvmorg-{version}/{name}"
# each computer's archive (sys.platform, platform.machine()) and its SHA-256,
# as the release lists them
LLVM_ARCHIVES: Dict[Tuple[str, str], Tuple[str, str]] = {
    ("linux", "x86_64"): (f"LLVM-{LLVM_VERSION}-Linux-X64.tar.xz",
                          "df0e1ecf16caf3489a272a5eea4eec9b0d82878f6477fa309504f918a0006384"),
    ("linux", "aarch64"): (f"LLVM-{LLVM_VERSION}-Linux-ARM64.tar.xz",
                           "805efad2bb91cb4967fa569e0881d10c0f69c04461cf671cccbae19f547acc34"),
    ("darwin", "arm64"): (f"LLVM-{LLVM_VERSION}-macOS-ARM64.tar.xz",
                          "f260f4f7c0d430828a81ae8a3826a1d63fc0963ec2459489308cc23b1f7eab4f"),
}
MACHINE_NAMES = {"amd64": "x86_64", "arm64": "aarch64"}
# what the guest builds use of the archive (a few hundred MB of its several
# GB): the compiler, the linker, the archiver, the disassembler, the
# profiles' tool (tools/pgo_train.py) and the symbolizer (crash addresses in
# the guest image), with what they link to, and clang's own headers; and
# what the guests' tests use besides (tools/test_host_units.py)
KEPT_FILES = (
    f"bin/clang-{LLVM_MAJOR}", "bin/clang", "bin/lld", "bin/ld.lld", "bin/llvm-ar", "bin/llvm-objdump",
    "bin/llvm-profdata", "bin/llvm-symbolizer", "bin/llvm-nm", "bin/llvm-objcopy",
)
KEPT_FOLDERS = (f"lib/clang/{LLVM_MAJOR}/include/",)
# the libraries the kept tools are linked to and do not use, which a
# stand-in in lib/ replaces (tools/library_stand_in.py)
STAND_INS = {
    "libxml2.so.2": "openh1: this LLVM's libxml2 is a stand-in (tools/guest_toolchain.py); "
                    "lld-link's manifests are not supported",
}
# what a complete folder has, last of all (written once the rest is there)
COMPLETE = "openh1-complete"
TRIES = 3
RETRY_SECONDS = 2


@dataclass(frozen=True)
class GuestToolchain:
    """the tools that build a guest image"""
    cc: str
    ld: str
    ar: str
    objdump: str


def llvm_archive(system: Optional[str] = None, machine: Optional[str] = None) -> Optional[Tuple[str, str]]:
    """this computer's release archive and its SHA-256, or None if LLVM has
    none for it"""
    system = system or ("darwin" if sys.platform == "darwin" else sys.platform)
    machine = (machine or platform.machine()).lower()
    if system == "darwin":
        machine = "arm64" if machine in ("arm64", "aarch64") else machine
    else:
        machine = MACHINE_NAMES.get(machine, machine)
    return LLVM_ARCHIVES.get((system, machine))


def default_cache() -> Path:
    """the user's cache folder for the downloaded toolchain"""
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Caches" / "openh1"
    if os.name == "nt":
        return Path(os.environ.get("LOCALAPPDATA") or Path.home() / "AppData" / "Local") / "openh1"
    return Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache") / "openh1"


def toolchain_in(folder: Path) -> GuestToolchain:
    """the tools of the LLVM installation in folder (its bin/)"""
    bin_dir = folder / "bin"
    missing = [name for name in ("clang", "ld.lld", "llvm-ar", "llvm-objdump") if not (bin_dir / name).exists()]
    if missing:
        raise RuntimeError(f"no {', '.join(missing)} in {bin_dir}")
    return GuestToolchain(cc=str(bin_dir / "clang"), ld=str(bin_dir / "ld.lld"), ar=str(bin_dir / "llvm-ar"),
                          objdump=str(bin_dir / "llvm-objdump"))


def _kept(name: str) -> bool:
    return name in KEPT_FILES or any(name.startswith(folder) for folder in KEPT_FOLDERS)


def _download(url: str, digest: str, destination: Path) -> None:
    """url into destination, checked against digest as it arrives"""
    errors = []
    partial = destination.with_name(destination.name + ".part")
    for attempt in range(1, TRIES + 1):
        if attempt > 1:
            time.sleep(RETRY_SECONDS * (attempt - 1))
        sha256 = hashlib.sha256()
        try:
            with urllib.request.urlopen(url, timeout=60) as response, open(partial, "wb") as out:
                while True:
                    chunk = response.read(1 << 20)
                    if not chunk:
                        break
                    sha256.update(chunk)
                    out.write(chunk)
        # (a connection cut short is http.client.IncompleteRead, not an
        # OSError)
        except (OSError, http.client.HTTPException) as error:
            errors.append(f"{url}: {error!r}")
            if isinstance(error, urllib.error.HTTPError) and error.code == 404:
                break
            continue
        if sha256.hexdigest() == digest:
            partial.replace(destination)
            return
        errors.append(f"{url}: SHA-256 {sha256.hexdigest()}, expected {digest}")
    partial.unlink(missing_ok=True)
    raise RuntimeError("cannot download " + "; ".join(errors))


def unpack(archive: Path, folder: Path) -> None:
    """the kept tools of the release archive into folder (the archive's
    top folder left out), read as a stream: its links as links, nothing
    outside folder"""
    with tarfile.open(archive, mode="r|*") as tar:
        for member in tar:
            parts = PurePosixPath(member.name).parts
            name = "/".join(parts[1:])
            if not name or ".." in parts or member.name.startswith("/") or not _kept(name):
                continue
            target = folder / name
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.issym():
                # (clang -> clang-22, ld.lld -> lld: a name in the same folder)
                if "/" in member.linkname or member.linkname in ("", ".", ".."):
                    raise RuntimeError(f"{archive.name}: {member.name} links outside its folder")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.unlink(missing_ok=True)
                target.symlink_to(member.linkname)
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                source = tar.extractfile(member)
                assert source is not None
                with open(target, "wb") as out:
                    shutil.copyfileobj(source, out, 1 << 20)
                target.chmod(0o755 if member.mode & 0o111 else 0o644)


def write_stand_ins(folder: Path) -> List[Path]:
    """the stand-ins (STAND_INS) that the kept tools of folder need and its
    lib/ lacks, written; the libraries written"""
    imports: Dict[str, set] = {}
    machine = 0
    for tool in sorted((folder / "bin").iterdir()):
        if tool.is_symlink() or not tool.is_file():
            continue
        found = library_stand_in.read_dependencies(tool)
        if found is None:
            continue
        for library in STAND_INS:
            if library in found.needed:
                if "$ORIGIN/../lib" not in found.runpath:
                    raise RuntimeError(f"{tool} does not look for {library} in its lib/ folder")
                imports.setdefault(library, set()).update(found.imports.get(library, ()))
                machine = found.machine
    written = []
    for library, symbols in sorted(imports.items()):
        target = folder / "lib" / library
        data = library_stand_in.stand_in(library, machine, sorted(symbols, key=str), STAND_INS[library])
        if target.is_file() and target.read_bytes() == data:
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        partial = target.with_name(target.name + ".part")
        partial.write_bytes(data)
        partial.chmod(0o755)
        partial.replace(target)
        written.append(target)
    return written


def fetch_llvm(cache: Path) -> Path:
    """the pinned LLVM's folder in cache, downloaded and unpacked the first
    time (configure time); raises RuntimeError if it cannot be"""
    found = llvm_archive()
    if found is None:
        raise RuntimeError(f"LLVM {LLVM_VERSION} has no release for {sys.platform} on {platform.machine()}: "
                           "configure with --guest-llvm=system or a compiler of the build's own")
    name, digest = found
    folder = cache / name.replace(".tar.xz", "")
    if (folder / COMPLETE).is_file():
        # (a folder unpacked before the stand-ins gets them now)
        write_stand_ins(folder)
        return folder
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / name
    print(f"Downloading LLVM {LLVM_VERSION} for the guest builds ({name}, about 1.5 GB, once) into {cache}",
          flush=True)
    _download(LLVM_URL.format(version=LLVM_VERSION, name=name), digest, archive)
    # (unpacked beside it and then moved, so that an interrupted unpacking
    # leaves no folder that a later configure would take as complete)
    unpacking = folder.with_name(folder.name + ".unpacking")
    shutil.rmtree(unpacking, ignore_errors=True)
    shutil.rmtree(folder, ignore_errors=True)
    unpacking.mkdir()
    try:
        unpack(archive, unpacking)
        write_stand_ins(unpacking)
        toolchain_in(unpacking)
        (unpacking / COMPLETE).write_text(f"{name} {digest}\n", encoding="utf-8")
        unpacking.rename(folder)
    except (OSError, tarfile.TarError) as error:
        raise RuntimeError(f"cannot unpack {archive} ({error})") from error
    finally:
        archive.unlink(missing_ok=True)
        shutil.rmtree(unpacking, ignore_errors=True)
    return folder


def guest_toolchain(sln: Any, explicit_cc: Optional[str] = None) -> Optional[GuestToolchain]:
    """the guest's tools by configure.py's settings: the pinned LLVM (the
    default), an installation's (--guest-llvm=FOLDER), or None for the
    build's own choice (a compiler of its own, or --guest-llvm=system)"""
    if explicit_cc:
        return None
    choice = getattr(sln, "guest_llvm", None) or "pinned"
    if choice == "system":
        return None
    if choice != "pinned":
        return toolchain_in(Path(choice))
    cache = getattr(sln, "guest_llvm_cache", None)
    return toolchain_in(fetch_llvm(Path(cache) if cache else default_cache()))


def main() -> int:
    parser = argparse.ArgumentParser(description="fetch the guests' pinned LLVM and print its bin/ folder")
    parser.add_argument("--cache", type=Path, default=default_cache(),
                        help="the folder it is kept in (configure.py --guest-llvm-cache; default: %(default)s)")
    args = parser.parse_args()
    try:
        folder = fetch_llvm(args.cache)
    except RuntimeError as error:
        print(error, file=sys.stderr)
        return 1
    print(folder.resolve() / "bin")
    return 0


if __name__ == "__main__":
    sys.exit(main())
