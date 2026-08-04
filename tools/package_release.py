#!/usr/bin/env python3
"""Create self-describing BuzzOS release artifacts after verification."""

import argparse
import hashlib
import json
import re
import shutil
import subprocess
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def command_version(command):
    try:
        result = subprocess.run(
            command, cwd=ROOT, capture_output=True, text=True, timeout=10,
            check=False,
        )
        return (result.stdout or result.stderr).strip().splitlines()[0]
    except (OSError, IndexError, subprocess.SubprocessError) as exc:
        return f"unavailable: {exc}"


def dependency_pins():
    source = (ROOT / "tools" / "fetch-netsurf.ps1").read_text(encoding="utf-8")
    pins = {}
    main_pin = re.search(r'\$revision\s*=\s*"([0-9a-f]{40})"', source)
    zlib_pin = re.search(r'\$zlibRevision\s*=\s*"([0-9a-f]{40})"', source)
    if main_pin:
        pins["netsurf"] = main_pin.group(1)
    for name, revision in re.findall(
        r'^\s*([A-Za-z0-9_]+)\s*=\s*"([0-9a-f]{40})"', source, re.MULTILINE
    ):
        pins[name] = revision
    if zlib_pin:
        pins["zlib"] = zlib_pin.group(1)
    return pins


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--image", default="build/buzzos.img")
    parser.add_argument("--out-dir", default="build/release")
    args = parser.parse_args()

    if not re.fullmatch(r"[A-Za-z0-9._-]+", args.version):
        raise SystemExit("version may contain only letters, digits, dot, underscore, and dash")
    image = (ROOT / args.image).resolve()
    if not image.is_file():
        raise SystemExit(f"missing image: {image}")

    release_dir = (ROOT / args.out_dir / args.version).resolve()
    release_dir.mkdir(parents=True, exist_ok=True)
    output_image = release_dir / f"buzzos-{args.version}-x86_64.img"
    shutil.copy2(image, output_image)
    digest = hashlib.sha256(output_image.read_bytes()).hexdigest()
    (release_dir / f"{output_image.name}.sha256").write_text(
        f"{digest}  {output_image.name}\n", encoding="ascii"
    )

    manifest = {
        "version": args.version,
        "architecture": "x86_64",
        "boot_model": "BIOS/Limine, uniprocessor",
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "image": output_image.name,
        "sha256": digest,
        "git_commit": command_version(["git", "rev-parse", "HEAD"]),
        "tools": {
            "python": command_version(["python", "--version"]),
            "clang": command_version(["clang", "--version"]),
            "lld": command_version(["ld.lld", "--version"]),
            "nasm": command_version(["nasm", "-v"]),
            "qemu": command_version(["qemu-system-x86_64", "--version"]),
        },
        "netsurf_pins": dependency_pins(),
    }
    (release_dir / "dependencies.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    (release_dir / "REPRODUCE.md").write_text(
        "# Reproduce BuzzOS " + args.version + "\n\n"
        "On Windows, fetch the pinned HTTPS dependencies and run:\n\n"
        "```powershell\n"
        "powershell -NoProfile -ExecutionPolicy Bypass -File tools/fetch-netsurf.ps1\n"
        "make -j2\n"
        "make verify QEMU=\"C:\\Program Files\\qemu\\qemu-system-x86_64.exe\"\n"
        f"make release VERSION={args.version}\n"
        "```\n",
        encoding="utf-8",
    )
    for log in (ROOT / "build").glob("serial-*.log"):
        shutil.copy2(log, release_dir / log.name)
    print(f"Release artifacts: {release_dir}")
    print(f"SHA-256: {digest}")


if __name__ == "__main__":
    main()
