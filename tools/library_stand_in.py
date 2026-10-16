"""Stand-ins for shared libraries that the guests' pinned LLVM links to and
does not use (tools/guest_toolchain.py).

LLVM's Linux release builds lld with libxml2, which only lld-link (COFF)
uses, to merge Windows manifests; the guests' links are ELF (and Mach-O on
macOS) and never call it. Its tools find their libraries in their lib/
folder first (RUNPATH $ORIGIN/../lib), so a library written there is the
one they load, on every computer: the one the release was built against
(libxml2.so.2) is no longer installed by every Linux distribution (Arch's
is libxml2.so.16), and without it lld does not start.

A stand-in is a small ELF shared library with the library's name (soname)
that defines what the tools import from it, each symbol with the version
it is asked for, read from the tools themselves: so the dynamic loader
finds everything it binds at start-up (lld is linked with BIND_NOW). Its
functions write a message and end the process with status 1; its objects
(libxml2's xmlFree: its allocator's function pointers) point to that
function. It is written from nothing but this file: no compiler or
download, and the same bytes on every computer of a processor.
"""

import struct
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Dict, List, Optional, Sequence, Tuple

EM_X86_64 = 62
EM_AARCH64 = 183
ELF_MAGIC = b"\x7fELF"
PAGE = 0x10000  # the largest page of either processor (AArch64's 64 KB)

SHT_PROGBITS, SHT_STRTAB, SHT_RELA, SHT_HASH, SHT_DYNAMIC, SHT_DYNSYM = 1, 3, 4, 5, 6, 11
SHT_GNU_VERDEF, SHT_GNU_VERNEED, SHT_GNU_VERSYM = 0x6ffffffd, 0x6ffffffe, 0x6fffffff
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4
STT_OBJECT, STT_FUNC = 1, 2
STB_GLOBAL = 1
DT_NULL, DT_NEEDED, DT_HASH, DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ, DT_RELAENT = 0, 1, 4, 5, 6, 7, 8, 9
DT_STRSZ, DT_SYMENT, DT_SONAME, DT_RUNPATH = 10, 11, 14, 29
DT_VERSYM, DT_VERDEF, DT_VERDEFNUM = 0x6ffffff0, 0x6ffffffc, 0x6ffffffd
PT_LOAD, PT_DYNAMIC, PT_GNU_STACK = 1, 2, 0x6474e551
PF_X, PF_W, PF_R = 1, 2, 4
VER_FLG_BASE = 1
RELATIVE = {EM_X86_64: 8, EM_AARCH64: 1027}


@dataclass(frozen=True)
class Import:
    """a symbol a program takes from a library, with its version"""
    name: str
    version: str
    function: bool


@dataclass(frozen=True)
class ElfDependencies:
    """what an ELF program asks of the dynamic loader"""
    machine: int
    needed: Tuple[str, ...]
    runpath: Tuple[str, ...]
    imports: Dict[str, Tuple[Import, ...]]  # each needed library's versioned imports


def _cstring(data: bytes, offset: int) -> str:
    return data[offset:data.index(b"\0", offset)].decode("ascii")


def _read(file: BinaryIO, offset: int, size: int) -> bytes:
    file.seek(offset)
    data = file.read(size)
    if len(data) != size:
        raise ValueError("truncated ELF file")
    return data


