#!/usr/bin/env python3
"""Write a controller macro for the Android build (files/macros/<name>.macro) from a list of presses.

    make_macro.py futsal "3:start 7:a 12:a 40:down 42:down 44:a"

Each step is <seconds>:<button>[:<hold ms>]; times count from when the macro starts playing. The file is
what Macro.kt reads: `<ms> <slot> <buttons> <lt> <rt> <lx> <ly> <rx> <ry>`, slot 4 being the pad.
Play it with tools/play_macro.sh.
"""
import sys

BUTTONS = {"up": 0x1, "down": 0x2, "left": 0x4, "right": 0x8, "start": 0x10, "back": 0x20,
           "lb": 0x100, "rb": 0x200, "a": 0x1000, "b": 0x2000, "x": 0x4000, "y": 0x8000}

def main():
    name, steps = sys.argv[1], sys.argv[2].split()
    events = []
    for step in steps:
        parts = step.split(":")
        at = int(float(parts[0]) * 1000)
        hold = int(parts[2]) if len(parts) > 2 else 120
        mask = BUTTONS[parts[1]]
        events.append((at, mask))
        events.append((at + hold, 0))
    events.sort()
    end = events[-1][0]
    out = [f"# pes17-macro v1, {len(events)} events, {end} ms"]
    out += [f"{ms} 4 {mask} 0 0 0.0 0.0 0.0 0.0" for ms, mask in events]
    out.append(f"# end {end}")
    path = sys.argv[3] if len(sys.argv) > 3 else f"macros/{name}.macro"
    open(path, "w").write("\n".join(out) + "\n")
    print(path, len(events), "events,", end / 1000, "s")

main()
