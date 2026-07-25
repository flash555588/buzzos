# Project Status

BuzzOS is a lightweight educational i386 operating system. The project is now
organized around a small kernel, user-space programs, a persistent mini
filesystem, and a user-space GUI app manager.

## Current Shape

- Boot: Limine BIOS path loads the kernel through the multiboot2 protocol.
- Kernel: GDT/IDT, paging, preemptive scheduling, syscalls, VFS, ATA, minifs,
  NE2000 networking, keyboard, mouse, framebuffer console, and framebuffer graphics syscalls.
- User space: `/bin/sh`, `/bin/echo`, `/bin/cat`, `nano`, `basm`, `/bin/gui`, and seeded GUI apps under
  `/fs/apps`.
- Pseudo filesystems: `/dev`, persistent `/fs`, and read-only `/proc` status
  files, including process, thread, network, memory, mount, and sync views.
- Project identity: `/proc/about`, text-shell `about`, desktop-terminal
  `about`, and `make report` expose the same compact project introduction and
  documentation map.
- Health interface: `/proc/health`, text-shell `health`, desktop-terminal
  `health`, and `make report` all expose the same compact project health
  surface.
- Interface matrix: `/proc/interfaces`, text-shell `interfaces`,
  desktop-terminal `interfaces`, and `make report` document the current
  stable/experimental entrypoints without adding a heavier registry service.
- Runtime limits: `/proc/limits`, text-shell `limits`, desktop-terminal
  `limits`, and `make report` expose capacity boundaries for tasks, fds, pipes,
  mounts, memory, and minifs without adding a configuration service.
- Filesystem status: `/proc/fs`, text-shell `fsinfo`, desktop-terminal `fsinfo`,
  `fsstat`, smoke coverage, and `make report` expose the live `/fs`/minifs
  counters and host-side check/repair entrypoints.
- GUI apps: `taskmanager`, `terminal`, `textedit`, `paint`, `calculator`,
  `filemanager`, `browser`, `doom`, `music`, and `gameboy`.
- System monitoring: `taskmanager` consumes `/proc/tasks` and
  `/proc/meminfo` to provide live sortable process CPU/resident-memory
  columns, CPU and memory histories, pause/refresh controls, and confirmed
  process-wide termination.
- User GUI helpers: seeded apps use `src/user/libc/appui.h` for lightweight
  controls and `src/user/libc/guiapp.h` for the desktop-hosted app protocol.
  Live-resize / composition trade-offs (1:1 blit, configure coalesce, SHM
  seqlock stride) are documented in `docs/user-gui.md`.
- App packaging: optional `.app` manifests provide `name`, `kind`, `version`,
  `summary`, `state`, `source`, and `readme` metadata for the App Manager.
- App registry: `tools/gen_app_registry.py` generates `build/generated/app_registry.h`
  from app sidecar metadata so kernel seeding stays data-driven.
- Host tooling: `make help` / `tools/workflow.py` lists the recommended local
  workflow, `make run-gui` opens the desktop directly, and `make doctor` /
  `tools/doctor.py` preflights the local Python, Make, PowerShell, NASM, LLVM,
  QEMU, and workspace paths.
- User documentation: `docs/boot-guide.md` covers local startup, QEMU input,
  and boot troubleshooting; `docs/user-guide.md` covers shell, GUI, text input,
  `/fs`, `/proc`, and common diagnostics.
- Initrd hygiene: user ELF payloads are section-stripped before embedding, and
  `tools/mkinitrd.py` emits compact 32-byte rows to reduce generated diff noise.
- App management: the text shell exposes `apps` and `apps info <name>` for
  manifest inspection; GUI apps are launched from the desktop.
- Shell execution: external commands support multi-stage pipelines plus
  `<`, `>`, and `>>` redirection through stdio-only fd inheritance, so temporary
  pipe endpoints do not leak into child processes.
- ELF loader: user executables are size-aware validated before loading; malformed
  headers, out-of-file segments, out-of-window `PT_LOAD` ranges, and invalid
  entry points are rejected before memory writes.
- In-system help: `help apps`, `help gui`, `help files`, `help proc`,
  `help edit`, and `help net` describe common workflows from inside BuzzOS.
- Persistence: `/fs` is preserved across normal image rebuilds.
- Minifs diagnostics: `tools/check_minifs.py` validates the on-image `/fs`
  superblock, inode table, block bitmap, directory tree, and block references.
- In-OS diagnostics: the shell `fsstat` command reports current `/fs` inode and
  block usage from kernel minifs metadata.
- Scheduler diagnostics: `/proc/threads` and the `threads` shell command show
  individual TID/PID state, making blocked futex waits visible.
- Network diagnostics: `/proc/net` and `netstat` show MAC/IP, DHCP state,
  gateway/DNS defaults, TCP state, frame counters, and per-protocol counters.
- Stream sockets carry per-socket TCP PCBs with lightweight input demux and
  small per-PCB receive buffers, so TCP connection state is no longer gated by
  one global active socket.
- The `wget` shell command supports `wget <host> [port]`, including direct IPv4
  targets for local QEMU user-network tests.
- The `tcptwotest` shell command opens two stream sockets and reads them in
  reverse order, exercising TCP PCB demux and receive buffering.
- Synchronization diagnostics: `/proc/sync` and `syncstat` show futex waiter
  table usage and active waiter slots.