def read_dependencies(path: Path) -> Optional[ElfDependencies]:
    """path's needed libraries, run path and versioned imports, or None if
    it is not a 64-bit little-endian ELF file (a Mach-O tool, a script)"""
    with open(path, "rb") as file:
        ident = file.read(64)
        if len(ident) < 64 or ident[:4] != ELF_MAGIC or ident[4] != 2 or ident[5] != 1:
            return None
        machine, = struct.unpack_from("<H", ident, 18)
        shoff, = struct.unpack_from("<Q", ident, 40)
        shentsize, shnum = struct.unpack_from("<HH", ident, 58)
        sections = [struct.unpack("<IIQQQQIIQQ", _read(file, shoff + index * shentsize, 64))
                    for index in range(shnum)]

        def contents(section) -> bytes:
            return _read(file, section[4], section[5])

        def find(kind: int):
            return next((section for section in sections if section[1] == kind), None)
        dynamic, dynsym = find(SHT_DYNAMIC), find(SHT_DYNSYM)
        if dynamic is None or dynsym is None:
            return ElfDependencies(machine, (), (), {})
        strings = contents(sections[dynamic[6]])
        needed, runpath = [], []
        entries = contents(dynamic)
        for offset in range(0, len(entries), 16):
            tag, value = struct.unpack_from("<qQ", entries, offset)
            if tag == DT_NEEDED:
                needed.append(_cstring(strings, value))
            elif tag == DT_RUNPATH:
                runpath.extend(_cstring(strings, value).split(":"))
        # which library each version index is asked of (.gnu.version_r)
        versions: Dict[int, Tuple[str, str]] = {}
        verneed = find(SHT_GNU_VERNEED)
        if verneed is not None:
            data = contents(verneed)
            offset = 0
            for _ in range(verneed[7]):
                _, count, file_name, aux, following = struct.unpack_from("<HHIII", data, offset)
                at = offset + aux
                for _ in range(count):
                    _, _, other, name, aux_next = struct.unpack_from("<IHHII", data, at)
                    versions[other] = (_cstring(strings, file_name), _cstring(strings, name))
                    at += aux_next
                offset += following
        versym = find(SHT_GNU_VERSYM)
        symbol_versions = contents(versym) if versym is not None else b""
        symbols = contents(dynsym)
        imports: Dict[str, List[Import]] = {}
        for index in range(len(symbols) // 24):
            name, info, _, shndx, _, _ = struct.unpack_from("<IBBHQQ", symbols, index * 24)
            if shndx != 0 or not name or not symbol_versions:
                continue
            version = struct.unpack_from("<H", symbol_versions, index * 2)[0] & 0x7fff
            if version in versions:
                library, version_name = versions[version]
                imports.setdefault(library, []).append(
                    Import(_cstring(strings, name), version_name, info & 0xf != STT_OBJECT))
        return ElfDependencies(machine, tuple(needed), tuple(runpath),
                               {library: tuple(found) for library, found in imports.items()})


def _elf_hash(name: bytes) -> int:
    value = 0
    for byte in name:
        value = (value << 4) + byte
        high = value & 0xf0000000
        if high:
            value ^= high >> 24
        value &= ~high & 0xffffffff
    return value


def _align(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def _code(machine: int, message_offset: int, message_size: int) -> bytes:
    """write(2, message, size); exit_group(1), message_offset from the
    code's start"""
    if machine == EM_X86_64:
        return (b"\x48\x8d\x35" + struct.pack("<i", message_offset - 7)   # lea rsi, [rip + message]
                + b"\xba" + struct.pack("<I", message_size)               # mov edx, size
                + b"\xbf\x02\x00\x00\x00"                                 # mov edi, 2
                + b"\xb8\x01\x00\x00\x00"                                 # mov eax, 1 (write)
                + b"\x0f\x05"                                             # syscall
                + b"\xbf\x01\x00\x00\x00"                                 # mov edi, 1
                + b"\xb8\xe7\x00\x00\x00"                                 # mov eax, 231 (exit_group)
                + b"\x0f\x05"                                             # syscall
                + b"\x0f\x0b")                                            # ud2
    if machine == EM_AARCH64:
        def movz(register: int, value: int) -> int:
            return 0xd2800000 | value << 5 | register
        adr = 0x10000000 | (message_offset & 3) << 29 | (message_offset >> 2 & 0x7ffff) << 5 | 1
        return struct.pack("<9I",
                           adr,                       # adr x1, message
                           movz(2, message_size),     # mov x2, size
                           movz(0, 2),                # mov x0, 2
                           movz(8, 64),               # mov x8, 64 (write)
                           0xd4000001,                # svc 0
                           movz(0, 1),                # mov x0, 1
                           movz(8, 94),               # mov x8, 94 (exit_group)
                           0xd4000001,                # svc 0
                           0xd4200000)                # brk 0
    raise ValueError(f"no stand-in code for ELF machine {machine}")


CODE_SIZE = {EM_X86_64: 38, EM_AARCH64: 36}


def stand_in(soname: str, machine: int, imports: Sequence[Import], message: str) -> bytes:
    """an ELF shared library named soname for machine that defines
    imports, each at its version; its functions write message to the
    standard error and exit with status 1"""
    if machine not in CODE_SIZE:
        raise ValueError(f"no stand-in code for ELF machine {machine}")
    symbols = sorted(set(imports), key=lambda found: (found.version, found.name))
    if len({found.name for found in symbols}) != len(symbols):
        raise ValueError(f"{soname}: a symbol is asked for at two versions")
    version_names = sorted({found.version for found in symbols})
    objects = [found for found in symbols if not found.function]
    text = message.encode() + b"\n"

    strings = bytearray(b"\0")
    string_offsets: Dict[str, int] = {}

    def string(value: str) -> int:
        if value not in string_offsets:
            string_offsets[value] = len(strings)
            strings.extend(value.encode() + b"\0")
        return string_offsets[value]
    string(soname)
    for name in version_names:
        string(name)
    for found in symbols:
        string(found.name)

    # the loaded part, read only: headers, the dynamic symbols and their
    # versions, the relocations, the code; then, writable, .dynamic and the
    # objects; then the section headers' names and the headers
    count = len(symbols) + 1
    buckets = max(1, len(symbols))
    sizes = {
        ".hash": 4 * (2 + buckets + count),
        ".dynsym": 24 * count,
        ".dynstr": len(strings),
        ".gnu.version": 2 * count,
        ".gnu.version_d": 28 * (1 + len(version_names)),
        ".rela.dyn": 24 * len(objects),
        ".text": CODE_SIZE[machine] + len(text),
    }
    dynamic_count = 13
    layout: Dict[str, Tuple[int, int]] = {}  # name: (file offset, address)
    offset = 64 + 56 * 4
    for name, alignment in ((".hash", 8), (".dynsym", 8), (".dynstr", 1), (".gnu.version", 2),
                            (".gnu.version_d", 8), (".rela.dyn", 8), (".text", 16)):
        offset = _align(offset, alignment)
        layout[name] = (offset, offset)
        offset += sizes[name]
    read_only_end = offset
    # (the writable segment at its file offset plus a page, so that each
    # segment's pages are its own)
    offset = _align(offset, 8)
    layout[".dynamic"] = (offset, offset + PAGE)
    offset += 16 * dynamic_count
    layout[".data"] = (offset, offset + PAGE)
    sizes[".dynamic"], sizes[".data"] = 16 * dynamic_count, 8 * len(objects)
    offset += sizes[".data"]
    writable_end = offset
    code = layout[".text"][1]

    def address(name: str) -> int:
        return layout[name][1]

    hash_buckets = [0] * buckets
    chains = [0] * count
    for index, found in enumerate(symbols, 1):
        bucket = _elf_hash(found.name.encode()) % buckets
        chains[index], hash_buckets[bucket] = hash_buckets[bucket], index
    object_slots = {found: address(".data") + 8 * index for index, found in enumerate(objects)}
    contents = {
        ".hash": struct.pack(f"<{2 + buckets + count}I", buckets, count, *hash_buckets, *chains),
        ".dynsym": bytes(24) + b"".join(
            struct.pack("<IBBHQQ", string_offsets[found.name],
                        STB_GLOBAL << 4 | (STT_FUNC if found.function else STT_OBJECT), 0,
                        7 if found.function else 9,  # (.text, .data)
                        code if found.function else object_slots[found],
                        CODE_SIZE[machine] if found.function else 8)
            for found in symbols),
        ".dynstr": bytes(strings),
        ".gnu.version": struct.pack(f"<{count}H", 0,
                                    *(2 + version_names.index(found.version) for found in symbols)),
        ".gnu.version_d": b"".join(
            struct.pack("<HHHHIIIII", 1, VER_FLG_BASE if index == 0 else 0, index + 1, 1,
                        _elf_hash(name.encode()), 20, 28 if index < len(version_names) else 0,
                        string_offsets[name], 0)
            for index, name in enumerate([soname, *version_names])),
        ".rela.dyn": b"".join(struct.pack("<QQq", object_slots[found], RELATIVE[machine], code)
                              for found in objects),
        ".text": _code(machine, CODE_SIZE[machine], len(text)) + text,
        ".dynamic": struct.pack(
            "<26q",
            DT_SONAME, string_offsets[soname], DT_HASH, address(".hash"), DT_STRTAB, address(".dynstr"),
            DT_SYMTAB, address(".dynsym"), DT_STRSZ, len(strings), DT_SYMENT, 24,
            DT_RELA, address(".rela.dyn"), DT_RELASZ, sizes[".rela.dyn"], DT_RELAENT, 24,
            DT_VERSYM, address(".gnu.version"), DT_VERDEF, address(".gnu.version_d"),
            DT_VERDEFNUM, 1 + len(version_names), DT_NULL, 0),
        ".data": bytes(sizes[".data"]),
    }
    for name, data in contents.items():
        assert len(data) == sizes[name], name

    # (each section: type, flags, link, info, alignment, entry size)
    section_list = [
        (".hash", SHT_HASH, SHF_ALLOC, 2, 0, 8, 4),
        (".dynsym", SHT_DYNSYM, SHF_ALLOC, 3, 1, 8, 24),
        (".dynstr", SHT_STRTAB, SHF_ALLOC, 0, 0, 1, 0),
        (".gnu.version", SHT_GNU_VERSYM, SHF_ALLOC, 2, 0, 2, 2),
        (".gnu.version_d", SHT_GNU_VERDEF, SHF_ALLOC, 3, 1 + len(version_names), 8, 0),
        (".rela.dyn", SHT_RELA, SHF_ALLOC, 2, 0, 8, 24),
        (".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, 0, 0, 16, 0),
        (".dynamic", SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE, 3, 0, 8, 16),
        (".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 0, 0, 8, 0),
    ]
    section_names = bytearray(b"\0")
    name_offsets = {}
    for name in [entry[0] for entry in section_list] + [".shstrtab"]:
        name_offsets[name] = len(section_names)
        section_names.extend(name.encode() + b"\0")
    shstrtab_offset = writable_end
    section_headers_offset = _align(shstrtab_offset + len(section_names), 8)
    headers = [bytes(64)]
    for name, kind, flags, link, info, alignment, entry_size in section_list:
        file_offset, at = layout[name]
        headers.append(struct.pack("<IIQQQQIIQQ", name_offsets[name], kind, flags, at, file_offset,
                                   sizes[name], link, info, alignment, entry_size))
    headers.append(struct.pack("<IIQQQQIIQQ", name_offsets[".shstrtab"], SHT_STRTAB, 0, 0, shstrtab_offset,
                               len(section_names), 0, 0, 1, 0))

    elf_header = (ELF_MAGIC + bytes([2, 1, 1, 0]) + bytes(8)
                  + struct.pack("<HHIQQQIHHHHHH", 3, machine, 1, 0, 64, section_headers_offset, 0,
                                64, 56, 4, 64, len(headers), len(headers) - 1))
    writable_offset, writable_address = layout[".dynamic"]
    program_headers = b"".join([
        struct.pack("<IIQQQQQQ", PT_LOAD, PF_R | PF_X, 0, 0, 0, read_only_end, read_only_end, PAGE),
        struct.pack("<IIQQQQQQ", PT_LOAD, PF_R | PF_W, writable_offset, writable_address, writable_address,
                    writable_end - writable_offset, writable_end - writable_offset, PAGE),
        struct.pack("<IIQQQQQQ", PT_DYNAMIC, PF_R | PF_W, writable_offset, writable_address, writable_address,
                    sizes[".dynamic"], sizes[".dynamic"], 8),
        struct.pack("<IIQQQQQQ", PT_GNU_STACK, PF_R | PF_W, 0, 0, 0, 0, 0, 16),
    ])
    image = bytearray(section_headers_offset + 64 * len(headers))
    image[0:64] = elf_header
    image[64:64 + len(program_headers)] = program_headers
    for name, data in contents.items():
        image[layout[name][0]:layout[name][0] + len(data)] = data
    image[shstrtab_offset:shstrtab_offset + len(section_names)] = section_names
    image[section_headers_offset:] = b"".join(headers)
    return bytes(image)
