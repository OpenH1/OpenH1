#!/usr/bin/env python3
"""The median and 95th percentile of each stage of debug.latency_log's lines (port/linux/game/latency_log.c), for
each kind of event (fire, move), in milliseconds.

    python tools/latency_summary.py latency.txt [more logs...]
"""

import argparse
import statistics
from pathlib import Path

STAGES = ["press_to_read", "read_to_act", "act_to_frame", "frame_to_shown", "render", "present", "total", "fraction",
          "frame"]


def percentile(values, fraction):
    """the value at that fraction of the sorted values (nearest rank)"""
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, max(0, round(fraction * len(ordered)) - 1))]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logs", nargs="+", type=Path)
    args = parser.parse_args()
    for log in args.logs:
        rate, events = None, {}
        for line in log.read_text().splitlines():
            words = line.split()
            if words[:4] == ["#", "latency", "log", "1"] and "rate" in words:
                rate = words[words.index("rate") + 1]
            elif words and words[0] in ("fire", "move") and len(words) >= 1 + len(STAGES):
                events.setdefault(words[0], []).append([float(word) for word in words[1:1 + len(STAGES)]])
        print(f"{log} (game.sim_rate {rate or '?'})")
        print(f"  {'':5} {'n':>3}  " + "  ".join(f"{stage:>18}" for stage in STAGES))
        for kind, rows in sorted(events.items()):
            cells = []
            for index in range(len(STAGES)):
                values = [row[index] for row in rows]
                cells.append(f"{statistics.median(values):7.1f} / {percentile(values, 0.95):6.1f}  ")
            print(f"  {kind:5} {len(rows):>3}  " + "  ".join(f"{cell:>18}" for cell in cells))
        print("  (each: median / 95th percentile, ms)")


if __name__ == "__main__":
    main()
