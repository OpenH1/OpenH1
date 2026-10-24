#!/usr/bin/env python3
"""How busy a running game keeps the GPU, from the kernel's own count: no
setting, no build and nothing in the game's frame (Linux, a DRM driver with
fdinfo usage stats: Mesa's Turnip on the Steam Frame's msm, amdgpu, i915/xe,
panfrost and others).

    python3 tools/gpu_busy.py                 (the process named halo)
    python3 tools/gpu_busy.py --hz 120 1234   (process 1234, at 120 Hz)

Each interval (a second by default) prints the GPU's busy time over it as a
share of the time, and, with --hz (the headset's refresh rate), as
milliseconds a frame beside the frame's budget. The kernel counts the time
the GPU spent on the process's work (each DRM client's drm-engine-* in
/proc/<pid>/fdinfo, docs/gpu/drm-usage-stats.rst in Linux): not the gaps
between its submissions, as the game's debug.vr_gpu_time timeline counts
them. Near 100% (or the budget), the GPU is what holds the frame rate; well
under it with the frame rate still short, the CPU is (debug.gpu_stats'
"a frame's draws" lines say which part). Copy it to the headset (scp) and
run it there over ssh while the game plays.
"""

import argparse
import os
import sys
import time
from pathlib import Path
from typing import Dict, Optional


def client_times(text: str) -> Optional[tuple]:
    """A DRM client's id and its engines' busy nanoseconds summed, from one
    fdinfo file's text; None for a file that is not a DRM client's."""
    client = None
    busy = 0
    seen = False
    for line in text.splitlines():
        key, _, value = line.partition(":")
        value = value.strip()
        if key == "drm-client-id":
            client = value
        elif key.startswith("drm-engine-") and not key.startswith("drm-engine-capacity-"):
            number, _, unit = value.partition(" ")
            if number.isdigit() and unit.strip() in ("", "ns"):
                busy += int(number)
                seen = True
    if client is None or not seen:
        return None
    return client, busy


def process_busy(pid: int, proc: Path = Path("/proc")) -> int:
    """The process's DRM clients' busy nanoseconds so far: each client once,
    however many of its file descriptors are open."""
    clients: Dict[str, int] = {}
    folder = proc / str(pid) / "fdinfo"
    for entry in folder.iterdir():
        try:
            times = client_times(entry.read_text())
        except OSError:
            continue
        if times:
            clients[times[0]] = max(clients.get(times[0], 0), times[1])
    return sum(clients.values())


def find_process(name: str, proc: Path = Path("/proc")) -> Optional[int]:
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if (entry / "comm").read_text().strip() == name:
                return int(entry.name)
        except OSError:
            continue
    return None


def report(busy_ns: int, wall_ns: int, hz: float) -> str:
    share = 100.0 * busy_ns / wall_ns if wall_ns > 0 else 0.0
    line = f"GPU busy {share:5.1f}%"
    if hz > 0:
        frames = wall_ns * hz / 1e9
        line += f" = {busy_ns / 1e6 / frames:5.2f} ms a frame of the {1000.0 / hz:.2f} ms at {hz:g} Hz"
    return line


def main() -> int:
    parser = argparse.ArgumentParser(description="the GPU's busy time of a running game, from the kernel")
    parser.add_argument("pid", nargs="?", type=int, help="the game's process (default: the one named halo)")
    parser.add_argument("--name", default="halo", help="the process's name, without a pid (default: halo)")
    parser.add_argument("--hz", type=float, default=0.0, help="the display's refresh rate, for ms a frame")
    parser.add_argument("--interval", type=float, default=1.0, help="seconds between reports (default: 1)")
    parser.add_argument("--count", type=int, default=0, help="reports to print, then stop (default: until ^C)")
    arguments = parser.parse_args()
    pid = arguments.pid or find_process(arguments.name)
    if not pid:
        print(f"no process named {arguments.name}", file=sys.stderr)
        return 1
    try:
        before, started = process_busy(pid), time.monotonic_ns()
    except OSError as error:
        print(f"cannot read /proc/{pid}/fdinfo: {error}", file=sys.stderr)
        return 1
    if not before:
        print("the process has no DRM client with engine times yet (the kernel or driver may not count them)",
              file=sys.stderr)
    printed = 0
    try:
        while not arguments.count or printed < arguments.count:
            time.sleep(arguments.interval)
            try:
                now, at = process_busy(pid), time.monotonic_ns()
            except OSError:
                print("the process has ended", file=sys.stderr)
                return 0
            print(report(now - before, at - started, arguments.hz), flush=True)
            before, started = now, at
            printed += 1
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
