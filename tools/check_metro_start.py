#!/usr/bin/env python3
"""Verify responsive Start geometry, pointer activation and stable scanout."""

import argparse
import json
from pathlib import Path
import shutil
import socket
import time

from check_desktop_features import Desktop
from check_gui_idle import check_idle, read_prompt


MODES = [(1280, 720), (1600, 900), (1920, 1080), (1280, 800),
         (1440, 900), (1680, 1050), (1920, 1200), (1024, 768),
         (1280, 960), (1600, 1200), (1280, 1024), (800, 600),
         (640, 960), (800, 1200)]


def run(args):
    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    image = out / "metro-test.img"
    shutil.copyfile(args.image, image)
    desktop = Desktop(args.qemu, image, out, "metro")

    def click(x, y):
        desktop.move(x, y)
        desktop.command("mouse_button 1")
        time.sleep(.08)
        desktop.command("mouse_button 0")
        time.sleep(.3)

    def color_bounds(frame, color, clip=None):
        width, height, rgb = frame
        tint = bytes(color)
        xs, ys = [], []
        x0, y0, cw, ch = clip or (0, 0, width, height)
        for y in range(y0, y0 + ch):
            line = rgb[(y*width+x0)*3:(y*width+x0+cw)*3]
            x = line.find(tint)
            while x >= 0:
                if x % 3 == 0:
                    xs.append(x0 + x // 3)
                    ys.append(y)
                x = line.find(tint, x + 1)
        assert xs, f"Tile color {color} missing at {width}x{height}"
        return min(xs), min(ys), max(xs)+1, max(ys)+1

    try:
        desktop.key("s")
        desktop.key("1")
        results = []
        for index, size in enumerate(MODES[:1] if args.quick else MODES):
            if index:
                desktop.key("s")
                desktop.key("bracket_right")
            time.sleep(.3)
            desktop.key("ctrl-w")
            time.sleep(.25)
            # Mode changes clamp the cursor; reset our external tracker too.
            desktop.pointer = [min(desktop.pointer[0], size[0]-1),
                               min(desktop.pointer[1], size[1]-1)]
            desktop.move(10, 10)
            desktop.key("home")
            frame = desktop.capture(f"{size[0]}x{size[1]}")
            assert frame[:2] == size, (frame[:2], size)
            assert frame[2][:3] == b"\x18\x00\x52", "Start backdrop changed"
            left, top, right, bottom = color_bounds(frame, (218, 83, 44))
            assert 0 <= left < right <= size[0]
            assert 110 <= top < bottom <= size[1]-54, (size, left, top, right, bottom)
            if size[0] < 1000:
                assert right-left >= 150 and bottom-top >= 150, "Compact tile must remain readable"
            else:
                a, b, c, d = color_bounds(frame, (126, 56, 120))
                assert c-a >= 100 and d-b >= 100, "Last group is clipped on tablet/desktop"
            launches = desktop.log().count("launch /fs/apps/terminal")
            click(left + 20, top + 20)
            assert desktop.log().count("launch /fs/apps/terminal") == launches + 1
            desktop.key("ctrl-w")
            time.sleep(.2)
            # Arrow keys follow geometry rather than executable ordering.
            compact_cols = 2 if size[0] - 48 < 600 else 3
            below = ("textedit" if compact_cols == 2 else "paint") if size[0] < 1000 else "filemanager"
            beside = "taskmanager" if size[0] < 1000 else "calculator"
            for direction, app in (("down", below), ("right", beside)):
                desktop.key("home")
                desktop.key(direction)
                launches = desktop.log().count(f"launch /fs/apps/{app}")
                desktop.key("ret")
                time.sleep(.45)
                assert desktop.log().count(f"launch /fs/apps/{app}") == launches + 1, (size, direction, app)
                desktop.key("ctrl-w")
            if size[0] < 1000:
                # Selection must scroll the last application into view.
                desktop.key("end")
                frame = desktop.capture(f"{size[0]}x{size[1]}-scrolled")
                left, top, right, bottom = color_bounds(frame, (0, 138, 0))
                assert 132 <= top < bottom <= size[1]-54, (size, top, bottom)
                launches = desktop.log().count("launch /fs/apps/luaide")
                click(left + 20, top + 20)
                assert desktop.log().count("launch /fs/apps/luaide") == launches + 1
                desktop.key("ctrl-w")
            desktop.key("ctrl-p")
            desktop.key("end")
            frame = desktop.capture(f"{size[0]}x{size[1]}-search-end")
            panel_w = size[0] if size[0] < 1000 else 480
            left, top, right, bottom = color_bounds(frame, (0, 138, 0),
                (size[0]-panel_w, 108, panel_w, size[1]-180))
            assert 108 <= top < bottom <= size[1]-72, "Last search result is clipped"
            desktop.key("home")
            desktop.move(size[0]-150, 300)
            for _ in range(12):
                desktop.command("mouse_move 0 0 1")
                time.sleep(.05)
            frame = desktop.capture(f"{size[0]}x{size[1]}-search-wheel")
            left, top, right, bottom = color_bounds(frame, (0, 138, 0),
                (size[0]-panel_w, 108, panel_w, size[1]-180))
            launches = desktop.log().count("launch /fs/apps/")
            click(size[0]-300, 24)
            assert desktop.log().count("launch /fs/apps/") == launches, "Clipped row captured a header click"
            launches = desktop.log().count("launch /fs/apps/luaide")
            click(left + 5, top + 5)
            assert desktop.log().count("launch /fs/apps/luaide") == launches + 1
            desktop.key("ctrl-w")
            desktop.key("ctrl-p")
            click(size[0]-48, size[1]-36)
            frame = desktop.capture(f"{size[0]}x{size[1]}-search-closed")
            offset = (10*size[0] + size[0]-1)*3
            assert frame[2][offset:offset+3] == b"\x18\x00\x52"
            assert "[gui] exited" not in desktop.log(), "Closing search exited desktop"
            desktop.key("ctrl-p")
            desktop.type("calc")
            desktop.capture(f"{size[0]}x{size[1]}-search")
            desktop.key("ret")
            time.sleep(.5)
            assert "launch /fs/apps/calculator" in desktop.log()
            geometry = desktop.calculator(f"{size[0]}x{size[1]}-calculator")
            assert geometry and geometry[2] >= 320 and geometry[0] >= 0, geometry
            if size[0] < 1000:
                assert geometry == (0, 0, size[0]), "Compact launch must use the work area"
            for key in ("2", "shift-equal", "3", "ret"):
                desktop.key(key)
            desktop.capture(f"{size[0]}x{size[1]}-result")
            desktop.key("ctrl-w")
            assert "EXCEPTION" not in desktop.log()
            results.append(size)
            print(f"{size[0]}x{size[1]}: tile click, search, Calculator passed", flush=True)
        desktop.key("s")
        desktop.key("2")
        desktop.key("ctrl-w")
        time.sleep(.9)  # Window exit, cleanup and finite Home entrance.
        desktop.key("home")  # Settle entrance before defining idle pixels.
        time.sleep(.3)
        desktop.monitor.close()
        desktop.monitor = None
        assert check_idle(desktop.port, out / "idle", 10, 30, 0)
        desktop.monitor = socket.create_connection(("127.0.0.1", desktop.port), timeout=10)
        read_prompt(desktop.monitor)
        desktop.capture("desktop-final")
        (out / "result.json").write_text(json.dumps({"modes": results,
            "compact": "width-fitted readable cards, vertical scroll, last-app click",
            "all_modes": "spatial arrows, pointer launch, scrollable search and safe close, usable Calculator",
            "idle": "whole-screen pixels stable for 10 seconds"}, indent=2) + "\n")
    finally:
        desktop.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, default=Path("build/metro-start"))
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--quick", action="store_true", help="Run the first mode for focused interaction checks")
    run(parser.parse_args())
