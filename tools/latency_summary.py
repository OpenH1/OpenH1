#!/usr/bin/env python3
"""The median and 95th percentile of each stage of debug.latency_log's lines (port/linux/game/latency_log.c), for
each kind of event (fire, move: the controller's; click, turn: the mouse's; menu: a press in a menu; the sound of a
shot; and a rumble's start), in milliseconds; those the player made in a vehicle's seat apart (fire+v, move+v...).

    python tools/latency_summary.py latency.txt [more logs...] [--frames frames.txt ...]

With --frames, the Vulkan renderer's frame logs (HALO_VULKAN_FRAME_LOG, port/linux/vulkan/xgpu_vulkan_pacing.c), one
for each latency log in order: each event's showing frame is found by its input read (the line's input_clock), and the
summary adds the press to that frame's present (press_to_present), to its time on screen (press_to_screen), and the
present to the screen (present_to_screen); and the frames' refreshes that showed a frame again.
"""

import argparse
import statistics
from pathlib import Path

KINDS = ("fire", "move", "click", "turn", "menu")
STAGES = ["press_to_read", "read_to_act", "act_to_frame", "frame_to_shown", "render", "present", "total", "fraction",
          "frame"]
SOUND_STAGES = ["act_to_start", "start_to_request", "answer", "queued", "system", "total", "buffer", "dropouts"]
RUMBLE_STAGES = ["step_to_call", "on", "duration"]


def percentile(values, fraction):
    """the value at that fraction of the sorted values (nearest rank)"""
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, max(0, round(fraction * len(ordered)) - 1))]


DISPLAY_STAGES = ["press_to_present", "press_to_screen", "present_to_screen"]


def frames_read(path):
    """{input read: (presented, shown)} of a frame log's frames (nanoseconds), and the refreshes that showed a frame
    again and the frames counted, of the paced and unpaced"""
    frames, repeated, counted = {}, 0, 0
    for line in path.read_text().splitlines():
        words = line.split()
        if words[:1] != ["frame"] or len(words) < 7:
            continue
        input_ns, queued, _finished, shown, refreshes = (int(word) for word in words[1:6])
        if input_ns:
            frames[input_ns] = (queued, shown)
        if refreshes:
            counted += 1
            repeated += max(0, refreshes - 1)
    return frames, repeated, counted


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--frames", nargs="*", type=Path, default=[])
    args = parser.parse_args()
    for number, log in enumerate(args.logs):
        rate, events = None, {}
        frames = None
        if number < len(args.frames):
            frames, repeated, counted = frames_read(args.frames[number])
        for line in log.read_text().splitlines():
            words = line.split()
            if words[:4] == ["#", "latency", "log", "1"] and "rate" in words:
                rate = words[words.index("rate") + 1]
            elif words and words[0] in KINDS and len(words) >= 1 + len(STAGES):
                row = [float(word) for word in words[1:1 + len(STAGES)]]
                # (in a vehicle's seat: the column after the clocks, in a log that has it)
                seated = len(words) > 3 + len(STAGES) and words[3 + len(STAGES)] == "1"
                if frames is not None:
                    # (the press to the showing frame's present and screen, by its input read)
                    shown = None
                    if len(words) >= 3 + len(STAGES):
                        press, input_ns = int(words[1 + len(STAGES)]), int(words[2 + len(STAGES)])
                        shown = frames.get(input_ns)
                    if not shown or not shown[1] or not press:
                        continue
                    row += [(shown[0] - press) / 1e6, (shown[1] - press) / 1e6, (shown[1] - shown[0]) / 1e6]
                events.setdefault(words[0] + ("+v" if seated else ""), []).append(row)
            elif words and words[0] == "sound" and len(words) >= 1 + len(SOUND_STAGES):
                events.setdefault("sound", []).append([float(word) for word in words[1:1 + len(SOUND_STAGES)]])
            elif words and words[0] == "rumble" and len(words) >= 1 + len(RUMBLE_STAGES):
                events.setdefault("rumble", []).append([float(word) for word in words[1:1 + len(RUMBLE_STAGES)]])
        stages = STAGES + (DISPLAY_STAGES if frames is not None else [])
        print(f"{log} (game.sim_rate {rate or '?'})")
        if frames is not None:
            print(f"  frames: {counted} timed, {repeated} refreshes showed a frame again")
        print(f"  {'':7} {'n':>3}  " + "  ".join(f"{stage:>18}" for stage in stages))
        for kind, rows in sorted(events.items(), key=lambda item: (
                KINDS.index(item[0].split("+")[0]) if item[0].split("+")[0] in KINDS else len(KINDS), item[0])):
            if kind in ("sound", "rumble"):
                continue
            cells = []
            for index in range(len(stages)):
                values = [row[index] for row in rows]
                cells.append(f"{statistics.median(values):7.1f} / {percentile(values, 0.95):6.1f}  ")
            print(f"  {kind:7} {len(rows):>3}  " + "  ".join(f"{cell:>18}" for cell in cells))
        for kind, stages in (("sound", SOUND_STAGES), ("rumble", RUMBLE_STAGES)):
            if kind not in events:
                continue
            rows = events[kind]
            print(f"  {'':7} {'n':>3}  " + "  ".join(f"{stage:>18}" for stage in stages))
            cells = []
            for index in range(len(stages)):
                values = [row[index] for row in rows]
                cells.append(f"{statistics.median(values):7.1f} / {percentile(values, 0.95):6.1f}  ")
            print(f"  {kind:7} {len(rows):>3}  " + "  ".join(f"{cell:>18}" for cell in cells))
        print("  (each: median / 95th percentile, ms)")


if __name__ == "__main__":
    main()
