#!/usr/bin/env python3
"""The guest image's build steps that are shell commands elsewhere
(tools/android_build.py, generate_guest_image; and the Vulkan host's,
tools/vulkan_build.py), for a computer without a POSIX shell: Windows,
where ninja runs each command as a program of its own (the 64-bit Windows
build, tools/windows_build.py). Each step writes what the shell command
writes.

    guest_build_steps.py alltypes OUTPUT ARCH_ALLTYPES MUSL_ALLTYPES
        musl's bits/alltypes.h (its tools/mkalltypes.sed over both files)
    guest_build_steps.py syscall-h OUTPUT INPUT
        bits/syscall.h: the input, then its __NR_ lines as SYS_ ones
    guest_build_steps.py version-h OUTPUT VERSION
        musl's version.h
    guest_build_steps.py ar ARCHIVER OUTPUT RESPONSE_FILE
        a new archive of the objects the response file names
    guest_build_steps.py copy INPUT OUTPUT
        a copy
    guest_build_steps.py xdk-enums OUTPUT XDK_PDB_H XDK_D3D8_H
        the Vulkan host's xdk_enums.h: xdk_pdb.h's enumerations (from their
        heading to the line before the structures' heading), then
        xdk_d3d8.h's D3DFORMAT, D3DSIZE and D3DTEXTURE defines
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path


def write_if_changed(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.is_file() or path.read_text(encoding="utf-8") != text:
        with open(path, "w", encoding="utf-8", newline="\n") as file:
            file.write(text)


def alltypes(output: Path, inputs: list) -> None:
    """musl's tools/mkalltypes.sed: each TYPEDEF, STRUCT and UNION line
    becomes its guarded definition; the other lines stay as they are"""
    lines = []
    for path in inputs:
        for line in Path(path).read_text(encoding="utf-8").splitlines():
            match = re.match(r"^TYPEDEF (.*) ([^ ]*);$", line)
            if match:
                kind, name = match.groups()
                lines += [f"#if defined(__NEED_{name}) && !defined(__DEFINED_{name})", f"typedef {kind} {name};",
                          f"#define __DEFINED_{name}", "#endif", ""]
                continue
            match = re.match(r"^(STRUCT|UNION) *([^ ]*) (.*);$", line)
            if match:
                tag, name, body = match.groups()
                tag = tag.lower()
                lines += [f"#if defined(__NEED_{tag}_{name}) && !defined(__DEFINED_{tag}_{name})",
                          f"{tag} {name} {body};", f"#define __DEFINED_{tag}_{name}", "#endif", ""]
                continue
            lines.append(line)
    write_if_changed(output, "\n".join(lines) + "\n")


def syscall_h(output: Path, source: Path) -> None:
    text = source.read_text(encoding="utf-8")
    extra = [line.replace("__NR_", "SYS_", 1) for line in text.splitlines() if "__NR_" in line]
    write_if_changed(output, text + ("" if text.endswith("\n") else "\n") + "".join(f"{line}\n" for line in extra))


def archive(archiver: str, output: Path, response: Path) -> None:
    if output.exists():
        output.unlink()
    subprocess.run([archiver, "rcs", str(output), f"@{response}"], check=True)


def xdk_enums(output: Path, pdb: Path, d3d8: Path) -> None:
    lines = []
    inside = False
    for line in pdb.read_text(encoding="utf-8").splitlines():
        if not inside and re.match(r"^/\* ---------- enumerations", line):
            inside = True
        if inside:
            lines.append(line)
            if re.match(r"^/\* ---------- structures and unions", line):
                break
    # (sed '$d': the structures' heading goes)
    if lines:
        lines.pop()
    lines += [line for line in d3d8.read_text(encoding="utf-8").splitlines()
              if re.match(r"^#define D3D(FORMAT|SIZE|TEXTURE)_", line)]
    write_if_changed(output, "".join(f"{line}\n" for line in lines))


def main(arguments: list) -> int:
    if not arguments:
        print(__doc__, file=sys.stderr)
        return 2
    step, rest = arguments[0], arguments[1:]
    if step == "alltypes" and len(rest) == 3:
        alltypes(Path(rest[0]), rest[1:])
    elif step == "syscall-h" and len(rest) == 2:
        syscall_h(Path(rest[0]), Path(rest[1]))
    elif step == "version-h" and len(rest) == 2:
        write_if_changed(Path(rest[0]), f'#define VERSION "{rest[1]}"\n')
    elif step == "ar" and len(rest) == 3:
        archive(rest[0], Path(rest[1]), Path(rest[2]))
    elif step == "copy" and len(rest) == 2:
        Path(rest[1]).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(rest[0], rest[1])
    elif step == "xdk-enums" and len(rest) == 3:
        xdk_enums(Path(rest[0]), Path(rest[1]), Path(rest[2]))
    else:
        print(__doc__, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
