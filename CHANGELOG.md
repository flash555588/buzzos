# Changelog

This file records the project-level evolution of BuzzOS. It is meant to be a
short log for reviewers and contributors; deeper design notes live under
`docs/`, and generated verification summaries can be produced with
`make report`.

## Unreleased

### Lua

- Ported Lua 5.4.7 as `/bin/lua` (vendored under `src/user/third_party/lua/`).
- Platform overrides in `src/user/ports/lua/buzzos_lua_port.h` (`LUA_USE_C89`,
  package path under `/fs`, no shell `os.execute`, fixed decimal point).
- Extended mini libc for the port: `setjmp`/`longjmp`, richer stdio
  (`freopen`, `ungetc`, `tmpfile`, …), math (`exp`/`log`/`asin`/…), ctype,
  `localeconv`, and more complete `strftime`.
- Seeds `/fs/hello.lua`; shell help topic `help lua`.
- Added desktop **LuaIDE** (`/fs/apps/luaide`): syntax highlighting, keyword
  auto-complete, New/Save, Run buffer, and an interactive REPL line.
- LuaIDE Open/New go through Files (`openfor:` / `newfor:` picker args);
  `.lua` files open in LuaIDE from the file manager.

### Project Introduction

- Clarified BuzzOS as a small i386 POSIX-like operating system for learning
  and experiments, with a user shell, multitasking, syscalls, VFS, a persistent
  mini filesystem, TCP/UDP/ICMP networking, pipes, futex-style synchronization,
  and a user-space GUI app manager.
- Expanded the documentation map with project status, GUI app examples,
  procfs notes, minifs notes, IPC notes, and work-item tracking.
- Added local-first run and verification guidance for Windows/QEMU workflows,
  including visible QEMU runs that keep keyboard input inside the emulator.
- Added focused local startup and user guides covering repository setup, QEMU
  focus/input, GUI demos, shell commands, `/fs`, `/proc`, and troubleshooting.

### User Experience

- Added `/bin/echo` and `/bin/cat`, multi-stage shell pipelines, basic
  redirection, and stdio inheritance for spawned user programs.
- Added a user-space GUI app center backed by `/fs/apps`.
- Seeded GUI examples:
  - `textedit`: a multiline text editor with persistent document storage.
  - `paint`: a mouse-driven canvas with color/tool controls and saved artwork.
  - `calculator`: a compact four-function calculator with keyboard and mouse
    input.
  - filemanager: a graphical file browser with navigation history, common
    locations, create/rename/delete operations, and keyboard/mouse controls.
- Extended the desktop app protocol with validated cross-app launch requests
  and document arguments; Files now opens regular files in TextEdit, and
  TextEdit loads and saves the requested path.
- Expanded desktop capacity to 10 external app windows, doubled the isolated
  user address range to 16 MiB, and replaced fixed App1/App2/App3 Dock labels
  with app-declared titles, hover tooltips, and an expandable task list.
- Made Files toolbar widths derive from the active font metrics so labels and
  click targets remain aligned without text clipping.
- Added source-side app metadata and registry generation so GUI apps can ship
  with `.app` manifests, readmes, optional seed files, and generated kernel
  seed data.
- Unified seeded desktop app drawing through `src/user/libc/appui.h` and the
  event/frame protocol in `src/user/libc/guiapp.h`; the app scaffolder now
  produces applications compatible with the current window manager.

### Kernel And Runtime

- Added page-table-aware syscall pointer validation, a private read-only user
  trampoline, and process-scoped termination for user-mode CPU exceptions.
- Made main-thread exit process-wide, reclaimed joined thread stack slots and
  process-owned sockets, and added repeated lifecycle regression fixtures.
- Preserved interrupt state across scheduler/lock paths and changed user
  syscall gates to trap gates so timer preemption can continue outside guarded
  critical sections.
- Added cumulative TCP ACK handling, receive-window advertisement, duplicate
  and out-of-order segment handling, bounded SYN/data retransmission, and an
  8 KiB receive queue with larger-flow smoke coverage.
- Added `/proc` diagnostics for tasks, threads, memory, networking, sync
  waiters, file descriptors, and mounts.
- Added a multi-interface project identity surface through `/proc/about`, the
  text-shell `about` command, GUI-shell `about`, smoke coverage, and
  `make report` project identity reporting.
- Added a compact multi-interface health surface through `/proc/health`, the
  text-shell `health` command, GUI-shell `health`, smoke coverage, and
  `make report` interface reporting.
- Added a lightweight `/proc/interfaces` capability matrix with text-shell,
  GUI-shell, smoke, and report coverage for stable/experimental entrypoints.
- Added `/proc/limits`, text-shell `limits`, GUI-shell `limits`, smoke
  coverage, and `make report` runtime limit reporting for lightweight capacity
  discovery.
- Added `/proc/fs`, text-shell `fsinfo`, GUI-shell `fsinfo`, smoke coverage,
  and `make report` filesystem interface reporting for `/fs`/minifs status and
  host-side check/repair entrypoints.
- Improved pipe behavior with blocking read/write wakeups and coverage for
  blocking pipe scenarios.
- Reworked futex wait/wake around scheduler-backed blocking, wake-by-address,
  timeout cleanup, and cancellation cleanup.
- Improved TCP sockets with per-socket PCB state, receive demux, buffering, and
  deterministic single/dual TCP smoke coverage.
- Hardened ELF loading with size-aware validation of ELF headers, program
  header ranges, segment sizes, user load ranges, and executable entry ranges.
- Tightened low-memory layout by moving the kernel load address and reserving
  kernel, stack, and user windows in the physical memory manager.

### Filesystem And Tooling

- Moved generated initrd and app-registry headers to `build/generated`, added a
  cross-platform source-only CI check, and made `make clean` idempotent.
- Added `make run-gui` as the visible QEMU shortcut for the desktop and seeded
  GUI applications.
- Added `make fs-repair` to write a conservatively repaired minifs image copy
  without overwriting the current image.
- Added `make help` / `tools/workflow.py` to print the recommended local
  workflow without building the image.
- Added `make doctor` / `tools/doctor.py` to preflight local Python, Make,
  PowerShell, NASM, LLVM, QEMU, and workspace paths before building or running.
- Added host-side minifs checks, negative checks, and repair checks.
- Added project consistency checks for image layout, stripped user ELF payloads,
  compact initrd rows, generated app registry data, and seeded app outputs.
- Added `make smoke`, `make gui-smoke`, `make verify`, and `make report`
  workflows as reviewer-friendly gates.

### Verification Log

- `make verify QEMU="C:\Program Files\qemu\qemu-system-i386.exe"`
- `make report`
- `python -m py_compile tools/check_project.py tools/project_report.py tools/mkinitrd.py`
- `make check-project`

The generated project report is written to `build/project-report.md`; the
`build/` directory remains a generated-artifact area.
