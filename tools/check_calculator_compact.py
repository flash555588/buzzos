#!/usr/bin/env python3
"""Check visible compact Calculator targets and pointer/keyboard equivalence."""

import argparse
import json
from pathlib import Path
import shutil
import time

from check_desktop_features import Desktop


def run(args):
    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    image = out / "calculator-test.img"
    if image == args.image.resolve():
        raise ValueError("Test copy must differ from source image")
    shutil.copyfile(args.image, image)
    desktop = Desktop(args.qemu, image, out, "calculator")
    try:
        desktop.key("s")
        desktop.key("1")
        for _ in range(11):
            desktop.key("bracket_right")
        desktop.key("ctrl-w")
        desktop.pointer = [min(desktop.pointer[0], 799), min(desktop.pointer[1], 599)]
        desktop.launch("calc")
        width, height, rgb = desktop.capture("keys")
        assert (width, height) == (800, 600)
        assert desktop.calculator("geometry") == (0, 0, 800)
        # Read actual button fill from the framebuffer, independently of the
        # layout formula. Five gray rows follow the colored Clear button.
        runs, start = [], None
        for y in range(60, 520):
            pixel = rgb[(y*width+36)*3:(y*width+36)*3+3]
            if pixel == b"\xf0\xf0\xf0":
                if start is None:
                    start = y
            elif start is not None:
                if y-start >= 10:
                    runs.append((start, y))
                start = None
        assert len(runs) == 5 and min(b-a for a, b in runs) >= 40, runs
        for key in ("2", "shift-equal", "3", "ret"):
            desktop.key(key)
        reference = desktop.capture("keyboard-result")[2]
        desktop.key("c")
        for x, row in ((300, 2), (700, 3), (500, 2), (600, 4)):
            desktop.move(x, sum(runs[row]) // 2)
            desktop.command("mouse_button 1")
            time.sleep(.08)
            desktop.command("mouse_button 0")
            time.sleep(.15)
        actual = desktop.capture("pointer-result")[2]
        def readout(pixels):
            return b"".join(pixels[(y*800+12)*3:(y*800+788)*3] for y in range(60, 130))
        assert readout(actual) == readout(reference), "Pointer differs from keyboard 2+3=5"
        assert "EXCEPTION" not in desktop.log()
        report = {"size": [800, 600], "key_height": min(b-a for a, b in runs),
                  "pointer": "2+3=5 matches keyboard readout"}
        (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report))
    finally:
        desktop.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, default=Path("build/calculator-compact"))
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    run(parser.parse_args())
