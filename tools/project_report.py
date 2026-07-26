#!/usr/bin/env python3
import argparse
import contextlib
import datetime as dt
import io
import json
import re
import subprocess
import struct
import sys
from pathlib import Path

from check_minifs import FsError, MiniFsImage, parse_make_int
from workflow import WORKFLOW

ROOT = Path(__file__).resolve().parents[1]


def read_text(path):
    return (ROOT / path).read_text(encoding="utf-8")


def read_text_if_exists(path):
    full = ROOT / path
    if not full.exists():
        return ""
    return full.read_text(encoding="utf-8")


def parse_make_words(text, name):
    m = re.search(rf"^{re.escape(name)}\s*:?=\s*(.*?)\s*$", text, re.M)
    return m.group(1).split() if m else []


def parse_manifest(text):
    result = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        result[key.strip()] = value.strip()
    return result


def rel(path):
    try:
        return str(path.relative_to(ROOT)).replace("\\", "/")
    except ValueError:
        return str(path).replace("\\", "/")


def bytes_text(value):
    if value is None:
        return "-"
    return f"{value} B ({value / 1024:.1f} KiB)"


def file_size(path):
    return path.stat().st_size if path.exists() else None


def collect_host_doctor(python_cmd, make_cmd, qemu_cmd):
    cmd = [
        sys.executable,
        str(ROOT / "tools" / "doctor.py"),
        "--json",
        "--soft",
        "--no-version",
        "--python", python_cmd,
        "--make", make_cmd,
        "--qemu", qemu_cmd,
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        data = json.loads(proc.stdout)
    except Exception as exc:
        return [{"check": "tools/doctor.py", "status": "error", "path": "", "hint": str(exc)}]
    rows = []
    for row in data.get("checks", []):
        rows.append({
            "check": row.get("name", ""),
            "status": row.get("status", ""),
            "path": row.get("path", ""),
            "hint": row.get("hint", ""),
        })
    return rows


def collect_local_workflow():
    return [
        {
            "phase": row["phase"],
            "command": row["command"],
            "purpose": row["purpose"],
        }
        for row in WORKFLOW
    ]


def collect_guide_docs():
    docs = [
        ("README.md", "main quickstart and feature map"),
        ("README.en.md", "English quickstart and feature map"),
        ("docs/boot-guide.md", "host setup, build, run, QEMU input, and boot troubleshooting"),
        ("docs/user-guide.md", "inside-BuzzOS shell, GUI, filesystem, and diagnostics guide"),
        ("docs/project-status.md", "maturity status, gates, and roadmap"),
        ("docs/user-gui.md", "seeded user GUI app examples"),
        ("docs/procfs.md", "runtime status files"),
        ("docs/minifs.md", "persistent filesystem checks and repair"),
    ]
    rows = []
    for path, purpose in docs:
        rows.append({
            "doc": path,
            "status": "present" if (ROOT / path).exists() else "missing",
            "purpose": purpose,
        })
    return rows


def elf_load_end(path):
    data = path.read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF":
        return None
    _ident, _etype, _emachine, _version, _entry, phoff, _shoff, _flags, _ehsize, phentsize, phnum, *_rest = struct.unpack_from(
        "<16sHHIIIIIHHHHHH", data, 0
    )
    if phentsize != 32 or phoff + phnum * phentsize > len(data):
        return None
    highest = 0
    for i in range(phnum):
        off = phoff + i * phentsize
        ptype, _poff, vaddr, _paddr, _filesz, memsz, _flags, _align = struct.unpack_from("<IIIIIIII", data, off)
        if ptype == 1:
            highest = max(highest, vaddr + memsz)
    return highest or None


def collect_user_elves():
    user_dir = ROOT / "build" / "user"
    rows = []
    if not user_dir.exists():
        return rows
    for elf in sorted(user_dir.glob("*.elf")):
        data = elf.read_bytes()
        end = elf_load_end(elf)
        stripped = "-"
        if len(data) >= 52 and data[:4] == b"\x7fELF":
            shoff = struct.unpack_from("<I", data, 32)[0]
            shentsize = struct.unpack_from("<H", data, 46)[0]
            shnum = struct.unpack_from("<H", data, 48)[0]
            stripped = "yes" if shoff == 0 and shentsize == 0 and shnum == 0 else "no"
        rows.append({
            "name": elf.name,
            "size": elf.stat().st_size,
            "load_end": f"0x{end:06X}" if end else "-",
            "stripped": stripped,
        })
    return rows


def collect_initrd_status():
    initrd = ROOT / "build" / "generated" / "initrd.h"
    mkinitrd = read_text_if_exists("tools/mkinitrd.py")
    bytes_per_line = "-"
    m = re.search(r"BYTES_PER_LINE\s*=\s*(\d+)", mkinitrd)
    if m:
        bytes_per_line = m.group(1)
    if not initrd.exists():
        return [{
            "file": rel(initrd),
            "status": "missing",
            "size": None,
            "lines": "-",
            "blobs": "-",
            "bytes_per_line": bytes_per_line,
        }]
    text = initrd.read_text(encoding="utf-8")
    return [{
        "file": rel(initrd),
        "status": "present",
        "size": initrd.stat().st_size,
        "lines": str(text.count("\n") + (0 if text.endswith("\n") else 1)),
        "blobs": str(len(re.findall(r"^#define\s+INITRD_[A-Z0-9_]+_SIZE\b", text, re.M))),
        "bytes_per_line": bytes_per_line,
    }]


def collect_apps():
    makefile = read_text_if_exists("Makefile")
    apps = parse_make_words(makefile, "GUI_APP_NAMES")
    rows = []
    for app in apps:
        manifest_path = ROOT / "src" / "user" / "bin" / f"{app}.app"
        manifest = parse_manifest(read_text_if_exists(rel(manifest_path))) if manifest_path.exists() else {}
        elf_path = ROOT / "build" / "user" / f"{app}.elf"
        rows.append({
            "app": app,
            "name": manifest.get("name", "-"),
            "version": manifest.get("version", "-"),
            "summary": manifest.get("summary", "-"),
            "state": manifest.get("state", "-"),
            "readme": manifest.get("readme", "-"),
            "elf_size": file_size(elf_path),
        })
    return rows


def collect_gui_style():
    makefile = read_text_if_exists("Makefile")
    apps = parse_make_words(makefile, "GUI_APP_NAMES")
    header = read_text_if_exists("src/user/libc/appui.h")
    helpers = ["appui_fill", "appui_border", "appui_text", "appui_button"]
    rows = [{
        "item": "src/user/libc/appui.h",
        "status": "present" if header else "missing",
        "evidence": "/".join(helpers)
                    if all(name in header for name in helpers)
                    else "incomplete",
    }]
    for app in apps:
        source = read_text_if_exists(f"src/user/bin/{app}.c")
        rows.append({
            "item": app,
            "status": "yes" if '#include "appui.h"' in source else "no",
            "evidence": ",".join(name for name in helpers if name in source) or "-",
        })
    return rows


def collect_minifs(image_path, fs_start, fs_sectors):
    path = ROOT / image_path
    if not path.exists():
        return {"image": image_path, "status": "missing"}
    try:
        data = path.read_bytes()
        fs = MiniFsImage(data, fs_start, fs_sectors)
        if fs.is_empty_region():
            return {"image": image_path, "status": "empty"}
        with contextlib.redirect_stdout(io.StringIO()):
            fs.check()
        used_inodes = [inode for inode in fs.inodes if inode["used"]]
        dirs = [inode for inode in used_inodes if inode["type"] == 1]
        files = [inode for inode in used_inodes if inode["type"] == 2]
        used_blocks = sum(1 for value in fs.bitmap if value)
        return {
            "image": image_path,
            "status": "ok",
            "inodes": f"{len(used_inodes)}/{len(fs.inodes)}",
            "dirs": str(len(dirs)),
            "files": str(len(files)),
            "blocks": f"{used_blocks}/{fs.expected_blocks}",
            "paths": str(len(fs.paths)),
        }
    except FsError as exc:
        return {"image": image_path, "status": "fail", "error": str(exc)}


def collect_procfs_entries():
    source = read_text_if_exists("src/kernel/fs/procfs.c")
    rows = []
    for name in re.findall(r'\{\s*"([^"]+)",\s*PROC_NODE_[A-Z_]+\s*\}', source):
        rows.append({"file": f"/proc/{name}", "status": "registered"})
    return rows


def collect_project_identity():
    procfs = read_text_if_exists("src/kernel/fs/procfs.c")
    m = re.search(r"static\s+int\s+proc_about_text\(.*?\{(.*?)return\s+pos;", procfs, re.S)
    rows = []
    if m:
        for line in re.findall(r'append_text\(buf,\s*&pos,\s*cap,\s*"([^"]+)\\n"\);', m.group(1)):
            parts = line.split(" ", 1)
            if len(parts) == 2:
                rows.append({"item": parts[0], "value": parts[1]})
    if not rows:
        rows.append({"item": "/proc/about", "value": "missing"})
    return rows


def collect_health_interfaces():
    procfs = read_text_if_exists("src/kernel/fs/procfs.c")
    shell = read_text_if_exists("src/user/bin/shell.c")
    gui = read_text_if_exists("src/user/bin/gui.c")
    smoke = read_text_if_exists("scripts/smoke.ps1")
    return [
        {
            "interface": "/proc/health",
            "status": "yes" if '"health", PROC_NODE_HEALTH' in procfs and "proc_health_text" in procfs else "no",
            "evidence": "procfs",
        },
        {
            "interface": "shell health",
            "status": "yes" if 'cmd_cat("/proc/health")' in shell else "no",
            "evidence": "text shell",
        },
        {
            "interface": "GUI shell health",
            "status": "yes" if 'shell_cmd_cat("/proc/health")' in gui else "no",
            "evidence": "user GUI",
        },
        {
            "interface": "smoke coverage",
            "status": "yes" if "cat /proc/health" in smoke and "status\\s+ok" in smoke else "no",
            "evidence": "serial smoke",
        },
    ]


def collect_fs_interfaces():
    procfs = read_text_if_exists("src/kernel/fs/procfs.c")
    shell = read_text_if_exists("src/user/bin/shell.c")
    gui = read_text_if_exists("src/user/bin/gui.c")
    smoke = read_text_if_exists("scripts/smoke.ps1")
    docs = read_text_if_exists("docs/procfs.md") + "\n" + read_text_if_exists("docs/user-guide.md")
    return [
        {
            "interface": "/proc/fs",
            "status": "yes" if '"fs", PROC_NODE_FS' in procfs and "proc_fs_text" in procfs else "no",
            "evidence": "procfs",
        },
        {
            "interface": "shell fsinfo",
            "status": "yes" if 'cmd_cat("/proc/fs")' in shell else "no",
            "evidence": "text shell",
        },
        {
            "interface": "GUI shell fsinfo",
            "status": "yes" if 'shell_cmd_cat("/proc/fs")' in gui else "no",
            "evidence": "user GUI",
        },
        {
            "interface": "smoke coverage",
            "status": "yes" if "cat /proc/fs" in smoke and "host_repair\\s+make fs-repair" in smoke else "no",
            "evidence": "serial smoke",
        },
        {
            "interface": "guide coverage",
            "status": "yes" if "/proc/fs" in docs and "fsinfo" in docs else "no",
            "evidence": "docs",
        },
    ]


def collect_runtime_interfaces():
    procfs = read_text_if_exists("src/kernel/fs/procfs.c")
    m = re.search(r"static\s+int\s+proc_interfaces_text\(.*?\{(.*?)return\s+pos;", procfs, re.S)
    if not m:
        return []
    rows = []
    for line in re.findall(r'append_text\(buf,\s*&pos,\s*cap,\s*"([^"]+)\\n"\);', m.group(1)):
        if line.startswith("NAME "):
            continue
        parts = line.split(" ", 2)
        if len(parts) != 3:
            continue
        rows.append({
            "name": parts[0],
            "status": parts[1],
            "entrypoints": parts[2],
        })
    return rows


def collect_runtime_limits():
    procfs = read_text_if_exists("src/kernel/fs/procfs.c")
    rows = []
    if "proc_limits_text" not in procfs:
        rows.append({"limit": "/proc/limits", "value": "missing"})
        return rows

    task_h = read_text_if_exists("src/kernel/sched/task.h")
    vfs_internal = read_text_if_exists("src/kernel/fs/vfs_internal.h")
    minifs_h = read_text_if_exists("src/kernel/fs/minifs/minifs.h")
    minifs_c = read_text_if_exists("src/kernel/fs/minifs/minifs.c")
    pmm_h = read_text_if_exists("src/kernel/mm/pmm.h")
    pmm_c = read_text_if_exists("src/kernel/mm/pmm.c")

    def c_int(text, name):
        m = re.search(rf"^\s*#define\s+{re.escape(name)}\s+(0x[0-9A-Fa-f]+|[0-9]+)[uUlL]*\b", text, re.M)
        if m:
            return int(m.group(1), 0)
        m = re.search(rf"\b{re.escape(name)}\s*=\s*(0x[0-9A-Fa-f]+|[0-9]+)[uUlL]*\b", text)
        return int(m.group(1), 0) if m else None

    max_tasks = c_int(task_h, "MAX_TASKS")
    max_fd = c_int(vfs_internal, "MAX_FD")
    max_pipes = c_int(vfs_internal, "MAX_PIPES")
    pipe_buf = c_int(vfs_internal, "PIPE_BUFSZ")
    max_mounts = c_int(vfs_internal, "MAX_MOUNTS")
    ramfs_nodes = c_int(vfs_internal, "FS_MAX_NODES")
    ramfs_file_buf = c_int(vfs_internal, "FS_FILE_BUFSZ")
    fs_name_len = c_int(vfs_internal, "FS_NAME_LEN")
    page_size = c_int(pmm_h, "PAGE_SIZE")
    managed_limit = c_int(pmm_c, "PMM_MANAGED_LIMIT")
    minifs_lba = c_int(minifs_h, "MINIFS_LBA_START")
    minifs_sectors = c_int(minifs_h, "MINIFS_SECTORS")
    minifs_inodes = c_int(minifs_c, "MINIFS_INODES")
    minifs_block_size = c_int(minifs_c, "MINIFS_BLOCK_SIZE")
    minifs_direct = c_int(minifs_c, "MINIFS_DIRECT")
    minifs_blocks = None
    minifs_max_file_size = None
    if minifs_sectors is not None and minifs_inodes is not None:
        minifs_blocks = minifs_sectors - 1 - minifs_inodes - 1
    if minifs_block_size is not None and minifs_direct is not None:
        meta_free = minifs_sectors - 1 - minifs_inodes
        bitmap_sectors = (meta_free + minifs_block_size) // (minifs_block_size + 1)
        minifs_max_file_size = (meta_free - bitmap_sectors) * minifs_block_size

    rows = [
        {"limit": "max_tasks", "value": max_tasks},
        {"limit": "max_fd_per_owner", "value": max_fd},
        {"limit": "max_pipes", "value": max_pipes},
        {"limit": "pipe_buf_bytes", "value": pipe_buf},
        {"limit": "max_mounts", "value": max_mounts},
        {"limit": "ramfs_nodes", "value": ramfs_nodes},
        {"limit": "ramfs_file_buf_bytes", "value": ramfs_file_buf},
        {"limit": "fs_name_len", "value": fs_name_len},
        {"limit": "page_size", "value": page_size},
        {"limit": "managed_limit_bytes", "value": managed_limit},
        {"limit": "minifs_lba_start", "value": minifs_lba},
        {"limit": "minifs_sectors", "value": minifs_sectors},
        {"limit": "minifs_inodes", "value": minifs_inodes},
        {"limit": "minifs_blocks", "value": minifs_blocks},
        {"limit": "minifs_max_file_size", "value": minifs_max_file_size},
    ]
    return rows


def collect_ipc_status():
    vfs_internal = read_text_if_exists("src/kernel/fs/vfs_internal.h")
    smoke = read_text_if_exists("scripts/smoke.ps1")
    pipe_buf = "-"
    m = re.search(r"^\s*#define\s+PIPE_BUFSZ\s+(\d+)", vfs_internal, re.M)
    if m:
        pipe_buf = m.group(1)
    checks = [
        ("pipetest", "pipe normal write/read"),
        ("pipeedgetest", "pipe eof/closed-reader edges"),
        ("pipeblocktest", "pipe reader/full-writer blocking"),
        ("futextest", "futex wait/wake"),
        ("futextimeouttest", "futex timeout/wake-before-timeout"),
        ("futexcanceltest", "futex dead-waiter cleanup"),
        ("futexblocktest", "futex blocked thread visibility"),
    ]
    rows = []
    for cmd, purpose in checks:
        rows.append({
            "check": cmd,
            "purpose": purpose,
            "smoke": "yes" if cmd in smoke else "no",
            "pipe_buf": pipe_buf if cmd.startswith("pipe") else "-",
        })
    return rows


def collect_screenshots():
    names = [
        "app-center", "launcher-search", "launcher-no-results", "textedit",
        "taskbar-minimized", "taskbar-restored", "taskbar-tooltip",
        "alt-tab-launcher", "textedit-maximized",
        "textedit-drag-restored", "textedit-snap-preview",
        "textedit-snapped-left",
        "textedit-snapped-right", "textedit-drag-maximized",
        "alt-f4-closed", "filemanager-terminal-exec", "paint",
        "calculator", "browser", "filemanager", "filemanager-textedit",
        "many-windows", "doom", "doom-input", "terminal-about",
    ]
    rows = []
    for name in names:
        path = ROOT / "build" / "gui-smoke" / f"{name}.png"
        rows.append({
            "name": name,
            "path": rel(path),
            "size": file_size(path),
            "status": "present" if path.exists() else "missing",
        })
    return rows


def collect_logs():
    rows = []
    for log in ["build/serial-smoke.log", "build/serial-gui-smoke.log"]:
        path = ROOT / log
        rows.append({
            "path": log,
            "status": "present" if path.exists() else "missing",
            "size": file_size(path),
        })
    return rows


def table(headers, rows):
    out = []
    out.append("| " + " | ".join(headers) + " |")
    out.append("| " + " | ".join("---" for _ in headers) + " |")
    for row in rows:
        out.append("| " + " | ".join(str(row.get(header, "")) for header in headers) + " |")
    return out


def build_report(python_cmd="python", make_cmd="make", qemu_cmd="qemu-system-i386"):
    makefile = read_text_if_exists("Makefile")
    boot_sectors = parse_make_int("BOOT_PARTITION_SECTORS", 0)
    fs_start = parse_make_int("FS_START_SECTOR", 512)
    fs_sectors = parse_make_int("FS_SECTORS", 512)
    kernel_path = ROOT / "build" / "obj" / "kernel" / "kernel.elf"
    image_path = ROOT / "build" / "buzzos.img"
    kernel_size = file_size(kernel_path)
    kernel_limit = boot_sectors * 512 if boot_sectors else None
    kernel_headroom = kernel_limit - kernel_size if kernel_limit is not None and kernel_size is not None else None
    headroom_status = "ok"
    if kernel_headroom is None:
        headroom_status = "missing"
    elif kernel_headroom < 8192:
        headroom_status = "low"

    lines = []
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    lines.append("# BuzzOS Project Report")
    lines.append("")
    lines.append(f"Generated: {stamp}")
    lines.append("")

    lines.append("## Host Doctor")
    lines.extend(table(["check", "status", "path", "hint"], collect_host_doctor(python_cmd, make_cmd, qemu_cmd)))
    lines.append("")

    lines.append("## Local Workflow")
    lines.extend(table(["phase", "command", "purpose"], collect_local_workflow()))
    lines.append("")

    lines.append("## Guide Docs")
    lines.extend(table(["doc", "status", "purpose"], collect_guide_docs()))
    lines.append("")

    lines.append("## Project Identity")
    lines.extend(table(["item", "value"], collect_project_identity()))
    lines.append("")

    lines.append("## Image")
    lines.extend(table(["item", "value"], [
        {"item": "image", "value": f"{rel(image_path)} ({bytes_text(file_size(image_path))})"},
        {"item": "kernel", "value": bytes_text(kernel_size)},
        {"item": "boot partition capacity", "value": bytes_text(kernel_limit)},
        {"item": "boot partition headroom", "value": f"{bytes_text(kernel_headroom)} [{headroom_status}]"},
        {"item": "boot partition sectors", "value": str(boot_sectors)},
        {"item": "fs region", "value": f"LBA {fs_start}..{fs_start + fs_sectors - 1} ({fs_sectors} sectors)"},
    ]))
    lines.append("")

    user_elves = collect_user_elves()
    lines.append("## User ELFs")
    if user_elves:
        lines.extend(table(["name", "size", "load_end", "stripped"], [
            {"name": row["name"], "size": bytes_text(row["size"]), "load_end": row["load_end"], "stripped": row["stripped"]}
            for row in user_elves
        ]))
    else:
        lines.append("No user ELFs found. Run `make` first.")
    lines.append("")

    lines.append("## Initrd")
    lines.extend(table(["file", "status", "size", "lines", "blobs", "bytes_per_line"], [
        {
            "file": row["file"],
            "status": row["status"],
            "size": bytes_text(row["size"]),
            "lines": row["lines"],
            "blobs": row["blobs"],
            "bytes_per_line": row["bytes_per_line"],
        }
        for row in collect_initrd_status()
    ]))
    lines.append("")

    apps = collect_apps()
    lines.append("## GUI Apps")
    if apps:
        lines.extend(table(["app", "name", "version", "summary", "state", "readme", "elf_size"], [
            {
                "app": row["app"],
                "name": row["name"],
                "version": row["version"],
                "summary": row["summary"],
                "state": row["state"],
                "readme": row["readme"],
                "elf_size": bytes_text(row["elf_size"]),
            }
            for row in apps
        ]))
    else:
        lines.append("No GUI_APP_NAMES found in Makefile.")
    lines.append("")

    lines.append("## GUI Style")
    lines.extend(table(["item", "status", "evidence"], collect_gui_style()))
    lines.append("")

    procfs = collect_procfs_entries()
    lines.append("## Procfs")
    if procfs:
        lines.extend(table(["file", "status"], procfs))
    else:
        lines.append("No procfs entries found.")
    lines.append("")

    lines.append("## Health Interfaces")
    lines.extend(table(["interface", "status", "evidence"], collect_health_interfaces()))
    lines.append("")

    runtime_interfaces = collect_runtime_interfaces()
    lines.append("## Runtime Interfaces")
    if runtime_interfaces:
        lines.extend(table(["name", "status", "entrypoints"], runtime_interfaces))
    else:
        lines.append("No `/proc/interfaces` matrix found.")
    lines.append("")

    lines.append("## Runtime Limits")
    lines.extend(table(["limit", "value"], collect_runtime_limits()))
    lines.append("")

    lines.append("## Filesystem Interfaces")
    lines.extend(table(["interface", "status", "evidence"], collect_fs_interfaces()))
    lines.append("")

    lines.append("## IPC")
    lines.extend(table(["check", "purpose", "smoke", "pipe_buf"], collect_ipc_status()))
    lines.append("")

    lines.append("## Minifs")
    fs_rows = [collect_minifs("build/buzzos.img", fs_start, fs_sectors),
               collect_minifs("build/buzzos-test.img", fs_start, fs_sectors)]
    lines.extend(table(["image", "status", "inodes", "dirs", "files", "blocks", "paths", "error"], fs_rows))
    lines.append("")

    lines.append("## GUI Smoke Artifacts")
    lines.extend(table(["name", "status", "size", "path"], [
        {"name": row["name"], "status": row["status"], "size": bytes_text(row["size"]), "path": row["path"]}
        for row in collect_screenshots()
    ]))
    lines.append("")

    lines.append("## Logs")
    lines.extend(table(["path", "status", "size"], [
        {"path": row["path"], "status": row["status"], "size": bytes_text(row["size"])}
        for row in collect_logs()
    ]))
    lines.append("")

    lines.append("## Gates")
    lines.append("")
    lines.append("- `make verify` runs project checks, serial smoke with deterministic single/dual TCP socket coverage, minifs positive/negative/repair checks, and GUI smoke.")
    lines.append("- `make check-project` includes image, memory/VGA-hole, ELF loader hardening, initrd hygiene, syscall ABI, futex scheduler-backed blocking, TCP PCB/demux buffer/single-dual smoke coverage, procfs identity/health/interface/limit/fs diagnostics, shell stdio-only inheritance, multi-stage pipeline/redirection support, pipe blocking semantics, user ELF, initrd reachability, guide docs, GUI style, and app manifest checks.")
    lines.append("- `make fs-repair` writes a conservatively repaired minifs image copy instead of overwriting the current image.")
    lines.append("- `make fs-check-repair` verifies conservative minifs repair on disposable corrupted image copies.")
    lines.append("- `make help` prints the recommended local workflow without building the image.")
    lines.append("- `make run-gui` opens the desktop and seeded GUI applications in visible QEMU.")
    lines.append("- `make doctor` checks the host build/run tools before a user spends time on a failing build.")
    lines.append("- `docs/boot-guide.md` and `docs/user-guide.md` cover local startup, QEMU input, shell, GUI, diagnostics, and troubleshooting.")
    lines.append("- `make report` writes this summary to `build/project-report.md`.")
    if headroom_status == "low":
        lines.append("- Boot partition headroom is low; increase or reshape the image layout before adding kernel payload.")
    lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description="Generate a BuzzOS local project report")
    parser.add_argument("--out", default="build/project-report.md", help="Markdown output path")
    parser.add_argument("--print", action="store_true", help="also print the report to stdout")
    parser.add_argument("--python", default="python", help="Python command shown in host doctor")
    parser.add_argument("--make", default="make", help="Make command shown in host doctor")
    parser.add_argument("--qemu", default="qemu-system-i386", help="QEMU command/path shown in host doctor")
    args = parser.parse_args()

    report = build_report(args.python, args.make, args.qemu)
    out = ROOT / args.out if not Path(args.out).is_absolute() else Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(report + "\n", encoding="utf-8", newline="\n")
    if args.print:
        print(report)
    else:
        print(f"Wrote {rel(out)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