- Futex waits are scheduler-backed: waiters enter `TASK_BLOCKED`, wakeups call
  back into the scheduler by futex address, and kill/exit paths clean waiter
  slots.
- FD diagnostics: `/proc/fds` and `fdstat` show the current fd-owner table,
  refs, flags, vnode kind/name, file positions, and pipe endpoint state.
- IPC diagnostics: `pipetest`, `pipeedgetest`, `pipeblocktest`, `futextest`,
  `futextimeouttest`, `futexcanceltest`, and `futexblocktest` cover normal,
  blocking, and edge behavior for pipe/futex primitives.

## Local Workflow

Show the local workflow:

```sh
make help
```

Check local tools before building:

```sh
make doctor QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
```

Build:

```sh
make
```

Run a visible QEMU session without stealing terminal input:

```sh
make run-local QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
```

Start directly in the GUI desktop:

```sh
make run-gui QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
```

Run the smoke test:

```sh
make smoke QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
```

Run the fast host-side consistency check:

```sh
make check-project
```

Inspect the persistent mini filesystem:

```sh
make fs-check
make fs-ls
make fs-repair
make fs-check-negative
make fs-check-repair
```

Inspect live kernel status from inside BuzzOS:

```text
help proc
ls /proc
cat /proc/about
cat /proc/tasks
cat /proc/threads
cat /proc/meminfo
cat /proc/health
cat /proc/interfaces
cat /proc/limits
cat /proc/fs
cat /proc/fds
cat /proc/net
cat /proc/sync
cat /proc/mounts
fdstat
about
health
interfaces
limits
fsinfo
```

Validate only seeded GUI app packaging:

```sh
make app-registry
make app-check
python tools/check_project.py --list-apps
```

Run the GUI smoke test:

```sh
make gui-smoke QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
```

Run all current checks:

```sh
make verify QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
```

Generate a local project status report:

```sh
make report
```

The report is written to `build/project-report.md` and summarizes the kernel
size budget, user ELF sizes, seeded GUI apps, minifs images, GUI smoke
screenshots, and serial logs.

Reset the persistent `/fs` area:

```sh
make image-reset-fs
```

## Verified

- `make` builds `build/buzzos.img`.
- `make smoke` boots QEMU, reaches the shell, verifies `/bin` and `/fs/apps`,
  checks `/proc`, verifies themed shell help and `/proc` read-only behavior,
  checks network status through `/proc/net` and `netstat`, serves deterministic
  host-side TCP HTTP responses, fetches one through `wget`, then exercises two
  concurrent BuzzOS stream sockets through `tcptwotest`, writes and reads a file
  under `/fs`, runs bad-ELF rejection fixtures, pipe and futex demos, checks
  pipe EOF/closed-end/blocking wakeups, verifies multi-stage pipelines and
  input/output/append redirection, verifies futex timeout, dead-waiter
  cleanup, and blocked waiter behavior through `/proc/threads` and
  `/proc/sync`, and checks process output.
- `make check-project` validates image layout, Limine/multiboot2 boot wiring,
  user ELF load ranges, stack bounds, initrd hygiene, ELF loader
  hardening and bad runtime fixture coverage, kernel/user syscall ABI agreement,
  futex scheduler-backed blocking, TCP PCB ownership/demux buffers and smoke
  coverage, shell stdio-only fd inheritance, multi-stage pipeline/redirection
  support, pipe blocking semantics, initrd blob reachability, procfs/health
  diagnostic entries, seeded app manifests, and framebuffer/user memory layout.
- `make app-check` validates seeded `/fs/apps` manifests and their generated
  user ELF files without running QEMU.
- `make run-gui` provides a one-command visible QEMU entrypoint for the desktop.
- `make fs-check` validates the minifs metadata in a raw disk image.
- `make fs-repair` writes a conservatively repaired minifs image copy and keeps
  the current image unchanged by default.
- `make fs-check-negative` mutates a disposable image in memory and verifies
  that the minifs checker rejects representative corruption cases.
- `make fs-check-repair` verifies that conservative minifs repair fixes stale
  free-inode metadata and bitmap drift while refusing unsafe corruptions.
- `fsinfo`, `cat /proc/fs`, and `fsstat` are covered by the serial smoke test
  and report `/fs` usage from inside BuzzOS.
- `make verify` runs the serial smoke test, checks the smoke test image at
  `build/buzzos-test.img`, and runs negative and repair minifs corruption
  fixtures, covering post-boot formatting, app seeding, file creation,
  deletion, checker failure behavior, and safe host-side repair behavior.
- `make gui-smoke` boots a disposable image copy, drives the user GUI, and
  validates generated screenshots under `build/gui-smoke`.
- `make report` writes a human-readable local status report to
  `build/project-report.md`, including kernel headroom, app inventory, minifs
  usage, screenshots, and logs.
- Visual QEMU screenshots verify the framebuffer desktop, application list, and
  seeded app windows.

## Lightweight Boundaries

- The GUI is a user-space multi-window desktop, not yet a standalone GUI server.
- The filesystem is intentionally small and fixed-size.
- The network stack is useful for experiments, but not a mature production TCP/IP
  stack.
- The kernel remains freestanding and compact; richer UI behavior belongs in
  user-space apps unless a kernel primitive is truly needed.

## Next Maturity Targets

- Extend TCP beyond bounded retransmission and cumulative ACKs with congestion
  control, selective acknowledgements, and longer-lived connection tests.
