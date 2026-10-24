#!/usr/bin/env python3
"""The second half of a Darwin guest's compile (arm64_32: Android, Linux
arm64, macOS), cached: the Darwin assembly the compiler left converted to
ELF assembly (tools/android_asm_convert.py) and assembled, or the object
the same conversion and assembly made before, from the cache folder.

    guest_asm_cache.py CACHE [--base-register] darwin.s out.s out.o -- ASSEMBLER...

The compile cache (ccache) gives the Darwin assembly back, but cannot cache
past it: without this, every object is converted and assembled again in
every build (60-70 ms each, and 7 s or more for hud_hires_assets). An
object is found by the SHA-256 of everything that makes it: the Darwin
assembly; this script's, the converter's and (with --base-register)
tools/guest_base_register.py's sources; the arguments (the files' names
and the assembler's command among them); and the assembler's
executable (its path, size and time). A cached object is the object the
conversion and assembly would make, byte for byte. On a hit out.s, the
ELF assembly no other step reads, is not written.

Each hit refreshes its entry's time, so that a build can keep only what it
used: the workflow deletes the entries older than its start before it
saves the folder (.github/workflows/build.yml). configure.py
--guest-asm-cache turns it on; without it the build converts and assembles
as before.
"""

import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

import android_asm_convert  # noqa: E402


def executable_identity(command: str) -> bytes:
    path = shutil.which(command)
    if not path:
        return command.encode()
    real = os.path.realpath(path)
    stat = os.stat(real)
    return f"{real}\0{stat.st_size}\0{stat.st_mtime_ns}".encode()


def key(arguments, darwin_s: Path, base_register: bool, assembler) -> str:
    digest = hashlib.sha256()
    sources = [Path(__file__).resolve(), TOOLS / "android_asm_convert.py"]
    if base_register:
        sources.append(TOOLS / "guest_base_register.py")
    for source in sources:
        digest.update(source.read_bytes())
        digest.update(b"\0")
    digest.update("\0".join(arguments).encode())
    digest.update(b"\0")
    digest.update(executable_identity(assembler[0]))
    digest.update(b"\0")
    with open(darwin_s, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    arguments = sys.argv[1:]
    if "--" not in arguments:
        print(__doc__, file=sys.stderr)
        return 2
    split = arguments.index("--")
    files, assembler = arguments[:split], arguments[split + 1:]
    cache = Path(files.pop(0)) if files else None
    base_register = bool(files) and files[0] == "--base-register"
    if base_register:
        files = files[1:]
    if cache is None or len(files) != 3 or not assembler:
        print(__doc__, file=sys.stderr)
        return 2
    darwin_s, elf_s, obj = (Path(f) for f in files)

    entry_key = key(arguments, darwin_s, base_register, assembler)
    entry = cache / entry_key[:2] / f"{entry_key}.o"
    if entry.is_file():
        temporary = obj.with_name(obj.name + ".tmp")
        shutil.copyfile(entry, temporary)
        os.replace(temporary, obj)
        os.utime(entry)
        return 0

    try:
        android_asm_convert.convert_file(darwin_s, elf_s, base_register)
    except android_asm_convert.ConvertError as e:
        print(f"{darwin_s}: {e}", file=sys.stderr)
        return 1
    result = subprocess.run(assembler)
    if result.returncode:
        return result.returncode
    entry.parent.mkdir(parents=True, exist_ok=True)
    temporary = entry.with_name(f"{entry.name}.{os.getpid()}.tmp")
    shutil.copyfile(obj, temporary)
    os.replace(temporary, entry)
    return 0


if __name__ == "__main__":
    sys.exit(main())
