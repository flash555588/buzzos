#!/usr/bin/env python3
import argparse
import json


WORKFLOW = [
    {
        "phase": "preflight",
        "command": "make doctor QEMU=\"C:\\Program Files\\qemu\\qemu-system-i386.exe\"",
        "purpose": "check host tools and QEMU path",
    },
    {
        "phase": "build",
        "command": "make",
        "purpose": "build boot sector, kernel, user ELFs, initrd, and image",
    },
    {
        "phase": "run",
        "command": "make run-local QEMU=\"C:\\Program Files\\qemu\\qemu-system-i386.exe\"",
        "purpose": "open a visible QEMU window while logging serial output",
    },
    {
        "phase": "run",
        "command": "make run-gui QEMU=\"C:\\Program Files\\qemu\\qemu-system-i386.exe\"",
        "purpose": "boot and open the user-space GUI app manager",
    },
    {
        "phase": "test",
        "command": "make smoke QEMU=\"C:\\Program Files\\qemu\\qemu-system-i386.exe\"",
        "purpose": "run serial QEMU smoke coverage",
    },
    {
        "phase": "test",
        "command": "make gui-smoke QEMU=\"C:\\Program Files\\qemu\\qemu-system-i386.exe\"",
        "purpose": "drive GUI examples and validate screenshots",
    },
    {
        "phase": "test",
        "command": "make verify QEMU=\"C:\\Program Files\\qemu\\qemu-system-i386.exe\"",
        "purpose": "run the full local verification gate",
    },
    {
        "phase": "inspect",
        "command": "make report",
        "purpose": "generate build/project-report.md",
    },
    {
        "phase": "inspect",
        "command": "make check-project",
        "purpose": "run fast host-side consistency checks",
    },
    {
        "phase": "apps",
        "command": "make app-check",
        "purpose": "validate seeded GUI app packaging",
    },
    {
        "phase": "fs",
        "command": "make fs-check",
        "purpose": "inspect the current persistent /fs image",
    },
    {
        "phase": "fs",
        "command": "make fs-repair",
        "purpose": "write a conservatively repaired /fs image copy",
    },
    {
        "phase": "fs",
        "command": "make image-reset-fs",
        "purpose": "rebuild the image with a clean /fs region",
    },
]


def print_text():
    print("BuzzOS local workflow")
    for row in WORKFLOW:
        print(f"{row['phase']:9} {row['command']}")
        print(f"{'':9} {row['purpose']}")


def print_markdown():
    print("| phase | command | purpose |")
    print("| --- | --- | --- |")
    for row in WORKFLOW:
        print(f"| {row['phase']} | `{row['command']}` | {row['purpose']} |")


def main():
    parser = argparse.ArgumentParser(description="Print BuzzOS local workflow commands")
    parser.add_argument("--json", action="store_true", help="emit workflow JSON")
    parser.add_argument("--markdown", action="store_true", help="emit a Markdown table")
    args = parser.parse_args()

    if args.json:
        print(json.dumps(WORKFLOW, indent=2))
    elif args.markdown:
        print_markdown()
    else:
        print_text()


if __name__ == "__main__":
    main()
