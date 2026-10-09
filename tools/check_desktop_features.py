#!/usr/bin/env python3
"""Exercise desktop search, window commands and saved settings on a copy."""

import argparse
import json
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import time
import zlib

from check_gui_idle import read_frame, read_prompt
from check_minifs import MINIFS_FILE, MINIFS_ROOT_INO
from install_doom_wad import MiniFsWriter


class Desktop:
    def __init__(self, qemu, image, out, name, network_args=(), cpu="qemu64,+rdrand", memory_mib=256, host_log=None, accelerator="whpx", display="none"):
        if display not in ("none", "gtk", "sdl"):
            raise ValueError("Unsupported QEMU display backend")
        self.out = out
        self.name = name
        self.serial = out / f"{name}-serial.log"
        self.serial.unlink(missing_ok=True)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            self.port = listener.getsockname()[1]
        flags = subprocess.CREATE_NO_WINDOW if hasattr(subprocess, "CREATE_NO_WINDOW") else 0
        self.process = subprocess.Popen([
            qemu, "-accel", accelerator, "-cpu", cpu, "-smp", "1", "-m", str(memory_mib),
            "-drive", f"format=raw,file={image}", "-serial", f"file:{self.serial}",
            "-display", display, "-vga", "std", "-no-reboot",
            "-monitor", f"tcp:127.0.0.1:{self.port},server,nowait",
            "-audiodev", "none,id=audio0", "-device", "AC97,audiodev=audio0", *network_args],
            creationflags=flags, stdout=host_log,
            stderr=subprocess.STDOUT if host_log is not None else None)
        self.monitor = None
        try:
            self.wait_for_shell()
            self.monitor = socket.create_connection(("127.0.0.1", self.port), timeout=10)
            read_prompt(self.monitor)
            for key in ("g", "u", "i", "ret"):
                self.key(key)
            time.sleep(.7)
            width, height, _ = self.capture("home")
            self.pointer = [width // 2, height // 2]
        except BaseException:
            self.close()
            raise

    def wait_for_shell(self, timeout=60):
        deadline = time.monotonic() + timeout
        while True:
            if ":/>" in self.log():
                return
            if self.process.poll() is not None:
                raise RuntimeError("QEMU exited during boot")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("Boot timeout")
            time.sleep(min(.15, remaining))

    def log(self):
        return self.serial.read_text(errors="replace") if self.serial.exists() else ""

    def command(self, text):
        self.monitor.sendall((text + "\n").encode())
        return read_prompt(self.monitor)

    def key(self, name):
        self.command(f"sendkey {name} 25")
        time.sleep(.085)

    def type(self, text):
        for char in text:
            self.key("shift-" + char.lower() if char.isupper() else char)

    def capture(self, name):
        path = self.out / f"{self.name}-{name}.ppm"
        self.command(f'screendump "{path.as_posix()}"')
        width, height, rgb = read_frame(path, 0)
        raw = b"".join(b"\0" + rgb[y*width*3:(y+1)*width*3] for y in range(height))
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind+data) & 0xffffffff)
        path.with_suffix(".png").write_bytes(
            b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
        return width, height, rgb

    def move(self, x, y):
        while self.pointer != [x, y]:
            dx = max(-24, min(24, x-self.pointer[0]))
            dy = max(-24, min(24, y-self.pointer[1]))
            self.command(f"mouse_move {dx} {dy}")
            self.pointer[0] += dx
            self.pointer[1] += dy
            time.sleep(.04)
        time.sleep(.15)

    def drag(self, start, end):
        self.move(*start)
        self.command("mouse_button 1")
        time.sleep(.12)
        self.move(*end)
        self.command("mouse_button 0")
        time.sleep(.4)

    def calculator(self, name):
        width, height, rgb = self.capture(name)
        # The active Calculator title's flat tint uniquely identifies its
        # geometry without depending on a text recognizer or internal hooks.
        tint = bytes((fg*95 + bg*160 + 127)//255
                     for fg, bg in zip((245, 212, 111), (255, 255, 255)))
        positions = []
        offset = 0
        while True:
            offset = rgb.find(tint, offset)
            if offset < 0:
                break
            if offset % 3 == 0:
                x, y = (offset//3) % width, (offset//3) // width
                if (0 if width < 1000 else 72) <= y < height-80:
                    positions.append((x, y))
            offset += 3
        if len(positions) < 1000:
            return None
        xs, ys = zip(*positions)
        return min(xs)-1, min(ys)-1, max(xs)-min(xs)+3

    def launch(self, query):
        self.key("ctrl-p")
        self.type(query)
        self.capture("search-" + query.lower())
        self.key("ret")
        time.sleep(.7)

    def close(self):
        if self.monitor:
            try:
                self.monitor.sendall(b"quit\n")
                while self.monitor.recv(65536):
                    pass
            except OSError:
                pass
            self.monitor.close()
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            self.process.wait(timeout=3)


def run_checks(args):
    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    image = out / "desktop-test.img"
    if image == args.image.resolve():
        raise ValueError("Test copy must differ from source image")
    shutil.copyfile(args.image, image)
    desktop = Desktop(args.qemu, image, out, "features")
    try:
        desktop.key("ctrl-p")
        desktop.type("zzzzzz")
        desktop.capture("no-results")
        desktop.key("ret")
        assert "launch /fs/apps/" not in desktop.log(), "Empty search launched an app"
        for key in ("delete", "home", "end"):
            desktop.key(key)
        assert "[gui] exited" not in desktop.log(), "Editing keys exited the desktop"
        desktop.key("esc")
        desktop.launch("CALC")
        assert "launch /fs/apps/calculator" in desktop.log()
        assert desktop.calculator("normal") == (445, 112, 710)
        for shortcut, expected in (("alt-left", (0, 72, 800)),
                                   ("alt-right", (800, 72, 800)),
                                   ("alt-up", (0, 72, 1600))):
            desktop.key(shortcut)
            time.sleep(.3)
            assert desktop.calculator(shortcut) == expected, shortcut
        desktop.drag((300, 96), (600, 230))
        restored = desktop.calculator("drag-restored")
        assert restored and restored[2] == 800, restored
        # The pre-maximize rectangle is the most recently tiled window.
        desktop.key("alt-down")
        time.sleep(.3)  # Allow the finite minimise exit to complete.
        assert desktop.calculator("minimized") is None
        desktop.key("alt-tab")
        assert desktop.calculator("task-restored") is not None
        desktop.key("alt-d")
        assert desktop.calculator("workspace") is None
        desktop.key("alt-tab")
        assert desktop.calculator("task-switched") is not None
        desktop.key("alt-f4")
        time.sleep(.3)
        assert desktop.calculator("closed") is None

        desktop.key("s")
        desktop.key("7")
        time.sleep(.3)
        assert desktop.capture("large-mode")[:2] == (1920, 1200)
        desktop.key("ctrl-w")
        desktop.launch("calc")
        x, y, width = desktop.calculator("large-normal")
        desktop.drag((x+width-3, y+497), (x+200, y+120))
        minimum = desktop.calculator("minimum")
        assert minimum and minimum[2] == 320, minimum
        for key in ("2", "shift-equal", "3", "ret"):
            desktop.key(key)
        desktop.capture("minimum-result")
        desktop.key("alt-d")
        desktop.key("s")
        desktop.key("8")
        time.sleep(.3)
        assert desktop.capture("small-mode")[:2] == (1024, 768)
        desktop.key("ctrl-w")
        desktop.key("alt-tab")
        resized = desktop.calculator("minimum-after-mode")
        assert resized and resized[2] >= 320 and resized[1] >= 72, resized
        desktop.key("alt-d")
        desktop.key("s")
        desktop.key("3")
        time.sleep(.3)
        assert desktop.capture("saved-mode")[:2] == (1920, 1080)
        desktop.key("ctrl-w")
        desktop.key("esc")
        time.sleep(.3)
        assert "[gui] exited 0" in desktop.log()
        assert "EXCEPTION" not in desktop.log()
    finally:
        desktop.close()

    writer = MiniFsWriter(bytearray(image.read_bytes()), 67584, 65536)
    root = writer.fs.inodes[MINIFS_ROOT_INO]
    settings = writer.add_child(root, "desktop.settings", MINIFS_FILE)
    saved = writer.fs.read_file_bytes(settings)
    assert struct.unpack("<IIII", saved) == (0x425A5531, 1920, 1080, 4), saved
    desktop = Desktop(args.qemu, image, out, "cold-boot")
    try:
        assert desktop.capture("restored")[:2] == (1920, 1080)
        assert "[gui] restored desktop settings" in desktop.log()
        desktop.key("ret")
        time.sleep(.7)
        assert "launch /fs/apps/calculator" in desktop.log(), "Selection was not restored"
    finally:
        desktop.close()

    for name, invalid in (("truncated", b"bad"),
                          ("unsupported", struct.pack("<IIII", 0x425A5531, 9999, 1080, 4))):
        data = bytearray(image.read_bytes())
        writer = MiniFsWriter(data, 67584, 65536)
        settings = writer.add_child(writer.fs.inodes[MINIFS_ROOT_INO], "desktop.settings", MINIFS_FILE)
        writer.set_contents(settings, invalid)
        writer.write_bitmap()
        image.write_bytes(data)
        desktop = Desktop(args.qemu, image, out, name)
        try:
            assert desktop.capture("fallback")[:2] == (1600, 900)
            assert "[gui] restored desktop settings" not in desktop.log()
        finally:
            desktop.close()
    report = {"search": "case-insensitive launch, empty result, editing keys",
              "windows": "tiling, maximize drag, minimize, task switch, workspace, close, minimum size",
              "settings": "saved bytes, cold boot, selection restore, malformed fallback"}
    (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--out-dir", type=Path, default=Path("build/desktop-features"))
    args = parser.parse_args()
    run_checks(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
