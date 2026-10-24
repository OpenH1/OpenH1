#!/usr/bin/env python3
"""Puts function names and source lines on the crash lines of a Windows
build's halo.log (port/windows/host/host_memory_watch_win.c's crash
report):

    python tools/symbolize_crash.py halo.log path/to/folder

The folder holds the build that crashed: halo.exe with its halo.pdb, and
halo_guest.elf, the guest image (a release's
OpenH1-Windows[-debug]-symbols.zip has halo.pdb and halo_guest.elf; the zip
of the same name without -symbols has halo.exe). Each frame of the report
names its module and the offset in it: halo.exe's are found in the PDB, the
guest image's in the image's own debugging information, by llvm-symbolizer.
The crash reports that reach Sentry need none of this for the host's frames:
Sentry symbolizes them itself.
"""

import argparse
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

# the report's lines with an address in them: the exception (its own
# address) and each frame (a return address, one past its call, but the
# first frame's, the exception's address again)
EXCEPTION = re.compile(r"exception \w+ at [0-9A-Fa-f]+ \((halo\.exe|halo_guest\.elf)\+0x([0-9A-Fa-f]+)")
FRAME = re.compile(r"frame +(\d+): [0-9A-Fa-f]+ (halo\.exe|halo_guest\.elf)\+0x([0-9A-Fa-f]+)")


def elf_base(image: Path) -> int:
    """the guest image's lowest loadable address (its link address, where
    the report's offsets start)"""
    data = image.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1:
        sys.exit(f"{image} is not the guest image (a 32-bit ELF file)")
    phoff, = struct.unpack_from("<I", data, 28)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    addresses = [struct.unpack_from("<I", data, phoff + index * phentsize + 8)[0] for index in range(phnum)
                 if struct.unpack_from("<I", data, phoff + index * phentsize)[0] == 1]
    return min(addresses) & ~0xfff


def symbolize(symbolizer: str, image: Path, addresses: list, relative: bool) -> dict:
    """{address: [lines of llvm-symbolizer's answer]}"""
    if not addresses:
        return {}
    output = subprocess.run(
        [symbolizer, f"--obj={image}", "--inlines", "--functions=linkage"] + (["--relative-address"] if relative else [])
        + [hex(address) for address in addresses],
        capture_output=True, text=True, check=True).stdout
    answers = output.strip("\n").split("\n\n")
    return {address: answer.splitlines() for address, answer in zip(addresses, answers)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("log", type=Path, help="halo.log (or the console's copy)")
    parser.add_argument("folder", type=Path, help="the build that crashed: halo.exe, halo.pdb, halo_guest.elf")
    args = parser.parse_args()

    symbolizer = shutil.which("llvm-symbolizer") or shutil.which("llvm-addr2line")
    if not symbolizer:
        sys.exit("llvm-symbolizer is not in the PATH (it comes with LLVM)")
    executable, image = args.folder / "halo.exe", args.folder / "halo_guest.elf"
    if not (args.folder / "halo.pdb").is_file():
        print(f"warning: no halo.pdb in {args.folder}", file=sys.stderr)
    base = elf_base(image) if image.is_file() else None

    lines = args.log.read_text(encoding="utf-8", errors="replace").splitlines()
    # each line's module and the offset to look up in it
    wanted = {}
    for index, line in enumerate(lines):
        match = EXCEPTION.search(line)
        if match:
            wanted[index] = (match.group(1), int(match.group(2), 16))
            continue
        match = FRAME.search(line)
        if match:
            offset = int(match.group(3), 16)
            wanted[index] = (match.group(2), offset if match.group(1) == "0" else offset - 1)
    if not wanted:
        print(f"{args.log} has no crash lines")
        return 1
    host = symbolize(symbolizer, executable, sorted({o for m, o in wanted.values() if m == "halo.exe"}), True) \
        if executable.is_file() else {}
    guest = symbolize(symbolizer, image, sorted({base + o for m, o in wanted.values() if m == "halo_guest.elf"}), False) \
        if base is not None else {}

    for index, line in enumerate(lines):
        print(line)
        if index not in wanted:
            continue
        module, offset = wanted[index]
        answer = host.get(offset) if module == "halo.exe" else guest.get(base + offset if base is not None else -1)
        if not answer or answer[0] == "??":
            print(f"        (not found in {module})")
            continue
        for function, location in zip(answer[0::2], answer[1::2]):
            print(f"        {function}  {location}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
