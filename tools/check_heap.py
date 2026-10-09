#!/usr/bin/env python3
"""Run the native heap regression ELF on a private QEMU disk without networking."""
import argparse
import json
from pathlib import Path
import re
import socket
import subprocess
import time

from check_gui_idle import read_prompt
from check_minifs import MINIFS_FILE, MINIFS_ROOT_INO
from install_doom_wad import MiniFsWriter


def heap_passes(log):
    log = re.sub(r"\[exec\] entry=0x[0-9a-fA-F]+ task=0x[0-9a-fA-F]+\r?\n", "", log)
    return re.findall(r"heaptest: ok [^\r\n]*?(?=\[(?:exec|syscall|pg|mem)\]|\r?\n|$)", log)


def run(args):
    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    image = out / "heap-test.img"
    if image.exists() or image == args.image.resolve():
        raise ValueError("Test disk must be a new private image")
    data = bytearray(args.image.read_bytes())
    writer = MiniFsWriter(data, 67584, 65536)
    binary = writer.add_child(writer.fs.inodes[MINIFS_ROOT_INO],
                              "heap-regression", MINIFS_FILE)
    writer.set_contents(binary, args.elf.read_bytes())
    writer.write_bitmap()
    image.write_bytes(data)
    serial = out / "heap-serial.log"
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    flags = subprocess.CREATE_NO_WINDOW if hasattr(subprocess, "CREATE_NO_WINDOW") else 0
    with (out / "qemu.log").open("w", encoding="utf-8") as qemu_log:
        process = subprocess.Popen([
            args.qemu, "-accel", "whpx", "-cpu", "qemu64,+rdrand",
            "-smp", "1", "-m", "256", "-nic", "none", "-display", "none",
            "-drive", f"format=raw,file={image}", "-serial", f"file:{serial}",
            "-monitor", f"tcp:127.0.0.1:{port},server,nowait", "-no-reboot"],
            stdout=qemu_log, stderr=qemu_log, creationflags=flags)
        monitor = None
        try:
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                if serial.exists() and ":/>" in serial.read_text(errors="replace"):
                    break
                if process.poll() is not None:
                    raise RuntimeError("QEMU exited during boot")
                time.sleep(.1)
            else:
                raise AssertionError("Guest shell boot timeout")
            monitor = socket.create_connection(("127.0.0.1", port), timeout=10)
            read_prompt(monitor)
            keymap = {" ": "spc", "/": "slash", "-": "minus", "\n": "ret"}
            for character in "exec /fs/heap-regression\n":
                monitor.sendall(f"sendkey {keymap.get(character, character)} 25\n".encode())
                read_prompt(monitor)
                time.sleep(.085)
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                log = serial.read_text(errors="replace")
                if "heaptest: failed" in log or "EXCEPTION" in log:
                    raise AssertionError(log[-3000:])
                if "[exec] exited 0" in log:
                    break
                if process.poll() is not None:
                    raise RuntimeError("QEMU exited during heap test")
                time.sleep(.1)
            else:
                raise AssertionError("Native heap test timeout")
            passed = heap_passes(log)
            assert "heaptest: ok 64-bit allocation boundaries" in passed, passed
            assert "heaptest: ok 320K realloc reuse" in passed, passed
            assert "heaptest: ok 4096 mixed allocation cycles" in passed, passed
            assert any(line.startswith("heaptest: ok physical reclamation ") for line in passed), passed
            assert "heaptest: ok physical OOM rollback and reuse" in passed, passed
            assert "heaptest: ok physical interior reclamation and recommit" in passed, passed
            result = {"passed": passed, "network": "disabled"}
            (out / "results.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
            print(json.dumps(result, indent=2))
        finally:
            if monitor:
                try:
                    monitor.sendall(b"quit\n")
                except OSError:
                    pass
                monitor.close()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=3)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--qemu", default="C:/Program Files/qemu/qemu-system-x86_64.exe")
    run(parser.parse_args())
