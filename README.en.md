# BuzzOS

BuzzOS is an i386 POSIX-like operating system designed for learning and experimentation. It has evolved into a complete small system that boots to a user-mode shell, runs multiple tasks, mounts persistent file systems, connects to networks, and displays a desktop via the Limine framebuffer.

Chinese version: [README.zh.md](README.zh.md)

Related documentation: [CHANGELOG.md](CHANGELOG.md), [Local Boot Guide](docs/boot-guide.md), [User Guide](docs/user-guide.md), [Project Status](docs/project-status.md), [User GUI App Guide](docs/user-gui.md). Run `make report` to generate a local verification report at `build/project-report.md`.

<table>
  <tr>
    <td><img src="/pic/demo1.png" alt="BuzzOS demo 1" width="480"></td>
    <td><img src="/pic/demo2.png" alt="BuzzOS demo 2" width="480"></td>
  </tr>
  <tr>
    <td><img src="/pic/demo3.png" alt="BuzzOS demo 3" width="480"></td>
    <td><img src="/pic/demo4.png" alt="BuzzOS demo 4" width="480"></td>
  </tr>
</table>

## Current Status

- **Boot chain**: Limine BIOS + multiboot2, requesting `1280x800x32` framebuffer by default.
- **Kernel**: GDT, IDT, exception handling, PIC, PIT, serial port, PS/2 keyboard & mouse, paging, E820/PMM, ELF32 loader.
- **User mode**: `/bin/sh` shell, `nano`, `basm`, `cat`, `echo`, `gui`.
- **Desktop**: User-mode multi-window desktop with support for window activation (raise to top), dragging, resizing, minimize, maximize, close, and scrollbars.
- **GUI apps**: TextEdit, Paint, Calculator as independent user ELF programs, registered via `/fs/apps/*.app` manifests.
- **File system**: VFS + initrd/ramfs + devfs + persistent minifs, `/fs` preserved by default across image rebuilds.
- **Network**: QEMU NE2000, DHCP, DNS, ICMP, UDP, TCP client, and user-mode socket API.
- **IPC/Synchronization**: pipes, blocking read/write wakeup, futex wait/wake.
- **Diagnostic interfaces**: `/proc/about`, `/proc/health`, `/proc/interfaces`, `/proc/limits`, `/proc/fs`, exposed to text shell, GUI shell, and `make report`.

## Quick Start

Required tools:

| Tool | Purpose |
| --- | --- |
| `nasm` | Assembly kernel entry and interrupt stubs |
| `clang` | Compile freestanding C kernel and user programs |
| `ld.lld` | Link kernel and ELF user programs |
| `llvm-objcopy` | Generate auxiliary binary artifacts |
| `python` | Generate initrd, app registry, disk images |
| `powershell` | Run scripts on Windows |
| `qemu-system-i386` | Run BuzzOS |
| Limine binary package | Install BIOS boot stages |

Default Limine path:

```sh
D:/limine-binary/limine-binary
```

This directory must at least contain:

```text
limine-bios.sys
limine-tool-windows-x86/limine.exe
```

If the path differs, pass `LIMINE_DIR=...` during build.

Show the recommended local workflow:

```sh
make help
```

Check your local environment:

```sh
make doctor QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
```

This target runs `tools/doctor.py`; pass `--soft` to that script when you only
want a report without a failing exit status.

Build and run:

```sh
make
make run
```

Run with a visible QEMU window and serial logs written to `build/serial-live.log`:

```sh
make run-local QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
```

Boot directly into the desktop:

```sh
make run-gui QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
```

Rebuild the image and clear `/fs`:

```sh
make image-reset-fs
```

If `build/buzzos.img` or `build/user/*.o` is locked, QEMU is likely still running. Close QEMU before rebuilding.

## Desktop and Apps

Type in the text shell:

```text
gui
```

The desktop opens by default with:

- `Applications`: Application list and manifest details.
- `Terminal`: Shell window inside the desktop.
- `System`: System status panel.

Window features:

- Click to activate and raise to top.
- Drag title bar to move.
- Drag edges or corners to resize.
- Minimize, maximize, close.
- Mouse wheel and scrollbars.
- Dock tasks use app-declared window titles; More expands the full task list and hover shows the complete title.
- The desktop supports up to 10 concurrent external GUI app windows in addition to Apps, Terminal, and System.

Default user GUI apps:

| App | Description |
| --- | --- |
| TextEdit | Text editor with editing area resizing with window, supports Enter, cursor movement, horizontal/vertical scrollbars, saves to `/fs/textedit.txt` |
| Paint | Bitmap drawing tool with canvas and toolbar resizing with window, supports pen, eraser, line, rectangle, fill, and continuous strokes |
| Calculator | Expression calculator supporting parentheses, decimals, and common arithmetic expressions |
| Files | File manager with directory navigation, create, rename, delete, and Open in TextEdit |

View app information in the text shell:

```text
apps
apps info textedit
apps info paint
apps info calculator
apps info filemanager
```

GUI apps are launched from the desktop and are not meant to be run directly from the text shell.

