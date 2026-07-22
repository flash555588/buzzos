#!/usr/bin/env python3
import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def resolve_command(command):
    if not command:
        return None
    expanded = os.path.expandvars(os.path.expanduser(command.strip().strip('"')))
    path = Path(expanded)
    if path.is_absolute() or any(sep in expanded for sep in ("/", "\\")):
        return str(path) if path.exists() else None
    return shutil.which(expanded)


def run_version(path, args):
    if not path or not args:
        return ""
    try:
        proc = subprocess.run([path, *args], capture_output=True, text=True, timeout=5)
    except Exception as exc:
        return f"version unavailable: {exc}"
    text = (proc.stdout or proc.stderr or "").strip().splitlines()
    return text[0] if text else "version unavailable"


def check_tool(name, command, version_args, hint, no_version=False):
    path = resolve_command(command)
    status = "ok" if path else "missing"
    detail = run_version(path, version_args) if path and not no_version else ""
    return {
        "name": name,
        "status": status,
        "command": command,
        "path": path or "",
        "detail": detail,
        "hint": "" if path else hint,
    }


def check_powershell(no_version=False):
    command = "powershell"
    path = resolve_command(command)
    if not path:
        command = "pwsh"
        path = resolve_command(command)
    detail = ""
    if path and not no_version:
        detail = run_version(path, ["-NoProfile", "-Command", "$PSVersionTable.PSVersion.ToString()"])
    return {
        "name": "powershell",
        "status": "ok" if path else "missing",
        "command": command,
        "path": path or "",
        "detail": detail,
        "hint": "" if path else "Install Windows PowerShell or PowerShell 7.",
    }


def check_python(command, no_version=False):
    path = resolve_command(command) or sys.executable
    detail = ""
    if not no_version:
        detail = f"Python {sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}"
    return {
        "name": "python",
        "status": "ok",
        "command": command,
        "path": path,
        "detail": detail,
        "hint": "",
    }


def check_workspace():
    checks = []
    for rel in ["Makefile", "linker.ld", "tools/mkbootimg.py", "scripts/run-local.ps1"]:
        path = ROOT / rel
        checks.append({
            "name": rel,
            "status": "ok" if path.exists() else "missing",
            "command": "",
            "path": str(path) if path.exists() else "",
            "detail": "present" if path.exists() else "",
            "hint": "" if path.exists() else "Run doctor from the BuzzOS repository root.",
        })
    return checks


def build_report(args):
    checks = [
        check_python(args.python, args.no_version),
        check_tool("make", args.make, ["--version"], "Install GNU Make or pass MAKE=/path/to/make.", args.no_version),
        check_powershell(args.no_version),
        check_tool("nasm", args.nasm, ["-v"], "Install NASM and ensure nasm is on PATH.", args.no_version),
        check_tool("clang", args.clang, ["--version"], "Install LLVM/Clang and ensure clang is on PATH.", args.no_version),
        check_tool("ld.lld", args.ld, ["--version"], "Install LLVM lld and ensure ld.lld is on PATH.", args.no_version),
        check_tool("llvm-objcopy", args.objcopy, ["--version"], "Install LLVM tools and ensure llvm-objcopy is on PATH.", args.no_version),
        check_tool("qemu", args.qemu, ["--version"],
                   "Install MSYS2 package mingw-w64-x86_64-qemu "
                   "(C:\\msys64\\mingw64\\bin\\qemu-system-x86_64.exe) "
                   "or pass QEMU=... . Prefer x86_64 + WHPX on Windows.",
                   args.no_version),
    ]
    checks.extend(check_workspace())
    ok_count = sum(1 for row in checks if row["status"] == "ok")
    return {
        "status": "ok" if ok_count == len(checks) else "fail",
        "ok": ok_count,
        "total": len(checks),
        "checks": checks,
    }


def print_human(report):
    print("BuzzOS host doctor")
    print(f"status: {report['status']} ({report['ok']}/{report['total']} checks ok)")
    for row in report["checks"]:
        label = "ok" if row["status"] == "ok" else "missing"
        line = f"[{label}] {row['name']}"
        if row["path"]:
            line += f" -> {row['path']}"
        if row["detail"]:
            line += f" ({row['detail']})"
        print(line)
        if row["hint"]:
            print(f"       hint: {row['hint']}")


def main():
    parser = argparse.ArgumentParser(description="Check the local BuzzOS build/run environment")
    parser.add_argument("--python", default=sys.executable, help="Python command used by Makefile")
    parser.add_argument("--make", default="make", help="GNU Make command")
    parser.add_argument(
        "--qemu",
        default=os.environ.get(
            "QEMU",
            r"C:\msys64\mingw64\bin\qemu-system-x86_64.exe",
        ),
        help="qemu-system-x86_64 (preferred, WHPX) or qemu-system-i386 path",
    )
    parser.add_argument("--nasm", default="nasm")
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--ld", default="ld.lld")
    parser.add_argument("--objcopy", default="llvm-objcopy")
    parser.add_argument("--json", action="store_true", help="emit JSON instead of text")
    parser.add_argument("--soft", action="store_true", help="always exit 0")
    parser.add_argument("--no-version", action="store_true", help="skip subprocess version probes")
    args = parser.parse_args()

    report = build_report(args)
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print_human(report)
    if report["status"] != "ok" and not args.soft:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
