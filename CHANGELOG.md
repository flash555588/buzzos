# Changelog

This file records the project-level evolution of BuzzOS. It is meant to be a
short log for reviewers and contributors; deeper design notes live under
`docs/`, and generated verification summaries can be produced with
`make report`.

## Unreleased

### Desktop Visual Overhaul (Modern Desktop)

- Added `src/user/libc/uikit.h` (+ `uikit_text.h`, `uikit_icon.h`), a rendering
  kernel with anti-aliased arbitrary-radius rounded rects, per-corner radii,
  gradients, soft shadows, acrylic blur, an integer AA line/arc rasteriser, and
  31 monoline chrome icons drawn from a normalized path table.
- Reworked `palette.h` into design tokens (`UI_*`): accent ramp around
  `#0078D4`, mica/layer/acrylic surfaces, control and stroke states, radii,
  elevation and motion constants. Legacy `THEME_*` macros are retained as
  aliases so every application tracked the new theme before being migrated.
- Rebuilt `appui.h` on top of uikit, keeping the whole existing call surface.
  Added list rows, tabs, checkbox, progress, cards, toolbars, icon buttons,
  scrim, and a **shared scrollbar** whose thumb geometry is used by both the
  painter and hit-testing — six applications previously each derived that math
  twice.
- Restructured `/bin/gui` to the modern desktop layout: no top bar, full-width 48px
  acrylic taskbar with centred Start and window buttons, tray with IME badge
  and clock, Start menu flyout with a pinned app grid, 8px rounded window
  corners with 46×32 caption buttons, Themed context menu and IME panel, thin
  scrollbars, and a gradient wallpaper with a separable bloom.
- Added edge snap: dragging a window into the top maximises, into the left or
  right halves the work area, with a translucent accent preview during the
  drag. `work_area()` is now the single source of truth for maximised bounds.
- Taskbar and Start geometry are produced once and consumed by both the painter
  and the hit tester, so a layout change cannot desynchronise clicks from
  pixels.
- Text is deliberately **not** auto-scaled: `appui_text` still draws on the
  native KFONT grid because Terminal, TextEdit, LuaIDE and Browser use it for
  character-cell layout. Scaled UI sizes are reached via `appui_label`.
- Added `make uikit-test` (`tests/uikit_test.c`), a host-side regression test
  for corner coverage, clipping, stride handling, glyph metrics, shadow seams
  and scrollbar round-tripping. Wired into `make verify`.
- Fixed a shadow seam: `ui_shadow` now attenuates by the **caster** rect's
  rounded coverage rather than by the offset shadow rect, which previously left
  the strip directly under each window covered by neither and showed a bright
  wallpaper edge.
- Fixed `uikit.h` not being listed in `USER_HEADERS`, so header edits did not
  trigger a rebuild.

### GPU (virgl 3D bring-up)

- `virtio_gpu.c` previously read only the high feature dword, so
  `VIRTIO_GPU_F_VIRGL` (low bit 0) was never observed. Feature negotiation now
  reads both and accepts virgl when offered.
- Added `virtio_gpu_3d.c` and `virgl_protocol.h`: context creation, 3D resource
  creation, command-stream submission, and a CLEAR self-test that proved the
  host GPU path end to end without any guest Mesa or OpenGL stack.
- Added `SYS_GPU3D_*` syscalls (`sys_gpu3d.c`), a user-space virgl encoder
  (`src/user/libc/virgl.h`), and `/bin/gputest`, which uploads a texture and
  draws blended, bilinear-filtered textured quads from user space.

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
- Added a graphical System Monitor with live sortable process CPU and
  resident-memory metrics, resource-history graphs, pause/refresh controls,
  and confirmed process termination.
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

- `make verify QEMU="C:\Program Files\qemu\qemu-system-x86_64.exe"`
- `make report`
- `python -m py_compile tools/check_project.py tools/project_report.py tools/mkinitrd.py`
- `make check-project`

The generated project report is written to `build/project-report.md`; the
`build/` directory remains a generated-artifact area.