## Writing Your Own GUI App

Recommended path for adding a new app:

```sh
make new-app APP=myapp
```

Then:

1. Implement the user-mode app in `src/user/bin/myapp.c`.
2. Write the manifest in `src/user/bin/myapp.app`.
3. Add `myapp` to `GUI_APP_NAMES` in the Makefile.
4. Run:

```sh
make app-registry
make app-check
make image-reset-fs run
```

The GUI app protocol is defined in [src/user/libc/guiapp.h](src/user/libc/guiapp.h), and basic control drawing utilities are in [src/user/libc/appui.h](src/user/libc/appui.h). The desktop sends init, resize, mouse, key, and close events via pipes, and the app returns either full frames or dirty rectangles.

## Shell Commands

After boot, you will see:

```text
=== BuzzOS User Shell ===
buzzos:/>
```

Common commands:

```text
help
about
health
interfaces
limits
fsinfo
fsstat
fdstat
ls [path]
cd [path]
pwd
stat <path>
cat <file>
mkdir <dir>
rmdir <dir>
touch <file>
write <file> <text>
rm <file>
mv <old> <new>
nano <file>
basm <input.asm> [output]
gui
apps [list|info <name>]
exec <program> [args...] [&|bg]
wait [pid]
kill <pid>
ps [-a]
echo <text>
sleep <seconds>
reboot
```

Network and IPC tests:

```text
ping <host-or-ip>
wget <host> [port]
tcptwotest <host> <port-a> <port-b>
dhcp
netstat
pipetest
pipeedgetest
pipeblocktest
futextest
futextimeouttest
futexcanceltest
futexblocktest
elfbadtest
```

Write and run assembly inside BuzzOS:

```text
nano /fs/demo.asm
basm /fs/demo.asm /fs/demo
exec /fs/demo
```

In `nano`, `Ctrl+T` inserts a minimal assembly template, `Ctrl+S` saves, and `Ctrl+C` exits.

## Image Layout

`build/buzzos.img` is a raw disk image:

| Region | Purpose |
| --- | --- |
| LBA 0 | MBR, Limine BIOS stage installed here |
| LBA 2048..67583 | FAT16 boot partition, contains `kernel.elf`, `limine.conf`, `limine-bios.sys` |
| LBA 67584..71679 | Raw `/fs` minifs partition, default 4096 sectors / 2 MiB |

`tools/mkbootimg.py` preserves the existing `/fs` region by default. To clear it, run `make image-reset-fs`.

Check and repair `/fs`:

```sh
make fs-check
make fs-ls
make fs-repair
```

`make fs-repair` writes `build/buzzos-repaired.img` by default and does not overwrite the current image.

## Verification

Quick consistency check:

```sh
make check-project
```

Serial smoke test:

```sh
make smoke QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
```

GUI smoke test:

```sh
make gui-smoke QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
```

Full verification:

```sh
make verify QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
```

## Design Boundaries

BuzzOS remains a teaching and experimentation system, not a complete Unix:

- No `fork/execve`, permission model, signals, dynamic linking, or mature device model.
- The network stack is a lightweight client implementation. TCP has cumulative ACKs, receive-window advertisement, and bounded retransmission, but no congestion control, selective ACKs, or mature long-lived connection management.
- minifs is a fixed-region, non-journaled small file system.
- The desktop is a user-mode window manager, not a standalone GUI server; the app protocol is already decoupled and can continue to evolve.
- Syscall user pointers are checked page by page for present/user/read-write permissions, but there is no complete `mmap` or demand-paging model.

## Code Entry Points

- Kernel entry: [src/kernel/core/kernel.c](src/kernel/core/kernel.c)
- Multiboot2 entry: [src/kernel/arch/i386/mb2_entry.asm](src/kernel/arch/i386/mb2_entry.asm)
- Framebuffer driver: [src/kernel/drv/fb.c](src/kernel/drv/fb.c)
- Scheduler/processes: [src/kernel/sched/task.c](src/kernel/sched/task.c)
- Syscalls: [src/kernel/syscall/syscall.c](src/kernel/syscall/syscall.c)
- Graphics syscall: [src/kernel/syscall/sys_gfx.c](src/kernel/syscall/sys_gfx.c)
- VFS core: [src/kernel/fs/vfs.c](src/kernel/fs/vfs.c)
- Mini FS: [src/kernel/fs/minifs/minifs.c](src/kernel/fs/minifs/minifs.c)
- Network stack: [src/kernel/net/net.c](src/kernel/net/net.c)
- User shell: [src/user/bin/shell.c](src/user/bin/shell.c)
- Desktop: [src/user/bin/gui.c](src/user/bin/gui.c)
- TextEdit: [src/user/bin/textedit.c](src/user/bin/textedit.c)
- Paint: [src/user/bin/paint.c](src/user/bin/paint.c)
- Calculator: [src/user/bin/calculator.c](src/user/bin/calculator.c)
- GUI app protocol: [src/user/libc/guiapp.h](src/user/libc/guiapp.h)
- User libc: [src/user/libc/libc.c](src/user/libc/libc.c)
