#!/usr/bin/env python3
"""Sample an idle QEMU desktop; only the bottom clock/taskbar may change."""

import argparse
import json
from pathlib import Path
import socket
import time


def read_frame(path: Path, exclude_bottom: int) -> tuple[int, int, bytes]:
    """Read the binary RGB PPM emitted by QEMU's screendump command."""
    magic, size, maximum, pixels = path.read_bytes().split(b"\n", 3)
    width, height = map(int, size.split())
    if magic != b"P6" or maximum != b"255" or len(pixels) != width * height * 3:
        raise ValueError("Invalid or incomplete QEMU PPM")
    if not 0 <= exclude_bottom < height:
        raise ValueError("Excluded rows must leave a nonempty comparison area")
    return width, height, pixels[:width * (height - exclude_bottom) * 3]


def read_prompt(monitor: socket.socket) -> bytes:
    response = b""
    while not response.endswith(b"(qemu) "):
        chunk = monitor.recv(65536)
        if not chunk:
            raise RuntimeError("QEMU monitor closed")
        response += chunk
    return response


def check_idle(port: int, out_dir: Path, seconds: float,
               interval_ms: int, exclude_bottom: int) -> bool:
    out_dir.mkdir(parents=True, exist_ok=True)
    capture = out_dir.resolve() / "sample.ppm"
    baseline = None
    changed = samples = 0
    with socket.create_connection(("127.0.0.1", port), timeout=5) as monitor:
        read_prompt(monitor)
        start = time.monotonic()
        while baseline is None or time.monotonic() - start < seconds:
            monitor.sendall(f'screendump "{capture.as_posix()}"\n'.encode("ascii"))
            response = read_prompt(monitor)
            if b"Error" in response:
                raise RuntimeError(response.decode("ascii", errors="replace"))
            frame = read_frame(capture, exclude_bottom)
            samples += 1
            if baseline is None:
                baseline = frame
                (out_dir / "baseline.ppm").write_bytes(capture.read_bytes())
            elif frame != baseline:
                changed += 1
                if changed == 1:
                    (out_dir / "changed.ppm").write_bytes(capture.read_bytes())
            time.sleep(interval_ms / 1000)
    report = {"samples": samples, "changed_frames": changed,
              "seconds": round(time.monotonic() - start, 2),
              "exclude_bottom": exclude_bottom, "size": list(baseline[:2])}
    (out_dir / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))
    return samples >= 2 and changed == 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--monitor-port", type=int, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--interval-ms", type=int, default=30)
    parser.add_argument("--exclude-bottom", type=int, default=80)
    args = parser.parse_args()
    if args.seconds <= 0 or args.interval_ms < 1:
        parser.error("Duration and sampling interval must be positive")
    return 0 if check_idle(args.monitor_port, args.out_dir, args.seconds,
                          args.interval_ms, args.exclude_bottom) else 1


if __name__ == "__main__":
    raise SystemExit(main())
