#!/usr/bin/env python3
"""A demo's contents (port/linux/game/demo.c, docs/demos.md): its header,
its blocks and its records by type, their sizes packed and unpacked, its
ticks, bookmarks and snapshots.

    python tools/demo_info.py demos/20261108-014642-a10.demo
    python tools/demo_info.py --patches demos/....demo    # each difference from outside the ticks
"""

import argparse
import struct
import sys
import zlib
from collections import Counter, defaultdict
from pathlib import Path

HEADER_SIZE = 512
BLOCK_HEADER_SIZE = 20
BLOCK_MAGIC = 0x4B4C4244
CONNECTIONS = {0: "local", 1: "network client", 2: "network server", 3: "film playback"}
SNAPSHOT_FLAGS = {0: "start", 1: "loaded", 2: "reset", 3: "seek"}


class Demo:
    def __init__(self, data: bytes):
        if len(data) < HEADER_SIZE or data[:7] != b"OH1DEMO":
            raise ValueError("not a demo")
        self.version = data[8]
        self.build = data[16:80].split(b"\0")[0].decode("ascii", "replace")
        values = struct.unpack_from("<10I", data, 80)
        (self.network_version, self.allocation_checksum, self.state_size, self.connection, self.rate,
         self.difficulty, self.start_tick, self.time, self.local_players, self.server) = values
        self.map_name = data[128:256].split(b"\0")[0].decode("ascii", "replace")
        self.blocks = []
        offset = HEADER_SIZE
        while offset + BLOCK_HEADER_SIZE <= len(data):
            magic, kind, first_tick, size, packed_size = struct.unpack_from("<5I", data, offset)
            if magic != BLOCK_MAGIC or offset + BLOCK_HEADER_SIZE + packed_size > len(data):
                break
            packed = data[offset + BLOCK_HEADER_SIZE:offset + BLOCK_HEADER_SIZE + packed_size]
            self.blocks.append((chr(kind), first_tick, size, packed_size, packed))
            offset += BLOCK_HEADER_SIZE + packed_size
        self.trailing = len(data) - offset

    def records(self, kinds="RK"):
        """each record of the blocks of those kinds: (block kind, type, bytes)"""
        for kind, _, size, _, packed in self.blocks:
            if kind not in kinds:
                continue
            raw = zlib.decompress(packed)
            if len(raw) != size:
                raise ValueError("a block unpacks to another size")
            position = 0
            while position + 5 <= len(raw):
                record_type = chr(raw[position])
                (length,) = struct.unpack_from("<I", raw, position + 1)
                yield kind, record_type, raw[position + 5:position + 5 + length]
                position += 5 + length


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("demo", type=Path)
    parser.add_argument("--patches", action="store_true", help="list each difference (P record) by its tick")
    args = parser.parse_args()
    demo = Demo(args.demo.read_bytes())
    print(f"{args.demo.name}: format {demo.version}, build {demo.build}, network version {demo.network_version}")
    print(f"  map {demo.map_name}, {CONNECTIONS.get(demo.connection, demo.connection)}, rate {demo.rate}, "
          f"difficulty {demo.difficulty}, from tick {demo.start_tick}, {demo.local_players} local players"
          f"{', a dedicated server' if demo.server else ''}")
    by_kind = defaultdict(lambda: [0, 0, 0])
    for kind, _, size, packed_size, _ in demo.blocks:
        by_kind[kind][0] += 1
        by_kind[kind][1] += size
        by_kind[kind][2] += packed_size
    total = sum(entry[2] for entry in by_kind.values())
    for kind, (count, size, packed) in sorted(by_kind.items()):
        name = {"R": "records", "K": "snapshots taken", "k": "seek snapshots"}.get(kind, kind)
        print(f"  {name}: {count} blocks, {size / 1e6:.2f} MB unpacked, {packed / 1e6:.2f} MB packed")
    types = Counter()
    sizes = Counter()
    ticks = 0
    first = last = None
    patches = []
    tick = None
    bookmarks = []
    for kind, record_type, data in demo.records("R"):
        types[record_type] += 1
        sizes[record_type] += len(data) + 5
        if record_type == "T":
            tick = struct.unpack_from("<I", data)[0]
            ticks += 1
            first = tick if first is None else first
            last = tick
        elif record_type == "P":
            patches.append((tick, len(data)))
        elif record_type == "B":
            bookmarks.append(struct.unpack_from("<II", data))
    seconds = ticks / 30.0
    print(f"  {ticks} ticks ({seconds / 60:.1f} min of game), ticks {first} to {last}; "
          f"{total / 1e6:.2f} MB, {total / 1e6 / max(seconds / 60, 1e-9):.2f} MB a minute"
          + (f"; {demo.trailing} bytes cut short at the end" if demo.trailing else ""))
    for record_type in sorted(types):
        print(f"    {record_type}: {types[record_type]} records, {sizes[record_type] / 1e6:.3f} MB unpacked")
    for tick_value, milliseconds in bookmarks:
        print(f"  bookmark at tick {tick_value}: the last {milliseconds / 1000:.0f} s")
    if args.patches:
        for tick_value, size in patches:
            print(f"  P at tick {tick_value}: {size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
