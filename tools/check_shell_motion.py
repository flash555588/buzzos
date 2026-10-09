#!/usr/bin/env python3
"""Observe finite shell transitions and verify input and resting pixels."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import socket
import time

from check_desktop_features import Desktop
from check_gui_idle import check_idle, read_prompt


def tile_bounds(frame):
    width, height, rgb = frame
    positions, offset = [], 0
    while True:
        offset = rgb.find(b"\xda\x53\x2c", offset)
        if offset < 0:
            break
        if offset % 3 == 0:
            positions.append((offset//3 % width, offset//3 // width))
        offset += 3
    assert positions, "Terminal resting fill missing"
    xs, ys = zip(*positions)
    return min(xs), min(ys), max(xs)+1, max(ys)+1


def icon_size(frame, bounds):
    width, height, rgb = frame
    left, top, right, bottom = bounds
    xs, ys = [], []
    for y in range(top+30, bottom-45):
        row = rgb[(y*width+left+30)*3:(y*width+right-30)*3]
        offset = row.find(b"\xff\xff\xff")
        while offset >= 0:
            if offset % 3 == 0:
                xs.append(offset//3)
                ys.append(y)
            offset = row.find(b"\xff\xff\xff", offset+1)
    assert xs, "Terminal icon missing"
    return max(xs)-min(xs)+1, max(ys)-min(ys)+1


def run(args):
    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    image = out / "motion-test.img"
    if image == args.image.resolve():
        raise ValueError("Test copy must differ from source image")
    shutil.copyfile(args.image, image)
    desktop = Desktop(args.qemu, image, out, "motion")
    results = []
    def sample(label, seconds=.5, region=None):
        start, hashes, frames = time.monotonic(), set(), []
        while time.monotonic() - start < seconds:
            frame = desktop.capture(f"{label}-{len(frames):02}")
            pixels = frame[2]
            if region:
                x, y, w, h = region
                pixels = b"".join(pixels[(row*frame[0]+x)*3:(row*frame[0]+x+w)*3]
                                  for row in range(y, y+h))
            hashes.add(hashlib.sha256(pixels).digest())
            frames.append(frame)
            time.sleep(.015)
        return hashes, frames
    try:
        for mode, size in ((11, (800, 600)), (12, (640, 960)), (0, (1280, 720)), (6, (1920, 1200))):
            if mode not in args.modes:
                continue
            desktop.key("s")
            desktop.key("1")
            for _ in range(mode):
                desktop.key("bracket_right")
            desktop.key("ctrl-w")
            time.sleep(.5)
            desktop.pointer = [min(desktop.pointer[0], size[0]-1), min(desktop.pointer[1], size[1]-1)]
            desktop.move(10, 10)
            baseline = desktop.capture(f"{mode}-rest")
            assert baseline[:2] == size
            # Wheel targets accumulate, reverse and settle without changing
            # the final pixels. Keep the pointer outside tile hover regions.
            desktop.move(size[0]-4, 100)
            desktop.key("home")
            time.sleep(.25)
            scroll_rest = desktop.capture(f"{mode}-scroll-rest")
            wheel_hashes = set()
            for step in range(4):
                desktop.command("mouse_move 0 0 1")
                wheel_hashes.add(hashlib.sha256(desktop.capture(f"{mode}-wheel-{step}")[2]).digest())
                time.sleep(.04)  # Separate PS/2 wheel packets in QEMU.
            hashes, frames = sample(f"{mode}-scroll-down", .5)
            if size[0] < 1000:  # Larger Start layouts fit without overflow.
                assert len(hashes | wheel_hashes) >= 3, "Wheel scroll lacked intermediate frames"
            for _ in range(4):
                desktop.command("mouse_move 0 0 -1")
                time.sleep(.04)
            sample(f"{mode}-scroll-up", .5)
            assert desktop.capture(f"{mode}-scroll-return") == scroll_rest, "Wheel reversal left stale headings or tiles"
            desktop.move(10, 10)
            baseline = desktop.capture(f"{mode}-rest")
            desktop.command("sendkey ctrl-p 20")
            hashes, frames = sample(f"{mode}-search-open")
            assert len(hashes) >= 3, "Search appeared without intermediate frames"
            assert frames[-1][2][(10*size[0]+size[0]-1)*3:][:3] == b"\xff\xff\xff"
            desktop.command("sendkey esc 20")
            hashes, frames = sample(f"{mode}-search-close")
            assert len(hashes) >= 3, "Search disappeared without intermediate frames"
            assert frames[-1] == baseline, "Search left a trail on Home"
            desktop.key("ctrl-p")
            desktop.key("esc")
            desktop.key("ctrl-p")
            time.sleep(.3)
            desktop.type("calc")
            desktop.command("sendkey ret 20")
            hashes, frames = sample(f"{mode}-window-open", .6)
            assert len(hashes) >= 3
            geometry = desktop.calculator(f"{mode}-window-rest")
            assert geometry is not None
            x, y, width = geometry
            # Move just one packet into the close control, then observe only
            # that control so uptime or unrelated pixels cannot pass the test.
            desktop.move(x+width-72, y+24)
            desktop.command("mouse_move 48 0")
            desktop.pointer[0] += 48
            hashes, frames = sample(f"{mode}-caption-hover", .3,
                                   (x+width-40, y+4, 36, 40))
            assert len(hashes) >= 2, "Caption hover lacked eased feedback"
            gx = min((size[0]-204)//2, size[0]-360)
            desktop.move(gx-24, size[1]-40)
            desktop.command("mouse_move 48 0")
            desktop.pointer[0] += 48
            hashes, frames = sample(f"{mode}-dock-hover", .3,
                                   (gx, size[1]-64, 48, 48))
            assert len(hashes) >= 2, "Taskbar hover lacked eased feedback"
            desktop.move(10, 10)
            desktop.command("sendkey alt-up 20")
            hashes, frames = sample(f"{mode}-maximize", .6)
            assert len(hashes) >= 3, "Maximize lacked arrival frames"
            desktop.command("sendkey alt-down 20")
            hashes, frames = sample(f"{mode}-minimize", .6)
            assert len(hashes) >= 2, "Minimize lacked exit frames"
            assert desktop.calculator(f"{mode}-minimized") is None
            desktop.key("alt-tab")
            time.sleep(.35)
            desktop.command("sendkey alt-d 20")
            hashes, frames = sample(f"{mode}-home", .6)
            assert len(hashes) >= 3, "Home tiles lacked staggered entrance"
            desktop.key("alt-tab")
            time.sleep(.3)
            desktop.command("sendkey ctrl-w 20")
            hashes, frames = sample(f"{mode}-window-close", .6)
            assert len(hashes) >= 3, "Close lacked exit frames"
            assert desktop.calculator(f"{mode}-closed") is None
            time.sleep(.4)  # Exit, process cleanup and the Home entrance.
            # Begin a separate pointer test at a resting keyboard-selected
            # tile; host wall time does not bound QEMU's emulated clock.
            desktop.key("home")
            time.sleep(.2)
            desktop.move(10, 10)
            # Find a point inside Terminal from its resting fill, then hold,
            # drag outside and release: a canceled press must not launch.
            width, height, rgb = desktop.capture(f"{mode}-press-rest")
            bounds = tile_bounds((width, height, rgb))
            x, y = bounds[0]+30, bounds[1]+30
            desktop.move(x, y)
            resting_icon = icon_size(desktop.capture(f"{mode}-hover-rest"), bounds)
            before = desktop.log().count("launch /fs/apps/terminal")
            desktop.command("mouse_button 1")
            hashes, frames = sample(f"{mode}-press", .15)
            assert len(hashes) >= 2, "Press lacked intermediate feedback"
            if bounds[3] - bounds[1] >= 200:
                pressed_icon = icon_size(frames[-1], bounds)
                assert all(b >= a*.88 for a, b in zip(resting_icon, pressed_icon)), "Icon jumped at height threshold"
            assert desktop.log().count("launch /fs/apps/terminal") == before
            desktop.move(10, 10)
            desktop.command("mouse_button 0")
            time.sleep(.2)
            assert desktop.log().count("launch /fs/apps/terminal") == before
            desktop.move(x, y)
            desktop.command("mouse_button 1")
            time.sleep(.12)
            desktop.command("mouse_button 0")
            time.sleep(.3)
            assert desktop.log().count("launch /fs/apps/terminal") == before + 1
            desktop.key("ctrl-w")
            time.sleep(.8)
            desktop.move(10, 10)
            assert "EXCEPTION" not in desktop.log()
            results.append(size)
            print(f"{size}: intermediate frames, reversal, pointer cancellation passed", flush=True)
        desktop.monitor.close()
        desktop.monitor = None
        assert check_idle(desktop.port, out / "idle", 10, 30, 0)
        desktop.monitor = socket.create_connection(("127.0.0.1", desktop.port), timeout=10)
        read_prompt(desktop.monitor)
        (out / "result.json").write_text(json.dumps({"modes": results, "idle": "10s unchanged"}, indent=2) + "\n")
    finally:
        desktop.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, default=Path("build/shell-motion"))
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--modes", type=int, nargs="+", choices=(11, 12, 0, 6), default=(11, 12, 0, 6))
    run(parser.parse_args())
